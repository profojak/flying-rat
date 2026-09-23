/* Vulkan pipeline. */

module;

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <print>
#include <vector>

module flying_rat.renderer;

import flying_rat.camera;
import flying_rat.config;
import flying_rat.maze;

namespace flying_rat {

// Read a whole binary file.
// - `path` - File to read.
// Return file bytes, empty on failure.
std::vector<std::uint32_t>
Renderer::ReadFile(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::ate | std::ios::binary);
  if (!file.is_open()) {
    return {};
  }
  std::streampos end = file.tellg();
  if (end == std::streampos(-1)) {
    return {};
  }
  auto size = static_cast<std::uint64_t>(end);
  if (size == 0 || size % sizeof(std::uint32_t) != 0) {
    return {};
  }
  std::vector<std::uint32_t> words(
      static_cast<std::size_t>(size / sizeof(std::uint32_t)));
  file.seekg(0);
  file.read(reinterpret_cast<char *>(words.data()),
            static_cast<std::streamsize>(size));
  if (!file) {
    return {};
  }
  return words;
}

// Resolve a shader file, trying the CMake SPIR-V directory first.
// - `filename` - Shader file name.
// Return the existing path, empty when not found.
std::filesystem::path
Renderer::FindShaderFile(const char *filename) {
  std::vector<std::filesystem::path> candidates;
  if (const char *env = std::getenv("SHADER_DIR")) {
    candidates.emplace_back(std::filesystem::path(env) / filename);
  }
  candidates.emplace_back(std::filesystem::path(SHADER_DIR) / filename);
  candidates.emplace_back(std::filesystem::path(filename));
  candidates.emplace_back(std::filesystem::path("build") / filename);
  for (const auto &p : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) {
      return p;
    }
  }
  return {};
}

// Create a shader module from SPIR-V code.
// - `code` - SPIR-V bytes.
// Return the module, or `VK_NULL_HANDLE` on failure.
VkShaderModule
Renderer::CreateShaderModule(const std::vector<std::uint32_t> &code) {
  if (code.empty()) {
    std::println("[renderer] vkCreateShaderModule failed: empty SPIR-V");
    return VK_NULL_HANDLE;
  }
  VkShaderModuleCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  info.codeSize = code.size() * sizeof(std::uint32_t);
  info.pCode = code.data();

  VkShaderModule module = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device_, &info, nullptr, &module) != VK_SUCCESS) {
    std::println("[renderer] vkCreateShaderModule failed");
    return VK_NULL_HANDLE;
  }
  return module;
}

// Create the render pass with color and depth attachments.
// Return true on success, false on failure.
bool Renderer::CreateRenderPass() {
  VkAttachmentDescription color{};
  color.format = swapchain_image_format_;
  color.samples = VK_SAMPLE_COUNT_1_BIT;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

  VkAttachmentDescription depth{};
  depth.format = FindDepthFormat();
  depth.samples = VK_SAMPLE_COUNT_1_BIT;
  depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkAttachmentReference color_ref{};
  color_ref.attachment = 0;
  color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkAttachmentReference depth_ref{};
  depth_ref.attachment = 1;
  depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &color_ref;
  subpass.pDepthStencilAttachment = &depth_ref;

  VkSubpassDependency dependency{};
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  dependency.dstSubpass = 0;
  dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

  VkAttachmentDescription attachments[] = {color, depth};
  VkRenderPassCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  info.attachmentCount = 2;
  info.pAttachments = attachments;
  info.subpassCount = 1;
  info.pSubpasses = &subpass;
  info.dependencyCount = 1;
  info.pDependencies = &dependency;

  if (vkCreateRenderPass(device_, &info, nullptr, &render_pass_) !=
      VK_SUCCESS) {
    std::println("[renderer] vkCreateRenderPass failed");
    return false;
  }
  return true;
}

