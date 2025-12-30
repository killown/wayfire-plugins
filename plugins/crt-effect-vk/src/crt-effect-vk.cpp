#include <wayfire/core.hpp>
#include <wayfire/output.hpp>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/util/duration.hpp>
#include <wayfire/util/log.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

extern "C" {
#include <vulkan/vulkan.h>
#include <wlr/render/vulkan.h>
}

namespace fs = std::filesystem;

/**
 * @brief SPIR-V Compatible Push Constant Block.
 * Aligned to 16-byte boundaries to satisfy std140/std430 requirements.
 */
struct alignas(16) CRTPushConstants {
  float res[2] = {0.0f, 0.0f};
  float time = 0.0f;
  float progress = 0.0f;
  int32_t mask_type = 0;
  float beam_sigma = 0.0f;
  float border_size = 0.0f;
  float scanline_wt = 0.0f;
  float brightness = 1.0f;
  float conv_x[2] = {0.0f, 0.0f};
  float conv_y[2] = {0.0f, 0.0f};
  int32_t distort = 0;
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
      if (cmd)
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

  wf::animation::simple_animation_t progression;
  std::chrono::steady_clock::time_point start_time;
  std::chrono::steady_clock::time_point last_swap_time;
  bool active = false;

  std::vector<std::string> shader_list;
  size_t current_idx = 0;
  std::string base_path;

  wf::option_wrapper_t<bool> opt_enabled{"crt-effect-vk/enabled"};
  wf::option_wrapper_t<int> opt_duration{"crt-effect-vk/duration"};
  wf::option_wrapper_t<wf::activatorbinding_t> toggle_key{
      "crt-effect-vk/toggle"};
  wf::option_wrapper_t<wf::activatorbinding_t> cycle_key{"crt-effect-vk/cycle"};

  bool ends_with(const std::string &str, const std::string &suffix) {
    return str.size() >= suffix.size() &&
           str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
  }

