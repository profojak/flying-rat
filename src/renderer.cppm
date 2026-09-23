/* Vulkan renderer. */

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
#include <optional>
#include <print>
#include <set>
#include <vector>

export module flying_rat.renderer;

import flying_rat.camera;
import flying_rat.config;
import flying_rat.maze;

namespace flying_rat {

// Vulkan renderer.
// NOTE: No other code deals with Vulkan API directly.
export class Renderer {
private:
  // GLFW window handle.
  GLFWwindow *window_ = nullptr;
  // Vulkan instance handle.
  VkInstance instance_ = VK_NULL_HANDLE;
  // Vulkan surface handle.
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  // Vulkan physical device handle.
  VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
  // Vulkan logical device handle.
  VkDevice device_ = VK_NULL_HANDLE;
  // Graphics queue handle.
  VkQueue graphics_queue_ = VK_NULL_HANDLE;
  // Present queue handle.
  VkQueue present_queue_ = VK_NULL_HANDLE;
  // Graphics queue family index.
  uint32_t graphics_queue_family_ = 0;
  // Present queue family index.
  uint32_t present_queue_family_ = 0;
  // Swapchain handle.
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  // Swapchain images.
  std::vector<VkImage> swapchain_images_;
  // Swapchain image format.
  VkFormat swapchain_image_format_ = VK_FORMAT_UNDEFINED;
  // Swapchain extent.
  VkExtent2D swapchain_extent_{0, 0};
  // Swapchain image views.
  std::vector<VkImageView> swapchain_image_views_;

  // Render pass with color and depth attachments.
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  // Descriptor set layout shared by wall and floor pipelines.
  VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
  // Pipeline layout with descriptor set and push constants.
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  // Wall graphics pipeline.
  VkPipeline wall_pipeline_ = VK_NULL_HANDLE;
  // Floor graphics pipeline.
  VkPipeline floor_pipeline_ = VK_NULL_HANDLE;
  // Framebuffers, one per swapchain image.
  std::vector<VkFramebuffer> framebuffers_;
  // Command pool for frame command buffers.
  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  // Command buffers, one per in-flight frame.
  std::vector<VkCommandBuffer> command_buffers_;

  // Depth image handle.
  VkImage depth_image_ = VK_NULL_HANDLE;
  // Depth image memory.
  VkDeviceMemory depth_memory_ = VK_NULL_HANDLE;
  // Depth image view.
  VkImageView depth_view_ = VK_NULL_HANDLE;

  // Per-frame uniform buffer object.
  // NOTE: Must match `FrameUniforms` in `data/wall.slang` and
  // `data/floor.slang`.
  struct FrameUniforms {
    // Column-major view-projection matrix.
    glm::mat4 view_projection{};
    // Light direction in world space.
    glm::vec3 light_direction{0.0f};
    // Ambient light intensity.
    float ambient = 0.0f;
  };

  static_assert(sizeof(FrameUniforms) == 80);
  static_assert(offsetof(FrameUniforms, light_direction) == 64);
  static_assert(offsetof(FrameUniforms, ambient) == 76);

  // Push constants.
  // NOTE: Must match `PushConstants` in both shaders.
  struct PushConstants {
    // Tile size in world units.
    float tile_size = config::tile_size;
    // Wall height in world units.
    float wall_height = config::wall_height;
  };

  static_assert(sizeof(PushConstants) == 8);

  // Uniform buffers, one per in-flight frame.
  std::vector<VkBuffer> uniform_buffers_;
  // Uniform buffer memories.
  std::vector<VkDeviceMemory> uniform_memories_;
  // Persistently mapped uniform buffer pointers.
  std::vector<void *> uniform_mapped_;

  // Wall tile storage buffer.
  VkBuffer wall_tile_buffer_ = VK_NULL_HANDLE;
  // Wall tile buffer memory.
  VkDeviceMemory wall_tile_memory_ = VK_NULL_HANDLE;
  // Floor tile storage buffer.
  VkBuffer floor_tile_buffer_ = VK_NULL_HANDLE;
  // Floor tile buffer memory.
  VkDeviceMemory floor_tile_memory_ = VK_NULL_HANDLE;
  // Number of wall instances.
  uint32_t wall_instance_count_ = 0;
  // Number of floor instances.
  uint32_t floor_instance_count_ = 0;

