#include <wayfire/core.hpp>
#include <wayfire/output.hpp>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/plugins/common/cairo-util.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/util/log.hpp>

extern "C" {
#include <vulkan/vulkan.h>
#include <wlr/render/vulkan.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
}

struct CRTParams {
  float resolution[2];
  float anim_progress;
  float time;
  int distort_enable;
  int scanlines_enable;
  int vignette_enable;
  int aberration_enable;
  int r_mask_type;
  float r_beam_sigma;
  float r_scanline_weight;
  float r_border_size;
  float r_brightness;
  float r_convergence_x[2];
  float r_convergence_y[2];
};

struct VulkanShaderResources {
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout desc_layout = VK_NULL_HANDLE;
  VkDescriptorPool desc_pool = VK_NULL_HANDLE;
  VkDescriptorSet desc_set = VK_NULL_HANDLE;
  VkBuffer ubo = VK_NULL_HANDLE;
  VkDeviceMemory ubo_mem = VK_NULL_HANDLE;

  void cleanup(VkDevice device) {
    if (pipeline)
      vkDestroyPipeline(device, pipeline, nullptr);
    if (layout)
      vkDestroyPipelineLayout(device, layout, nullptr);
    if (desc_layout)
      vkDestroyDescriptorSetLayout(device, desc_layout, nullptr);
    if (desc_pool)
      vkDestroyDescriptorPool(device, desc_pool, nullptr);
    if (ubo)
      vkDestroyBuffer(device, ubo, nullptr);
    if (ubo_mem)
      vkFreeMemory(device, ubo_mem, nullptr);
  }
};

class wayfire_crt_vulkan : public wf::per_output_plugin_instance_t {
  std::map<std::string, std::unique_ptr<VulkanShaderResources>> modes;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;

  wf::option_wrapper_t<std::string> opt_mode{"crt-effect/mode"};
  wf::option_wrapper_t<double> r_beam_sigma{"crt-effect/royale_beam_sigma"};

  void transition_layout(VkCommandBuffer cmd, VkImage img, VkImageLayout old_ly,
                         VkImageLayout new_ly) {
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = old_ly;
    barrier.newLayout = new_ly;
    barrier.image = img;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask =
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
  }

  void update_uniforms(VulkanShaderResources &res,
                       const wf::render_buffer_t &dest) {
    CRTParams params;
    params.resolution[0] = dest.get_size().width;
    params.resolution[1] = dest.get_size().height;
    params.r_beam_sigma = (float)r_beam_sigma;

    void *data;
    auto *renderer = wf::get_core().renderer;
    VkDevice device = wlr_vk_renderer_get_device(renderer);
    vkMapMemory(device, res.ubo_mem, 0, sizeof(CRTParams), 0, &data);
    memcpy(data, &params, sizeof(CRTParams));
    vkUnmapMemory(device, res.ubo_mem);
  }

  wf::post_hook_t render_hook = [=](wf::auxilliary_buffer_t &source,
                                    const wf::render_buffer_t &destination) {
    auto *renderer = wf::get_core().renderer;
    VkDevice device = wlr_vk_renderer_get_device(renderer);
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, wlr_vk_renderer_get_queue_family(renderer), 0,
                     &queue);

    auto it = modes.find((std::string)opt_mode);
    if (it == modes.end())
      return;
    auto &res = *it->second;

    auto *src_tex = wlr_texture_from_buffer(renderer, source.get_buffer());
    auto *dst_tex = wlr_texture_from_buffer(renderer, destination.get_buffer());

    wlr_vk_image_attribs src_attr, dst_attr;
    wlr_vk_texture_get_image_attribs(src_tex, &src_attr);
    wlr_vk_texture_get_image_attribs(dst_tex, &dst_attr);

    update_uniforms(res, destination);

    vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &fence);

    VkCommandBufferBeginInfo begin_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd, &begin_info);

    transition_layout(cmd, src_attr.image,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition_layout(cmd, dst_attr.image,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderPassBeginInfo rp_info = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    // Architect Prime: In a production port, you would use a dedicated
    // RenderPass here.

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, res.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, res.layout, 0,
                            1, &res.desc_set, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &submit, fence);

    wlr_texture_destroy(src_tex);
    wlr_texture_destroy(dst_tex);
  };

public:
  void init() override {
    if (!wf::get_core().is_vulkan())
      return;
    // Resource initialization logic here...
    output->render->add_post(&render_hook);
  }

  void fini() override {
    output->render->rem_post(&render_hook);
    auto *renderer = wf::get_core().renderer;
    VkDevice device = wlr_vk_renderer_get_device(renderer);
    for (auto &m : modes)
      m.second->cleanup(device);
  }
};

DECLARE_WAYFIRE_PLUGIN(wf::per_output_plugin_t<wayfire_crt_vulkan>);
