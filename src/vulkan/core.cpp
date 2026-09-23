/* Vulkan core. */

module;

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <print>
#include <vector>

module flying_rat.renderer;

import flying_rat.camera;
import flying_rat.config;
import flying_rat.maze;

namespace flying_rat {

// Create `VkInstance`, pick a physical device, create the logical device,
// swapchain, image views, render pass, pipelines, buffers, and
// synchronization.
// - `window` - Already created GLFW window.
// Return true on success, false on failure.
bool Renderer::Initialize(GLFWwindow *window) {
  window_ = window;

  if (!CreateInstance())
    return false;
  if (!CreateSurface())
    return false;
  if (!PickPhysicalDevice())
    return false;
  if (!CreateLogicalDevice())
    return false;
  if (!CreateSwapchain())
    return false;
  if (!CreateImageViews())
    return false;
  if (!CreateRenderPass())
    return false;
  if (!CreateDescriptorSetLayout())
    return false;
  if (!CreateGraphicsPipelines())
    return false;
  if (!CreateMinimapPipeline())
    return false;
  if (!CreateDepthResources())
    return false;
  if (!CreateFramebuffers())
    return false;
  if (!CreateUniformBuffers())
    return false;
  if (!CreateTileBuffersEmpty())
    return false;
  if (!CreateDescriptorPool())
    return false;
  if (!AllocateDescriptorSets())
    return false;
  UpdateDescriptors();
  if (!CreateCommandPool())
    return false;
  if (!CreateCommandBuffers())
    return false;
  if (!CreateSyncObjects())
    return false;

  return true;
}

// Build tile instance buffers from the maze.
// - `maze` - Generated maze to render.
// NOTE: Walls get one instance per `Tile::Wall`, floors get one instance per
// `Tile::Empty`.
void Renderer::BuildMaze(const Maze &maze) {
  if (device_ == VK_NULL_HANDLE) {
    return;
  }
  vkDeviceWaitIdle(device_);

  std::vector<glm::vec2> walls;
  std::vector<glm::vec2> floors;
  walls.reserve(static_cast<std::size_t>(maze.Size().x * maze.Size().y) / 2);
  floors.reserve(static_cast<std::size_t>(maze.Size().x * maze.Size().y));

  for (int y = 0; y < maze.Size().y; ++y) {
    for (int x = 0; x < maze.Size().x; ++x) {
      const Tile tile = maze.At(x, y);
      if (tile == Tile::Wall) {
        walls.emplace_back(static_cast<float>(x), static_cast<float>(y));
      } else if (tile == Tile::Empty) {
        floors.emplace_back(static_cast<float>(x), static_cast<float>(y));
      }
    }
  }

  wall_tiles_cpu_ = walls;
  floor_tiles_cpu_ = floors;
  RecreateTileBuffer(wall_tile_buffer_, wall_tile_memory_, walls,
                     wall_instance_count_);
  RecreateTileBuffer(floor_tile_buffer_, floor_tile_memory_, floors,
                     floor_instance_count_);
  BuildMinimapBuffer(maze);
  UpdateDescriptors();
  UpdateMinimapDescriptors();

  std::println("[renderer] maze instances: {} walls, {} floors",
               wall_instance_count_, floor_instance_count_);
}

std::array<glm::vec4, 6>
Renderer::ExtractFrustumPlanes(const glm::mat4 &view_projection) {
  const glm::vec4 row0(view_projection[0][0], view_projection[1][0],
                       view_projection[2][0], view_projection[3][0]);
  const glm::vec4 row1(view_projection[0][1], view_projection[1][1],
                       view_projection[2][1], view_projection[3][1]);
  const glm::vec4 row2(view_projection[0][2], view_projection[1][2],
                       view_projection[2][2], view_projection[3][2]);
  const glm::vec4 row3(view_projection[0][3], view_projection[1][3],
                       view_projection[2][3], view_projection[3][3]);

  std::array<glm::vec4, 6> planes{row3 + row0, row3 - row0, row3 + row1,
                                  row3 - row1, row2,        row3 - row2};
  for (auto &plane : planes) {
    const float length = glm::length(glm::vec3(plane.x, plane.y, plane.z));
    if (length > 1e-8f) {
      plane /= length;
    }
  }
  return planes;
}

bool Renderer::BoxInFrustum(const glm::vec3 &box_min, const glm::vec3 &box_max,
                            const std::array<glm::vec4, 6> &planes) {
  for (const auto &plane : planes) {
    const glm::vec3 normal(plane.x, plane.y, plane.z);
    glm::vec3 positive = box_min;
    if (normal.x >= 0.0f) {
      positive.x = box_max.x;
    }
    if (normal.y >= 0.0f) {
      positive.y = box_max.y;
    }
    if (normal.z >= 0.0f) {
      positive.z = box_max.z;
    }
    if (glm::dot(normal, positive) + plane.w < 0.0f) {
      return false;
    }
  }
  return true;
}

void Renderer::UpdateMinimapCulling(const std::array<glm::vec4, 6> &planes) {
  if (device_ == VK_NULL_HANDLE || minimap_buffer_ == VK_NULL_HANDLE ||
      minimap_memory_ == VK_NULL_HANDLE) {
    return;
  }
  if (minimap_grid_.x <= 0 || minimap_grid_.y <= 0 ||
      minimap_cells_cpu_.empty()) {
    return;
  }

  const float tile = config::tile_size;
  const float height = config::wall_height;
  const std::size_t count = minimap_cells_cpu_.size();

  std::vector<std::uint32_t> encoded;
  encoded.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const int x = static_cast<int>(
        i % static_cast<std::size_t>(std::max(minimap_grid_.x, 1)));
    const int y = static_cast<int>(
        i / static_cast<std::size_t>(std::max(minimap_grid_.x, 1)));
    const bool is_wall = minimap_cells_cpu_[i] == 0u;
    const glm::vec3 box_min(static_cast<float>(x) * tile,
                            is_wall ? 0.0f : -0.1f,
                            static_cast<float>(y) * tile);
    const glm::vec3 box_max((static_cast<float>(x) + 1.0f) * tile,
                            is_wall ? height : 0.1f,
                            (static_cast<float>(y) + 1.0f) * tile);
    const bool visible = BoxInFrustum(box_min, box_max, planes);
    encoded.push_back(minimap_cells_cpu_[i] + (visible ? 2u : 0u));
  }

