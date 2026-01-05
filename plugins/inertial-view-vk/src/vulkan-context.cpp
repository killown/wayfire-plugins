#include "vulkan-context.hpp"
#include <fstream>
#include <wayfire/util/log.hpp>

VulkanContext::VulkanContext(wlr_renderer *renderer) {
  device = wlr_vk_renderer_get_device(renderer);
  phdev = wlr_vk_renderer_get_physical_device(renderer);
  uint32_t family = wlr_vk_renderer_get_queue_family(renderer);
  vkGetDeviceQueue(device, family, 0, &queue);

  VkCommandPoolCreateInfo cp_ci{};
  cp_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cp_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cp_ci.queueFamilyIndex = family;
  vkCreateCommandPool(device, &cp_ci, nullptr, &pool);

  VkCommandBufferAllocateInfo a_ci{};
  a_ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  a_ci.commandPool = pool;
  a_ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  a_ci.commandBufferCount = 1;
  vkAllocateCommandBuffers(device, &a_ci, &cmd);

  VkFenceCreateInfo f_ci{};
  f_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  f_ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  vkCreateFence(device, &f_ci, nullptr, &fence);
}

VulkanContext::~VulkanContext() {
  wait_idle();
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

void VulkanContext::wait_idle() {
  if (device)
    vkDeviceWaitIdle(device);
}

VkShaderModule VulkanContext::load_shader(const std::string &path) {
  std::ifstream file(path, std::ios::ate | std::ios::binary);
  if (!file.is_open())
    return VK_NULL_HANDLE;
  size_t size = (size_t)file.tellg();
  std::vector<char> buffer(size);
  file.seekg(0);
  file.read(buffer.data(), size);
  VkShaderModuleCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  ci.codeSize = buffer.size();
  ci.pCode = reinterpret_cast<const uint32_t *>(buffer.data());
  VkShaderModule mod;
  vkCreateShaderModule(device, &ci, nullptr, &mod);
  return mod;
}

void VulkanContext::insert_barrier(VkCommandBuffer cmd, VkImage image,
                                   VkImageLayout old_ly, VkImageLayout new_ly,
                                   VkAccessFlags src_acc, VkAccessFlags dst_acc,
                                   VkPipelineStageFlags src_stg,
                                   VkPipelineStageFlags dst_stg) {
  VkImageMemoryBarrier b{};
  b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b.srcAccessMask = src_acc;
  b.dstAccessMask = dst_acc;
  b.oldLayout = old_ly;
  b.newLayout = new_ly;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, src_stg, dst_stg, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void VulkanContext::ensure_scratch(VkExtent2D ext) {
  if (extent.width == ext.width && extent.height == ext.height)
    return;
  wait_idle();
  if (framebuffer)
    vkDestroyFramebuffer(device, framebuffer, nullptr);
  if (scratch_view)
    vkDestroyImageView(device, scratch_view, nullptr);
  if (scratch_img)
    vkDestroyImage(device, scratch_img, nullptr);
  if (scratch_mem)
    vkFreeMemory(device, scratch_mem, nullptr);
  extent = ext;
  VkImageCreateInfo i_ci{};
  i_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  i_ci.imageType = VK_IMAGE_TYPE_2D;
  i_ci.format = VK_FORMAT_B8G8R8A8_UNORM;
  i_ci.extent = {ext.width, ext.height, 1};
  i_ci.mipLevels = 1;
  i_ci.arrayLayers = 1;
  i_ci.samples = VK_SAMPLE_COUNT_1_BIT;
  i_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  i_ci.usage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  vkCreateImage(device, &i_ci, nullptr, &scratch_img);
  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(device, scratch_img, &mr);
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(phdev, &mp);
  uint32_t type = 0;
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
    if ((mr.memoryTypeBits & (1 << i)) &&
        (mp.memoryTypes[i].propertyFlags &
         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
      type = i;
      break;
    }
  }
  VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
                             mr.size, type};
  vkAllocateMemory(device, &ai, nullptr, &scratch_mem);
  vkBindImageMemory(device, scratch_img, scratch_mem, 0);
  VkImageViewCreateInfo v_ci{};
  v_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  v_ci.image = scratch_img;
  v_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  v_ci.format = VK_FORMAT_B8G8R8A8_UNORM;
  v_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCreateImageView(device, &v_ci, nullptr, &scratch_view);
  VkFramebufferCreateInfo fb_ci{};
  fb_ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fb_ci.renderPass = render_pass;
  fb_ci.attachmentCount = 1;
  fb_ci.pAttachments = &scratch_view;
  fb_ci.width = ext.width;
  fb_ci.height = ext.height;
  fb_ci.layers = 1;
  vkCreateFramebuffer(device, &fb_ci, nullptr, &framebuffer);
}

void VulkanContext::setup_pipeline() {
  const char *home = getenv("HOME");
  std::string base = (home ? std::string(home) : "") +
                     "/.local/share/wayfire/inertial-vk/shaders/";
  vert_mod = load_shader(base + "inertial.vert.spv");
  frag_mod = load_shader(base + "inertial.frag.spv");
  if (!vert_mod || !frag_mod)
    return;

  VkDescriptorSetLayoutBinding b = {0,
                                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                    1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo dl_ci{};
  dl_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  dl_ci.bindingCount = 1;
  dl_ci.pBindings = &b;
  vkCreateDescriptorSetLayout(device, &dl_ci, nullptr, &desc_layout);

  VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
  VkDescriptorPoolCreateInfo dp_ci{};
  dp_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dp_ci.maxSets = 1;
  dp_ci.poolSizeCount = 1;
  dp_ci.pPoolSizes = &ps;
  vkCreateDescriptorPool(device, &dp_ci, nullptr, &desc_pool);

  VkDescriptorSetAllocateInfo ds_ai{};
  ds_ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  ds_ai.descriptorPool = desc_pool;
  ds_ai.descriptorSetCount = 1;
  ds_ai.pSetLayouts = &desc_layout;
  vkAllocateDescriptorSets(device, &ds_ai, &desc_set);

  VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT |
                                 VK_SHADER_STAGE_FRAGMENT_BIT,
                             0, sizeof(PushConstants)};
  VkPipelineLayoutCreateInfo pl_ci{};
  pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pl_ci.setLayoutCount = 1;
  pl_ci.pSetLayouts = &desc_layout;
  pl_ci.pushConstantRangeCount = 1;
  pl_ci.pPushConstantRanges = &pcr;
  vkCreatePipelineLayout(device, &pl_ci, nullptr, &pipe_layout);

  VkSamplerCreateInfo s_ci{};
  s_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  s_ci.magFilter = VK_FILTER_LINEAR;
  s_ci.minFilter = VK_FILTER_LINEAR;
  vkCreateSampler(device, &s_ci, nullptr, &sampler);

  VkAttachmentDescription att{};
  att.format = VK_FORMAT_B8G8R8A8_UNORM;
  att.samples = VK_SAMPLE_COUNT_1_BIT;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;

  VkRenderPassCreateInfo rp_ci{};
  rp_ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rp_ci.attachmentCount = 1;
  rp_ci.pAttachments = &att;
  rp_ci.subpassCount = 1;
  rp_ci.pSubpasses = &sub;
  vkCreateRenderPass(device, &rp_ci, nullptr, &render_pass);

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert_mod;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag_mod;
  stages[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkPipelineInputAssemblyStateCreateInfo ia{};
  ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp_ci{};
  vp_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  vp_ci.viewportCount = 1;
  vp_ci.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{};
  rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.lineWidth = 1.0f;
  rs.cullMode = VK_CULL_MODE_NONE;
  VkPipelineMultisampleStateCreateInfo ms{};
  ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState cba{};
  cba.blendEnable = VK_TRUE;
  cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  cba.colorBlendOp = VK_BLEND_OP_ADD;
  cba.colorWriteMask = 0xf;
  VkPipelineColorBlendStateCreateInfo cb{};
  cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  cb.attachmentCount = 1;
  cb.pAttachments = &cba;
  VkDynamicState dyn_s[] = {VK_DYNAMIC_STATE_VIEWPORT,
                            VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{};
  dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dyn.dynamicStateCount = 2;
  dyn.pDynamicStates = dyn_s;

  VkGraphicsPipelineCreateInfo gp_ci{};
  gp_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  gp_ci.stageCount = 2;
  gp_ci.pStages = stages;
  gp_ci.pVertexInputState = &vi;
  gp_ci.pInputAssemblyState = &ia;
  gp_ci.pViewportState = &vp_ci;
  gp_ci.pRasterizationState = &rs;
  gp_ci.pMultisampleState = &ms;
  gp_ci.pColorBlendState = &cb;
  gp_ci.pDynamicState = &dyn;
  gp_ci.layout = pipe_layout;
  gp_ci.renderPass = render_pass;
  vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp_ci, nullptr,
                            &pipeline);
  initialized = true;
}
