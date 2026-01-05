#include <wayfire/core.hpp>
#include <wayfire/output-layout.hpp>
#include <wayfire/output.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/scene.hpp>
#include <wayfire/seat.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/util/log.hpp>

#include <chrono>
#include <map>
#include <vector>

#include "inertial-node.hpp"
#include "vulkan-context.hpp"

extern "C" {
#include <wlr/render/wlr_texture.h>
}

class wayfire_inertial_vk : public wf::plugin_interface_t {
  std::unique_ptr<VulkanContext> ctx;
  std::chrono::steady_clock::time_point start_time;

  struct active_view_t {
    std::shared_ptr<inertial_node_t> node;
    wayfire_toplevel_view view;
    wf::signal::connection_t<wf::view_pre_unmap_signal> on_unmap;
    std::shared_ptr<wf::scene::view_2d_transformer_t> alpha_tr;
  };
  std::map<wayfire_toplevel_view, std::unique_ptr<active_view_t>> active_views;

  wf::geometry_t get_total_layout_geometry() {
    auto outputs = wf::get_core().output_layout->get_outputs();
    if (outputs.empty())
      return {0, 0, 0, 0};
    int x1 = INT_MAX, y1 = INT_MAX, x2 = INT_MIN, y2 = INT_MIN;
    for (auto &o : outputs) {
      auto g = o->get_layout_geometry();
      x1 = std::min(x1, g.x);
      y1 = std::min(y1, g.y);
      x2 = std::max(x2, g.x + g.width);
      y2 = std::max(y2, g.y + g.height);
    }
    return {x1, y1, x2 - x1, y2 - y1};
  }