  void *mapped = nullptr;
  const VkDeviceSize size = encoded.size() * sizeof(std::uint32_t);
  if (vkMapMemory(device_, minimap_memory_, 0, size, 0, &mapped) ==
          VK_SUCCESS &&
      mapped != nullptr) {
    std::memcpy(mapped, encoded.data(), static_cast<std::size_t>(size));
    vkUnmapMemory(device_, minimap_memory_);
  }
}

void Renderer::CullTilesToFrustum(const Camera &camera, float aspect) {
  if (device_ == VK_NULL_HANDLE) {
    return;
  }
  if (wall_tiles_cpu_.empty() && floor_tiles_cpu_.empty()) {
    wall_instance_count_ = 0;
    floor_instance_count_ = 0;
    return;
  }

  for (auto fence : in_flight_) {
    if (fence != VK_NULL_HANDLE) {
      vkWaitForFences(device_, 1, &fence, VK_TRUE,
                      std::numeric_limits<uint64_t>::max());
    }
  }

  const glm::mat4 view_projection =
      Camera::ProjectionMatrix(aspect) * camera.ViewMatrix();
  const auto planes = ExtractFrustumPlanes(view_projection);

  const float tile = config::tile_size;
  const float height = config::wall_height;

  std::vector<glm::vec2> visible;
  visible.reserve(wall_tiles_cpu_.size());
  for (const auto &tile_xy : wall_tiles_cpu_) {
    const glm::vec3 box_min(tile_xy.x * tile, 0.0f, tile_xy.y * tile);
    const glm::vec3 box_max((tile_xy.x + 1.0f) * tile, height,
                            (tile_xy.y + 1.0f) * tile);
    if (BoxInFrustum(box_min, box_max, planes)) {
      visible.push_back(tile_xy);
    }
  }
  if (!visible.empty() && wall_tile_memory_ != VK_NULL_HANDLE) {
    void *mapped = nullptr;
    const VkDeviceSize size = visible.size() * sizeof(glm::vec2);
    if (vkMapMemory(device_, wall_tile_memory_, 0, size, 0, &mapped) ==
            VK_SUCCESS &&
        mapped != nullptr) {
      std::memcpy(mapped, visible.data(), static_cast<std::size_t>(size));
      vkUnmapMemory(device_, wall_tile_memory_);
    }
  }
  wall_instance_count_ = static_cast<uint32_t>(visible.size());

  visible.clear();
  visible.reserve(floor_tiles_cpu_.size());
  for (const auto &tile_xy : floor_tiles_cpu_) {
    const glm::vec3 box_min(tile_xy.x * tile, -0.1f, tile_xy.y * tile);
    const glm::vec3 box_max((tile_xy.x + 1.0f) * tile, 0.1f,
                            (tile_xy.y + 1.0f) * tile);
    if (BoxInFrustum(box_min, box_max, planes)) {
      visible.push_back(tile_xy);
    }
  }
  if (!visible.empty() && floor_tile_memory_ != VK_NULL_HANDLE) {
    void *mapped = nullptr;
    const VkDeviceSize size = visible.size() * sizeof(glm::vec2);
    if (vkMapMemory(device_, floor_tile_memory_, 0, size, 0, &mapped) ==
            VK_SUCCESS &&
        mapped != nullptr) {
      std::memcpy(mapped, visible.data(), static_cast<std::size_t>(size));
      vkUnmapMemory(device_, floor_tile_memory_);
    }
  }
  floor_instance_count_ = static_cast<uint32_t>(visible.size());

  UpdateMinimapCulling(planes);
}

