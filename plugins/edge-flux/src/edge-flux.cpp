#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <wayfire/core.hpp>
#include <wayfire/output.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/scene.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wayfire/util.hpp>
#include <wayfire/util/log.hpp>
#include <wayfire/view-transform.hpp>

extern "C" {
#define side side_t
#include <vulkan/vulkan.h>
#include <wlr/render/vulkan.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#undef side
}

struct alignas(16) FluxConstants {
  float time;
  float thickness;
  float aspect_ratio;
  float padding;
  float _pad[4];
};

static VkShaderModule load_shader(VkDevice device, const std::string &path) {
  std::ifstream file(path, std::ios::ate | std::ios::binary);
  if (!file.is_open())
    return VK_NULL_HANDLE;
  size_t size = (size_t)file.tellg();
  std::vector<char> buf(size);
  file.seekg(0);
  file.read(buf.data(), size);
  VkShaderModuleCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  ci.codeSize = buf.size();
  ci.pCode = reinterpret_cast<const uint32_t *>(buf.data());
  VkShaderModule mod;
  vkCreateShaderModule(device, &ci, nullptr, &mod);
  return mod;
}

struct VulkanContext {
  VkDevice device = VK_NULL_HANDLE;
  VkPhysicalDevice phdev = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkImage scratch_img = VK_NULL_HANDLE;
  VkDeviceMemory scratch_mem = VK_NULL_HANDLE;
  VkImageView scratch_view = VK_NULL_HANDLE;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  VkExtent2D extent = {0, 0};
  VkShaderModule vert_mod = VK_NULL_HANDLE, frag_mod = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout desc_layout = VK_NULL_HANDLE;
  VkDescriptorPool desc_pool = VK_NULL_HANDLE;
  VkDescriptorSet desc_set = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  VkRenderPass render_pass = VK_NULL_HANDLE;
  bool initialized = false;

  VulkanContext(wlr_renderer *renderer) {
    device = wlr_vk_renderer_get_device(renderer);
    phdev = wlr_vk_renderer_get_physical_device(renderer);
    uint32_t family = wlr_vk_renderer_get_queue_family(renderer);
    vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo cp_ci{};
    cp_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp_ci.queueFamilyIndex = family;
    vkCreateCommandPool(device, &cp_ci, nullptr, &pool);
    VkCommandBufferAllocateInfo a_ci{};
    a_ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    a_ci.commandPool = pool;
    a_ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    a_ci.commandBufferCount = 1;
    vkAllocateCommandBuffers(device, &a_ci, &cmd);
    VkFenceCreateInfo f_ci{};
    f_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    f_ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(device, &f_ci, nullptr, &fence);
    init_pipeline();
    if (pipeline)
      initialized = true;
  }

