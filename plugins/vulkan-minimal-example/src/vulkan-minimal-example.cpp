#include <wayfire/core.hpp>
#include <wayfire/output.hpp>
#include <wayfire/per-output-plugin.hpp>

#include <vulkan/vulkan.hpp>

class wayfire_vulkan_minimal_example_plugin : public wf::per_output_plugin_instance_t
{
public:
    void init() override
    {
        LOGI("vulkan-minimal-example loaded!");
        try
        {
            // Initialize Vulkan instance only
            vk::ApplicationInfo app_info("vulkan-minimal-example", VK_MAKE_VERSION(1, 0, 0),
                                        "No Engine", VK_MAKE_VERSION(1, 0, 0), VK_API_VERSION_1_0);

            vk::InstanceCreateInfo create_info({}, &app_info);
            instance = vk::createInstance(create_info);
            LOGI("Vulkan instance created successfully.");
        } catch (const vk::SystemError& err)
        {
            LOGE("Failed to create Vulkan instance: %s", err.what());
        }
    }

    void fini() override
    {
        if (instance)
        {
            instance.destroy();
        }
    }

private:
    vk::Instance instance;
};

DECLARE_WAYFIRE_PLUGIN(wf::per_output_plugin_t<wayfire_vulkan_minimal_example_plugin>);