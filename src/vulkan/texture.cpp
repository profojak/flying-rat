/* Vulkan texture. */

module;

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#define STB_IMAGE_IMPLEMENTATION
#include "../vendor/stb_image.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
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

// Resolve the texture file.
// - `filename` - Texture file name.
// Return the existing path, empty when not found.
std::filesystem::path Renderer::FindTextureFile(const char *filename) {
  std::vector<std::filesystem::path> candidates;
  candidates.emplace_back(std::filesystem::path(SHADER_DIR) / filename);
  candidates.emplace_back(std::filesystem::path("build") / filename);
  candidates.emplace_back(std::filesystem::path(filename));
  for (const auto &p : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) {
      return p;
    }
  }
  return {};
}

// Create an image and allocate bound memory.
// - `width`, `height` - Image extent in texels.
// - `format` - Image format, normally sRGB RGBA8 for the wall texture.
// - `tiling` - Image tiling, normally optimal.
// - `usage` - Image usage flags.
// - `properties` - Required memory properties.
// - `image`, `memory` - Created image and memory.
// Return true on success, false on failure.
bool Renderer::CreateImage(uint32_t width, uint32_t height, VkFormat format,
                           VkImageTiling tiling, VkImageUsageFlags usage,
                           VkMemoryPropertyFlags properties, VkImage &image,
                           VkDeviceMemory &memory) {
  image = VK_NULL_HANDLE;
  memory = VK_NULL_HANDLE;

  VkImageCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.extent.width = width;
  info.extent.height = height;
  info.extent.depth = 1;
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.format = format;
  info.tiling = tiling;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.samples = VK_SAMPLE_COUNT_1_BIT;

  if (vkCreateImage(device_, &info, nullptr, &image) != VK_SUCCESS) {
    std::println("[renderer] failed to create wall texture image");
    return false;
  }

  VkMemoryRequirements requirements{};
  vkGetImageMemoryRequirements(device_, image, &requirements);
  uint32_t index = FindMemoryType(requirements.memoryTypeBits, properties);
  if (index == std::numeric_limits<uint32_t>::max()) {
    std::println("[renderer] no suitable memory type for wall texture");
    vkDestroyImage(device_, image, nullptr);
    image = VK_NULL_HANDLE;
    return false;
  }

  VkMemoryAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize = requirements.size;
  alloc.memoryTypeIndex = index;
  if (vkAllocateMemory(device_, &alloc, nullptr, &memory) != VK_SUCCESS) {
    std::println("[renderer] failed to allocate wall texture memory");
    vkDestroyImage(device_, image, nullptr);
    image = VK_NULL_HANDLE;
    return false;
  }
  vkBindImageMemory(device_, image, memory, 0);
  return true;
}

// Begin one-time commands on a transient pool.
// - `pool` - Receives the transient command pool.
// Return the command buffer, or `VK_NULL_HANDLE` on failure.
VkCommandBuffer Renderer::BeginSingleTimeCommands(VkCommandPool &pool) {
  pool = VK_NULL_HANDLE;
  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pool_info.queueFamilyIndex = graphics_queue_family_;
  if (vkCreateCommandPool(device_, &pool_info, nullptr, &pool) != VK_SUCCESS) {
    std::println("[renderer] failed to create transient command pool");
    return VK_NULL_HANDLE;
  }

  VkCommandBufferAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc.commandPool = pool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;

  VkCommandBuffer cmd = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(device_, &alloc, &cmd) != VK_SUCCESS) {
    std::println("[renderer] failed to allocate transient command buffer");
    vkDestroyCommandPool(device_, pool, nullptr);
    pool = VK_NULL_HANDLE;
    return VK_NULL_HANDLE;
  }

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) {
    std::println("[renderer] failed to begin transient command buffer");
    vkDestroyCommandPool(device_, pool, nullptr);
    pool = VK_NULL_HANDLE;
    return VK_NULL_HANDLE;
  }
  return cmd;
}

// Submit one-time commands and destroy the transient pool.
// - `cmd` - Command buffer from `BeginSingleTimeCommands`.
// - `pool` - Transient command pool to submit on and destroy.
void Renderer::EndSingleTimeCommands(VkCommandBuffer cmd, VkCommandPool pool) {
  vkEndCommandBuffer(cmd);
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  vkQueueSubmit(graphics_queue_, 1, &submit, VK_NULL_HANDLE);
  vkQueueWaitIdle(graphics_queue_);
  vkDestroyCommandPool(device_, pool, nullptr);
}

// Transition an image layout with a one-time command buffer.
// - `image` - Image to transition.
// - `old_layout`, `new_layout` - Layouts to transition between.
void Renderer::TransitionImageLayout(VkImage image, VkImageLayout old_layout,
                                     VkImageLayout new_layout) {
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = BeginSingleTimeCommands(pool);
  if (cmd == VK_NULL_HANDLE) {
    return;
  }

  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;

  VkPipelineStageFlags src_stage = 0;
  VkPipelineStageFlags dst_stage = 0;
  if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
      new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
             new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  } else {
    std::println("[renderer] unsupported wall texture layout transition");
    vkDestroyCommandPool(device_, pool, nullptr);
    return;
  }

  vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1,
                       &barrier);
  EndSingleTimeCommands(cmd, pool);
}

