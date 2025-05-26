#include <wayfire/core.hpp>
#include <wayfire/output.hpp>
#include <wayfire/per-output-plugin.hpp>

extern "C" {
#include <vulkan/vulkan.h>
#include <wlr/render/vulkan.h> // Needed for Vulkan helpers
}

class wayfire_vk_interact_plugin : public wf::per_output_plugin_instance_t {
public:
  void init() override {
    LOGI("Vulkan interact plugin initialized!");

    if (!wf::get_core().is_vulkan()) {
      LOGE("Error: This plugin requires the Vulkan renderer!");
      return;
    }

    struct wlr_renderer *renderer = wf::get_core().renderer;
    if (!wlr_renderer_is_vk(renderer)) {
      LOGE("Renderer claims to be Vulkan but fails wlr_renderer_is_vk()");
      return;
    }

    VkDevice device = wlr_vk_renderer_get_device(renderer);
    uint32_t queue_family = wlr_vk_renderer_get_queue_family(renderer);

    LOGI("Successfully accessed Vulkan device and queue family.");

    // Create command pool
    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = queue_family;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    if (auto res =
            vkCreateCommandPool(device, &pool_info, nullptr, &command_pool);
        res != VK_SUCCESS) {
      LOGE("Failed to create command pool: %d", res);
      return;
    }

    LOGI("Successfully created Vulkan command pool.");
  }

  void fini() override {
    if (!wf::get_core().is_vulkan())
      return;

    struct wlr_renderer *renderer = wf::get_core().renderer;
    if (!renderer || !wlr_renderer_is_vk(renderer))
      return;

    if (command_pool != VK_NULL_HANDLE) {
      VkDevice device = wlr_vk_renderer_get_device(renderer);
      vkDestroyCommandPool(device, command_pool, nullptr);
      command_pool = VK_NULL_HANDLE;
    }
  }

private:
  VkCommandPool command_pool = VK_NULL_HANDLE;
};

DECLARE_WAYFIRE_PLUGIN(wf::per_output_plugin_t<wayfire_vk_interact_plugin>);
