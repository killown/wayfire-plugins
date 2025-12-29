#include <fstream>
#include <wayfire/core.hpp>
#include <wayfire/output.hpp>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/util/log.hpp>

extern "C" {
#include <vulkan/vulkan.h>
#include <wlr/render/vulkan.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
}

struct VulkanContext {
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkImage scratch_image = VK_NULL_HANDLE;
  VkDeviceMemory scratch_mem = VK_NULL_HANDLE;
  VkBuffer dump_buffer = VK_NULL_HANDLE;
  VkDeviceMemory dump_mem = VK_NULL_HANDLE;

  ~VulkanContext() {
    if (device != VK_NULL_HANDLE) {
      vkDeviceWaitIdle(device);
      if (scratch_image)
        vkDestroyImage(device, scratch_image, nullptr);
      if (scratch_mem)
        vkFreeMemory(device, scratch_mem, nullptr);
      if (dump_buffer)
        vkDestroyBuffer(device, dump_buffer, nullptr);
      if (dump_mem)
        vkFreeMemory(device, dump_mem, nullptr);
      if (fence)
        vkDestroyFence(device, fence, nullptr);
      if (pool)
        vkDestroyCommandPool(device, pool, nullptr);
    }
  }
};

class wayfire_vk_visible_plugin : public wf::per_output_plugin_instance_t {
  std::unique_ptr<VulkanContext> ctx;
  bool has_dumped = false;

public:
  void init() override {
    if (!wf::get_core().is_vulkan())
      return;
    ctx = std::make_unique<VulkanContext>();
    if (!initialize_vulkan_resources()) {
      ctx.reset();
      return;
    }
    output->render->add_post(&render_hook);
    output->render->damage_whole();
  }

  void fini() override {
    output->render->rem_post(&render_hook);
    ctx.reset();
  }

private:
  uint32_t find_memory_type(VkPhysicalDevice pd, uint32_t filter,
                            VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(pd, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
      if ((filter & (1 << i)) &&
          (mem_props.memoryTypes[i].propertyFlags & props) == props)
        return i;
    }
    return 0;
  }

