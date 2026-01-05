#pragma once

#include <cstring>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

extern "C" {
#include <wlr/render/vulkan.h>
}

struct alignas(16) PushConstants {
  float anchor_pos[2];
  float velocity[2];
  float spring_k;
  float time;
  float aspect_ratio;
  float margin;
};

class VulkanContext {
public:
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
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout desc_layout = VK_NULL_HANDLE;
  VkDescriptorPool desc_pool = VK_NULL_HANDLE;
  VkDescriptorSet desc_set = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  VkRenderPass render_pass = VK_NULL_HANDLE;
  VkShaderModule vert_mod = VK_NULL_HANDLE;
  VkShaderModule frag_mod = VK_NULL_HANDLE;
  bool initialized = false;

  VulkanContext(wlr_renderer *renderer);
  ~VulkanContext();

  void wait_idle();
  void ensure_scratch(VkExtent2D ext);
  void setup_pipeline();
  VkShaderModule load_shader(const std::string &path);
  void insert_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout old_ly,
                      VkImageLayout new_ly, VkAccessFlags src_acc,
                      VkAccessFlags dst_acc, VkPipelineStageFlags src_stg,
                      VkPipelineStageFlags dst_stg);
};
