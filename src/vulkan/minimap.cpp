/* Vulkan minimap overlay. */

module;

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <print>
#include <vector>

module flying_rat.renderer;

import flying_rat.camera;
import flying_rat.config;
import flying_rat.maze;

namespace flying_rat {

// Create the minimap overlay pipeline.
// Return true on success, false on failure.
bool Renderer::CreateMinimapPipeline() {
  auto vert_path = FindShaderFile("minimap.vert.spv");
  auto frag_path = FindShaderFile("minimap.frag.spv");
  if (vert_path.empty() || frag_path.empty()) {
    std::println("[renderer] missing minimap SPIR-V shaders in '{}' (cwd "
                 "'{}'), did the 'shaders' target build?",
                 SHADER_DIR, std::filesystem::current_path().string());
    return false;
  }

  auto vert = ReadFile(vert_path);
  auto frag = ReadFile(frag_path);
  if (vert.empty() || frag.empty()) {
    std::println("[renderer] failed to read minimap SPIR-V shaders");
    return false;
  }

  VkShaderModule vert_module = CreateShaderModule(vert);
  VkShaderModule frag_module = CreateShaderModule(frag);
  if (vert_module == VK_NULL_HANDLE || frag_module == VK_NULL_HANDLE) {
    if (vert_module != VK_NULL_HANDLE) {
      vkDestroyShaderModule(device_, vert_module, nullptr);
    }
    if (frag_module != VK_NULL_HANDLE) {
      vkDestroyShaderModule(device_, frag_module, nullptr);
    }
    return false;
  }

  // No vertex buffers, the shader expands `SV_VertexID` procedurally.
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

  // The overlay draws on top of the scene, no depth testing.
  VkPipelineDepthStencilStateCreateInfo depth_stencil{};
  depth_stencil.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depth_stencil.depthTestEnable = VK_FALSE;
  depth_stencil.depthWriteEnable = VK_FALSE;
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

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert_module;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag_module;
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

  bool ok =
      vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr,
                                &minimap_pipeline_) == VK_SUCCESS;

  vkDestroyShaderModule(device_, vert_module, nullptr);
  vkDestroyShaderModule(device_, frag_module, nullptr);

  if (!ok) {
    std::println("[renderer] failed to create minimap pipeline");
    return false;
  }
  return true;
}

// Build the minimap cell buffer from the maze.
// - `maze` - Generated maze to render.
void Renderer::BuildMinimapBuffer(const Maze &maze) {
  DestroyMinimapBuffer();
  minimap_grid_ = maze.Size();
  minimap_start_ = maze.Start();
  minimap_exit_ = maze.Exit();

  std::vector<std::uint32_t> cells;
  minimap_cells_cpu_.clear();
  if (minimap_grid_.x > 0 && minimap_grid_.y > 0) {
    cells.reserve(static_cast<std::size_t>(minimap_grid_.x) *
                  static_cast<std::size_t>(minimap_grid_.y));
    minimap_cells_cpu_.reserve(static_cast<std::size_t>(minimap_grid_.x) *
                               static_cast<std::size_t>(minimap_grid_.y));
    for (int y = 0; y < minimap_grid_.y; ++y) {
      for (int x = 0; x < minimap_grid_.x; ++x) {
        const auto base = maze.At(x, y) == Tile::Wall ? 0u : 1u;
        minimap_cells_cpu_.push_back(base);
        // Start culled; first `CullTilesToFrustum` refreshes visibility.
        cells.push_back(base);
      }
    }
  }
  if (cells.empty()) {
    cells.push_back(0u);
  }

  VkDeviceSize size = cells.size() * sizeof(std::uint32_t);
  if (!CreateBuffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    minimap_buffer_, minimap_memory_)) {
    return;
  }

  void *mapped = nullptr;
  vkMapMemory(device_, minimap_memory_, 0, size, 0, &mapped);
  std::memcpy(mapped, cells.data(), static_cast<std::size_t>(size));
  vkUnmapMemory(device_, minimap_memory_);
}

// Destroy the minimap cell buffer and its memory.
void Renderer::DestroyMinimapBuffer() {
  if (minimap_buffer_ != VK_NULL_HANDLE) {
    vkDestroyBuffer(device_, minimap_buffer_, nullptr);
    minimap_buffer_ = VK_NULL_HANDLE;
  }
  if (minimap_memory_ != VK_NULL_HANDLE) {
    vkFreeMemory(device_, minimap_memory_, nullptr);
    minimap_memory_ = VK_NULL_HANDLE;
  }
  minimap_grid_ = {0, 0};
  minimap_start_ = {0, 0};
  minimap_exit_ = {0, 0};
  minimap_cells_cpu_.clear();
}

