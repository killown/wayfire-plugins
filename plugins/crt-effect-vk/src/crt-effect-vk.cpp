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

    vkGetDeviceQueue(ctx->device, wlr_vk_renderer_get_queue_family(renderer), 0,
                     &ctx->queue);
    LOGI("CRT-VK: Borrowed Device: ", ctx->device, " PhDev: ", ctx->phdev);
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
  void transition(VkCommandBuffer cmd, VkImage img, VkImageLayout old_ly,
                  VkImageLayout new_ly) {
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = old_ly,
        .newLayout = new_ly,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
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
      LOGI("CRT-VK Hook: Resolution Changed. Reallocating.");
      reallocate_scratch(ext);
    }

    auto *renderer = wf::get_core().renderer;
    src_frame.tex = wlr_texture_from_buffer(renderer, source.get_buffer());
    wlr_vk_image_attribs src_attr;
    wlr_vk_texture_get_image_attribs(src_frame.tex, &src_attr);

    wlr_texture *dst_tex =
        wlr_texture_from_buffer(renderer, destination.get_buffer());
    wlr_vk_image_attribs dst_attr;
    wlr_vk_texture_get_image_attribs(dst_tex, &dst_attr);

    VkImageViewCreateInfo src_v_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = src_attr.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .components = {VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY},
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCreateImageView(ctx->device, &src_v_ci, nullptr, &src_frame.view);

    VkDescriptorImageInfo i_info = {sampler, src_frame.view,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = desc_set,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &i_info,
        .pBufferInfo = nullptr,
        .pTexelBufferView = nullptr};
    vkUpdateDescriptorSets(ctx->device, 1, &write, 0, nullptr);

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr};
    vkBeginCommandBuffer(ctx->cmd, &begin_info);

    transition(ctx->cmd, src_attr.image,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition(ctx->cmd, ctx->scratch_img, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkClearValue clear = {.color = {{0, 0, 0, 1}}};
    VkRenderPassBeginInfo rp_bi = {.sType =
                                       VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                   .pNext = nullptr,
                                   .renderPass = render_pass,
                                   .framebuffer = ctx->framebuffer,
                                   .renderArea = {{0, 0}, ctx->extent},
                                   .clearValueCount = 1,
                                   .pClearValues = &clear};

    vkCmdBeginRenderPass(ctx->cmd, &rp_bi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, (float)ctx->extent.width, (float)ctx->extent.height,
                  0, 1};
    vkCmdSetViewport(ctx->cmd, 0, 1, &vp);
    VkRect2D sci{{0, 0}, ctx->extent};
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
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(ctx->cmd, dst_attr.image,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageBlit blit_region = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffsets = {{0, 0, 0},
                       {(int32_t)ctx->extent.width, (int32_t)ctx->extent.height,
                        1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffsets = {
            {0, 0, 0},
            {(int32_t)ctx->extent.width, (int32_t)ctx->extent.height, 1}}};
    vkCmdBlitImage(ctx->cmd, ctx->scratch_img,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_attr.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit_region,
                   VK_FILTER_LINEAR);

    transition(ctx->cmd, dst_attr.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    vkEndCommandBuffer(ctx->cmd);

    VkSubmitInfo sub = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .pNext = nullptr,
                        .waitSemaphoreCount = 0,
                        .pWaitSemaphores = nullptr,
                        .pWaitDstStageMask = nullptr,
                        .commandBufferCount = 1,
                        .pCommandBuffers = &ctx->cmd,
                        .signalSemaphoreCount = 0,
                        .pSignalSemaphores = nullptr};
    vkQueueSubmit(ctx->queue, 1, &sub, ctx->fence);

    wlr_texture_destroy(dst_tex);
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
    VkImageCreateInfo i_ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .pNext = nullptr,
                              .flags = 0,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .format = VK_FORMAT_B8G8R8A8_UNORM,
                              .extent = {ext.width, ext.height, 1},
                              .mipLevels = 1,
                              .arrayLayers = 1,
                              .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                       VK_IMAGE_USAGE_SAMPLED_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                              .queueFamilyIndexCount = 0,
                              .pQueueFamilyIndices = nullptr,
                              .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    vkCreateImage(ctx->device, &i_ci, nullptr, &ctx->scratch_img);

    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(ctx->device, ctx->scratch_img, &mem_req);
    VkPhysicalDeviceMemoryProperties mem_p;
    vkGetPhysicalDeviceMemoryProperties(ctx->phdev, &mem_p);
    uint32_t mt = 0;
    for (uint32_t i = 0; i < mem_p.memoryTypeCount; i++)
      if ((mem_req.memoryTypeBits & (1 << i)) &&
          (mem_p.memoryTypes[i].propertyFlags &
           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        mt = i;
        break;
      }

    VkMemoryAllocateInfo alloc = {.sType =
                                      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                  .pNext = nullptr,
                                  .allocationSize = mem_req.size,
                                  .memoryTypeIndex = mt};
    vkAllocateMemory(ctx->device, &alloc, nullptr, &ctx->scratch_mem);
    vkBindImageMemory(ctx->device, ctx->scratch_img, ctx->scratch_mem, 0);

    VkImageViewCreateInfo v_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = ctx->scratch_img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .components = {VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY},
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCreateImageView(ctx->device, &v_ci, nullptr, &ctx->scratch_view);

    VkFramebufferCreateInfo fb_ci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .renderPass = render_pass,
        .attachmentCount = 1,
        .pAttachments = &ctx->scratch_view,
        .width = ext.width,
        .height = ext.height,
        .layers = 1};
    vkCreateFramebuffer(ctx->device, &fb_ci, nullptr, &ctx->framebuffer);
  }

  bool initialize_resources() {
    VkCommandPoolCreateInfo cp_ci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex =
            wlr_vk_renderer_get_queue_family(wf::get_core().renderer)};
    vkCreateCommandPool(ctx->device, &cp_ci, nullptr, &ctx->pool);
    VkCommandBufferAllocateInfo a_ci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = ctx->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1};
    vkAllocateCommandBuffers(ctx->device, &a_ci, &ctx->cmd);
    VkFenceCreateInfo f_ci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                              .pNext = nullptr,
                              .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    vkCreateFence(ctx->device, &f_ci, nullptr, &ctx->fence);

    VkAttachmentDescription att = {
        .flags = 0,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference ref = {
        .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sd = {.flags = 0,
                               .pipelineBindPoint =
                                   VK_PIPELINE_BIND_POINT_GRAPHICS,
                               .inputAttachmentCount = 0,
                               .pInputAttachments = nullptr,
                               .colorAttachmentCount = 1,
                               .pColorAttachments = &ref,
                               .pResolveAttachments = nullptr,
                               .pDepthStencilAttachment = nullptr,
                               .preserveAttachmentCount = 0,
                               .pPreserveAttachments = nullptr};
    VkRenderPassCreateInfo rp_ci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .attachmentCount = 1,
        .pAttachments = &att,
        .subpassCount = 1,
        .pSubpasses = &sd,
        .dependencyCount = 0,
        .pDependencies = nullptr};
    vkCreateRenderPass(ctx->device, &rp_ci, nullptr, &render_pass);

    frag_module =
        load_spv("/home/neo/.local/share/wayfire/crt-effect/shaders/crt.spv");
    vert_module = load_spv("/home/neo/.local/share/wayfire/crt-effect/shaders/"
                           "fullscreen.vert.spv");
    setup_descriptors();
    setup_pipeline();
    return (pipeline != VK_NULL_HANDLE);
  }

  VkShaderModule load_spv(const std::string &path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open())
      return VK_NULL_HANDLE;
    size_t size = (size_t)file.tellg();
    std::vector<char> buffer(size);
    file.seekg(0);
    file.read(buffer.data(), size);
    VkShaderModuleCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = buffer.size(),
        .pCode = reinterpret_cast<const uint32_t *>(buffer.data())};
    VkShaderModule mod;
    vkCreateShaderModule(ctx->device, &ci, nullptr, &mod);
    return mod;
  }

  void setup_descriptors() {
    VkSamplerCreateInfo s_ci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipLodBias = 0.0f,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0f,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .minLod = 0.0f,
        .maxLod = 0.0f,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        .unnormalizedCoordinates = VK_FALSE};
    vkCreateSampler(ctx->device, &s_ci, nullptr, &sampler);
    VkDescriptorSetLayoutBinding b = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .pImmutableSamplers = nullptr};
    VkDescriptorSetLayoutCreateInfo dl_ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .bindingCount = 1,
        .pBindings = &b};
    vkCreateDescriptorSetLayout(ctx->device, &dl_ci, nullptr, &desc_layout);
    VkDescriptorPoolSize ps = {.type =
                                   VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               .descriptorCount = 1};
    VkDescriptorPoolCreateInfo dp_ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &ps};
    vkCreateDescriptorPool(ctx->device, &dp_ci, nullptr, &desc_pool);
    VkDescriptorSetAllocateInfo ds_ai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = nullptr,
        .descriptorPool = desc_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &desc_layout};
    vkAllocateDescriptorSets(ctx->device, &ds_ai, &desc_set);
  }

  void setup_pipeline() {
    VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                               .offset = 0,
                               .size = sizeof(CRTPushConstants)};
    VkPipelineLayoutCreateInfo pl_ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = &desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr};
    vkCreatePipelineLayout(ctx->device, &pl_ci, nullptr, &pipeline_layout);
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .pNext = nullptr,
         .flags = 0,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vert_module,
         .pName = "main",
         .pSpecializationInfo = nullptr},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .pNext = nullptr,
         .flags = 0,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = frag_module,
         .pName = "main",
         .pSpecializationInfo = nullptr}};
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .vertexBindingDescriptionCount = 0,
        .pVertexBindingDescriptions = nullptr,
        .vertexAttributeDescriptionCount = 0,
        .pVertexAttributeDescriptions = nullptr};
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE};
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
        .depthBiasConstantFactor = 0.0f,
        .depthBiasClamp = 0.0f,
        .depthBiasSlopeFactor = 0.0f,
        .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        .sampleShadingEnable = VK_FALSE,
        .minSampleShading = 0.0f,
        .pSampleMask = nullptr,
        .alphaToCoverageEnable = VK_FALSE,
        .alphaToOneEnable = VK_FALSE};
    VkPipelineColorBlendAttachmentState cba = {
        .blendEnable = VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ZERO,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = 0xf};
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = 1,
        .pAttachments = &cba,
        .blendConstants = {0, 0, 0, 0}};
    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .viewportCount = 1,
        .pViewports = nullptr,
        .scissorCount = 1,
        .pScissors = nullptr};
    VkDynamicState ds[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .dynamicStateCount = 2,
        .pDynamicStates = ds};
    VkGraphicsPipelineCreateInfo gp_ci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vi,
        .pInputAssemblyState = &ia,
        .pTessellationState = nullptr,
        .pViewportState = &vps,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pDepthStencilState = nullptr,
        .pColorBlendState = &cb,
        .pDynamicState = &dyn,
        .layout = pipeline_layout,
        .renderPass = render_pass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = -1};
    vkCreateGraphicsPipelines(ctx->device, VK_NULL_HANDLE, 1, &gp_ci, nullptr,
                              &pipeline);
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