// Find a supported depth format, preferring 32-bit float.
// Return the depth format.
VkFormat Renderer::FindDepthFormat() {
  const VkFormat candidates[] = {
      VK_FORMAT_D32_SFLOAT,
      VK_FORMAT_D32_SFLOAT_S8_UINT,
      VK_FORMAT_D24_UNORM_S8_UINT,
  };
  for (auto format : candidates) {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(physical_device_, format, &props);
    if ((props.optimalTilingFeatures &
         VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
      return format;
    }
  }
  return VK_FORMAT_D32_SFLOAT;
}

// Create the descriptor set layout shared by both pipelines.
// Return true on success, false on failure.
bool Renderer::CreateDescriptorSetLayout() {
  VkDescriptorSetLayoutBinding ubo{};
  ubo.binding = 0;
  ubo.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  ubo.descriptorCount = 1;
  ubo.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutBinding tiles{};
  tiles.binding = 1;
  tiles.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  tiles.descriptorCount = 1;
  tiles.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

  VkDescriptorSetLayoutBinding bindings[] = {ubo, tiles};
  VkDescriptorSetLayoutCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  info.bindingCount = 2;
  info.pBindings = bindings;

  if (vkCreateDescriptorSetLayout(device_, &info, nullptr,
                                  &descriptor_set_layout_) != VK_SUCCESS) {
    std::println("[renderer] vkCreateDescriptorSetLayout failed");
    return false;
  }
  return true;
}

// Create wall and floor graphics pipelines with push constants.
// Return true on success, false on failure.
bool Renderer::CreateGraphicsPipelines() {
  auto wall_vert_path = FindShaderFile("wall.vert.spv");
  auto wall_frag_path = FindShaderFile("wall.frag.spv");
  auto floor_vert_path = FindShaderFile("floor.vert.spv");
  auto floor_frag_path = FindShaderFile("floor.frag.spv");
  if (wall_vert_path.empty() || wall_frag_path.empty() ||
      floor_vert_path.empty() || floor_frag_path.empty()) {
    std::println(
        "[renderer] missing SPIR-V shaders in '{}' (cwd '{}'), did the "
        "'shaders' target build?",
        SHADER_DIR, std::filesystem::current_path().string());
    return false;
  }

  auto wall_vert = ReadFile(wall_vert_path);
  auto wall_frag = ReadFile(wall_frag_path);
  auto floor_vert = ReadFile(floor_vert_path);
  auto floor_frag = ReadFile(floor_frag_path);
  if (wall_vert.empty() || wall_frag.empty() || floor_vert.empty() ||
      floor_frag.empty()) {
    std::println("[renderer] failed to read SPIR-V shaders");
    return false;
  }

  VkShaderModule wall_vert_module = CreateShaderModule(wall_vert);
  VkShaderModule wall_frag_module = CreateShaderModule(wall_frag);
  VkShaderModule floor_vert_module = CreateShaderModule(floor_vert);
  VkShaderModule floor_frag_module = CreateShaderModule(floor_frag);
  if (wall_vert_module == VK_NULL_HANDLE ||
      wall_frag_module == VK_NULL_HANDLE ||
      floor_vert_module == VK_NULL_HANDLE ||
      floor_frag_module == VK_NULL_HANDLE) {
    if (wall_vert_module != VK_NULL_HANDLE)
      vkDestroyShaderModule(device_, wall_vert_module, nullptr);
    if (wall_frag_module != VK_NULL_HANDLE)
      vkDestroyShaderModule(device_, wall_frag_module, nullptr);
    if (floor_vert_module != VK_NULL_HANDLE)
      vkDestroyShaderModule(device_, floor_vert_module, nullptr);
    if (floor_frag_module != VK_NULL_HANDLE)
      vkDestroyShaderModule(device_, floor_frag_module, nullptr);
    return false;
  }

  // One shared range covers both the scene and the minimap push constants.
  VkPushConstantRange push{};
  push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  push.offset = 0;
  push.size = static_cast<std::uint32_t>(
      std::max(sizeof(PushConstants), sizeof(MinimapPushConstants)));

  VkPipelineLayoutCreateInfo layout_info{};
  layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &descriptor_set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push;

  if (vkCreatePipelineLayout(device_, &layout_info, nullptr,
                             &pipeline_layout_) != VK_SUCCESS) {
    std::println("[renderer] vkCreatePipelineLayout failed");
    vkDestroyShaderModule(device_, wall_vert_module, nullptr);
    vkDestroyShaderModule(device_, wall_frag_module, nullptr);
    vkDestroyShaderModule(device_, floor_vert_module, nullptr);
    vkDestroyShaderModule(device_, floor_frag_module, nullptr);
    return false;
  }

  // No vertex buffers, shaders expand `SV_VertexID` procedurally.
  VkPipelineVertexInputStateCreateInfo vertex_input{};
  vertex_input.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo assembly{};
  assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo viewport_state{};
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.viewportCount = 1;
  viewport_state.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster{};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depth_stencil{};
  depth_stencil.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depth_stencil.depthTestEnable = VK_TRUE;
  depth_stencil.depthWriteEnable = VK_TRUE;
  depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;
  depth_stencil.depthBoundsTestEnable = VK_FALSE;
  depth_stencil.stencilTestEnable = VK_FALSE;

  VkPipelineColorBlendAttachmentState blend_attachment{};
  blend_attachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

  VkPipelineColorBlendStateCreateInfo blend{};
  blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blend.attachmentCount = 1;
  blend.pAttachments = &blend_attachment;

  VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT,
                               VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{};
  dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamics;

  auto make_pipeline = [&](VkShaderModule vert, VkShaderModule frag,
                           VkPipeline &pipeline) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth_stencil;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = pipeline_layout_;
    info.renderPass = render_pass_;
    info.subpass = 0;

    return vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr,
                                     &pipeline) == VK_SUCCESS;
  };

  bool ok =
      make_pipeline(wall_vert_module, wall_frag_module, wall_pipeline_) &&
      make_pipeline(floor_vert_module, floor_frag_module, floor_pipeline_);

  vkDestroyShaderModule(device_, wall_vert_module, nullptr);
  vkDestroyShaderModule(device_, wall_frag_module, nullptr);
  vkDestroyShaderModule(device_, floor_vert_module, nullptr);
  vkDestroyShaderModule(device_, floor_frag_module, nullptr);

  if (!ok) {
    std::println("[renderer] vkCreateGraphicsPipelines failed");
    return false;
  }
  return true;
}