// Point minimap descriptor sets at the current cell buffer.
void Renderer::UpdateMinimapDescriptors() {
  if (descriptor_pool_ == VK_NULL_HANDLE || minimap_buffer_ == VK_NULL_HANDLE) {
    return;
  }
  if (minimap_sets_.size() != max_frames_in_flight_ ||
      uniform_buffers_.size() != max_frames_in_flight_) {
    return;
  }

  std::uint64_t cell_count =
      static_cast<std::uint64_t>(std::max(minimap_grid_.x, 0)) *
      static_cast<std::uint64_t>(std::max(minimap_grid_.y, 0));
  VkDeviceSize range =
      std::max<std::uint64_t>(cell_count, 1) * sizeof(std::uint32_t);

  for (std::size_t i = 0; i < max_frames_in_flight_; ++i) {
    VkDescriptorBufferInfo ubo_info{};
    ubo_info.buffer = uniform_buffers_[i];
    ubo_info.offset = 0;
    ubo_info.range = sizeof(FrameUniforms);

    VkDescriptorBufferInfo cells_info{};
    cells_info.buffer = minimap_buffer_;
    cells_info.offset = 0;
    cells_info.range = range;

    VkDescriptorImageInfo texture_info{};
    texture_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    texture_info.imageView = wall_image_view_;
    texture_info.sampler = wall_sampler_;
    const bool has_texture =
        wall_image_view_ != VK_NULL_HANDLE && wall_sampler_ != VK_NULL_HANDLE;

    VkWriteDescriptorSet writes[3]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = minimap_sets_[i];
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &ubo_info;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = minimap_sets_[i];
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &cells_info;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = minimap_sets_[i];
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].pImageInfo = &texture_info;
    vkUpdateDescriptorSets(device_, has_texture ? 3u : 2u, writes, 0, nullptr);
  }
}

// Compute minimap push constants for one frame.
// - `minimap` - Minimap overlay request for this frame.
// Return push constants positioning a centered overlay.
Renderer::MinimapPushConstants
Renderer::PushForMinimap(const MinimapArgs &minimap) const {
  MinimapPushConstants push{};
  if (minimap_grid_.x <= 0 || minimap_grid_.y <= 0) {
    return push;
  }
  float extent_w = static_cast<float>(swapchain_extent_.width);
  float extent_h = static_cast<float>(swapchain_extent_.height);
  if (extent_w <= 0.0f || extent_h <= 0.0f) {
    return push;
  }

  // Fit the whole maze into a centered box.
  float side = config::minimap_scale * std::min(extent_w, extent_h);
  float cell_px =
      side / static_cast<float>(std::max(minimap_grid_.x, minimap_grid_.y));
  float grid_w = cell_px * static_cast<float>(minimap_grid_.x);
  float grid_h = cell_px * static_cast<float>(minimap_grid_.y);
  float origin_x = (extent_w - grid_w) * 0.5f;
  float origin_y = (extent_h - grid_h) * 0.5f;

  push.origin = {(origin_x / extent_w) * 2.0f - 1.0f,
                 (origin_y / extent_h) * 2.0f - 1.0f};
  push.cell = {(cell_px / extent_w) * 2.0f, (cell_px / extent_h) * 2.0f};
  push.grid = minimap_grid_;
  push.player = minimap.player_cell;
  push.start = minimap_start_;
  push.exit = minimap_exit_;
  return push;
}

// Record the minimap overlay draw into a command buffer.
// - `cmd` - Command buffer to record.
// - `frame` - In-flight frame index selecting the descriptor set.
// - `minimap` - Minimap overlay request for this frame.
void Renderer::RecordMinimap(VkCommandBuffer cmd, std::size_t frame,
                             const MinimapArgs &minimap) {
  if (!minimap.visible) {
    return;
  }
  if (minimap_pipeline_ == VK_NULL_HANDLE ||
      minimap_buffer_ == VK_NULL_HANDLE) {
    return;
  }
  if (minimap_grid_.x <= 0 || minimap_grid_.y <= 0) {
    return;
  }
  if (frame >= minimap_sets_.size()) {
    return;
  }
  std::uint64_t total = static_cast<std::uint64_t>(minimap_grid_.x) *
                            static_cast<std::uint64_t>(minimap_grid_.y) +
                        1u; // Cells plus the background panel.
  if (total > std::numeric_limits<std::uint32_t>::max()) {
    return;
  }

  MinimapPushConstants push = PushForMinimap(minimap);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, minimap_pipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipeline_layout_, 0, 1, &minimap_sets_[frame], 0,
                          nullptr);
  vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                     sizeof(push), &push);

  // One quad per cell plus one background panel instance.
  vkCmdDraw(cmd, 6, static_cast<std::uint32_t>(total), 0, 0);
}

} // namespace flying_rat
