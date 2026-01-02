/**
 * @file inertial-view-vk.cpp
 * @brief Layout-Absolute Vulkan-accelerated inertial window transformation.
 */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <vector>

#include <wayfire/core.hpp>
#include <wayfire/output-layout.hpp>
#include <wayfire/output.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/scene.hpp>
#include <wayfire/seat.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wayfire/util.hpp>
#include <wayfire/util/log.hpp>
#include <wayfire/view-transform.hpp>

#include <wayfire/plugins/common/move-drag-interface.hpp>
#include <wayfire/plugins/common/shared-core-data.hpp>

extern "C" {
#include <vulkan/vulkan.h>
#include <wlr/render/vulkan.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
}

struct alignas(16) PushConstants {
  float anchor_pos[2];
  float velocity[2];
  float spring_k;
  float friction;
  float aspect_ratio;
  float padding;
};

static std::string fmt_f(float f) {
  std::stringstream ss;
  ss << std::fixed << std::setprecision(4) << f;
  return ss.str();
}

static VkShaderModule load_shader(VkDevice device, const std::string &path) {
  LOGI("[SHADER] Loading binary: ", path);
  std::ifstream file(path, std::ios::ate | std::ios::binary);
  if (!file.is_open()) {
    LOGE("[SHADER] FAILED to open: ", path);
    return VK_NULL_HANDLE;
  }

  size_t size = (size_t)file.tellg();
  std::vector<char> buf(size);
  file.seekg(0);
  file.read(buf.data(), size);

  VkShaderModuleCreateInfo ci = {};
  ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  ci.codeSize = buf.size();
  ci.pCode = reinterpret_cast<const uint32_t *>(buf.data());

  VkShaderModule mod;
  if (vkCreateShaderModule(device, &ci, nullptr, &mod) != VK_SUCCESS) {
    LOGE("[SHADER] FAILED creation for: ", path);
    return VK_NULL_HANDLE;
  }
  LOGI("[SHADER] SUCCESS: ", path, " (Size: ", size, ")");
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
    LOGI("[VK] Initializing context for renderer: ", (void *)renderer);
    device = wlr_vk_renderer_get_device(renderer);
    phdev = wlr_vk_renderer_get_physical_device(renderer);
    uint32_t family = wlr_vk_renderer_get_queue_family(renderer);
    vkGetDeviceQueue(device, family, 0, &queue);

    VkCommandPoolCreateInfo cp_ci = {
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp_ci.queueFamilyIndex = family;
    vkCreateCommandPool(device, &cp_ci, nullptr, &pool);

    VkCommandBufferAllocateInfo a_ci = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    a_ci.commandPool = pool;
    a_ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    a_ci.commandBufferCount = 1;
    vkAllocateCommandBuffers(device, &a_ci, &cmd);

    VkFenceCreateInfo f_ci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    f_ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(device, &f_ci, nullptr, &fence);

    init_pipeline();
    if (pipeline)
      initialized = true;
  }

  void init_pipeline() {
    const char *home = getenv("HOME");
    std::string base = (home ? std::string(home) : "") +
                       "/.local/share/wayfire/inertial-vk/shaders/";
    vert_mod = load_shader(device, base + "inertial.vert.spv");
    frag_mod = load_shader(device, base + "inertial.frag.spv");
    if (!vert_mod || !frag_mod) {
      LOGE("[VK] Shaders failed to load from: ", base);
      return;
    }

    VkDescriptorSetLayoutBinding b = {};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dl_ci = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl_ci.bindingCount = 1;
    dl_ci.pBindings = &b;
    vkCreateDescriptorSetLayout(device, &dl_ci, nullptr, &desc_layout);

    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dp_ci = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp_ci.maxSets = 1;
    dp_ci.poolSizeCount = 1;
    dp_ci.pPoolSizes = &ps;
    vkCreateDescriptorPool(device, &dp_ci, nullptr, &desc_pool);

    VkDescriptorSetAllocateInfo ds_ai = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ds_ai.descriptorPool = desc_pool;
    ds_ai.descriptorSetCount = 1;
    ds_ai.pSetLayouts = &desc_layout;
    vkAllocateDescriptorSets(device, &ds_ai, &desc_set);

    VkPushConstantRange pcr = {};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo pl_ci = {
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl_ci.setLayoutCount = 1;
    pl_ci.pSetLayouts = &desc_layout;
    pl_ci.pushConstantRangeCount = 1;
    pl_ci.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(device, &pl_ci, nullptr, &pipe_layout);

    VkSamplerCreateInfo s_ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    s_ci.magFilter = VK_FILTER_LINEAR;
    s_ci.minFilter = VK_FILTER_LINEAR;
    vkCreateSampler(device, &s_ci, nullptr, &sampler);

    VkAttachmentDescription att = {};
    att.format = VK_FORMAT_B8G8R8A8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;

    VkRenderPassCreateInfo rp_ci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp_ci.attachmentCount = 1;
    rp_ci.pAttachments = &att;
    rp_ci.subpassCount = 1;
    rp_ci.pSubpasses = &sub;
    vkCreateRenderPass(device, &rp_ci, nullptr, &render_pass);

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_mod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_mod;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba = {};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = 0xf;

    VkPipelineColorBlendStateCreateInfo cb = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn_s[] = {VK_DYNAMIC_STATE_VIEWPORT,
                              VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_s;

    VkGraphicsPipelineCreateInfo gp_ci = {
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
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
    LOGI("[VK] Pipeline successfully baked.");
  }

  ~VulkanContext() {
    if (!device)
      return;
    LOGI("[VK] Destroying context.");
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

class inertial_node_t : public wf::scene::transformer_base_node_t {
public:
  wf::pointf_t phys_pos_global;
  wf::pointf_t velocity = {0, 0};
  wf::pointf_t grab_off_global;
  bool is_moving = false;
  wayfire_toplevel_view view;
  VulkanContext *ctx;

  inertial_node_t(wayfire_toplevel_view v, VulkanContext *c)
      : transformer_base_node_t(false), view(v), ctx(c) {}

  wf::geometry_t get_bounding_box() override {
    if (!view)
      return {0, 0, 0, 0};
    auto g = view->get_geometry();
    auto out = view->get_output();
    if (!out)
      return g;
    auto ob = out->get_layout_geometry();

    int vg_x = g.x + ob.x;
    int vg_y = g.y + ob.y;

    int min_x = std::min((int)std::floor(phys_pos_global.x), vg_x) - 100;
    int min_y = std::min((int)std::floor(phys_pos_global.y), vg_y) - 100;
    int max_x =
        std::max((int)std::ceil(phys_pos_global.x + g.width), vg_x + g.width) +
        100;
    int max_y = std::max((int)std::ceil(phys_pos_global.y + g.height),
                         vg_y + g.height) +
                100;

    return {min_x, min_y, max_x - min_x, max_y - min_y};
  }

  void gen_render_instances(std::vector<wf::scene::render_instance_uptr> &inst,
                            wf::scene::damage_callback push,
                            wf::output_t *out) override;
};

class inertial_render_instance_t
    : public wf::scene::transformer_render_instance_t<inertial_node_t> {
public:
  using transformer_render_instance_t::transformer_render_instance_t;

  void render(const wf::scene::render_instruction_t &data) override {
    if (!self->ctx || !self->ctx->initialized) {
      if (!this->children.empty()) {
        this->children[0]->render(data);
      }
      return;
    }

    auto tex_wrapper = get_texture(data.target.scale);
    wlr_buffer *dst_buffer = data.target.get_buffer();
    if (!tex_wrapper.texture || !dst_buffer) {
      if (!this->children.empty()) {
        this->children[0]->render(data);
      }
      return;
    }

    wlr_vk_image_attribs src_at, dst_at;
    wlr_vk_texture_get_image_attribs(tex_wrapper.texture, &src_at);

    wlr_texture *dst_tex =
        wlr_texture_from_buffer(wf::get_core().renderer, dst_buffer);
    if (!dst_tex) {
      if (!this->children.empty()) {
        this->children[0]->render(data);
      }
      return;
    }
    wlr_vk_texture_get_image_attribs(dst_tex, &dst_at);

    auto ctx = self->ctx;
    vkWaitForFences(ctx->device, 1, &ctx->fence, VK_TRUE, UINT64_MAX);
    vkResetFences(ctx->device, 1, &ctx->fence);

    const int margin = 32;
    int32_t win_w = tex_wrapper.texture->width;
    int32_t win_h = tex_wrapper.texture->height;

    VkExtent2D ext = {(uint32_t)win_w + (uint32_t)(margin),
                      (uint32_t)win_h + (uint32_t)(margin)};
    ensure_scratch(ext);

    VkImageViewCreateInfo iv_ci;
    memset(&iv_ci, 0, sizeof(iv_ci));
    iv_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    iv_ci.image = src_at.image;
    iv_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    iv_ci.format = src_at.format;
    iv_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    iv_ci.subresourceRange.levelCount = 1;
    iv_ci.subresourceRange.layerCount = 1;

    VkImageView v_iv;
    vkCreateImageView(ctx->device, &iv_ci, nullptr, &v_iv);

    VkDescriptorImageInfo d_inf;
    memset(&d_inf, 0, sizeof(d_inf));
    d_inf.sampler = ctx->sampler;
    d_inf.imageView = v_iv;
    d_inf.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet ds_w;
    memset(&ds_w, 0, sizeof(ds_w));
    ds_w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ds_w.dstSet = ctx->desc_set;
    ds_w.descriptorCount = 1;
    ds_w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ds_w.pImageInfo = &d_inf;
    vkUpdateDescriptorSets(ctx->device, 1, &ds_w, 0, nullptr);

    VkCommandBufferBeginInfo cb_bi;
    memset(&cb_bi, 0, sizeof(cb_bi));
    cb_bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cb_bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(ctx->cmd, &cb_bi);

    insert_barrier(ctx->cmd, src_at.image, src_at.layout,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_SHADER_READ_BIT);
    insert_barrier(ctx->cmd, ctx->scratch_img, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

    VkClearValue clr;
    memset(&clr, 0, sizeof(clr));
    clr.color.float32[3] = 0.0f;

    VkRenderPassBeginInfo rp_bi;
    memset(&rp_bi, 0, sizeof(rp_bi));
    rp_bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_bi.renderPass = ctx->render_pass;
    rp_bi.framebuffer = ctx->framebuffer;
    rp_bi.renderArea.extent = ext;
    rp_bi.clearValueCount = 1;
    rp_bi.pClearValues = &clr;

    vkCmdBeginRenderPass(ctx->cmd, &rp_bi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp = {0.0f, 0.0f, (float)ext.width, (float)ext.height,
                     0.0f, 1.0f};
    VkRect2D sc = {{0, 0}, ext};
    vkCmdSetViewport(ctx->cmd, 0, 1, &vp);
    vkCmdSetScissor(ctx->cmd, 0, 1, &sc);

    vkCmdBindPipeline(ctx->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx->pipeline);
    vkCmdBindDescriptorSets(ctx->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            ctx->pipe_layout, 0, 1, &ctx->desc_set, 0, nullptr);

    auto g = self->view->get_geometry();
    PushConstants pc;
    memset(&pc, 0, sizeof(pc));
    pc.anchor_pos[0] = (float)self->grab_off_global.x / (float)g.width;
    pc.anchor_pos[1] = (float)self->grab_off_global.y / (float)g.height;
    pc.velocity[0] = (float)self->velocity.x;
    pc.velocity[1] = (float)self->velocity.y;
    pc.spring_k = 0.65f;
    pc.friction = 0.85f;
    pc.aspect_ratio = (float)g.width / (float)g.height;
    pc.padding = (float)margin;

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
    double lx = (self->phys_pos_global.x - data.target.geometry.x) * s - margin;
    double ly = (self->phys_pos_global.y - data.target.geometry.y) * s - margin;

    int32_t dx = (int32_t)std::round(lx);
    int32_t dy = (int32_t)std::round(ly);

    VkImageBlit blit;
    memset(&blit, 0, sizeof(blit));
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[1].x = (int32_t)ext.width;
    blit.srcOffsets[1].y = (int32_t)ext.height;
    blit.srcOffsets[1].z = 1;

    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[0].x = dx;
    blit.dstOffsets[0].y = dy;
    blit.dstOffsets[1].x = dx + (int32_t)ext.width;
    blit.dstOffsets[1].y = dy + (int32_t)ext.height;
    blit.dstOffsets[1].z = 1;

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

    VkSubmitInfo submit;
    memset(&submit, 0, sizeof(submit));
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &ctx->cmd;
    vkQueueSubmit(ctx->queue, 1, &submit, ctx->fence);

    vkWaitForFences(ctx->device, 1, &ctx->fence, VK_TRUE, UINT64_MAX);

    if (!this->children.empty()) {
      this->children[0]->render(data);
    }

    vkDestroyImageView(ctx->device, v_iv, nullptr);
    wlr_texture_destroy(dst_tex);
  }

private:
  void ensure_scratch(VkExtent2D ext) {
    auto ctx = self->ctx;
    if (ctx->extent.width == ext.width && ctx->extent.height == ext.height)
      return;
    LOGI("[VK] Scratch resize -> ", ext.width, "x", ext.height);
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
    VkImageCreateInfo i_ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    i_ci.imageType = VK_IMAGE_TYPE_2D;
    i_ci.format = VK_FORMAT_B8G8R8A8_UNORM;
    i_ci.extent = {ext.width, ext.height, 1};
    i_ci.mipLevels = 1;
    i_ci.arrayLayers = 1;
    i_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    i_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    i_ci.usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    i_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    i_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
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

    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = type;
    vkAllocateMemory(ctx->device, &ai, nullptr, &ctx->scratch_mem);
    vkBindImageMemory(ctx->device, ctx->scratch_img, ctx->scratch_mem, 0);

    VkImageViewCreateInfo v_ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    v_ci.image = ctx->scratch_img;
    v_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    v_ci.format = VK_FORMAT_B8G8R8A8_UNORM;
    v_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(ctx->device, &v_ci, nullptr, &ctx->scratch_view);

    VkFramebufferCreateInfo fb_ci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
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
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = dst;
    b.oldLayout = old;
    b.newLayout = next;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
  }
};

void inertial_node_t::gen_render_instances(
    std::vector<wf::scene::render_instance_uptr> &inst,
    wf::scene::damage_callback push, wf::output_t *out) {
  inst.push_back(std::make_unique<inertial_render_instance_t>(this, push, out));
}

class wayfire_inertial_vk : public wf::plugin_interface_t {
  std::unique_ptr<VulkanContext> ctx;
  struct active_view_t {
    std::shared_ptr<inertial_node_t> node;
    wayfire_toplevel_view view;
    wf::signal::connection_t<wf::view_pre_unmap_signal> on_unmap;
  };
  std::map<wayfire_toplevel_view, active_view_t> active_views;

  wf::effect_hook_t physics_tick = [=]() {
    auto cursor = wf::get_core().get_cursor_position();
    std::vector<wayfire_toplevel_view> to_remove;
    for (auto &[view, entry] : active_views) {
      auto node = entry.node;
      wf::pointf_t goal = {(float)cursor.x - node->grab_off_global.x,
                           (float)cursor.y - node->grab_off_global.y};
      float dx = goal.x - node->phys_pos_global.x;
      float dy = goal.y - node->phys_pos_global.y;
      node->velocity.x = (node->velocity.x + dx * 0.16f) * 0.78f;
      node->velocity.y = (node->velocity.y + dy * 0.16f) * 0.78f;
      node->phys_pos_global.x += node->velocity.x;
      node->phys_pos_global.y += node->velocity.y;
      view->damage();

      if (!node->is_moving && (std::abs(node->velocity.x) < 0.05f) &&
          (std::abs(node->velocity.y) < 0.05f) && (std::hypot(dx, dy) < 0.5f))
        to_remove.push_back(view);
    }
    for (auto v : to_remove)
      deactivate_view(v, true);
    if (active_views.empty()) {
      for (auto &out : wf::get_core().output_layout->get_outputs())
        out->render->rem_effect(&physics_tick);
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
            active_views[view].node->is_moving = false;
          }
        }
      };

  void activate_view(wayfire_toplevel_view view) {
    auto out = view->get_output();
    if (!out)
      return;

    LOGI("[PLUGIN] ACTIVATING for: ", view->get_title());
    auto node = std::make_shared<inertial_node_t>(view, ctx.get());
    auto wm_box = view->get_geometry();
    auto out_box = out->get_layout_geometry();

    node->phys_pos_global = {(float)(wm_box.x + out_box.x),
                             (float)(wm_box.y + out_box.y)};
    auto cursor = wf::get_core().get_cursor_position();
    node->grab_off_global = {(float)cursor.x - node->phys_pos_global.x,
                             (float)cursor.y - node->phys_pos_global.y};
    node->is_moving = true;

    active_views[view].node = node;
    active_views[view].view = view;
    active_views[view].on_unmap = [=](wf::view_pre_unmap_signal *ev) {
      if (ev->view == view)
        deactivate_view(view, false);
    };
    wf::get_core().connect(&active_views[view].on_unmap);

    view->get_transformed_node()->add_transformer(
        node, wf::TRANSFORMER_HIGHLEVEL, "inertial-vk");
    for (auto &o : wf::get_core().output_layout->get_outputs())
      o->render->add_effect(&physics_tick, wf::OUTPUT_EFFECT_PRE);

    view->damage();
  }

  void deactivate_view(wayfire_toplevel_view view, bool apply_final_pos) {
    if (!active_views.count(view))
      return;
    LOGI("[PLUGIN] DEACTIVATING for: ", view->get_title());
    auto &entry = active_views[view];
    if (apply_final_pos) {
      auto out = view->get_output();
      if (out) {
        auto out_box = out->get_layout_geometry();
        view->move((int)std::round(entry.node->phys_pos_global.x - out_box.x),
                   (int)std::round(entry.node->phys_pos_global.y - out_box.y));
      }
    }
    view->get_transformed_node()->rem_transformer("inertial-vk");
    entry.on_unmap.disconnect();
    active_views.erase(view);
    view->damage();
  }

public:
  void init() override {
    LOGI("[PLUGIN] Init start.");
    if (!wf::get_core().is_vulkan()) {
      LOGE("[PLUGIN] FAILED: Core NOT in Vulkan mode.");
      return;
    }
    ctx = std::make_unique<VulkanContext>(wf::get_core().renderer);
    wf::get_core().connect(&on_move);
    LOGI("[PLUGIN] Init success.");
  }

  void fini() override {
    LOGI("[PLUGIN] Shutdown.");
    auto it = active_views.begin();
    while (it != active_views.end())
      deactivate_view((it++)->first, false);
    for (auto &out : wf::get_core().output_layout->get_outputs())
      out->render->rem_effect(&physics_tick);
  }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_inertial_vk);
