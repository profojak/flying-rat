/* Vulkan device. */

module;

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <print>
#include <set>
#include <vector>

module flying_rat.renderer;

import flying_rat.camera;
import flying_rat.config;
import flying_rat.maze;

namespace flying_rat {

// Create Vulkan instance.
// Return true on success, false on failure.
bool Renderer::CreateInstance() {
  VkApplicationInfo app_info{};
  app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app_info.pApplicationName = "flying-rat";
  app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
  app_info.pEngineName = "flying-rat";
  app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
  app_info.apiVersion = VK_API_VERSION_1_2;

  uint32_t glfw_count = 0;
  const char **glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_count);

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
bool Renderer::CreateSurface() {
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
[[nodiscard]] Renderer::QueueFamilyIndices
Renderer::FindQueueFamilies(VkPhysicalDevice device) {
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
    vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &present_support);
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
[[nodiscard]] bool
Renderer::CheckDeviceExtensionSupport(VkPhysicalDevice device) {
  uint32_t count = 0;
  vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> available(count);
  vkEnumerateDeviceExtensionProperties(device, nullptr, &count,
                                       available.data());
  for (const auto &ext : available) {
    if (std::strcmp(ext.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
      return true;
    }
  }
  return false;
}

// Query swapchain support details for a physical device.
// - `device` - Physical device to inspect.
// Return swapchain support details.
[[nodiscard]] Renderer::SwapchainSupport
Renderer::QuerySwapchainSupport(VkPhysicalDevice device) {
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
[[nodiscard]] bool Renderer::IsDeviceSuitable(VkPhysicalDevice device) {
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
bool Renderer::PickPhysicalDevice() {
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
bool Renderer::CreateLogicalDevice() {
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
[[nodiscard]] VkSurfaceFormatKHR Renderer::ChooseSwapSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR> &formats) {
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
[[nodiscard]] VkPresentModeKHR
Renderer::ChooseSwapPresentMode(const std::vector<VkPresentModeKHR> &modes) {
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
Renderer::ChooseSwapExtent(const VkSurfaceCapabilitiesKHR &capabilities) {
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
  extent.height = std::clamp(extent.height, capabilities.minImageExtent.height,
                             capabilities.maxImageExtent.height);
  return extent;
}

// Create swapchain and retrieve its images.
// Return true on success, false on failure.
bool Renderer::CreateSwapchain() {
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
bool Renderer::CreateImageViews() {
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
[[nodiscard]] uint32_t
Renderer::FindMemoryType(uint32_t filter, VkMemoryPropertyFlags properties) {
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
bool Renderer::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
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

} // namespace flying_rat
