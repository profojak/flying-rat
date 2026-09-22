/* Application. */

module;

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <chrono>
#include <print>

export module flying_rat.app;

import flying_rat.config;

namespace flying_rat {

// Application class opens a window and manages the game loop.
// NOTE: No other code deals with GLFW window management directly.
export class App {
private:
  // GLFW window handle.
  GLFWwindow *window_ = nullptr;

public:
  App() = default;
  App(const App &) = delete;
  App &operator=(const App &) = delete;
  ~App() { Destroy(); }

  // Run the application.
  // Return 0 on success, 1 on failure.
  int Run() {
    if (!InitializeWindow()) {
      return 1;
    }

    Loop();

    Destroy();
    return 0;
  }

private:
  // Create GLFW window and configure its behavior.
  // Return true on success, false on failure.
  bool InitializeWindow() {
    if (glfwInit() == GLFW_FALSE) {
      std::println("[app] glfwInit failed");
      return false;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    window_ = glfwCreateWindow(1280, 720, "Procedural Maze Explorer", nullptr,
                               nullptr);
    if (window_ == nullptr) {
      std::println("[app] glfwCreateWindow failed");
      glfwTerminate();
      return false;
    }

    glfwSetWindowUserPointer(window_, this);
    glfwSetCursorPosCallback(window_, &App::OnCursorPosition);
    glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    return true;
  }

  // Main game loop.
  void Loop() {
    using Clock = std::chrono::steady_clock;
    auto then = Clock::now();

    while (glfwWindowShouldClose(window_) == GLFW_FALSE) {
      glfwPollEvents();
      if (glfwGetKey(window_, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
      }

      auto now = Clock::now();
      float delta = std::chrono::duration<float>(now - then).count();
      then = now;
      if (delta > 0.1f) {
        delta = 0.1f;
      }
    }
  }

  // Shut down the application and clean up resources.
  void Destroy() {
    if (window_ != nullptr) {
      glfwDestroyWindow(window_);
      window_ = nullptr;
    }
    glfwTerminate();
  }

  // Callback for cursor position events.
  // - `window` - Window that received the event.
  // - `x`, `y` - Cursor position in window coordinates.
  static void OnCursorPosition(GLFWwindow *window, double x, double y) {
    auto *self = static_cast<App *>(glfwGetWindowUserPointer(window));
    if (self == nullptr) {
      return;
    }
  }
};

} // namespace flying_rat
