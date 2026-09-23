/* Vulkan resources. */

module;

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <print>
#include <vector>

module flying_rat.renderer;

import flying_rat.camera;
import flying_rat.config;
import flying_rat.maze;

namespace flying_rat {

// Destroy a tile buffer and its memory.
// - `buffer`, `memory` - Buffer and memory to destroy.
void Renderer::DestroyTileBuffer(VkBuffer &buffer, VkDeviceMemory &memory) {
  if (buffer != VK_NULL_HANDLE) {
    vkDestroyBuffer(device_, buffer, nullptr);
    buffer = VK_NULL_HANDLE;
  }
  if (memory != VK_NULL_HANDLE) {
    vkFreeMemory(device_, memory, nullptr);
    memory = VK_NULL_HANDLE;
  }
}

// Create a tile SSBO from CPU tile coordinates.
// - `buffer`, `memory` - Buffer and memory to recreate.
// - `tiles` - Tile coordinates.
// - `out_count` - Receives the instance count for `vkCmdDraw`.
void Renderer::RecreateTileBuffer(VkBuffer &buffer, VkDeviceMemory &memory,
                                  const std::vector<glm::vec2> &tiles,
                                  uint32_t &out_count) {
  DestroyTileBuffer(buffer, memory);
  out_count = static_cast<uint32_t>(tiles.size());

  VkDeviceSize size = (tiles.empty() ? 1u : tiles.size()) * sizeof(glm::vec2);
  if (!CreateBuffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    buffer, memory)) {
    out_count = 0;
    return;
  }

  void *mapped = nullptr;
  vkMapMemory(device_, memory, 0, size, 0, &mapped);
  if (!tiles.empty()) {
    std::memcpy(mapped, tiles.data(), tiles.size() * sizeof(glm::vec2));
  } else {
    std::memset(mapped, 0, size);
  }
  vkUnmapMemory(device_, memory);
}

// Create one host-visible uniform buffer per in-flight frame.
// Return true on success, false on failure.
bool Renderer::CreateUniformBuffers() {
  uniform_buffers_.resize(max_frames_in_flight_, VK_NULL_HANDLE);
  uniform_memories_.resize(max_frames_in_flight_, VK_NULL_HANDLE);
  uniform_mapped_.resize(max_frames_in_flight_, nullptr);

  for (std::size_t i = 0; i < max_frames_in_flight_; ++i) {
    if (!CreateBuffer(sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      uniform_buffers_[i], uniform_memories_[i])) {
      return false;
    }
    vkMapMemory(device_, uniform_memories_[i], 0, sizeof(FrameUniforms), 0,
                &uniform_mapped_[i]);
  }
  return true;
}

// Create placeholder tile buffers so descriptors are valid before
// the first `BuildMaze` call.
// Return true on success, false on failure.
bool Renderer::CreateTileBuffersEmpty() {
  std::vector<glm::vec2> empty;
  uint32_t dummy = 0;
  RecreateTileBuffer(wall_tile_buffer_, wall_tile_memory_, empty, dummy);
  wall_instance_count_ = 0;
  RecreateTileBuffer(floor_tile_buffer_, floor_tile_memory_, empty, dummy);
  floor_instance_count_ = 0;
  return wall_tile_buffer_ != VK_NULL_HANDLE &&
         floor_tile_buffer_ != VK_NULL_HANDLE;
}

// Create the descriptor pool for wall, floor, and minimap sets.
// Return true on success, false on failure.
bool Renderer::CreateDescriptorPool() {
  constexpr std::uint32_t sets_per_frame = 3;
  VkDescriptorPoolSize sizes[3]{};
  sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  sizes[0].descriptorCount =
      static_cast<std::uint32_t>(max_frames_in_flight_ * sets_per_frame);
  sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  sizes[1].descriptorCount =
      static_cast<std::uint32_t>(max_frames_in_flight_ * sets_per_frame);
  sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  sizes[2].descriptorCount =
      static_cast<std::uint32_t>(max_frames_in_flight_ * sets_per_frame);

  VkDescriptorPoolCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  info.maxSets =
      static_cast<std::uint32_t>(max_frames_in_flight_ * sets_per_frame);
  info.poolSizeCount = 3;
  info.pPoolSizes = sizes;

  if (vkCreateDescriptorPool(device_, &info, nullptr, &descriptor_pool_) !=
      VK_SUCCESS) {
    std::println("[renderer] vkCreateDescriptorPool failed");
    return false;
  }
  return true;
}

