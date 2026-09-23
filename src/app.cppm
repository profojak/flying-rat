/* Application. */

module;

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#include <chrono>
#include <print>
#include <random>

export module flying_rat.app;

import flying_rat.camera;
import flying_rat.config;
import flying_rat.maze;
import flying_rat.player;
import flying_rat.renderer;

namespace flying_rat {

// Application class opens a window and manages the game loop.
// NOTE: No other code deals with GLFW window management directly.
export class App {
private:
  // GLFW window handle.
  GLFWwindow *window_ = nullptr;
  // Maze.
  Maze maze_;
  // Player.
  Player player_;
  // Renderer.
  Renderer renderer_;

  // Last cursor position.
  glm::vec2 last_mouse_position_ = {0.0, 0.0};
  // Accumulated mouse deltas, consumed once per frame.
  float pending_yaw_ = 0.0f;
  float pending_pitch_ = 0.0f;
  // True until the first mouse event after capture to avoid a view jump.
  bool first_mouse_ = true;
  // True after announcing exit once to avoid log spam.
  bool exit_announced_ = false;

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

    if (!renderer_.Initialize(window_)) {
      Destroy();
      return 1;
    }

    auto rd = std::random_device{};
    std::mt19937 rng{rd()};
    maze_.Resize(config::maze_width, config::maze_height);
    maze_.Generate(rng);
    renderer_.BuildMaze(maze_);
    player_.Spawn(maze_);

    Loop();

    renderer_.WaitIdle();

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

  // Return current input state.
  [[nodiscard]] MoveInput CollectInput() const {
    MoveInput input;
    if (glfwGetKey(window_, GLFW_KEY_W) == GLFW_PRESS ||
        glfwGetKey(window_, GLFW_KEY_UP) == GLFW_PRESS) {
      input.forward_axis += 1.0f;
    }
    if (glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS ||
        glfwGetKey(window_, GLFW_KEY_DOWN) == GLFW_PRESS) {
      input.forward_axis -= 1.0f;
    }
    if (glfwGetKey(window_, GLFW_KEY_D) == GLFW_PRESS ||
        glfwGetKey(window_, GLFW_KEY_RIGHT) == GLFW_PRESS) {
      input.strafe_axis += 1.0f;
    }
    if (glfwGetKey(window_, GLFW_KEY_A) == GLFW_PRESS ||
        glfwGetKey(window_, GLFW_KEY_LEFT) == GLFW_PRESS) {
      input.strafe_axis -= 1.0f;
    }
    return input;
  }

  // Main game loop: poll input, move player, draw from player camera.
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

      MoveInput input = CollectInput();
      input.yaw_delta = -pending_yaw_;
      input.pitch_delta = -pending_pitch_;
      pending_yaw_ = 0.0f;
      pending_pitch_ = 0.0f;
      player_.Update(delta, input, maze_);

      int width = 0;
      int height = 0;
      glfwGetFramebufferSize(window_, &width, &height);
      float aspect =
          (height > 0) ? static_cast<float>(width) / static_cast<float>(height)
                       : 1.0f;
      renderer_.Draw(player_.GetCamera(), aspect);

      if (player_.AtExit(maze_)) {
        if (!exit_announced_) {
          std::println("[app] reached the exit!");
          exit_announced_ = true;
        }
      } else {
        exit_announced_ = false;
      }
    }
  }

  // Shut down the application and clean up resources.
  void Destroy() {
    renderer_.Destroy();
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

    // First event after capture would otherwise cause a view jump.
    if (self->first_mouse_) {
      self->last_mouse_position_ = {x, y};
      self->first_mouse_ = false;
      return;
    }
    auto dx = static_cast<float>(x - self->last_mouse_position_.x);
    auto dy = static_cast<float>(y - self->last_mouse_position_.y);
    self->last_mouse_position_ = {x, y};
    self->pending_yaw_ += dx * config::mouse_sensitivity;
    self->pending_pitch_ += dy * config::mouse_sensitivity;
  }
};

} // namespace flying_rat