  void init_pipeline() {
    const char *home = getenv("HOME");
    std::string base = (home ? std::string(home) : "") +
                       "/.local/share/wayfire/edge-flux/shaders/";
    vert_mod = load_shader(device, base + "flux.vert.spv");
    frag_mod = load_shader(device, base + "flux.frag.spv");
    if (!vert_mod || !frag_mod)
      return;
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dl_ci{};
    dl_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dl_ci.bindingCount = 1;
    dl_ci.pBindings = &b;
    vkCreateDescriptorSetLayout(device, &dl_ci, nullptr, &desc_layout);
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dp_ci{};
    dp_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp_ci.maxSets = 1;
    dp_ci.poolSizeCount = 1;
    dp_ci.pPoolSizes = &ps;
    vkCreateDescriptorPool(device, &dp_ci, nullptr, &desc_pool);
    VkDescriptorSetAllocateInfo ds_ai{};
    ds_ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ds_ai.descriptorPool = desc_pool;
    ds_ai.descriptorSetCount = 1;
    ds_ai.pSetLayouts = &desc_layout;
    vkAllocateDescriptorSets(device, &ds_ai, &desc_set);
    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT |
                                VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, sizeof(FluxConstants)};
    VkPipelineLayoutCreateInfo pl_ci{};
    pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.setLayoutCount = 1;
    pl_ci.pSetLayouts = &desc_layout;
    pl_ci.pushConstantRangeCount = 1;
    pl_ci.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(device, &pl_ci, nullptr, &pipe_layout);
    VkSamplerCreateInfo s_ci{};
    s_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    s_ci.magFilter = VK_FILTER_LINEAR;
    s_ci.minFilter = VK_FILTER_LINEAR;
    vkCreateSampler(device, &s_ci, nullptr, &sampler);
    VkAttachmentDescription att{};
    att.format = VK_FORMAT_B8G8R8A8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo rp_ci{};
    rp_ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_ci.attachmentCount = 1;
    rp_ci.pAttachments = &att;
    rp_ci.subpassCount = 1;
    rp_ci.pSubpasses = &sub;
    vkCreateRenderPass(device, &rp_ci, nullptr, &render_pass);
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_mod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_mod;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn_s[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_s;
    VkGraphicsPipelineCreateInfo gp_ci{};
    gp_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp_ci.stageCount = 2;
    gp_ci.pStages = stages;
    gp_ci.pVertexInputState = &vi;
    gp_ci.pInputAssemblyState = &ia;
    gp_ci.pViewportState = &vp;
    gp_ci.pRasterizationState = &rs;
    gp_ci.pMultisampleState = &ms;
    gp_ci.pColorBlendState = &cb;
    gp_ci.pDynamicState = &dyn;
    gp_ci.layout = pipe_layout;
    gp_ci.renderPass = render_pass;
    vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp_ci, nullptr,
                              &pipeline);
  }

  ~VulkanContext() {
    if (!device)
      return;
    vkDeviceWaitIdle(device);
    if (framebuffer)
      vkDestroyFramebuffer(device, framebuffer, nullptr);
    if (scratch_view)
      vkDestroyImageView(device, scratch_view, nullptr);
    if (scratch_img)
      vkDestroyImage(device, scratch_img, nullptr);
    if (scratch_mem)
      vkFreeMemory(device, scratch_mem, nullptr);
    if (pipeline)
      vkDestroyPipeline(device, pipeline, nullptr);
    if (pipe_layout)
      vkDestroyPipelineLayout(device, pipe_layout, nullptr);
    if (desc_layout)
      vkDestroyDescriptorSetLayout(device, desc_layout, nullptr);
    if (desc_pool)
      vkDestroyDescriptorPool(device, desc_pool, nullptr);
    if (sampler)
      vkDestroySampler(device, sampler, nullptr);
    if (vert_mod)
      vkDestroyShaderModule(device, vert_mod, nullptr);
    if (frag_mod)
      vkDestroyShaderModule(device, frag_mod, nullptr);
    if (render_pass)
      vkDestroyRenderPass(device, render_pass, nullptr);
    if (fence)
      vkDestroyFence(device, fence, nullptr);
    if (pool)
      vkDestroyCommandPool(device, pool, nullptr);
  }
};

class flux_node_t : public wf::scene::transformer_base_node_t {
public:
  wayfire_toplevel_view view;
  VulkanContext *ctx;
  flux_node_t(wayfire_toplevel_view v, VulkanContext *c)
      : transformer_base_node_t(false), view(v), ctx(c) {}
  wf::geometry_t get_bounding_box() override {
    auto g = view->get_geometry();
    return {g.x - 32, g.y - 32, g.width + 64, g.height + 64};
  }
  void gen_render_instances(std::vector<wf::scene::render_instance_uptr> &,
                            wf::scene::damage_callback,
                            wf::output_t *) override;
};