// Allocate wall, floor, and minimap descriptor sets, one per in-flight
// frame.
// Return true on success, false on failure.
bool Renderer::AllocateDescriptorSets() {
  wall_sets_.resize(max_frames_in_flight_);
  floor_sets_.resize(max_frames_in_flight_);
  minimap_sets_.resize(max_frames_in_flight_);

  std::vector<VkDescriptorSetLayout> layouts(max_frames_in_flight_,
                                             descriptor_set_layout_);
  VkDescriptorSetAllocateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  info.descriptorPool = descriptor_pool_;
  info.descriptorSetCount = static_cast<uint32_t>(max_frames_in_flight_);
  info.pSetLayouts = layouts.data();

  if (vkAllocateDescriptorSets(device_, &info, wall_sets_.data()) !=
      VK_SUCCESS) {
    std::println("[renderer] failed to allocate wall descriptor sets");
    return false;
  }
  if (vkAllocateDescriptorSets(device_, &info, floor_sets_.data()) !=
      VK_SUCCESS) {
    std::println("[renderer] failed to allocate floor descriptor sets");
    return false;
  }
  if (vkAllocateDescriptorSets(device_, &info, minimap_sets_.data()) !=
      VK_SUCCESS) {
    std::println("[renderer] failed to allocate minimap descriptor sets");
    return false;
  }
  return true;
}

// Point wall and floor descriptor sets at the current buffers and the wall
// texture.  Floor sets share the layout and the texture slot even though
// the floor shader leaves binding 2 unused.
void Renderer::UpdateDescriptors() {
  if (descriptor_pool_ == VK_NULL_HANDLE) {
    return;
  }
  VkDeviceSize wall_size =
      std::max<VkDeviceSize>(1, wall_instance_count_) * sizeof(glm::vec2);
  VkDeviceSize floor_size =
      std::max<VkDeviceSize>(1, floor_instance_count_) * sizeof(glm::vec2);

  for (std::size_t i = 0; i < max_frames_in_flight_; ++i) {
    VkDescriptorBufferInfo ubo_info{};
    ubo_info.buffer = uniform_buffers_[i];
    ubo_info.offset = 0;
    ubo_info.range = sizeof(FrameUniforms);

    VkDescriptorBufferInfo wall_info{};
    wall_info.buffer = wall_tile_buffer_;
    wall_info.offset = 0;
    wall_info.range = wall_size;

    VkDescriptorImageInfo texture_info{};
    texture_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    texture_info.imageView = wall_image_view_;
    texture_info.sampler = wall_sampler_;
    const bool has_texture =
        wall_image_view_ != VK_NULL_HANDLE && wall_sampler_ != VK_NULL_HANDLE;

    VkWriteDescriptorSet wall_writes[3]{};
    wall_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wall_writes[0].dstSet = wall_sets_[i];
    wall_writes[0].dstBinding = 0;
    wall_writes[0].descriptorCount = 1;
    wall_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    wall_writes[0].pBufferInfo = &ubo_info;
    wall_writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wall_writes[1].dstSet = wall_sets_[i];
    wall_writes[1].dstBinding = 1;
    wall_writes[1].descriptorCount = 1;
    wall_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wall_writes[1].pBufferInfo = &wall_info;
    wall_writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wall_writes[2].dstSet = wall_sets_[i];
    wall_writes[2].dstBinding = 2;
    wall_writes[2].descriptorCount = 1;
    wall_writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wall_writes[2].pImageInfo = &texture_info;
    vkUpdateDescriptorSets(device_, has_texture ? 3u : 2u, wall_writes, 0,
                           nullptr);

    VkDescriptorBufferInfo floor_info{};
    floor_info.buffer = floor_tile_buffer_;
    floor_info.offset = 0;
    floor_info.range = floor_size;

    VkWriteDescriptorSet floor_writes[3]{};
    floor_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    floor_writes[0].dstSet = floor_sets_[i];
    floor_writes[0].dstBinding = 0;
    floor_writes[0].descriptorCount = 1;
    floor_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    floor_writes[0].pBufferInfo = &ubo_info;
    floor_writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    floor_writes[1].dstSet = floor_sets_[i];
    floor_writes[1].dstBinding = 1;
    floor_writes[1].descriptorCount = 1;
    floor_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    floor_writes[1].pBufferInfo = &floor_info;
    floor_writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    floor_writes[2].dstSet = floor_sets_[i];
    floor_writes[2].dstBinding = 2;
    floor_writes[2].descriptorCount = 1;
    floor_writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    floor_writes[2].pImageInfo = &texture_info;
    vkUpdateDescriptorSets(device_, has_texture ? 3u : 2u, floor_writes, 0,
                           nullptr);
  }
}

// Create the command pool for frame command buffers.
// Return true on success, false on failure.
bool Renderer::CreateCommandPool() {
  VkCommandPoolCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  info.queueFamilyIndex = graphics_queue_family_;
  if (vkCreateCommandPool(device_, &info, nullptr, &command_pool_) !=
      VK_SUCCESS) {
    std::println("[renderer] vkCreateCommandPool failed");
    return false;
  }
  return true;
}