  void insert_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout old_ly,
                      VkImageLayout new_ly, VkAccessFlags src_acc,
                      VkAccessFlags dst_acc, VkPipelineStageFlags src_stg,
                      VkPipelineStageFlags dst_stg) {
    VkImageMemoryBarrier b;
    std::memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src_acc;
    b.dstAccessMask = dst_acc;
    b.oldLayout = old_ly;
    b.newLayout = new_ly;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, src_stg, dst_stg, 0, 0, nullptr, 0, nullptr, 1,
                         &b);
  }

  wf::post_hook_t render_hook = [this](wf::auxilliary_buffer_t &source,
                                       const wf::render_buffer_t &destination) {
    if (!pipeline || !ctx || ctx->device == VK_NULL_HANDLE)
      return;

    vkWaitForFences(ctx->device, 1, &ctx->fence, VK_TRUE, UINT64_MAX);
    vkResetFences(ctx->device, 1, &ctx->fence);

    VkExtent2D ext;
    ext.width = (uint32_t)destination.get_size().width;
    ext.height = (uint32_t)destination.get_size().height;

    if (ctx->extent.width != ext.width || ctx->extent.height != ext.height) {
      reallocate_scratch(ext);
    }

    auto renderer = wf::get_core().renderer;
    wlr_texture *src_tex =
        wlr_texture_from_buffer(renderer, source.get_buffer());
    wlr_texture *dst_tex =
        wlr_texture_from_buffer(renderer, destination.get_buffer());

    wlr_vk_image_attribs src_attr, dst_attr;
    wlr_vk_texture_get_image_attribs(src_tex, &src_attr);
    wlr_vk_texture_get_image_attribs(dst_tex, &dst_attr);

    VkImageView src_view;
    VkImageViewCreateInfo iv_ci;
    std::memset(&iv_ci, 0, sizeof(iv_ci));
    iv_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    iv_ci.image = src_attr.image;
    iv_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    iv_ci.format = src_attr.format;
    iv_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    iv_ci.subresourceRange.levelCount = 1;
    iv_ci.subresourceRange.layerCount = 1;
    vkCreateImageView(ctx->device, &iv_ci, nullptr, &src_view);

    VkDescriptorImageInfo d_info;
    d_info.sampler = sampler;
    d_info.imageView = src_view;
    d_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write;
    std::memset(&write, 0, sizeof(write));
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = desc_set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &d_info;
    vkUpdateDescriptorSets(ctx->device, 1, &write, 0, nullptr);

    VkCommandBufferBeginInfo begin;
    std::memset(&begin, 0, sizeof(begin));
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(ctx->cmd, &begin);

    insert_barrier(ctx->cmd, src_attr.image, src_attr.layout,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    insert_barrier(ctx->cmd, ctx->scratch_img, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    VkClearValue clear;
    std::memset(&clear, 0, sizeof(clear));
    clear.color.float32[3] = 1.0f;

    VkRenderPassBeginInfo rp_bi;
    std::memset(&rp_bi, 0, sizeof(rp_bi));
    rp_bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_bi.renderPass = render_pass;
    rp_bi.framebuffer = ctx->framebuffer;
    rp_bi.renderArea.extent = ctx->extent;
    rp_bi.clearValueCount = 1;
    rp_bi.pClearValues = &clear;

    vkCmdBeginRenderPass(ctx->cmd, &rp_bi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = {0, 0, (float)ext.width, (float)ext.height, 0, 1};
    vkCmdSetViewport(ctx->cmd, 0, 1, &vp);
    VkRect2D sci = {{0, 0}, ext};
    vkCmdSetScissor(ctx->cmd, 0, 1, &sci);

    vkCmdBindPipeline(ctx->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(ctx->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline_layout, 0, 1, &desc_set, 0, nullptr);

    CRTPushConstants pcs;
    pcs.res[0] = (float)ext.width;
    pcs.res[1] = (float)ext.height;
    pcs.time = std::chrono::duration<float>(std::chrono::steady_clock::now() -
                                            start_time)
                   .count();
    pcs.progress = (float)progression;

    vkCmdPushConstants(ctx->cmd, pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pcs), &pcs);
    vkCmdDraw(ctx->cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(ctx->cmd);

    insert_barrier(
        ctx->cmd, ctx->scratch_img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);

    insert_barrier(ctx->cmd, dst_attr.image, dst_attr.layout,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImageBlit blit;
    std::memset(&blit, 0, sizeof(blit));
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[1].x = (int32_t)ext.width;
    blit.srcOffsets[1].y = (int32_t)ext.height;
    blit.srcOffsets[1].z = 1;
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[1].x = (int32_t)ext.width;
    blit.dstOffsets[1].y = (int32_t)ext.height;
    blit.dstOffsets[1].z = 1;

    vkCmdBlitImage(ctx->cmd, ctx->scratch_img,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_attr.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   VK_FILTER_LINEAR);

    insert_barrier(
        ctx->cmd, dst_attr.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        dst_attr.layout, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    vkEndCommandBuffer(ctx->cmd);

    VkSubmitInfo submit;
    std::memset(&submit, 0, sizeof(submit));
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &ctx->cmd;
    vkQueueSubmit(ctx->queue, 1, &submit, ctx->fence);

    vkQueueWaitIdle(ctx->queue);
    vkDestroyImageView(ctx->device, src_view, nullptr);
    wlr_texture_destroy(src_tex);
    wlr_texture_destroy(dst_tex);
  };

public:
  void init() override {
    if (!wf::get_core().is_vulkan())
      return;

    char *home = std::getenv("HOME");
    if (!home)
      return;
    base_path = std::string(home) + "/.local/share/wayfire/crt-effect/shaders/";

    ctx = std::make_unique<VulkanContext>();
    auto renderer = wf::get_core().renderer;
    ctx->device = wlr_vk_renderer_get_device(renderer);
    ctx->phdev = wlr_vk_renderer_get_physical_device(renderer);

    if (ctx->device == VK_NULL_HANDLE) {
      ctx.reset();
      return;
    }

    vkGetDeviceQueue(ctx->device, wlr_vk_renderer_get_queue_family(renderer), 0,
                     &ctx->queue);

    scan_shaders();
    if (!initialize_resources())
      return;

    output->add_activator(toggle_key, &on_toggle);
    output->add_activator(cycle_key, &on_cycle);
    progression =
        wf::animation::simple_animation_t(wf::create_option<int>(opt_duration));
    start_time = std::chrono::steady_clock::now();
    last_swap_time = start_time;

    if (opt_enabled) {
      active = true;
      output->render->add_post(&render_hook);
      progression.animate(0, 1);
      output->render->damage_whole();
    }
  }

  void fini() override {
    output->rem_binding(&on_toggle);
    output->rem_binding(&on_cycle);
    if (active)
      output->render->rem_post(&render_hook);
    cleanup_pipeline();
    if (render_pass && ctx && ctx->device != VK_NULL_HANDLE)
      vkDestroyRenderPass(ctx->device, render_pass, nullptr);
    ctx.reset();
  }

private:
  wf::activator_callback on_toggle = [this](auto) {
    if (active)
      output->render->rem_post(&render_hook);
    else {
      output->render->add_post(&render_hook);
      progression.animate(0, 1);
    }
    active = !active;
    output->render->damage_whole();
    return true;
  };

  wf::activator_callback on_cycle = [this](auto) {
    auto now = std::chrono::steady_clock::now();
    if (now - last_swap_time < std::chrono::seconds(1))
      return true;
    if (shader_list.empty())
      return true;

    bool was_active = active;
    if (active) {
      output->render->rem_post(&render_hook);
      active = false;
    }

    current_idx = (current_idx + 1) % shader_list.size();
    vkDeviceWaitIdle(ctx->device);

    if (pipeline)
      vkDestroyPipeline(ctx->device, pipeline, nullptr);
    if (frag_module)
      vkDestroyShaderModule(ctx->device, frag_module, nullptr);

    frag_module = load_spv(shader_list[current_idx]);
    setup_pipeline();

    if (was_active && pipeline != VK_NULL_HANDLE) {
      output->render->add_post(&render_hook);
      active = true;
    }

    last_swap_time = std::chrono::steady_clock::now();
    output->render->damage_whole();
    return true;
  };

  void scan_shaders() {
    shader_list.clear();
    if (!fs::exists(base_path))
      return;
    for (const auto &entry : fs::directory_iterator(base_path)) {
      std::string p = entry.path().string();
      if (ends_with(p, ".spv") &&
          p.find("fullscreen.vert.spv") == std::string::npos) {
        shader_list.push_back(p);
      }
    }
    std::sort(shader_list.begin(), shader_list.end());
    for (size_t i = 0; i < shader_list.size(); ++i) {
      if (shader_list[i].find("crt.spv") != std::string::npos) {
        current_idx = i;
        break;
      }
    }
  }

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
    VkImageCreateInfo i_ci;
    std::memset(&i_ci, 0, sizeof(i_ci));
    i_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    i_ci.imageType = VK_IMAGE_TYPE_2D;
    i_ci.format = VK_FORMAT_B8G8R8A8_UNORM;
    i_ci.extent.width = ext.width;
    i_ci.extent.height = ext.height;
    i_ci.extent.depth = 1;
    i_ci.mipLevels = 1;
    i_ci.arrayLayers = 1;
    i_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    i_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    i_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    vkCreateImage(ctx->device, &i_ci, nullptr, &ctx->scratch_img);

    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(ctx->device, ctx->scratch_img, &mem_req);
    VkPhysicalDeviceMemoryProperties mem_p;
    vkGetPhysicalDeviceMemoryProperties(ctx->phdev, &mem_p);
    uint32_t mt = 0;
    for (uint32_t i = 0; i < mem_p.memoryTypeCount; i++) {
      if ((mem_req.memoryTypeBits & (1 << i)) &&
          (mem_p.memoryTypes[i].propertyFlags &
           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        mt = i;
        break;
      }
    }

    VkMemoryAllocateInfo alloc;
    std::memset(&alloc, 0, sizeof(alloc));
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = mem_req.size;
    alloc.memoryTypeIndex = mt;
    vkAllocateMemory(ctx->device, &alloc, nullptr, &ctx->scratch_mem);
    vkBindImageMemory(ctx->device, ctx->scratch_img, ctx->scratch_mem, 0);

    VkImageViewCreateInfo v_ci;
    std::memset(&v_ci, 0, sizeof(v_ci));
    v_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    v_ci.image = ctx->scratch_img;
    v_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    v_ci.format = VK_FORMAT_B8G8R8A8_UNORM;
    v_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    v_ci.subresourceRange.levelCount = 1;
    v_ci.subresourceRange.layerCount = 1;
    vkCreateImageView(ctx->device, &v_ci, nullptr, &ctx->scratch_view);

    VkFramebufferCreateInfo fb_ci;
    std::memset(&fb_ci, 0, sizeof(fb_ci));
    fb_ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_ci.renderPass = render_pass;
    fb_ci.attachmentCount = 1;
    fb_ci.pAttachments = &ctx->scratch_view;
    fb_ci.width = ext.width;
    fb_ci.height = ext.height;
    fb_ci.layers = 1;
    vkCreateFramebuffer(ctx->device, &fb_ci, nullptr, &ctx->framebuffer);
  }

  bool initialize_resources() {
    VkCommandPoolCreateInfo cp_ci;
    std::memset(&cp_ci, 0, sizeof(cp_ci));
    cp_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp_ci.queueFamilyIndex =
        wlr_vk_renderer_get_queue_family(wf::get_core().renderer);
    vkCreateCommandPool(ctx->device, &cp_ci, nullptr, &ctx->pool);

    VkCommandBufferAllocateInfo a_ci;
    std::memset(&a_ci, 0, sizeof(a_ci));
    a_ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    a_ci.commandPool = ctx->pool;
    a_ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    a_ci.commandBufferCount = 1;
    vkAllocateCommandBuffers(ctx->device, &a_ci, &ctx->cmd);

    VkFenceCreateInfo f_ci;
    std::memset(&f_ci, 0, sizeof(f_ci));
    f_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    f_ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(ctx->device, &f_ci, nullptr, &ctx->fence);

    VkAttachmentDescription att;
    std::memset(&att, 0, sizeof(att));
    att.format = VK_FORMAT_B8G8R8A8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sd;
    std::memset(&sd, 0, sizeof(sd));
    sd.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sd.colorAttachmentCount = 1;
    sd.pColorAttachments = &ref;

    VkRenderPassCreateInfo rp_ci;
    std::memset(&rp_ci, 0, sizeof(rp_ci));
    rp_ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_ci.attachmentCount = 1;
    rp_ci.pAttachments = &att;
    rp_ci.subpassCount = 1;
    rp_ci.pSubpasses = &sd;
    vkCreateRenderPass(ctx->device, &rp_ci, nullptr, &render_pass);

    vert_module = load_spv(base_path + "fullscreen.vert.spv");
    if (vert_module == VK_NULL_HANDLE)
      return false;

    if (!shader_list.empty())
      frag_module = load_spv(shader_list[current_idx]);
    if (frag_module == VK_NULL_HANDLE)
      return false;

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
    VkShaderModuleCreateInfo ci;
    std::memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = buffer.size();
    ci.pCode = reinterpret_cast<const uint32_t *>(buffer.data());
    VkShaderModule mod;
    vkCreateShaderModule(ctx->device, &ci, nullptr, &mod);
    return mod;
  }

  void setup_descriptors() {
    VkSamplerCreateInfo s_ci;
    std::memset(&s_ci, 0, sizeof(s_ci));
    s_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    s_ci.magFilter = VK_FILTER_LINEAR;
    s_ci.minFilter = VK_FILTER_LINEAR;
    s_ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    s_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    s_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    s_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(ctx->device, &s_ci, nullptr, &sampler);

    VkDescriptorSetLayoutBinding bindings[2];
    std::memset(bindings, 0, sizeof(bindings));
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dl_ci;
    std::memset(&dl_ci, 0, sizeof(dl_ci));
    dl_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dl_ci.bindingCount = 2;
    dl_ci.pBindings = bindings;
    vkCreateDescriptorSetLayout(ctx->device, &dl_ci, nullptr, &desc_layout);

    VkDescriptorPoolSize ps[2];
    ps[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps[0].descriptorCount = 1;
    ps[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ps[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo dp_ci;
    std::memset(&dp_ci, 0, sizeof(dp_ci));
    dp_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp_ci.maxSets = 1;
    dp_ci.poolSizeCount = 2;
    dp_ci.pPoolSizes = ps;
    vkCreateDescriptorPool(ctx->device, &dp_ci, nullptr, &desc_pool);

    VkDescriptorSetAllocateInfo ds_ai;
    std::memset(&ds_ai, 0, sizeof(ds_ai));
    ds_ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ds_ai.descriptorPool = desc_pool;
    ds_ai.descriptorSetCount = 1;
    ds_ai.pSetLayouts = &desc_layout;
    vkAllocateDescriptorSets(ctx->device, &ds_ai, &desc_set);
  }

  void setup_pipeline() {
    if (frag_module == VK_NULL_HANDLE || vert_module == VK_NULL_HANDLE ||
        ctx->device == VK_NULL_HANDLE)
      return;

    VkPushConstantRange pcr;
    pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(CRTPushConstants);

    if (pipeline_layout == VK_NULL_HANDLE) {
      VkPipelineLayoutCreateInfo pl_ci;
      std::memset(&pl_ci, 0, sizeof(pl_ci));
      pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
      pl_ci.setLayoutCount = 1;
      pl_ci.pSetLayouts = &desc_layout;
      pl_ci.pushConstantRangeCount = 1;
      pl_ci.pPushConstantRanges = &pcr;
      vkCreatePipelineLayout(ctx->device, &pl_ci, nullptr, &pipeline_layout);
    }

    VkPipelineShaderStageCreateInfo stages[2];
    std::memset(stages, 0, sizeof(stages));
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_module;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi;
    std::memset(&vi, 0, sizeof(vi));
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia;
    std::memset(&ia, 0, sizeof(ia));
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps;
    std::memset(&vps, 0, sizeof(vps));
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs;
    std::memset(&rs, 0, sizeof(rs));
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms;
    std::memset(&ms, 0, sizeof(ms));
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba;
    std::memset(&cba, 0, sizeof(cba));
    cba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb;
    std::memset(&cb, 0, sizeof(cb));
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn_s[] = {VK_DYNAMIC_STATE_VIEWPORT,
                              VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn;
    std::memset(&dyn, 0, sizeof(dyn));
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_s;

    VkGraphicsPipelineCreateInfo gp_ci;
    std::memset(&gp_ci, 0, sizeof(gp_ci));
    gp_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp_ci.stageCount = 2;
    gp_ci.pStages = stages;
    gp_ci.pVertexInputState = &vi;
    gp_ci.pInputAssemblyState = &ia;
    gp_ci.pViewportState = &vps;
    gp_ci.pRasterizationState = &rs;
    gp_ci.pMultisampleState = &ms;
    gp_ci.pColorBlendState = &cb;
    gp_ci.pDynamicState = &dyn;
    gp_ci.layout = pipeline_layout;
    gp_ci.renderPass = render_pass;

    vkCreateGraphicsPipelines(ctx->device, VK_NULL_HANDLE, 1, &gp_ci, nullptr,
                              &pipeline);
  }

  void cleanup_pipeline() {
    if (!ctx || ctx->device == VK_NULL_HANDLE)
      return;
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