// Copy a staging buffer into an image.
// - `buffer` - Staging buffer holding tightly packed RGBA8 texels.
// - `image` - Destination image in transfer-dst-optimal layout.
// - `width`, `height` - Image extent in texels.
void Renderer::CopyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width,
                                 uint32_t height) {
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = BeginSingleTimeCommands(pool);
  if (cmd == VK_NULL_HANDLE) {
    return;
  }

  VkBufferImageCopy region{};
  region.bufferOffset = 0;
  region.bufferRowLength = 0;
  region.bufferImageHeight = 0;
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.mipLevel = 0;
  region.imageSubresource.baseArrayLayer = 0;
  region.imageSubresource.layerCount = 1;
  region.imageOffset = {0, 0, 0};
  region.imageExtent = {width, height, 1};

  vkCmdCopyBufferToImage(cmd, buffer, image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  EndSingleTimeCommands(cmd, pool);
}

// Load `data/wall.jpg` into a sampled image with view and sampler.
// Return true on success, false on failure.
bool Renderer::CreateWallTexture() {
  DestroyWallTexture();

  auto path = FindTextureFile("wall.jpg");
  if (path.empty()) {
    std::println("[renderer] wall texture 'wall.jpg' not found");
    return false;
  }

  int tex_width = 0;
  int tex_height = 0;
  stbi_uc *pixels =
      stbi_load(path.c_str(), &tex_width, &tex_height, nullptr, STBI_rgb_alpha);
  if (pixels == nullptr || tex_width <= 0 || tex_height <= 0) {
    std::println("[renderer] failed to load wall texture '{}': {}",
                 path.string(),
                 stbi_failure_reason() != nullptr ? stbi_failure_reason()
                                                  : "unknown error");
    return false;
  }

  const auto width = static_cast<uint32_t>(tex_width);
  const auto height = static_cast<uint32_t>(tex_height);
  const VkDeviceSize image_size =
      static_cast<VkDeviceSize>(width) * height * 4u;

  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory staging_memory = VK_NULL_HANDLE;
  if (!CreateBuffer(image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    staging, staging_memory)) {
    stbi_image_free(pixels);
    return false;
  }
  void *mapped = nullptr;
  vkMapMemory(device_, staging_memory, 0, image_size, 0, &mapped);
  std::memcpy(mapped, pixels, static_cast<std::size_t>(image_size));
  vkUnmapMemory(device_, staging_memory);
  stbi_image_free(pixels);

  constexpr VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;
  if (!CreateImage(width, height, format, VK_IMAGE_TILING_OPTIMAL,
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, wall_image_,
                   wall_image_memory_)) {
    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, staging_memory, nullptr);
    return false;
  }

  TransitionImageLayout(wall_image_, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  CopyBufferToImage(staging, wall_image_, width, height);
  TransitionImageLayout(wall_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  vkDestroyBuffer(device_, staging, nullptr);
  vkFreeMemory(device_, staging_memory, nullptr);

  VkImageViewCreateInfo view_info{};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = wall_image_;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = format;
  view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  view_info.subresourceRange.baseMipLevel = 0;
  view_info.subresourceRange.levelCount = 1;
  view_info.subresourceRange.baseArrayLayer = 0;
  view_info.subresourceRange.layerCount = 1;
  if (vkCreateImageView(device_, &view_info, nullptr, &wall_image_view_) !=
      VK_SUCCESS) {
    std::println("[renderer] failed to create wall texture view");
    DestroyWallTexture();
    return false;
  }

  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(physical_device_, &props);
  VkPhysicalDeviceFeatures supported{};
  vkGetPhysicalDeviceFeatures(physical_device_, &supported);
  const bool anisotropy = supported.samplerAnisotropy == VK_TRUE;

  VkSamplerCreateInfo sampler_info{};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.anisotropyEnable = anisotropy ? VK_TRUE : VK_FALSE;
  sampler_info.maxAnisotropy =
      anisotropy ? std::min(8.0f, props.limits.maxSamplerAnisotropy) : 1.0f;
  sampler_info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
  sampler_info.unnormalizedCoordinates = VK_FALSE;
  sampler_info.compareEnable = VK_FALSE;
  sampler_info.compareOp = VK_COMPARE_OP_ALWAYS;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sampler_info.mipLodBias = 0.0f;
  sampler_info.minLod = 0.0f;
  sampler_info.maxLod = 0.0f;
  if (vkCreateSampler(device_, &sampler_info, nullptr, &wall_sampler_) !=
      VK_SUCCESS) {
    std::println("[renderer] failed to create wall texture sampler");
    DestroyWallTexture();
    return false;
  }

  std::println("[renderer] wall texture '{}' ({}x{})", path.string(), width,
               height);
  return true;
}

// Destroy the wall texture image, view, and sampler.
void Renderer::DestroyWallTexture() {
  if (wall_sampler_ != VK_NULL_HANDLE) {
    if (device_ != VK_NULL_HANDLE) {
      vkDestroySampler(device_, wall_sampler_, nullptr);
    }
    wall_sampler_ = VK_NULL_HANDLE;
  }
  if (wall_image_view_ != VK_NULL_HANDLE) {
    if (device_ != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, wall_image_view_, nullptr);
    }
    wall_image_view_ = VK_NULL_HANDLE;
  }
  if (wall_image_ != VK_NULL_HANDLE) {
    if (device_ != VK_NULL_HANDLE) {
      vkDestroyImage(device_, wall_image_, nullptr);
    }
    wall_image_ = VK_NULL_HANDLE;
  }
  if (wall_image_memory_ != VK_NULL_HANDLE) {
    if (device_ != VK_NULL_HANDLE) {
      vkFreeMemory(device_, wall_image_memory_, nullptr);
    }
    wall_image_memory_ = VK_NULL_HANDLE;
  }
}

} // namespace flying_rat
