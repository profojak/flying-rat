/* Vulkan renderer. */

module;

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <vector>

export module flying_rat.renderer;

import flying_rat.camera;
import flying_rat.config;
import flying_rat.maze;

namespace flying_rat {

// Minimap overlay request for one frame.
export struct MinimapArgs {
  // True when the minimap should be drawn.
  bool visible = false;
  // Player tile coordinates.
  glm::ivec2 player_cell{0, 0};
};

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
  // Minimap overlay pipeline.
  VkPipeline minimap_pipeline_ = VK_NULL_HANDLE;
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

  // Push constants for the minimap overlay.
  // NOTE: Must match `MinimapPush` in `data/minimap.slang`.
  struct MinimapPushConstants {
    // Top-left corner of the grid area in NDC.
    glm::vec2 origin{};
    // Size of one cell in NDC.
    glm::vec2 cell{};
    // Maze dimensions in tiles.
    glm::ivec2 grid{0, 0};
    // Player tile coordinates.
    glm::ivec2 player{0, 0};
    // Start tile coordinates.
    glm::ivec2 start{0, 0};
    // Exit tile coordinates.
    glm::ivec2 exit{0, 0};
  };

  static_assert(sizeof(MinimapPushConstants) == 48);
  static_assert(offsetof(MinimapPushConstants, grid) == 16);
  static_assert(offsetof(MinimapPushConstants, start) == 32);
  static_assert(offsetof(MinimapPushConstants, exit) == 40);

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
  // Minimap cell type buffer, one uint per maze tile, row-major.
  VkBuffer minimap_buffer_ = VK_NULL_HANDLE;
  // Minimap buffer memory.
  VkDeviceMemory minimap_memory_ = VK_NULL_HANDLE;
  // Minimap grid dimensions in tiles, cached from `BuildMaze`.
  glm::ivec2 minimap_grid_{0, 0};
  // Minimap start tile, cached from `BuildMaze`.
  glm::ivec2 minimap_start_{0, 0};
  // Minimap exit tile, cached from `BuildMaze`.
  glm::ivec2 minimap_exit_{0, 0};

  // Descriptor pool holding wall and floor sets for all frames.
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  // Wall descriptor sets, one per in-flight frame.
  std::vector<VkDescriptorSet> wall_sets_;
  // Floor descriptor sets, one per in-flight frame.
  std::vector<VkDescriptorSet> floor_sets_;
  // Minimap descriptor sets, one per in-flight frame.
  std::vector<VkDescriptorSet> minimap_sets_;

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
  bool Initialize(GLFWwindow *window);

  // Build tile instance buffers from the maze.
  // - `maze` - Generated maze to render.
  // NOTE: Walls get one instance per `Tile::Wall`, floors get one instance per
  // `Tile::Empty`.
  void BuildMaze(const Maze &maze);

  // Record and submit one frame with instanced wall and floor draws,
  // plus the minimap overlay when requested.
  // - `camera` - Camera used for the view matrix.
  // - `aspect` - Aspect ratio of the viewport.
  // - `minimap` - Minimap overlay request for this frame.
  void Draw(const Camera &camera, float aspect, const MinimapArgs &minimap);

  // Block until the GPU is idle.
  void WaitIdle();

  // Destroy Vulkan resources.
  void Destroy();

private:
  // Create Vulkan instance.
  // Return true on success, false on failure.
  bool CreateInstance();

  // Create Vulkan surface.
  // Return true on success, false on failure.
  bool CreateSurface();