  // Descriptor pool holding wall and floor sets for all frames.
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  // Wall descriptor sets, one per in-flight frame.
  std::vector<VkDescriptorSet> wall_sets_;
  // Floor descriptor sets, one per in-flight frame.
  std::vector<VkDescriptorSet> floor_sets_;

  // Image-available semaphores, one per in-flight frame.
  std::vector<VkSemaphore> image_available_;
  // Render-finished semaphores, one per swapchain image.
  std::vector<VkSemaphore> render_finished_;
  // In-flight fences, one per in-flight frame.
  std::vector<VkFence> in_flight_;
  // Per-swapchain-image fence tracking.
  std::vector<VkFence> image_fences_;
  // Current in-flight frame index.
  std::size_t current_frame_ = 0;

  // Frames that can be in flight at once.
  static constexpr std::size_t max_frames_in_flight_ = 2;
  // Vertices per wall instance: 4 sides x 2 triangles x 3 verts.
  static constexpr uint32_t wall_vertices_ = 24;
  // Vertices per floor instance: 1 quad x 2 triangles x 3 verts.
  static constexpr uint32_t floor_vertices_ = 6;

  // Queue family indices used for device selection.
  struct QueueFamilyIndices {
    std::optional<uint32_t> graphics;
    std::optional<uint32_t> present;

    // Return true when both a graphics and a present family were found.
    [[nodiscard]] bool IsComplete() const {
      return graphics.has_value() && present.has_value();
    }
  };

  // Swapchain support details for a physical device.
  struct SwapchainSupport {
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> present_modes;
  };

public:
  Renderer() = default;
  Renderer(const Renderer &) = delete;
  Renderer &operator=(const Renderer &) = delete;
  ~Renderer() { Destroy(); }

