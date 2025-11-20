#include <wayfire/core.hpp>
#include <wayfire/output.hpp>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/util/log.hpp>

extern "C" {
#include <vulkan/vulkan.h>
#include <wlr/render/vulkan.h>
}

struct VulkanContext {
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool cmd_pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd_buffer = VK_NULL_HANDLE;

  VkImage scratch_image = VK_NULL_HANDLE;
  VkDeviceMemory scratch_memory = VK_NULL_HANDLE;

  ~VulkanContext() {
    if (device) {
      vkDeviceWaitIdle(device);
      if (scratch_image)
        vkDestroyImage(device, scratch_image, nullptr);
      if (scratch_memory)
        vkFreeMemory(device, scratch_memory, nullptr);
      if (cmd_pool)
        vkDestroyCommandPool(device, cmd_pool, nullptr);
    }
  }
};

class wayfire_vk_visible_plugin : public wf::per_output_plugin_instance_t {
  std::unique_ptr<VulkanContext> ctx;
  wf::effect_hook_t render_hook;

public:
  void init() override {
    if (!wf::get_core().is_vulkan()) {
      LOGE("Wayfire is NOT in Vulkan mode.");
      return;
    }

    ctx = std::make_unique<VulkanContext>();
    if (!initialize_vulkan_resources()) {
      LOGE("Failed to initialize Vulkan resources.");
      ctx.reset();
      return;
    }

    render_hook = [=]() { this->render_frame(); };
    output->render->add_effect(&render_hook, wf::OUTPUT_EFFECT_POST);

    LOGI("Vulkan Plugin Initialized: Rendering to internal scratch image.");
  }

  void fini() override {
    if (output)
      output->render->rem_effect(&render_hook);
    ctx.reset();
  }

private:
  uint32_t find_memory_type(VkPhysicalDevice physical_device,
                            uint32_t type_filter,
                            VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
      if ((type_filter & (1 << i)) &&
          (mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
        return i;
      }
    }
    return 0;
  }

  bool initialize_vulkan_resources() {
    auto *wlr_renderer = wf::get_core().renderer;
    ctx->device = wlr_vk_renderer_get_device(wlr_renderer);

    uint32_t q_fam = wlr_vk_renderer_get_queue_family(wlr_renderer);
    vkGetDeviceQueue(ctx->device, q_fam, 0, &ctx->queue);

    VkCommandPoolCreateInfo pool_info = {
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = q_fam;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(ctx->device, &pool_info, nullptr, &ctx->cmd_pool) !=
        VK_SUCCESS)
      return false;

    VkCommandBufferAllocateInfo alloc_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc_info.commandPool = ctx->cmd_pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(ctx->device, &alloc_info, &ctx->cmd_buffer) !=
        VK_SUCCESS)
      return false;

    // Create Scratch Image (100x100 Red Target)
    VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.extent.width = 100;
    image_info.extent.height = 100;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;

    if (vkCreateImage(ctx->device, &image_info, nullptr, &ctx->scratch_image) !=
        VK_SUCCESS)
      return false;

    // Allocate Memory for Image
    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(ctx->device, ctx->scratch_image, &mem_reqs);

    // We need the physical device to find memory type. Retrieve via wlroots
    VkPhysicalDevice physical_device =
        wlr_vk_renderer_get_physical_device(wlr_renderer);

    VkMemoryAllocateInfo alloc_mem_info = {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc_mem_info.allocationSize = mem_reqs.size;
    alloc_mem_info.memoryTypeIndex =
        find_memory_type(physical_device, mem_reqs.memoryTypeBits,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(ctx->device, &alloc_mem_info, nullptr,
                         &ctx->scratch_memory) != VK_SUCCESS)
      return false;
    vkBindImageMemory(ctx->device, ctx->scratch_image, ctx->scratch_memory, 0);

    return true;
  }

  void render_frame() {
    if (!ctx || !ctx->cmd_buffer)
      return;

    VkCommandBufferBeginInfo begin_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(ctx->cmd_buffer, &begin_info);

    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.image = ctx->scratch_image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(ctx->cmd_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);

    // Clear RED
    VkClearColorValue color = {{1.0f, 0.0f, 0.0f, 1.0f}};
    vkCmdClearColorImage(ctx->cmd_buffer, ctx->scratch_image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                         &barrier.subresourceRange);

    vkEndCommandBuffer(ctx->cmd_buffer);

    VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &ctx->cmd_buffer;

    vkQueueSubmit(ctx->queue, 1, &submit_info, VK_NULL_HANDLE);
    vkQueueWaitIdle(ctx->queue);

    // LOGI("Vulkan Frame Completed"); // Uncomment to flood logs as proof
  }
};

DECLARE_WAYFIRE_PLUGIN(wf::per_output_plugin_t<wayfire_vk_visible_plugin>);
