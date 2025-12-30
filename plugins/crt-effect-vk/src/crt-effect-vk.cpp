#include <chrono>
#include <cstring>
#include <fstream>
#include <vector>
#include <wayfire/core.hpp>
#include <wayfire/output.hpp>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/util/duration.hpp>
#include <wayfire/util/log.hpp>

extern "C" {
#include <vulkan/vulkan.h>
#include <wlr/render/vulkan.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
}

struct alignas(16) CRTPushConstants {
  float res[2] = {0.0f, 0.0f};
  float time = 0.0f;
  float progress = 0.0f;
  int mask_type = 0;
  float beam_sigma = 0.0f;
  float border_size = 0.0f;
  float scanline_weight = 0.0f;
  float brightness = 0.0f;
  float conv_x[2] = {0.0f, 0.0f};
  float conv_y[2] = {0.0f, 0.0f};
  int distort_enable = 0;
};

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

  ~VulkanContext() {
    LOGI("CRT-VK: Destroying VulkanContext.");
    if (device != VK_NULL_HANDLE) {
      vkDeviceWaitIdle(device);
      if (framebuffer)
        vkDestroyFramebuffer(device, framebuffer, nullptr);
      if (scratch_view)
        vkDestroyImageView(device, scratch_view, nullptr);
      if (scratch_img)
        vkDestroyImage(device, scratch_img, nullptr);
      if (scratch_mem)
        vkFreeMemory(device, scratch_mem, nullptr);
      if (cmd && pool)
        vkFreeCommandBuffers(device, pool, 1, &cmd);
      if (pool)
        vkDestroyCommandPool(device, pool, nullptr);
      if (fence)
        vkDestroyFence(device, fence, nullptr);
    }
  }
};

class wayfire_crt_vulkan : public wf::per_output_plugin_instance_t {
  std::unique_ptr<VulkanContext> ctx;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout desc_layout = VK_NULL_HANDLE;
  VkDescriptorPool desc_pool = VK_NULL_HANDLE;
  VkDescriptorSet desc_set = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  VkShaderModule frag_module = VK_NULL_HANDLE;
  VkShaderModule vert_module = VK_NULL_HANDLE;
  VkRenderPass render_pass = VK_NULL_HANDLE;

  struct {
    VkImageView view = VK_NULL_HANDLE;
    wlr_texture *tex = nullptr;
  } src_frame;

  wf::animation::simple_animation_t progression;
  std::chrono::steady_clock::time_point start_time;

  bool active = false;
  wf::option_wrapper_t<bool> opt_enabled{"crt-effect-vk/enabled"};
  wf::option_wrapper_t<int> opt_duration{"crt-effect-vk/duration"};
  wf::option_wrapper_t<wf::activatorbinding_t> toggle_key{
      "crt-effect-vk/toggle"};

  wf::activator_callback on_toggle = [this](auto) {
    if (active) {
      LOGI("CRT-VK: Toggling Effect OFF.");
      output->render->rem_post(&render_hook);
    } else {
      LOGI("CRT-VK: Toggling Effect ON.");
      output->render->add_post(&render_hook);
      progression.animate(0, 1);
    }
    active = !active;
    output->render->damage_whole();
    return true;
  };

public:
  void init() override {
    LOGI("CRT-VK: Beginning Architecture Initialization.");
    if (!wf::get_core().is_vulkan())
      return;

    ctx = std::make_unique<VulkanContext>();
    auto *renderer = wf::get_core().renderer;
    ctx->device = wlr_vk_renderer_get_device(renderer);
    ctx->phdev = wlr_vk_renderer_get_physical_device(renderer);

    LOGI("CRT-VK: Borrowed Device: ", ctx->device, " PhDev: ", ctx->phdev);
    vkGetDeviceQueue(ctx->device, wlr_vk_renderer_get_queue_family(renderer), 0,
                     &ctx->queue);
    LOGI("CRT-VK: Queue acquired for family: ",
         wlr_vk_renderer_get_queue_family(renderer));

    if (!initialize_resources())
      return;

    output->add_activator(toggle_key, &on_toggle);
    start_time = std::chrono::steady_clock::now();
    progression =
        wf::animation::simple_animation_t(wf::create_option<int>(opt_duration));

    if (opt_enabled) {
      LOGI("CRT-VK: Effect enabled. Attaching post-hook to output.");
      active = true;
      output->render->add_post(&render_hook);
      progression.animate(0, 1);
      output->render->damage_whole();
    }
  }