  // Create `VkInstance`, pick a physical device, create the logical device,
  // swapchain, image views, render pass, pipelines, buffers, and
  // synchronization.
  // - `window` - Already created GLFW window.
  // Return true on success, false on failure.
  bool Initialize(GLFWwindow *window) {
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
  void BuildMaze(const Maze &maze) {
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
    UpdateDescriptors();

    std::println("[renderer] maze instances: {} walls, {} floors",
                 wall_instance_count_, floor_instance_count_);
  }

  // Record and submit one frame with instanced wall and floor draws.
  // - `camera` - Camera used for the view matrix.
  // - `aspect` - Aspect ratio of the viewport.
  void Draw(const Camera &camera, float aspect) {
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
                        current_frame_);

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

    if (vkQueueSubmit(graphics_queue_, 1, &submit,
                      in_flight_[current_frame_]) != VK_SUCCESS) {
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
  void WaitIdle() {
    if (device_ != VK_NULL_HANDLE) {
      vkDeviceWaitIdle(device_);
    }
  }

  // Destroy Vulkan resources.
  void Destroy() {
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

private:
  // Create Vulkan instance.
  // Return true on success, false on failure.
  bool CreateInstance() {
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "flying-rat";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "flying-rat";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion = VK_API_VERSION_1_2;

    uint32_t glfw_count = 0;
    const char **glfw_extensions =
        glfwGetRequiredInstanceExtensions(&glfw_count);

    // MoltenVK on MacOS needs `VK_KHR_portability_enumeration`.
    std::vector<const char *> extensions;
    for (uint32_t i = 0; i < glfw_count; ++i) {
      extensions.push_back(glfw_extensions[i]);
    }
    extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);

    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app_info;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;

    // Validation layers are optional, continue without them on machines
    // that only ship the loader and MoltenVK.
    const char *validation = "VK_LAYER_KHRONOS_validation";
    uint32_t layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
    std::vector<VkLayerProperties> layers(layer_count);
    vkEnumerateInstanceLayerProperties(&layer_count, layers.data());
    for (const auto &l : layers) {
      if (std::strcmp(l.layerName, validation) == 0) {
        info.enabledLayerCount = 1;
        info.ppEnabledLayerNames = &validation;
        break;
      }
    }

    if (vkCreateInstance(&info, nullptr, &instance_) != VK_SUCCESS) {
      std::println("[renderer] vkCreateInstance failed");
      return false;
    }
    return true;
  }

  // Create Vulkan surface.
  // Return true on success, false on failure.
  bool CreateSurface() {
    VkResult result =
        glfwCreateWindowSurface(instance_, window_, nullptr, &surface_);
    if (result != VK_SUCCESS) {
      std::println("[renderer] glfwCreateWindowSurface failed: {}",
                   static_cast<int>(result));
      return false;
    }
    return true;
  }

  // Find graphics and present queue families for a physical device.
  // - `device` - Physical device to inspect.
  // Return queue family indices of the graphics and present queues.
  [[nodiscard]] QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device) {
    QueueFamilyIndices indices;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, queues.data());
    for (uint32_t i = 0; i < count; ++i) {
      if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
        indices.graphics = i;
      }
      VkBool32 present_support = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_,
                                           &present_support);
      if (present_support == VK_TRUE) {
        indices.present = i;
      }
      if (indices.IsComplete()) {
        break;
      }
    }
    return indices;
  }

  // Check that a physical device supports the required device extensions.
  // - `device` - Physical device to inspect.
  // Return true when `VK_KHR_swapchain` is available.
  [[nodiscard]] bool CheckDeviceExtensionSupport(VkPhysicalDevice device) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count,
                                         available.data());
    for (const auto &ext : available) {
      if (std::strcmp(ext.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) ==
          0) {
        return true;
      }
    }
    return false;
  }

  // Query swapchain support details for a physical device.
  // - `device` - Physical device to inspect.
  // Return swapchain support details.
  [[nodiscard]] SwapchainSupport
  QuerySwapchainSupport(VkPhysicalDevice device) {
    SwapchainSupport support;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface_,
                                              &support.capabilities);

    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &format_count,
                                         nullptr);
    if (format_count != 0) {
      support.formats.resize(format_count);
      vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &format_count,
                                           support.formats.data());
    }

    uint32_t mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &mode_count,
                                              nullptr);
    if (mode_count != 0) {
      support.present_modes.resize(mode_count);
      vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &mode_count,
                                                support.present_modes.data());
    }
    return support;
  }

  // Check that a physical device can present a swapchain to our surface.
  // - `device` - Physical device to inspect.
  // Return true when queues, extensions, and swapchain details are adequate.
  [[nodiscard]] bool IsDeviceSuitable(VkPhysicalDevice device) {
    if (!FindQueueFamilies(device).IsComplete()) {
      return false;
    }
    if (!CheckDeviceExtensionSupport(device)) {
      return false;
    }
    auto support = QuerySwapchainSupport(device);
    return !support.formats.empty() && !support.present_modes.empty();
  }

  // Pick Vulkan physical device.
  // Return true on success, false on failure.
  bool PickPhysicalDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) {
      std::println("[renderer] no Vulkan physical devices found");
      return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    for (auto dev : devices) {
      if (!IsDeviceSuitable(dev)) {
        continue;
      }
      auto indices = FindQueueFamilies(dev);
      physical_device_ = dev;
      graphics_queue_family_ = indices.graphics.value();
      present_queue_family_ = indices.present.value();

      VkPhysicalDeviceProperties props{};
      vkGetPhysicalDeviceProperties(dev, &props);
      std::println("[renderer] GPU: {}", props.deviceName);
      return true;
    }
    std::println("[renderer] no suitable GPU found");
    return false;
  }

  // Create logical device with graphics and present queues.
  // Return true on success, false on failure.
  bool CreateLogicalDevice() {
    std::set<uint32_t> unique_families = {graphics_queue_family_,
                                          present_queue_family_};

    float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    queue_infos.reserve(unique_families.size());
    for (auto family : unique_families) {
      VkDeviceQueueCreateInfo queue_info{};
      queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
      queue_info.queueFamilyIndex = family;
      queue_info.queueCount = 1;
      queue_info.pQueuePriorities = &priority;
      queue_infos.push_back(queue_info);
    }

    VkPhysicalDeviceFeatures features{};

    // Query support for shader `DrawParameters` and enable it.
    VkPhysicalDeviceVulkan11Features query11{};
    query11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &query11;
    vkGetPhysicalDeviceFeatures2(physical_device_, &features2);
    if (query11.shaderDrawParameters != VK_TRUE) {
      std::println("[renderer] GPU lacks shaderDrawParameters, "
                   "wall and floor shaders need it");
      return false;
    }
    VkPhysicalDeviceVulkan11Features enabled11{};
    enabled11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    enabled11.shaderDrawParameters = VK_TRUE;

    // `VK_KHR_swapchain` is required.
    std::vector<const char *> extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    {
      uint32_t count = 0;
      vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &count,
                                           nullptr);
      std::vector<VkExtensionProperties> available(count);
      vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &count,
                                           available.data());
      for (const auto &ext : available) {
        if (std::strcmp(ext.extensionName, "VK_KHR_portability_subset") == 0) {
          extensions.push_back("VK_KHR_portability_subset");
          break;
        }
      }
    }

    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = &enabled11;
    info.queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size());
    info.pQueueCreateInfos = queue_infos.data();
    info.pEnabledFeatures = &features;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();

    if (vkCreateDevice(physical_device_, &info, nullptr, &device_) !=
        VK_SUCCESS) {
      std::println("[renderer] vkCreateDevice failed");
      return false;
    }

    vkGetDeviceQueue(device_, graphics_queue_family_, 0, &graphics_queue_);
    vkGetDeviceQueue(device_, present_queue_family_, 0, &present_queue_);
    return true;
  }

  // Pick the swapchain surface format, preferring sRGB.
  // - `formats` - Formats supported by the surface.
  // Return the chosen surface format.
  [[nodiscard]] static VkSurfaceFormatKHR
  ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &formats) {
    for (const auto &format : formats) {
      if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
          format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        return format;
      }
    }
    return formats[0];
  }

  // Pick the swapchain present mode, preferring mailbox.
  // - `modes` - Present modes supported by the surface.
  // Return the chosen present mode.
  [[nodiscard]] static VkPresentModeKHR
  ChooseSwapPresentMode(const std::vector<VkPresentModeKHR> &modes) {
    for (auto mode : modes) {
      if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
        return mode;
      }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
  }

  // Pick the swapchain extent, clamping the window size to the capabilities.
  // - `capabilities` - Surface capabilities of the physical device.
  // Return the chosen swapchain extent.
  [[nodiscard]] VkExtent2D
  ChooseSwapExtent(const VkSurfaceCapabilitiesKHR &capabilities) {
    if (capabilities.currentExtent.width !=
        std::numeric_limits<uint32_t>::max()) {
      return capabilities.currentExtent;
    }
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window_, &width, &height);
    VkExtent2D extent{
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
    };
    extent.width = std::clamp(extent.width, capabilities.minImageExtent.width,
                              capabilities.maxImageExtent.width);
    extent.height =
        std::clamp(extent.height, capabilities.minImageExtent.height,
                   capabilities.maxImageExtent.height);
    return extent;
  }

  // Create swapchain and retrieve its images.
  // Return true on success, false on failure.
  bool CreateSwapchain() {
    auto support = QuerySwapchainSupport(physical_device_);
    auto surface_format = ChooseSwapSurfaceFormat(support.formats);
    auto present_mode = ChooseSwapPresentMode(support.present_modes);
    auto extent = ChooseSwapExtent(support.capabilities);

    uint32_t image_count = support.capabilities.minImageCount + 1;
    if (support.capabilities.maxImageCount != 0 &&
        image_count > support.capabilities.maxImageCount) {
      image_count = support.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface_;
    info.minImageCount = image_count;
    info.imageFormat = surface_format.format;
    info.imageColorSpace = surface_format.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    uint32_t families[] = {graphics_queue_family_, present_queue_family_};
    if (graphics_queue_family_ != present_queue_family_) {
      info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
      info.queueFamilyIndexCount = 2;
      info.pQueueFamilyIndices = families;
    } else {
      info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    info.preTransform = support.capabilities.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = present_mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = VK_NULL_HANDLE;

    if (vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_) !=
        VK_SUCCESS) {
      std::println("[renderer] vkCreateSwapchainKHR failed");
      return false;
    }

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
    swapchain_images_.resize(count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &count,
                            swapchain_images_.data());

    swapchain_image_format_ = surface_format.format;
    swapchain_extent_ = extent;
    return true;
  }

  // Create one image view per swapchain image.
  // Return true on success, false on failure.
  bool CreateImageViews() {
    swapchain_image_views_.resize(swapchain_images_.size());
    for (std::size_t i = 0; i < swapchain_images_.size(); ++i) {
      VkImageViewCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      info.image = swapchain_images_[i];
      info.viewType = VK_IMAGE_VIEW_TYPE_2D;
      info.format = swapchain_image_format_;
      info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
      info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
      info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
      info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
      info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      info.subresourceRange.baseMipLevel = 0;
      info.subresourceRange.levelCount = 1;
      info.subresourceRange.baseArrayLayer = 0;
      info.subresourceRange.layerCount = 1;

      if (vkCreateImageView(device_, &info, nullptr,
                            &swapchain_image_views_[i]) != VK_SUCCESS) {
        std::println("[renderer] vkCreateImageView failed for image {}", i);
        return false;
      }
    }
    return true;
  }

  // Find a memory type index satisfying the filter and properties.
  // - `filter` - Bitmask of suitable memory types.
  // - `properties` - Required memory property flags.
  // Return the memory type index, or `UINT32_MAX` when none matches.
  [[nodiscard]] uint32_t FindMemoryType(uint32_t filter,
                                        VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
      if ((filter & (1u << i)) != 0u &&
          (mem.memoryTypes[i].propertyFlags & properties) == properties) {
        return i;
      }
    }
    return std::numeric_limits<uint32_t>::max();
  }

  // Create a buffer and allocate bound memory.
  // - `size` - Buffer size in bytes, must be non-zero.
  // - `usage` - Buffer usage flags.
  // - `properties` - Required memory properties.
  // - `buffer`, `memory` - Created buffer and memory.
  // Return true on success, false on failure.
  bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags properties, VkBuffer &buffer,
                    VkDeviceMemory &memory) {
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device_, &buffer_info, nullptr, &buffer) != VK_SUCCESS) {
      std::println("[renderer] vkCreateBuffer failed");
      return false;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, buffer, &requirements);

    uint32_t index = FindMemoryType(requirements.memoryTypeBits, properties);
    if (index == std::numeric_limits<uint32_t>::max()) {
      std::println("[renderer] no suitable memory type for buffer");
      vkDestroyBuffer(device_, buffer, nullptr);
      buffer = VK_NULL_HANDLE;
      return false;
    }

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = requirements.size;
    alloc.memoryTypeIndex = index;

    if (vkAllocateMemory(device_, &alloc, nullptr, &memory) != VK_SUCCESS) {
      std::println("[renderer] vkAllocateMemory failed for buffer");
      vkDestroyBuffer(device_, buffer, nullptr);
      buffer = VK_NULL_HANDLE;
      return false;
    }

    vkBindBufferMemory(device_, buffer, memory, 0);
    return true;
  }

  // Destroy a tile buffer and its memory.
  // - `buffer`, `memory` - Buffer and memory to destroy.
  void DestroyTileBuffer(VkBuffer &buffer, VkDeviceMemory &memory) {
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
  void RecreateTileBuffer(VkBuffer &buffer, VkDeviceMemory &memory,
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

  // Read a whole binary file.
  // - `path` - File to read.
  // Return file bytes, empty on failure.
  [[nodiscard]] static std::vector<char>
  ReadFile(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
      return {};
    }
    auto size = file.tellg();
    std::vector<char> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    file.read(bytes.data(), size);
    return bytes;
  }

  // Resolve a shader file, trying the CMake SPIR-V directory first.
  // - `filename` - Shader file name.
  // Return the existing path, empty when not found.
  [[nodiscard]] static std::filesystem::path
  FindShaderFile(const char *filename) {
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
  [[nodiscard]] VkShaderModule
  CreateShaderModule(const std::vector<char> &code) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode = reinterpret_cast<const uint32_t *>(code.data());

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &info, nullptr, &module) != VK_SUCCESS) {
      std::println("[renderer] vkCreateShaderModule failed");
      return VK_NULL_HANDLE;
    }
    return module;
  }

  // Create the render pass with color and depth attachments.
  // Return true on success, false on failure.
  bool CreateRenderPass() {
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
  [[nodiscard]] VkFormat FindDepthFormat() {
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
  bool CreateDescriptorSetLayout() {
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
  bool CreateGraphicsPipelines() {
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

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push.offset = 0;
    push.size = sizeof(PushConstants);

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
    assembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
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

      return vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info,
                                       nullptr, &pipeline) == VK_SUCCESS;
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
  bool CreateDepthResources() {
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
  bool CreateFramebuffers() {
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

  // Create one host-visible uniform buffer per in-flight frame.
  // Return true on success, false on failure.
  bool CreateUniformBuffers() {
    uniform_buffers_.resize(max_frames_in_flight_, VK_NULL_HANDLE);
    uniform_memories_.resize(max_frames_in_flight_, VK_NULL_HANDLE);
    uniform_mapped_.resize(max_frames_in_flight_, nullptr);

    for (std::size_t i = 0; i < max_frames_in_flight_; ++i) {
      if (!CreateBuffer(sizeof(FrameUniforms),
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
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
  bool CreateTileBuffersEmpty() {
    std::vector<glm::vec2> empty;
    uint32_t dummy = 0;
    RecreateTileBuffer(wall_tile_buffer_, wall_tile_memory_, empty, dummy);
    wall_instance_count_ = 0;
    RecreateTileBuffer(floor_tile_buffer_, floor_tile_memory_, empty, dummy);
    floor_instance_count_ = 0;
    return wall_tile_buffer_ != VK_NULL_HANDLE &&
           floor_tile_buffer_ != VK_NULL_HANDLE;
  }

  // Create the descriptor pool for wall and floor sets.
  // Return true on success, false on failure.
  bool CreateDescriptorPool() {
    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[0].descriptorCount = static_cast<uint32_t>(max_frames_in_flight_ * 2);
    sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sizes[1].descriptorCount = static_cast<uint32_t>(max_frames_in_flight_ * 2);

    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets = static_cast<uint32_t>(max_frames_in_flight_ * 2);
    info.poolSizeCount = 2;
    info.pPoolSizes = sizes;

    if (vkCreateDescriptorPool(device_, &info, nullptr, &descriptor_pool_) !=
        VK_SUCCESS) {
      std::println("[renderer] vkCreateDescriptorPool failed");
      return false;
    }
    return true;
  }

  // Allocate wall and floor descriptor sets, one per in-flight frame.
  // Return true on success, false on failure.
  bool AllocateDescriptorSets() {
    wall_sets_.resize(max_frames_in_flight_);
    floor_sets_.resize(max_frames_in_flight_);

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
    return true;
  }

  // Point wall and floor descriptor sets at the current buffers.
  void UpdateDescriptors() {
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

      VkWriteDescriptorSet wall_writes[2]{};
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
      vkUpdateDescriptorSets(device_, 2, wall_writes, 0, nullptr);

      VkDescriptorBufferInfo floor_info{};
      floor_info.buffer = floor_tile_buffer_;
      floor_info.offset = 0;
      floor_info.range = floor_size;

      VkWriteDescriptorSet floor_writes[2]{};
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
      vkUpdateDescriptorSets(device_, 2, floor_writes, 0, nullptr);
    }
  }

  // Create the command pool for frame command buffers.
  // Return true on success, false on failure.
  bool CreateCommandPool() {
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
  bool CreateCommandBuffers() {
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
  bool CreateSyncObjects() {
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
      if (vkCreateSemaphore(device_, &sem_info, nullptr,
                            &image_available_[i]) != VK_SUCCESS ||
          vkCreateFence(device_, &fence_info, nullptr, &in_flight_[i]) !=
              VK_SUCCESS) {
        std::println("[renderer] failed to create per-frame sync objects");
        return false;
      }
    }
    for (std::size_t i = 0; i < swapchain_images_.size(); ++i) {
      if (vkCreateSemaphore(device_, &sem_info, nullptr,
                            &render_finished_[i]) != VK_SUCCESS) {
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
  void UpdateUniformBuffer(std::size_t frame, const Camera &camera,
                           float aspect) {
    FrameUniforms uniforms{};
    uniforms.view_projection =
        camera.ProjectionMatrix(aspect) * camera.ViewMatrix();
    uniforms.light_direction = glm::normalize(glm::vec3(-0.6f, -1.0f, -0.4f));
    uniforms.ambient = 0.35f;
    std::memcpy(uniform_mapped_[frame], &uniforms, sizeof(uniforms));
  }

  // Record wall and floor instanced draws into a command buffer.
  // - `cmd` - Command buffer to record.
  // - `image_index` - Swapchain image (framebuffer) index.
  // - `frame` - In-flight frame index selecting the descriptor sets.
  void RecordCommandBuffer(VkCommandBuffer cmd, uint32_t image_index,
                           std::size_t frame) {
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &begin);

    VkClearValue clear[2]{};
    clear[0].color = {{0.08f, 0.09f, 0.11f, 1.0f}};
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

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
  }
};

} // namespace flying_rat