  wf::post_hook_t render_hook = [this](wf::auxilliary_buffer_t &source,
                                       const wf::render_buffer_t &destination) {
    if (!ctx || !ctx->initialized || active_views.empty())
      return;

    auto now = std::chrono::steady_clock::now();
    float elapsed = std::chrono::duration<float>(now - start_time).count();

    vkWaitForFences(ctx->device, 1, &ctx->fence, VK_TRUE, UINT64_MAX);
    vkResetFences(ctx->device, 1, &ctx->fence);

    VkExtent2D ext = {(uint32_t)destination.get_size().width,
                      (uint32_t)destination.get_size().height};
    ctx->ensure_scratch(ext);

    auto renderer = wf::get_core().renderer;
    wlr_texture *src_tex =
        wlr_texture_from_buffer(renderer, source.get_buffer());
    wlr_texture *dst_tex =
        wlr_texture_from_buffer(renderer, destination.get_buffer());
    wlr_vk_image_attribs src_attr, dst_attr;
    wlr_vk_texture_get_image_attribs(src_tex, &src_attr);
    wlr_vk_texture_get_image_attribs(dst_tex, &dst_attr);

    VkImageView src_v;
    VkImageViewCreateInfo iv_ci{};
    iv_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    iv_ci.image = src_attr.image;
    iv_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    iv_ci.format = src_attr.format;
    iv_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(ctx->device, &iv_ci, nullptr, &src_v);

    VkDescriptorImageInfo d_info = {ctx->sampler, src_v,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = ctx->desc_set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &d_info;
    vkUpdateDescriptorSets(ctx->device, 1, &write, 0, nullptr);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(ctx->cmd, &begin);

    ctx->insert_barrier(ctx->cmd, src_attr.image, src_attr.layout,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_ACCESS_SHADER_READ_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    ctx->insert_barrier(ctx->cmd, ctx->scratch_img, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    VkClearValue clear = {{{0, 0, 0, 0}}};
    VkRenderPassBeginInfo rp_bi{};
    rp_bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_bi.renderPass = ctx->render_pass;
    rp_bi.framebuffer = ctx->framebuffer;
    rp_bi.renderArea.extent = ctx->extent;
    rp_bi.clearValueCount = 1;
    rp_bi.pClearValues = &clear;

    vkCmdBeginRenderPass(ctx->cmd, &rp_bi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = {0, 0, (float)ext.width, (float)ext.height, 0, 1};
    vkCmdSetViewport(ctx->cmd, 0, 1, &vp);
    VkRect2D sci = {{0, 0}, ext};
    vkCmdSetScissor(ctx->cmd, 0, 1, &sci);
    vkCmdBindPipeline(ctx->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx->pipeline);
    vkCmdBindDescriptorSets(ctx->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            ctx->pipe_layout, 0, 1, &ctx->desc_set, 0, nullptr);

    for (auto &[v, entry] : active_views) {
      auto g = v->get_geometry();
      auto out_box = v->get_output() ? v->get_output()->get_layout_geometry()
                                     : wf::geometry_t{0, 0, 0, 0};
      float localized_x =
          (entry->node->phys_pos_global.x + (float)g.x) - (float)out_box.x;
      float localized_y =
          (entry->node->phys_pos_global.y + (float)g.y) - (float)out_box.y;

      PushConstants pc = {
          {(float)entry->node->grab_off_global.x / (float)g.width,
           (float)entry->node->grab_off_global.y / (float)g.height},
          {localized_x, localized_y},
          0.05f,
          elapsed,
          (float)g.width / (float)g.height,
          128.0f};

      vkCmdPushConstants(ctx->cmd, ctx->pipe_layout,
                         VK_SHADER_STAGE_VERTEX_BIT |
                             VK_SHADER_STAGE_FRAGMENT_BIT,
                         0, sizeof(pc), &pc);
      vkCmdDraw(ctx->cmd, 3, 1, 0, 0);
    }

    vkCmdEndRenderPass(ctx->cmd);

    ctx->insert_barrier(
        ctx->cmd, ctx->scratch_img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);

    ctx->insert_barrier(ctx->cmd, dst_attr.image, dst_attr.layout,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {(int32_t)ext.width, (int32_t)ext.height, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {(int32_t)ext.width, (int32_t)ext.height, 1};
    vkCmdBlitImage(ctx->cmd, ctx->scratch_img,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_attr.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   VK_FILTER_LINEAR);

    ctx->insert_barrier(
        ctx->cmd, dst_attr.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        dst_attr.layout, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    vkEndCommandBuffer(ctx->cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &ctx->cmd;
    vkQueueSubmit(ctx->queue, 1, &submit, ctx->fence);
    vkQueueWaitIdle(ctx->queue);

    vkDestroyImageView(ctx->device, src_v, nullptr);
    wlr_texture_destroy(src_tex);
    wlr_texture_destroy(dst_tex);
  };

  wf::effect_hook_t physics_tick = [=]() {
    auto cursor = wf::get_core().get_cursor_position();
    auto combined_geo = get_total_layout_geometry();
    std::vector<wayfire_toplevel_view> to_remove;
    for (auto &[v, entry] : active_views) {
      auto node = entry->node;
      if (!node->is_moving) {
        to_remove.push_back(v);
        continue;
      }
      const float threshold = 2.0f;
      if (cursor.x <= (combined_geo.x + threshold) ||
          cursor.x >= (combined_geo.x + combined_geo.width - threshold) ||
          cursor.y <= (combined_geo.y + threshold) ||
          cursor.y >= (combined_geo.y + combined_geo.height - threshold)) {
        node->is_moving = false;
        to_remove.push_back(v);
        continue;
      }
      wf::pointf_t goal = {(float)cursor.x - (float)node->grab_off_global.x,
                           (float)cursor.y - (float)node->grab_off_global.y};
      float dx = goal.x - node->phys_pos_global.x;
      float dy = goal.y - node->phys_pos_global.y;
      node->velocity.x = (node->velocity.x + dx * 0.03f) * 0.94f;
      node->velocity.y = (node->velocity.y + dy * 0.03f) * 0.94f;
      node->phys_pos_global.x += node->velocity.x;
      node->phys_pos_global.y += node->velocity.y;
      for (auto &o : wf::get_core().output_layout->get_outputs())
        o->render->damage_whole();
    }
    for (auto v : to_remove)
      deactivate_view(v, true);
    if (active_views.empty()) {
      for (auto &o : wf::get_core().output_layout->get_outputs()) {
        o->render->rem_effect(&physics_tick);
        o->render->rem_post(&render_hook);
      }
    }
  };

  wf::signal::connection_t<wf::output_plugin_activated_changed_signal> on_move =
      [=](auto *ev) {
        if (ev->plugin_name == "move") {
          auto view = wf::toplevel_cast(wf::get_core().seat->get_active_view());
          if (ev->activated) {
            if (view && !active_views.count(view))
              activate_view(view);
          } else if (view && active_views.count(view)) {
            active_views[view]->node->is_moving = false;
          }
        }
      };

  void activate_view(wayfire_toplevel_view view) {
    if (!view->get_output())
      return;
    auto entry = std::make_unique<active_view_t>();
    entry->view = view;
    entry->node = std::make_shared<inertial_node_t>(view, ctx.get());
    auto view_box = view->get_geometry();
    auto out_box = view->get_output()->get_layout_geometry();
    entry->node->phys_pos_global = {(float)(view_box.x + out_box.x),
                                    (float)(view_box.y + out_box.y)};
    auto cursor = wf::get_core().get_cursor_position();
    entry->node->grab_off_global = {
        (float)cursor.x - entry->node->phys_pos_global.x,
        (float)cursor.y - entry->node->phys_pos_global.y};
    entry->node->is_moving = true;
    entry->alpha_tr = std::make_shared<wf::scene::view_2d_transformer_t>(view);
    entry->alpha_tr->alpha = 0.999f;
    view->get_transformed_node()->add_transformer(
        entry->alpha_tr, wf::TRANSFORMER_HIGHLEVEL, "alpha-bypass");
    view->get_transformed_node()->add_transformer(
        entry->node, wf::TRANSFORMER_HIGHLEVEL, "inertial-vk");
    entry->on_unmap.set_callback([=](wf::view_pre_unmap_signal *ev) {
      if (ev->view == view)
        deactivate_view(view, false);
    });
    active_views[view] = std::move(entry);
    wf::get_core().connect(&active_views[view]->on_unmap);
    for (auto &o : wf::get_core().output_layout->get_outputs()) {
      o->render->add_effect(&physics_tick, wf::OUTPUT_EFFECT_PRE);
      o->render->add_post(&render_hook);
    }
  }

  void deactivate_view(wayfire_toplevel_view view, bool apply_final_pos) {
    if (!active_views.count(view))
      return;
    auto &entry = active_views[view];
    if (apply_final_pos && view->get_output()) {
      auto out_box = view->get_output()->get_layout_geometry();
      view->move((int)std::round(entry->node->phys_pos_global.x - out_box.x),
                 (int)std::round(entry->node->phys_pos_global.y - out_box.y));
    }
    view->get_transformed_node()->rem_transformer("inertial-vk");
    view->get_transformed_node()->rem_transformer("alpha-bypass");
    if (entry->on_unmap.is_connected())
      entry->on_unmap.disconnect();
    active_views.erase(view);
    view->damage();
    if (active_views.empty()) {
      for (auto &o : wf::get_core().output_layout->get_outputs()) {
        o->render->rem_effect(&physics_tick);
        o->render->rem_post(&render_hook);
      }
    }
  }

public:
  void init() override {
    if (!wf::get_core().is_vulkan())
      return;
    ctx = std::make_unique<VulkanContext>(wf::get_core().renderer);
    start_time = std::chrono::steady_clock::now();
    ctx->setup_pipeline();
    wf::get_core().connect(&on_move);
  }

  void fini() override {
    auto it = active_views.begin();
    while (it != active_views.end())
      deactivate_view((it++)->first, false);
  }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_inertial_vk);