  void fini() override {
    LOGI("CRT-VK: Detaching plugin and cleaning up pipelines.");
    output->rem_binding(&on_toggle);
    if (active)
      output->render->rem_post(&render_hook);

    if (ctx && ctx->device) {
      vkDeviceWaitIdle(ctx->device);
      if (src_frame.view)
        vkDestroyImageView(ctx->device, src_frame.view, nullptr);
      if (src_frame.tex)
        wlr_texture_destroy(src_frame.tex);
      if (render_pass)
        vkDestroyRenderPass(ctx->device, render_pass, nullptr);
    }
    cleanup_pipeline();
    ctx.reset();
  }

private:
  bool initialize_resources() {
    LOGI("CRT-VK: Creating Core Vulkan 1.0 RenderPass.");
    VkCommandPoolCreateInfo p_ci = {
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        wlr_vk_renderer_get_queue_family(wf::get_core().renderer)};
    vkCreateCommandPool(ctx->device, &p_ci, nullptr, &ctx->pool);

    VkCommandBufferAllocateInfo a_ci = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, ctx->pool,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    vkAllocateCommandBuffers(ctx->device, &a_ci, &ctx->cmd);

    VkFenceCreateInfo f_ci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr,
                              VK_FENCE_CREATE_SIGNALED_BIT};
    vkCreateFence(ctx->device, &f_ci, nullptr, &ctx->fence);

    VkAttachmentDescription att = {0,
                                   VK_FORMAT_B8G8R8A8_UNORM,
                                   VK_SAMPLE_COUNT_1_BIT,
                                   VK_ATTACHMENT_LOAD_OP_CLEAR,
                                   VK_ATTACHMENT_STORE_OP_STORE,
                                   VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                   VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                   VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {0,       VK_PIPELINE_BIND_POINT_GRAPHICS,
                                0,       nullptr,
                                1,       &ref,
                                nullptr, nullptr,
                                0,       nullptr};
    VkRenderPassCreateInfo rp_ci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                    nullptr,
                                    0,
                                    1,
                                    &att,
                                    1,
                                    &sub,
                                    0,
                                    nullptr};
    vkCreateRenderPass(ctx->device, &rp_ci, nullptr, &render_pass);

    LOGI("CRT-VK: Loading Shader SPIR-V Binaries.");
    frag_module =
        load_spv("/home/neo/.local/share/wayfire/crt-effect/shaders/crt.spv");
    vert_module = load_spv("/home/neo/.local/share/wayfire/crt-effect/shaders/"
                           "fullscreen.vert.spv");

