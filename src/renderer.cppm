/* Vulkan renderer. */

module;

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <print>

export module flying_rat.renderer;

import flying_rat.camera;
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
  // Vulkan graphics queue family index.
  uint32_t graphics_queue_family_ = 0;

public:
  Renderer() = default;
  Renderer(const Renderer &) = delete;
  Renderer &operator=(const Renderer &) = delete;
  ~Renderer() { Destroy(); }

  // Create `VkInstance`, pick a physical device, create the surface.
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

    return true;
  }

  // Destroy Vulkan resources.
  void Destroy() {
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
      uint32_t queue_count = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(dev, &queue_count, nullptr);
      std::vector<VkQueueFamilyProperties> queues(queue_count);
      vkGetPhysicalDeviceQueueFamilyProperties(dev, &queue_count,
                                               queues.data());
      for (uint32_t i = 0; i < queue_count; ++i) {
        if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
          physical_device_ = dev;
          graphics_queue_family_ = i;

          VkPhysicalDeviceProperties props{};
          vkGetPhysicalDeviceProperties(dev, &props);
          std::println("[renderer] GPU: {}", props.deviceName);
          return true;
        }
      }
    }
    std::println("[renderer] no graphics-capable queue found");
    return false;
  }
};

} // namespace flying_rat