  // Find graphics and present queue families for a physical device.
  // - `device` - Physical device to inspect.
  // Return queue family indices of the graphics and present queues.
  [[nodiscard]] QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device);

  // Check that a physical device supports the required device extensions.
  // - `device` - Physical device to inspect.
  // Return true when `VK_KHR_swapchain` is available.
  [[nodiscard]] bool CheckDeviceExtensionSupport(VkPhysicalDevice device);

  // Query swapchain support details for a physical device.
  // - `device` - Physical device to inspect.
  // Return swapchain support details.
  [[nodiscard]] SwapchainSupport QuerySwapchainSupport(VkPhysicalDevice device);

  // Check that a physical device can present a swapchain to our surface.
  // - `device` - Physical device to inspect.
  // Return true when queues, extensions, and swapchain details are adequate.
  [[nodiscard]] bool IsDeviceSuitable(VkPhysicalDevice device);

  // Pick Vulkan physical device.
  // Return true on success, false on failure.
  bool PickPhysicalDevice();

  // Create logical device with graphics and present queues.
  // Return true on success, false on failure.
  bool CreateLogicalDevice();

  // Pick the swapchain surface format, preferring sRGB.
  // - `formats` - Formats supported by the surface.
  // Return the chosen surface format.
  [[nodiscard]] static VkSurfaceFormatKHR
  ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &formats);

  // Pick the swapchain present mode, preferring mailbox.
  // - `modes` - Present modes supported by the surface.
  // Return the chosen present mode.
  [[nodiscard]] static VkPresentModeKHR
  ChooseSwapPresentMode(const std::vector<VkPresentModeKHR> &modes);

  // Pick the swapchain extent, clamping the window size to the capabilities.
  // - `capabilities` - Surface capabilities of the physical device.
  // Return the chosen swapchain extent.
  [[nodiscard]] VkExtent2D
  ChooseSwapExtent(const VkSurfaceCapabilitiesKHR &capabilities);

  // Create swapchain and retrieve its images.
  // Return true on success, false on failure.
  bool CreateSwapchain();

  // Create one image view per swapchain image.
  // Return true on success, false on failure.
  bool CreateImageViews();

  // Find a memory type index satisfying the filter and properties.
  // - `filter` - Bitmask of suitable memory types.
  // - `properties` - Required memory property flags.
  // Return the memory type index, or `UINT32_MAX` when none matches.
  [[nodiscard]] uint32_t FindMemoryType(uint32_t filter,
                                        VkMemoryPropertyFlags properties);

  // Create a buffer and allocate bound memory.
  // - `size` - Buffer size in bytes, must be non-zero.
  // - `usage` - Buffer usage flags.
  // - `properties` - Required memory properties.
  // - `buffer`, `memory` - Created buffer and memory.
  // Return true on success, false on failure.
  bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags properties, VkBuffer &buffer,
                    VkDeviceMemory &memory);

  // Destroy a tile buffer and its memory.
  // - `buffer`, `memory` - Buffer and memory to destroy.
  void DestroyTileBuffer(VkBuffer &buffer, VkDeviceMemory &memory);

  // Create a tile SSBO from CPU tile coordinates.
  // - `buffer`, `memory` - Buffer and memory to recreate.
  // - `tiles` - Tile coordinates.
  // - `out_count` - Receives the instance count for `vkCmdDraw`.
  void RecreateTileBuffer(VkBuffer &buffer, VkDeviceMemory &memory,
                          const std::vector<glm::vec2> &tiles,
                          uint32_t &out_count);

  // Read a whole SPIR-V file.
  // - `path` - File to read.
  // Return file words (4-byte aligned for `vkCreateShaderModule`), empty on
  // failure or when the size is not a multiple of 4.
  [[nodiscard]] static std::vector<std::uint32_t>
  ReadFile(const std::filesystem::path &path);

  // Resolve a shader file, trying the CMake SPIR-V directory first.
  // - `filename` - Shader file name.
  // Return the existing path, empty when not found.
  [[nodiscard]] static std::filesystem::path
  FindShaderFile(const char *filename);

  // Create a shader module from SPIR-V code.
  // - `code` - SPIR-V words, must be 4-byte aligned and non-empty.
  // Return the module, or `VK_NULL_HANDLE` on failure.
  [[nodiscard]] VkShaderModule
  CreateShaderModule(const std::vector<std::uint32_t> &code);

  // Create the render pass with color and depth attachments.
  // Return true on success, false on failure.
  bool CreateRenderPass();

  // Find a supported depth format, preferring 32-bit float.
  // Return the depth format.
  [[nodiscard]] VkFormat FindDepthFormat();

  // Create the descriptor set layout shared by both pipelines.
  // Return true on success, false on failure.
  bool CreateDescriptorSetLayout();

  // Create wall and floor graphics pipelines with push constants.
  // Return true on success, false on failure.
  bool CreateGraphicsPipelines();

  // Create the minimap overlay pipeline.
  // Return true on success, false on failure.
  bool CreateMinimapPipeline();

  // Build the minimap cell buffer from the maze.
  // - `maze` - Generated maze to render.
  void BuildMinimapBuffer(const Maze &maze);

  // Destroy the minimap cell buffer and its memory.
  void DestroyMinimapBuffer();

  // Point minimap descriptor sets at the current cell buffer.
  void UpdateMinimapDescriptors();

  // Compute minimap push constants for one frame.
  // - `minimap` - Minimap overlay request for this frame.
  // Return push constants positioning a centered overlay.
  [[nodiscard]] MinimapPushConstants
  PushForMinimap(const MinimapArgs &minimap) const;

  // Record the minimap overlay draw into a command buffer.
  // - `cmd` - Command buffer to record.
  // - `frame` - In-flight frame index selecting the descriptor set.
  // - `minimap` - Minimap overlay request for this frame.
  void RecordMinimap(VkCommandBuffer cmd, std::size_t frame,
                     const MinimapArgs &minimap);

  // Create the depth image and view matching the swapchain extent.
  // Return true on success, false on failure.
  bool CreateDepthResources();

  // Create one framebuffer per swapchain image.
  // Return true on success, false on failure.
  bool CreateFramebuffers();

  // Create one host-visible uniform buffer per in-flight frame.
  // Return true on success, false on failure.
  bool CreateUniformBuffers();

  // Create placeholder tile buffers so descriptors are valid before
  // the first `BuildMaze` call.
  // Return true on success, false on failure.
  bool CreateTileBuffersEmpty();

  // Create the descriptor pool for wall and floor sets.
  // Return true on success, false on failure.
  bool CreateDescriptorPool();

  // Allocate wall and floor descriptor sets, one per in-flight frame.
  // Return true on success, false on failure.
  bool AllocateDescriptorSets();

  // Point wall and floor descriptor sets at the current buffers.
  void UpdateDescriptors();

  // Create the command pool for frame command buffers.
  // Return true on success, false on failure.
  bool CreateCommandPool();

  // Allocate one primary command buffer per in-flight frame.
  // Return true on success, false on failure.
  bool CreateCommandBuffers();

  // Create semaphores and fences.
  // Return true on success, false on failure.
  bool CreateSyncObjects();

  // Upload view-projection, light, and ambient for one frame.
  // - `frame` - In-flight frame index.
  // - `camera` - Camera used for the view matrix.
  // - `aspect` - Aspect ratio of the viewport.
  void UpdateUniformBuffer(std::size_t frame, const Camera &camera,
                           float aspect);

  // Record wall, floor, and minimap draws into a command buffer.
  // - `cmd` - Command buffer to record.
  // - `image_index` - Swapchain image (framebuffer) index.
  // - `frame` - In-flight frame index selecting the descriptor sets.
  // - `minimap` - Minimap overlay request for this frame.
  void RecordCommandBuffer(VkCommandBuffer cmd, uint32_t image_index,
                           std::size_t frame, const MinimapArgs &minimap);
};

} // namespace flying_rat