// Create the depth image and view matching the swapchain extent.
// Return true on success, false on failure.
bool Renderer::CreateDepthResources() {
  VkFormat format = FindDepthFormat();

  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format;
  image_info.extent.width = swapchain_extent_.width;
  image_info.extent.height = swapchain_extent_.height;
  image_info.extent.depth = 1;
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  if (vkCreateImage(device_, &image_info, nullptr, &depth_image_) !=
      VK_SUCCESS) {
    std::println("[renderer] failed to create depth image");
    return false;
  }

  VkMemoryRequirements requirements{};
  vkGetImageMemoryRequirements(device_, depth_image_, &requirements);
  uint32_t index = FindMemoryType(requirements.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (index == std::numeric_limits<uint32_t>::max()) {
    std::println("[renderer] no suitable memory type for depth image");
    return false;
  }

  VkMemoryAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize = requirements.size;
  alloc.memoryTypeIndex = index;
  if (vkAllocateMemory(device_, &alloc, nullptr, &depth_memory_) !=
      VK_SUCCESS) {
    std::println("[renderer] failed to allocate depth memory");
    return false;
  }
  vkBindImageMemory(device_, depth_image_, depth_memory_, 0);

  VkImageViewCreateInfo view_info{};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = depth_image_;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = format;
  view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  view_info.subresourceRange.baseMipLevel = 0;
  view_info.subresourceRange.levelCount = 1;
  view_info.subresourceRange.baseArrayLayer = 0;
  view_info.subresourceRange.layerCount = 1;

  if (vkCreateImageView(device_, &view_info, nullptr, &depth_view_) !=
      VK_SUCCESS) {
    std::println("[renderer] failed to create depth image view");
    return false;
  }
  return true;
}

// Create one framebuffer per swapchain image.
// Return true on success, false on failure.
bool Renderer::CreateFramebuffers() {
  framebuffers_.resize(swapchain_image_views_.size());
  for (std::size_t i = 0; i < swapchain_image_views_.size(); ++i) {
    VkImageView attachments[] = {swapchain_image_views_[i], depth_view_};
    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = render_pass_;
    info.attachmentCount = 2;
    info.pAttachments = attachments;
    info.width = swapchain_extent_.width;
    info.height = swapchain_extent_.height;
    info.layers = 1;
    if (vkCreateFramebuffer(device_, &info, nullptr, &framebuffers_[i]) !=
        VK_SUCCESS) {
      std::println("[renderer] vkCreateFramebuffer failed for image {}", i);
      return false;
    }
  }
  return true;
}

} // namespace flying_rat
