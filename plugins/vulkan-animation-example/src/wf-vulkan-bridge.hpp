#pragma once

#include <chrono>
#include <cmath>
#include <fstream>
#include <wayfire/core.hpp>
#include <wayfire/output.hpp>
#include <wayfire/util/log.hpp>

extern "C" {
#include <vulkan/vulkan.h>
#include <wlr/render/vulkan.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
}

/**
 * @brief RAII container for Vulkan resources borrowed from the compositor.
 */
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
      if (cmd && pool)
        vkFreeCommandBuffers(device, pool, 1, &cmd);
      if (pool)
        vkDestroyCommandPool(device, pool, nullptr);
      if (fence)
        vkDestroyFence(device, fence, nullptr);
      if (scratch_image)
        vkDestroyImage(device, scratch_image, nullptr);
      if (scratch_mem)
        vkFreeMemory(device, scratch_mem, nullptr);
      if (dump_buffer)
        vkDestroyBuffer(device, dump_buffer, nullptr);
      if (dump_mem)
        vkFreeMemory(device, dump_mem, nullptr);
    }
  }
};

/**
 * @brief Utility namespace for Vulkan helper operations.
 */
namespace wf_vk_utils {
static uint32_t find_memory_type(VkPhysicalDevice pd, uint32_t filter,
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

static void transition_layout(VkCommandBuffer cmd, VkImage img,
                              VkImageLayout old_ly, VkImageLayout new_ly) {
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
} // namespace wf_vk_utils
