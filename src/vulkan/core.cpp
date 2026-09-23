/* Vulkan core. */

module;

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

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
  if (wall_instance_count_ == 0 && floor_instance_count_ == 0) {
    return;
  }

  vkWaitForFences(device_, 1, &in_flight_[current_frame_], VK_TRUE,
                  std::numeric_limits<uint64_t>::max());

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
void Renderer::Destroy() {
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