// Allocate one primary command buffer per in-flight frame.
// Return true on success, false on failure.
bool Renderer::CreateCommandBuffers() {
  command_buffers_.resize(max_frames_in_flight_);
  VkCommandBufferAllocateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  info.commandPool = command_pool_;
  info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  info.commandBufferCount = static_cast<uint32_t>(max_frames_in_flight_);
  if (vkAllocateCommandBuffers(device_, &info, command_buffers_.data()) !=
      VK_SUCCESS) {
    std::println("[renderer] vkAllocateCommandBuffers failed");
    return false;
  }
  return true;
}

// Create semaphores and fences.
// Return true on success, false on failure.
bool Renderer::CreateSyncObjects() {
  image_available_.assign(max_frames_in_flight_, VK_NULL_HANDLE);
  render_finished_.assign(swapchain_images_.size(), VK_NULL_HANDLE);
  in_flight_.assign(max_frames_in_flight_, VK_NULL_HANDLE);
  image_fences_.assign(swapchain_images_.size(), VK_NULL_HANDLE);

  VkSemaphoreCreateInfo sem_info{};
  sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

  for (std::size_t i = 0; i < max_frames_in_flight_; ++i) {
    if (vkCreateSemaphore(device_, &sem_info, nullptr, &image_available_[i]) !=
            VK_SUCCESS ||
        vkCreateFence(device_, &fence_info, nullptr, &in_flight_[i]) !=
            VK_SUCCESS) {
      std::println("[renderer] failed to create per-frame sync objects");
      return false;
    }
  }
  for (std::size_t i = 0; i < swapchain_images_.size(); ++i) {
    if (vkCreateSemaphore(device_, &sem_info, nullptr, &render_finished_[i]) !=
        VK_SUCCESS) {
      std::println("[renderer] failed to create present sync objects");
      return false;
    }
  }
  return true;
}

// Upload view-projection, light, and ambient for one frame.
// - `frame` - In-flight frame index.
// - `camera` - Camera used for the view matrix.
// - `aspect` - Aspect ratio of the viewport.
void Renderer::UpdateUniformBuffer(std::size_t frame, const Camera &camera,
                                   float aspect) {
  FrameUniforms uniforms{};
  uniforms.view_projection =
      camera.ProjectionMatrix(aspect) * camera.ViewMatrix();
  uniforms.light_direction = glm::normalize(config::light_direction);
  uniforms.ambient = config::ambient_strength;
  std::memcpy(uniform_mapped_[frame], &uniforms, sizeof(uniforms));
}

// Record wall, floor, and minimap draws into a command buffer.
// - `cmd` - Command buffer to record.
// - `image_index` - Swapchain image (framebuffer) index.
// - `frame` - In-flight frame index selecting the descriptor sets.
// - `minimap` - Minimap overlay request for this frame.
void Renderer::RecordCommandBuffer(VkCommandBuffer cmd, uint32_t image_index,
                                   std::size_t frame,
                                   const MinimapArgs &minimap) {
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  vkBeginCommandBuffer(cmd, &begin);

  VkClearValue clear[2]{};
  clear[0].color = {{config::clear_color.r, config::clear_color.g,
                     config::clear_color.b, config::clear_color.a}};
  clear[1].depthStencil = {1.0f, 0};

  VkRenderPassBeginInfo pass{};
  pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  pass.renderPass = render_pass_;
  pass.framebuffer = framebuffers_[image_index];
  pass.renderArea.offset = {0, 0};
  pass.renderArea.extent = swapchain_extent_;
  pass.clearValueCount = 2;
  pass.pClearValues = clear;
  vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);

  VkViewport viewport{};
  viewport.x = 0.0f;
  viewport.y = 0.0f;
  viewport.width = static_cast<float>(swapchain_extent_.width);
  viewport.height = static_cast<float>(swapchain_extent_.height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(cmd, 0, 1, &viewport);

  VkRect2D scissor{};
  scissor.offset = {0, 0};
  scissor.extent = swapchain_extent_;
  vkCmdSetScissor(cmd, 0, 1, &scissor);

  PushConstants push{};
  push.tile_size = config::tile_size;
  push.wall_height = config::wall_height;
  vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                     sizeof(push), &push);

  if (wall_instance_count_ > 0) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, wall_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline_layout_, 0, 1, &wall_sets_[frame], 0,
                            nullptr);

    // Procedural box sides: 24 vertices per wall tile instance.
    vkCmdDraw(cmd, wall_vertices_, wall_instance_count_, 0, 0);
  }

  if (floor_instance_count_ > 0) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, floor_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline_layout_, 0, 1, &floor_sets_[frame], 0,
                            nullptr);

    // Procedural floor quad: 6 vertices per floor tile instance.
    vkCmdDraw(cmd, floor_vertices_, floor_instance_count_, 0, 0);
  }

  RecordMinimap(cmd, frame, minimap);

  vkCmdEndRenderPass(cmd);
  vkEndCommandBuffer(cmd);
}

} // namespace flying_rat