// Record and submit one frame with instanced wall and floor draws,
// plus the minimap overlay when requested.
// - `camera` - Camera used for the view matrix.
// - `aspect` - Aspect ratio of the viewport.
// - `minimap` - Minimap overlay request for this frame.
void Renderer::Draw(const Camera &camera, float aspect,
                    const MinimapArgs &minimap) {
  if (device_ == VK_NULL_HANDLE || swapchain_ == VK_NULL_HANDLE) {
    return;
  }
  if (wall_tiles_cpu_.empty() && floor_tiles_cpu_.empty()) {
    return;
  }

  CullTilesToFrustum(camera, aspect);
  if (wall_instance_count_ == 0 && floor_instance_count_ == 0) {
    return;
  }

  uint32_t image_index = 0;
  VkResult acquire = vkAcquireNextImageKHR(
      device_, swapchain_, std::numeric_limits<uint64_t>::max(),
      image_available_[current_frame_], VK_NULL_HANDLE, &image_index);
  if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
    return;
  }
  if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
    std::println("[renderer] vkAcquireNextImageKHR failed: {}",
                 static_cast<int>(acquire));
    return;
  }

  if (image_fences_[image_index] != VK_NULL_HANDLE) {
    vkWaitForFences(device_, 1, &image_fences_[image_index], VK_TRUE,
                    std::numeric_limits<uint64_t>::max());
  }
  image_fences_[image_index] = in_flight_[current_frame_];

  UpdateUniformBuffer(current_frame_, camera, aspect);

  vkResetFences(device_, 1, &in_flight_[current_frame_]);
  vkResetCommandBuffer(command_buffers_[current_frame_], 0);
  RecordCommandBuffer(command_buffers_[current_frame_], image_index,
                      current_frame_, minimap);

  VkSemaphore wait[] = {image_available_[current_frame_]};
  VkPipelineStageFlags wait_stages[] = {
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
  VkSemaphore signal[] = {render_finished_[image_index]};

  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.waitSemaphoreCount = 1;
  submit.pWaitSemaphores = wait;
  submit.pWaitDstStageMask = wait_stages;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command_buffers_[current_frame_];
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = signal;

  if (vkQueueSubmit(graphics_queue_, 1, &submit, in_flight_[current_frame_]) !=
      VK_SUCCESS) {
    std::println("[renderer] vkQueueSubmit failed");
    return;
  }

  VkSwapchainKHR swapchains[] = {swapchain_};
  VkPresentInfoKHR present{};
  present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = signal;
  present.swapchainCount = 1;
  present.pSwapchains = swapchains;
  present.pImageIndices = &image_index;

  VkResult present_result = vkQueuePresentKHR(present_queue_, &present);
  if (present_result == VK_ERROR_OUT_OF_DATE_KHR ||
      present_result == VK_SUBOPTIMAL_KHR) {
    // Window resize handling is out of scope.
  } else if (present_result != VK_SUCCESS) {
    std::println("[renderer] vkQueuePresentKHR failed: {}",
                 static_cast<int>(present_result));
  }

  current_frame_ = (current_frame_ + 1) % max_frames_in_flight_;
}

// Block until the GPU is idle.
void Renderer::WaitIdle() {
  if (device_ != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(device_);
  }
}