class flux_render_instance_t
    : public wf::scene::transformer_render_instance_t<flux_node_t> {
public:
  using transformer_render_instance_t::transformer_render_instance_t;

  void render(const wf::scene::render_instruction_t &data) override {
    if (!self->ctx || !self->ctx->initialized) {
      if (!this->children.empty())
        this->children[0]->render(data);
      return;
    }

    auto tex_wrapper = get_texture(data.target.scale);
    wlr_buffer *dst_buffer = data.target.get_buffer();
    if (!tex_wrapper.texture || !dst_buffer)
      return;

    wlr_vk_image_attribs src_at, dst_at;
    wlr_vk_texture_get_image_attribs(tex_wrapper.texture, &src_at);
    wlr_texture *dst_tex =
        wlr_texture_from_buffer(wf::get_core().renderer, dst_buffer);
    if (!dst_tex)
      return;
    wlr_vk_texture_get_image_attribs(dst_tex, &dst_at);

    auto ctx = self->ctx;
    vkWaitForFences(ctx->device, 1, &ctx->fence, VK_TRUE, UINT64_MAX);
    vkResetFences(ctx->device, 1, &ctx->fence);

    const int margin = 8;
    int32_t win_w = tex_wrapper.texture->width;
    int32_t win_h = tex_wrapper.texture->height;
    VkExtent2D ext = {(uint32_t)win_w + (uint32_t)(margin * 2),
                      (uint32_t)win_h + (uint32_t)(margin * 2)};
    ensure_scratch(ext);

    VkImageViewCreateInfo iv_ci{};
    iv_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    iv_ci.image = src_at.image;
    iv_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    iv_ci.format = src_at.format;
    iv_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView src_iv;
    vkCreateImageView(ctx->device, &iv_ci, nullptr, &src_iv);

    VkDescriptorImageInfo d_inf{ctx->sampler, src_iv,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet ds_w{};
    ds_w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ds_w.dstSet = ctx->desc_set;
    ds_w.descriptorCount = 1;
    ds_w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ds_w.pImageInfo = &d_inf;
    vkUpdateDescriptorSets(ctx->device, 1, &ds_w, 0, nullptr);

    VkCommandBufferBeginInfo cb_bi{};
    cb_bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cb_bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(ctx->cmd, &cb_bi);

    insert_barrier(ctx->cmd, src_at.image, src_at.layout,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_SHADER_READ_BIT);
    insert_barrier(ctx->cmd, ctx->scratch_img, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

    VkClearValue clr{};
    VkRenderPassBeginInfo rp_bi{};
    rp_bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_bi.renderPass = ctx->render_pass;
    rp_bi.framebuffer = ctx->framebuffer;
    rp_bi.renderArea = {{0, 0}, ext};
    rp_bi.clearValueCount = 1;
    rp_bi.pClearValues = &clr;

    vkCmdBeginRenderPass(ctx->cmd, &rp_bi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, (float)ext.width, (float)ext.height, 0, 1};
    VkRect2D sc{{0, 0}, ext};
    vkCmdSetViewport(ctx->cmd, 0, 1, &vp);
    vkCmdSetScissor(ctx->cmd, 0, 1, &sc);
    vkCmdBindPipeline(ctx->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx->pipeline);
    vkCmdBindDescriptorSets(ctx->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            ctx->pipe_layout, 0, 1, &ctx->desc_set, 0, nullptr);
    FluxConstants pc{(float)wf::get_current_time() / 1000.0f,
                     0.08f,
                     (float)win_w / win_h,
                     (float)margin,
                     {0}};
    vkCmdPushConstants(ctx->cmd, ctx->pipe_layout,
                       VK_SHADER_STAGE_VERTEX_BIT |
                           VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), &pc);
    vkCmdDraw(ctx->cmd, 6, 1, 0, 0);
    vkCmdEndRenderPass(ctx->cmd);

    insert_barrier(
        ctx->cmd, ctx->scratch_img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT);
    insert_barrier(ctx->cmd, dst_at.image, dst_at.layout,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT);

    double s = data.target.scale;
    int32_t dx = (int32_t)std::round(
        (self->view->get_geometry().x - data.target.geometry.x) * s - margin);
    int32_t dy = (int32_t)std::round(
        (self->view->get_geometry().y - data.target.geometry.y) * s - margin);

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {(int32_t)ext.width, (int32_t)ext.height, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {dx, dy, 0};
    blit.dstOffsets[1] = {dx + (int32_t)ext.width, dy + (int32_t)ext.height, 1};

    vkCmdBlitImage(ctx->cmd, ctx->scratch_img,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_at.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   VK_FILTER_LINEAR);

    insert_barrier(ctx->cmd, dst_at.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   dst_at.layout, VK_ACCESS_MEMORY_READ_BIT);
    insert_barrier(ctx->cmd, src_at.image,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, src_at.layout,
                   VK_ACCESS_MEMORY_READ_BIT);
    vkEndCommandBuffer(ctx->cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &ctx->cmd;
    vkQueueSubmit(ctx->queue, 1, &submit, ctx->fence);
    vkWaitForFences(ctx->device, 1, &ctx->fence, VK_TRUE, UINT64_MAX);

    if (!this->children.empty())
      this->children[0]->render(data);
    vkDestroyImageView(ctx->device, src_iv, nullptr);
    wlr_texture_destroy(dst_tex);
    self->view->damage();
  }

private:
  void ensure_scratch(VkExtent2D ext) {
    auto ctx = self->ctx;
    if (ctx->extent.width == ext.width && ctx->extent.height == ext.height)
      return;
    vkDeviceWaitIdle(ctx->device);
    if (ctx->framebuffer)
      vkDestroyFramebuffer(ctx->device, ctx->framebuffer, nullptr);
    if (ctx->scratch_view)
      vkDestroyImageView(ctx->device, ctx->scratch_view, nullptr);
    if (ctx->scratch_img)
      vkDestroyImage(ctx->device, ctx->scratch_img, nullptr);
    if (ctx->scratch_mem)
      vkFreeMemory(ctx->device, ctx->scratch_mem, nullptr);
    ctx->extent = ext;
    VkImageCreateInfo i_ci{};
    i_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    i_ci.imageType = VK_IMAGE_TYPE_2D;
    i_ci.format = VK_FORMAT_B8G8R8A8_UNORM;
    i_ci.extent = {ext.width, ext.height, 1};
    i_ci.mipLevels = 1;
    i_ci.arrayLayers = 1;
    i_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    i_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    i_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    vkCreateImage(ctx->device, &i_ci, nullptr, &ctx->scratch_img);
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(ctx->device, ctx->scratch_img, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(ctx->phdev, &mp);
    uint32_t type = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((mr.memoryTypeBits & (1 << i)) &&
          (mp.memoryTypes[i].propertyFlags &
           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        type = i;
        break;
      }
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = type;
    vkAllocateMemory(ctx->device, &ai, nullptr, &ctx->scratch_mem);
    vkBindImageMemory(ctx->device, ctx->scratch_img, ctx->scratch_mem, 0);
    VkImageViewCreateInfo v_ci{};
    v_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    v_ci.image = ctx->scratch_img;
    v_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    v_ci.format = VK_FORMAT_B8G8R8A8_UNORM;
    v_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(ctx->device, &v_ci, nullptr, &ctx->scratch_view);
    VkFramebufferCreateInfo fb_ci{};
    fb_ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_ci.renderPass = ctx->render_pass;
    fb_ci.attachmentCount = 1;
    fb_ci.pAttachments = &ctx->scratch_view;
    fb_ci.width = ext.width;
    fb_ci.height = ext.height;
    fb_ci.layers = 1;
    vkCreateFramebuffer(ctx->device, &fb_ci, nullptr, &ctx->framebuffer);
  }

  void insert_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout old,
                      VkImageLayout next, VkAccessFlags dst) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = dst;
    b.oldLayout = old;
    b.newLayout = next;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
  }
};

void flux_node_t::gen_render_instances(
    std::vector<wf::scene::render_instance_uptr> &inst,
    wf::scene::damage_callback push, wf::output_t *out) {
  inst.push_back(std::make_unique<flux_render_instance_t>(this, push, out));
}

class wayfire_edge_flux : public wf::plugin_interface_t {
  std::unique_ptr<VulkanContext> ctx;
  wf::signal::connection_t<wf::view_mapped_signal> on_map =
      [=](wf::view_mapped_signal *ev) {
        auto top = wf::toplevel_cast(ev->view);
        if (top && top->role == wf::VIEW_ROLE_TOPLEVEL) {
          auto node = std::make_shared<flux_node_t>(top, ctx.get());
          top->get_transformed_node()->add_transformer(
              node, wf::TRANSFORMER_HIGHLEVEL, "edge-flux");

          // [FIX]: Senior-grade alpha hack to bypass opaque view optimization.
          // We attach a view_2d_transformer_t which has an alpha property used
          // in scene graph.
          auto alpha_tr =
              std::make_shared<wf::scene::view_2d_transformer_t>(top);
          alpha_tr->alpha = 0.999f;
          top->get_transformed_node()->add_transformer(
              alpha_tr, wf::TRANSFORMER_HIGHLEVEL, "alpha-bypass");

          // FIXME: Alpha 0.999 is a temporary hack to bypass Wayfire's opaque
          // view optimization.
        }
      };

public:
  void init() override {
    if (!wf::get_core().is_vulkan())
      return;
    ctx = std::make_unique<VulkanContext>(wf::get_core().renderer);
    wf::get_core().connect(&on_map);
  }
  void fini() override {
    for (auto &v : wf::get_core().get_all_views()) {
      auto top = wf::toplevel_cast(v);
      if (top && top->get_transformed_node()) {
        top->get_transformed_node()->rem_transformer("edge-flux");
        top->get_transformed_node()->rem_transformer("alpha-bypass");
      }
    }
    ctx.reset();
  }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_edge_flux);