    setup_descriptors();
    setup_pipeline();
    return (pipeline != VK_NULL_HANDLE);
  }

  VkShaderModule load_spv(const std::string &path) {
    LOGI("CRT-VK: Loading SPV: ", path);
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open())
      return VK_NULL_HANDLE;
    size_t size = (size_t)file.tellg();
    std::vector<char> buffer(size);
    file.seekg(0);
    file.read(buffer.data(), size);
    VkShaderModuleCreateInfo ci = {
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, buffer.size(),
        reinterpret_cast<const uint32_t *>(buffer.data())};
    VkShaderModule mod;
    vkCreateShaderModule(ctx->device, &ci, nullptr, &mod);
    return mod;
  }

  void setup_descriptors() {
    LOGI("CRT-VK: Setting up Descriptor Set Layouts.");
    VkSamplerCreateInfo s_ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                nullptr,
                                0,
                                VK_FILTER_LINEAR,
                                VK_FILTER_LINEAR,
                                VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                0.0f,
                                VK_FALSE,
                                1.0f,
                                VK_FALSE,
                                VK_COMPARE_OP_ALWAYS,
                                0.0f,
                                0.0f,
                                VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
                                VK_FALSE};
    vkCreateSampler(ctx->device, &s_ci, nullptr, &sampler);
    VkDescriptorSetLayoutBinding b = {0,
                                      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo d_ci = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 1, &b};
    vkCreateDescriptorSetLayout(ctx->device, &d_ci, nullptr, &desc_layout);
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pool_ci = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 1, 1, &ps};
    vkCreateDescriptorPool(ctx->device, &pool_ci, nullptr, &desc_pool);
    VkDescriptorSetAllocateInfo sa = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, desc_pool, 1,
        &desc_layout};
    vkAllocateDescriptorSets(ctx->device, &sa, &desc_set);
  }

  void setup_pipeline() {
    LOGI("CRT-VK: Creating Graphics Pipeline.");
    VkPushConstantRange pcr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(CRTPushConstants)};
    VkPipelineLayoutCreateInfo pl_ci = {
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        nullptr,
        0,
        1,
        &desc_layout,
        1,
        &pcr};
    vkCreatePipelineLayout(ctx->device, &pl_ci, nullptr, &pipeline_layout);
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_VERTEX_BIT, vert_module, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_FRAGMENT_BIT, frag_module, "main", nullptr}};
    VkPipelineVertexInputStateCreateInfo vi = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        nullptr,
        0,
        0,
        nullptr,
        0,
        nullptr};
    VkPipelineInputAssemblyStateCreateInfo ia = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
    VkPipelineRasterizationStateCreateInfo rs = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        nullptr,
        0,
        VK_FALSE,
        VK_FALSE,
        VK_POLYGON_MODE_FILL,
        VK_CULL_MODE_NONE,
        VK_FRONT_FACE_COUNTER_CLOCKWISE,
        VK_FALSE,
        0.0f,
        0.0f,
        0.0f,
        1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        nullptr,
        0,
        VK_SAMPLE_COUNT_1_BIT,
        VK_FALSE,
        1.0f,
        nullptr,
        VK_FALSE,
        VK_FALSE};
    VkPipelineColorBlendAttachmentState cb_as = {VK_FALSE,
                                                 VK_BLEND_FACTOR_ONE,
                                                 VK_BLEND_FACTOR_ZERO,
                                                 VK_BLEND_OP_ADD,
                                                 VK_BLEND_FACTOR_ONE,
                                                 VK_BLEND_FACTOR_ZERO,
                                                 VK_BLEND_OP_ADD,
                                                 0xf};
    VkPipelineColorBlendStateCreateInfo cb = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        nullptr,
        0,
        VK_FALSE,
        VK_LOGIC_OP_COPY,
        1,
        &cb_as,
        {0, 0, 0, 0}};
    VkPipelineViewportStateCreateInfo vp = {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        nullptr,
        0,
        1,
        nullptr,
        1,
        nullptr};
    VkDynamicState dyn_s[] = {VK_DYNAMIC_STATE_VIEWPORT,
                              VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 2,
        dyn_s};
    VkGraphicsPipelineCreateInfo p_ci = {
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        nullptr,
        0,
        2,
        stages,
        &vi,
        &ia,
        nullptr,
        &vp,
        &rs,
        &ms,
        nullptr,
        &cb,
        &dyn,
        pipeline_layout,
        render_pass,
        0,
        VK_NULL_HANDLE,
        -1};
    vkCreateGraphicsPipelines(ctx->device, VK_NULL_HANDLE, 1, &p_ci, nullptr,
                              &pipeline);
  }

  void transition(VkCommandBuffer cmd, VkImage img, VkImageLayout old_ly,
                  VkImageLayout new_ly) {
    VkImageMemoryBarrier b = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        nullptr,
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        old_ly,
        new_ly,
        VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED,
        img,
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
  }

  wf::post_hook_t render_hook = [this](wf::auxilliary_buffer_t &source,
                                       const wf::render_buffer_t &destination) {
    if (!pipeline)
      return;

    vkWaitForFences(ctx->device, 1, &ctx->fence, VK_TRUE, UINT64_MAX);
    vkResetFences(ctx->device, 1, &ctx->fence);

    if (src_frame.view)
      vkDestroyImageView(ctx->device, src_frame.view, nullptr);
    if (src_frame.tex)
      wlr_texture_destroy(src_frame.tex);

    VkExtent2D ext = {(uint32_t)destination.get_size().width,
                      (uint32_t)destination.get_size().height};

    if (ctx->extent.width != ext.width || ctx->extent.height != ext.height) {
      LOGI("CRT-VK Hook: Resolution Changed to ", ext.width, "x", ext.height,
           ". Reallocating scratch.");
      reallocate_scratch(ext);
    }

    auto *renderer = wf::get_core().renderer;
    src_frame.tex = wlr_texture_from_buffer(renderer, source.get_buffer());
    wlr_vk_image_attribs src_attr;
    wlr_vk_texture_get_image_attribs(src_frame.tex, &src_attr);

    VkImageViewCreateInfo src_v_ci = {
        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        nullptr,
        0,
        src_attr.image,
        VK_IMAGE_VIEW_TYPE_2D,
        VK_FORMAT_B8G8R8A8_UNORM,
        {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
         VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCreateImageView(ctx->device, &src_v_ci, nullptr, &src_frame.view);

    VkDescriptorImageInfo i_info = {sampler, src_frame.view,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  nullptr,
                                  desc_set,
                                  0,
                                  0,
                                  1,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  &i_info,
                                  nullptr,
                                  nullptr};
    vkUpdateDescriptorSets(ctx->device, 1, &write, 0, nullptr);

    VkCommandBufferBeginInfo begin_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
    vkBeginCommandBuffer(ctx->cmd, &begin_info);

    transition(ctx->cmd, src_attr.image,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition(ctx->cmd, ctx->scratch_img, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkClearValue clear = {.color = {{0, 0, 0, 1}}};
    VkRenderPassBeginInfo rp_bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                   nullptr,
                                   render_pass,
                                   ctx->framebuffer,
                                   {{0, 0}, ctx->extent},
                                   1,
                                   &clear};

    vkCmdBeginRenderPass(ctx->cmd, &rp_bi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, (float)ctx->extent.width, (float)ctx->extent.height,
                  0, 1};
    VkRect2D sci{{0, 0}, ctx->extent};
    vkCmdSetViewport(ctx->cmd, 0, 1, &vp);
    vkCmdSetScissor(ctx->cmd, 0, 1, &sci);
    vkCmdBindPipeline(ctx->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(ctx->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline_layout, 0, 1, &desc_set, 0, nullptr);

    CRTPushConstants pcs = {
        {(float)ctx->extent.width, (float)ctx->extent.height},
        (float)std::chrono::duration<float>(std::chrono::steady_clock::now() -
                                            start_time)
            .count(),
        (float)progression};
    vkCmdPushConstants(ctx->cmd, pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pcs), &pcs);
    vkCmdDraw(ctx->cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(ctx->cmd);

    transition(ctx->cmd, ctx->scratch_img,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkEndCommandBuffer(ctx->cmd);

    VkSubmitInfo sub = {VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        nullptr,
                        0,
                        nullptr,
                        nullptr,
                        1,
                        &ctx->cmd,
                        0,
                        nullptr};
    vkQueueSubmit(ctx->queue, 1, &sub, ctx->fence);

    // Standard Wayfire blit: source is the original frame,
    // destination is the output. Geometry defines the target area.
    wf::geometry_t g{0, 0, (int)ctx->extent.width, (int)ctx->extent.height};
    wlr_fbox box{0, 0, (float)g.width, (float)g.height};
    destination.blit(source, box, g, WLR_SCALE_FILTER_BILINEAR);
  };

  void reallocate_scratch(VkExtent2D ext) {
    if (ctx->framebuffer)
      vkDestroyFramebuffer(ctx->device, ctx->framebuffer, nullptr);
    if (ctx->scratch_view)
      vkDestroyImageView(ctx->device, ctx->scratch_view, nullptr);
    if (ctx->scratch_img)
      vkDestroyImage(ctx->device, ctx->scratch_img, nullptr);
    if (ctx->scratch_mem)
      vkFreeMemory(ctx->device, ctx->scratch_mem, nullptr);

    ctx->extent = ext;
    VkImageCreateInfo i_ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              nullptr,
                              0,
                              VK_IMAGE_TYPE_2D,
                              VK_FORMAT_B8G8R8A8_UNORM,
                              {ext.width, ext.height, 1},
                              1,
                              1,
                              VK_SAMPLE_COUNT_1_BIT,
                              VK_IMAGE_TILING_OPTIMAL,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_SAMPLED_BIT,
                              VK_SHARING_MODE_EXCLUSIVE,
                              0,
                              nullptr,
                              VK_IMAGE_LAYOUT_UNDEFINED};
    vkCreateImage(ctx->device, &i_ci, nullptr, &ctx->scratch_img);

    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(ctx->device, ctx->scratch_img, &mem_req);
    VkPhysicalDeviceMemoryProperties mem_p;
    vkGetPhysicalDeviceMemoryProperties(ctx->phdev, &mem_p);
    uint32_t mem_type = 0;
    for (uint32_t i = 0; i < mem_p.memoryTypeCount; i++) {
      if ((mem_req.memoryTypeBits & (1 << i)) &&
          (mem_p.memoryTypes[i].propertyFlags &
           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        mem_type = i;
        break;
      }
    }

    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                  nullptr, mem_req.size, mem_type};
    vkAllocateMemory(ctx->device, &alloc, nullptr, &ctx->scratch_mem);
    vkBindImageMemory(ctx->device, ctx->scratch_img, ctx->scratch_mem, 0);

    VkImageViewCreateInfo v_ci = {
        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        nullptr,
        0,
        ctx->scratch_img,
        VK_IMAGE_VIEW_TYPE_2D,
        VK_FORMAT_B8G8R8A8_UNORM,
        {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
         VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCreateImageView(ctx->device, &v_ci, nullptr, &ctx->scratch_view);

    VkFramebufferCreateInfo fb_ci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                     nullptr,
                                     0,
                                     render_pass,
                                     1,
                                     &ctx->scratch_view,
                                     ext.width,
                                     ext.height,
                                     1};
    vkCreateFramebuffer(ctx->device, &fb_ci, nullptr, &ctx->framebuffer);
  }

  void cleanup_pipeline() {
    if (pipeline)
      vkDestroyPipeline(ctx->device, pipeline, nullptr);
    if (pipeline_layout)
      vkDestroyPipelineLayout(ctx->device, pipeline_layout, nullptr);
    if (desc_layout)
      vkDestroyDescriptorSetLayout(ctx->device, desc_layout, nullptr);
    if (desc_pool)
      vkDestroyDescriptorPool(ctx->device, desc_pool, nullptr);
    if (sampler)
      vkDestroySampler(ctx->device, sampler, nullptr);
    if (frag_module)
      vkDestroyShaderModule(ctx->device, frag_module, nullptr);
    if (vert_module)
      vkDestroyShaderModule(ctx->device, vert_module, nullptr);
  }
};

DECLARE_WAYFIRE_PLUGIN(wf::per_output_plugin_t<wayfire_crt_vulkan>);