  void transition_layout(VkCommandBuffer cmd, VkImage img, VkImageLayout old_ly,
                         VkImageLayout new_ly) {
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = old_ly;
    barrier.newLayout = new_ly;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
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

  bool initialize_vulkan_resources() {
    auto *renderer = wf::get_core().renderer;
    ctx->device = wlr_vk_renderer_get_device(renderer);
    uint32_t family = wlr_vk_renderer_get_queue_family(renderer);
    vkGetDeviceQueue(ctx->device, family, 0, &ctx->queue);
    VkCommandPoolCreateInfo pool_info = {
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = family;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCreateCommandPool(ctx->device, &pool_info, nullptr, &ctx->pool);
    VkCommandBufferAllocateInfo cb_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cb_info.commandPool = ctx->pool;
    cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_info.commandBufferCount = 1;
    vkAllocateCommandBuffers(ctx->device, &cb_info, &ctx->cmd);
    VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(ctx->device, &fence_info, nullptr, &ctx->fence);
    VkImageCreateInfo img_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    img_info.imageType = VK_IMAGE_TYPE_2D;
    img_info.extent = {500, 500, 1};
    img_info.mipLevels = 1;
    img_info.arrayLayers = 1;
    img_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    img_info.usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    img_info.samples = VK_SAMPLE_COUNT_1_BIT;
    vkCreateImage(ctx->device, &img_info, nullptr, &ctx->scratch_image);
    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(ctx->device, ctx->scratch_image, &mem_reqs);
    VkMemoryAllocateInfo mem_alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mem_alloc.allocationSize = mem_reqs.size;
    mem_alloc.memoryTypeIndex = find_memory_type(
        wlr_vk_renderer_get_physical_device(renderer), mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(ctx->device, &mem_alloc, nullptr, &ctx->scratch_mem);
    vkBindImageMemory(ctx->device, ctx->scratch_image, ctx->scratch_mem, 0);
    VkBufferCreateInfo buf_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buf_info.size = 500 * 500 * 4;
    buf_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    vkCreateBuffer(ctx->device, &buf_info, nullptr, &ctx->dump_buffer);
    vkGetBufferMemoryRequirements(ctx->device, ctx->dump_buffer, &mem_reqs);
    mem_alloc.allocationSize = mem_reqs.size;
    mem_alloc.memoryTypeIndex = find_memory_type(
        wlr_vk_renderer_get_physical_device(renderer), mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(ctx->device, &mem_alloc, nullptr, &ctx->dump_mem);
    vkBindBufferMemory(ctx->device, ctx->dump_buffer, ctx->dump_mem, 0);
    return true;
  }

  void dump_to_tmp() {
    void *data;
    vkMapMemory(ctx->device, ctx->dump_mem, 0, 500 * 500 * 4, 0, &data);
    std::ofstream f("/tmp/vulkan_debug_dump.ppm", std::ios::binary);
    f << "P6\n500 500\n255\n";
    uint8_t *pixels = (uint8_t *)data;
    for (int i = 0; i < 500 * 500; i++)
      f.write((char *)&pixels[i * 4], 3);
    f.close();
    vkUnmapMemory(ctx->device, ctx->dump_mem);
    has_dumped = true;
  }

  wf::post_hook_t render_hook = [=](wf::auxilliary_buffer_t &source,
                                    const wf::render_buffer_t &destination) {
    auto *renderer = wf::get_core().renderer;
    auto *src_tex = wlr_texture_from_buffer(renderer, source.get_buffer());
    auto *dest_tex =
        wlr_texture_from_buffer(renderer, destination.get_buffer());
    struct wlr_vk_image_attribs src_attribs, dest_attribs;
    wlr_vk_texture_get_image_attribs(src_tex, &src_attribs);
    wlr_vk_texture_get_image_attribs(dest_tex, &dest_attribs);

    vkWaitForFences(ctx->device, 1, &ctx->fence, VK_TRUE, UINT64_MAX);
    vkResetFences(ctx->device, 1, &ctx->fence);

    VkCommandBufferBeginInfo begin_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(ctx->cmd, &begin_info);

    transition_layout(ctx->cmd, ctx->scratch_image, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue red_color = {{1.0f, 0.0f, 0.0f, 1.0f}};
    VkImageSubresourceRange sub_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(ctx->cmd, ctx->scratch_image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red_color, 1,
                         &sub_range);

    transition_layout(ctx->cmd, ctx->scratch_image,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition_layout(ctx->cmd, src_attribs.image,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition_layout(ctx->cmd, dest_attribs.image,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageBlit full_blit = {};
    full_blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    full_blit.srcOffsets[0] = VkOffset3D{0, 0, 0};
    full_blit.srcOffsets[1] =
        VkOffset3D{(int32_t)src_tex->width, (int32_t)src_tex->height, 1};
    full_blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    full_blit.dstOffsets[0] = VkOffset3D{0, 0, 0};
    full_blit.dstOffsets[1] =
        VkOffset3D{(int32_t)dest_tex->width, (int32_t)dest_tex->height, 1};
    vkCmdBlitImage(ctx->cmd, src_attribs.image,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest_attribs.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &full_blit,
                   VK_FILTER_LINEAR);

    VkImageBlit rect_blit = {};
    rect_blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rect_blit.srcOffsets[0] = VkOffset3D{0, 0, 0};
    rect_blit.srcOffsets[1] = VkOffset3D{500, 500, 1};
    rect_blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rect_blit.dstOffsets[0] = VkOffset3D{100, 100, 0};
    rect_blit.dstOffsets[1] = VkOffset3D{600, 600, 1};
    vkCmdBlitImage(ctx->cmd, ctx->scratch_image,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest_attribs.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rect_blit,
                   VK_FILTER_NEAREST);

    if (!has_dumped) {
      VkBufferImageCopy copy_region = {};
      copy_region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy_region.imageExtent = VkExtent3D{500, 500, 1};
      vkCmdCopyImageToBuffer(ctx->cmd, ctx->scratch_image,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             ctx->dump_buffer, 1, &copy_region);
    }

    transition_layout(ctx->cmd, src_attribs.image,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition_layout(ctx->cmd, dest_attribs.image,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    vkEndCommandBuffer(ctx->cmd);

    VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &ctx->cmd;
    vkQueueSubmit(ctx->queue, 1, &submit_info, ctx->fence);

    if (!has_dumped && vkWaitForFences(ctx->device, 1, &ctx->fence, VK_TRUE,
                                       100000000) == VK_SUCCESS)
      dump_to_tmp();
    wlr_texture_destroy(src_tex);
    wlr_texture_destroy(dest_tex);
  };
};

DECLARE_WAYFIRE_PLUGIN(wf::per_output_plugin_t<wayfire_vk_visible_plugin>);