// Destroy Vulkan resources.
void Renderer::Destroy() noexcept {
  if (device_ != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(device_);
  }

  for (auto sem : image_available_) {
    if (sem != VK_NULL_HANDLE) {
      vkDestroySemaphore(device_, sem, nullptr);
    }
  }
  image_available_.clear();
  for (auto sem : render_finished_) {
    if (sem != VK_NULL_HANDLE) {
      vkDestroySemaphore(device_, sem, nullptr);
    }
  }
  render_finished_.clear();
  for (auto fence : in_flight_) {
    if (fence != VK_NULL_HANDLE) {
      vkDestroyFence(device_, fence, nullptr);
    }
  }
  in_flight_.clear();
  image_fences_.clear();

  if (command_pool_ != VK_NULL_HANDLE) {
    vkDestroyCommandPool(device_, command_pool_, nullptr);
    command_pool_ = VK_NULL_HANDLE;
  }
  command_buffers_.clear();

  if (descriptor_pool_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    descriptor_pool_ = VK_NULL_HANDLE;
  }
  wall_sets_.clear();
  floor_sets_.clear();
  minimap_sets_.clear();

  DestroyMinimapBuffer();

  DestroyTileBuffer(wall_tile_buffer_, wall_tile_memory_);
  DestroyTileBuffer(floor_tile_buffer_, floor_tile_memory_);
  wall_instance_count_ = 0;
  floor_instance_count_ = 0;
  wall_tiles_cpu_.clear();
  floor_tiles_cpu_.clear();

  for (std::size_t i = 0; i < uniform_buffers_.size(); ++i) {
    if (uniform_mapped_[i] != nullptr) {
      vkUnmapMemory(device_, uniform_memories_[i]);
      uniform_mapped_[i] = nullptr;
    }
    if (uniform_buffers_[i] != VK_NULL_HANDLE) {
      vkDestroyBuffer(device_, uniform_buffers_[i], nullptr);
      uniform_buffers_[i] = VK_NULL_HANDLE;
    }
    if (uniform_memories_[i] != VK_NULL_HANDLE) {
      vkFreeMemory(device_, uniform_memories_[i], nullptr);
      uniform_memories_[i] = VK_NULL_HANDLE;
    }
  }
  uniform_buffers_.clear();
  uniform_memories_.clear();
  uniform_mapped_.clear();

  for (auto fb : framebuffers_) {
    if (fb != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(device_, fb, nullptr);
    }
  }
  framebuffers_.clear();

  if (depth_view_ != VK_NULL_HANDLE) {
    vkDestroyImageView(device_, depth_view_, nullptr);
    depth_view_ = VK_NULL_HANDLE;
  }
  if (depth_image_ != VK_NULL_HANDLE) {
    vkDestroyImage(device_, depth_image_, nullptr);
    depth_image_ = VK_NULL_HANDLE;
  }
  if (depth_memory_ != VK_NULL_HANDLE) {
    vkFreeMemory(device_, depth_memory_, nullptr);
    depth_memory_ = VK_NULL_HANDLE;
  }

  if (minimap_pipeline_ != VK_NULL_HANDLE) {
    vkDestroyPipeline(device_, minimap_pipeline_, nullptr);
    minimap_pipeline_ = VK_NULL_HANDLE;
  }
  if (wall_pipeline_ != VK_NULL_HANDLE) {
    vkDestroyPipeline(device_, wall_pipeline_, nullptr);
    wall_pipeline_ = VK_NULL_HANDLE;
  }
  if (floor_pipeline_ != VK_NULL_HANDLE) {
    vkDestroyPipeline(device_, floor_pipeline_, nullptr);
    floor_pipeline_ = VK_NULL_HANDLE;
  }
  if (pipeline_layout_ != VK_NULL_HANDLE) {
    vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    pipeline_layout_ = VK_NULL_HANDLE;
  }
  if (descriptor_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
    descriptor_set_layout_ = VK_NULL_HANDLE;
  }
  if (render_pass_ != VK_NULL_HANDLE) {
    vkDestroyRenderPass(device_, render_pass_, nullptr);
    render_pass_ = VK_NULL_HANDLE;
  }

  for (auto view : swapchain_image_views_) {
    if (view != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, view, nullptr);
    }
  }
  swapchain_image_views_.clear();
  if (swapchain_ != VK_NULL_HANDLE) {
    vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
  }
  swapchain_images_.clear();
  swapchain_image_format_ = VK_FORMAT_UNDEFINED;
  swapchain_extent_ = {0, 0};
  if (device_ != VK_NULL_HANDLE) {
    vkDestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;
  }
  graphics_queue_ = VK_NULL_HANDLE;
  present_queue_ = VK_NULL_HANDLE;
  if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
    vkDestroySurfaceKHR(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
  }
  if (instance_ != VK_NULL_HANDLE) {
    vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
  }
  window_ = nullptr;
  physical_device_ = VK_NULL_HANDLE;
  current_frame_ = 0;
}

} // namespace flying_rat
