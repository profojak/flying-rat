/* Player. */

module;

#include <glm/glm.hpp>

#include <numbers>

export module flying_rat.player;

import flying_rat.camera;
import flying_rat.config;
import flying_rat.maze;

namespace flying_rat {

// Move input from the player.
export struct MoveInput {
  // Forward/backward axis.
  float forward_axis = 0.0f;
  // Strafe axis.
  float strafe_axis = 0.0f;
  // Yaw delta from mouse.
  float yaw_delta = 0.0f;
  // Pitch delta from mouse.
  float pitch_delta = 0.0f;
};

// Player with a first-person camera.
export class Player {
private:
  // Camera.
  Camera camera_;

public:
  Player() = default;

  // Place the player at maze start, eye height above the floor, facing open
  // corridor.
  // - `maze` - Generated maze to spawn into.
  void Spawn(const Maze &maze) {
    glm::ivec2 start = maze.Start();
    camera_.position =
        glm::vec3((static_cast<float>(start.x) + 0.5f) * config::tile_size,
                  config::player_eye_height,
                  (static_cast<float>(start.y) + 0.5f) * config::tile_size);
    if (!maze.IsWall(start.x + 1, start.y)) {
      camera_.yaw = -std::numbers::pi_v<float> / 2.0f;
    } else {
      camera_.yaw = std::numbers::pi_v<float>;
    }
    camera_.pitch = 0.0f;
  }

  // Update the player state for a single frame.
  // - `delta` - Seconds since last frame, clamped by the caller.
  // - `input` - Move and look intent for this frame.
  // - `maze` - Maze to collide against.
  void Update(float delta, const MoveInput &input, const Maze &maze) {
    camera_.Rotate(input.yaw_delta, input.pitch_delta);

    glm::vec3 wish = ForwardOnPlane() * input.forward_axis +
                     camera_.Right() * input.strafe_axis;
    if (glm::dot(wish, wish) > 1.0f) {
      wish = glm::normalize(wish);
    }
    glm::vec3 step = wish * (config::move_speed * delta);

    // Axis-separated so the player slides along walls instead of sticking
    // on corners.
    TryMove(glm::vec3(step.x, 0.0f, 0.0f), maze);
    TryMove(glm::vec3(0.0f, 0.0f, step.z), maze);
  }

  // Return the camera for rendering.
  [[nodiscard]] const ::flying_rat::Camera &GetCamera() const noexcept {
    return camera_;
  }

  // Return the eye position in world space.
  [[nodiscard]] glm::vec3 Position() const noexcept { return camera_.position; }

  // - `maze` - Maze holding the exit tile.
  // Return true when the eye is inside the exit cell.
  [[nodiscard]] bool AtExit(const Maze &maze) const noexcept {
    int cx =
        static_cast<int>(std::floor(camera_.position.x / config::tile_size));
    int cz =
        static_cast<int>(std::floor(camera_.position.z / config::tile_size));
    glm::ivec2 exit = maze.Exit();
    return cx == exit.x && cz == exit.y;
  }

private:
  // Return normalized camera forward direction projected onto XZ plane.
  [[nodiscard]] glm::vec3 ForwardOnPlane() const noexcept {
    glm::vec3 forward = camera_.Forward();
    forward.y = 0.0f;
    if (glm::dot(forward, forward) < 1e-8f) {
      return glm::vec3(0.0f, 0.0f, -1.0f);
    }
    return glm::normalize(forward);
  }

  // Perform a circle to wall-cell collision test at a candidate position.
  // - `pos` - Candidate eye position.
  // - `maze` - Maze to collide against.
  // Return true if the candidate position collides with a wall cell.
  [[nodiscard]] bool CollidesAt(const glm::vec3 &pos,
                                const Maze &maze) const noexcept {
    float radius = config::player_radius;
    int min_x =
        static_cast<int>(std::floor((pos.x - radius) / config::tile_size));
    int max_x =
        static_cast<int>(std::floor((pos.x + radius) / config::tile_size));
    int min_z =
        static_cast<int>(std::floor((pos.z - radius) / config::tile_size));
    int max_z =
        static_cast<int>(std::floor((pos.z + radius) / config::tile_size));

    for (int cz = min_z; cz <= max_z; ++cz) {
      for (int cx = min_x; cx <= max_x; ++cx) {
        if (!maze.IsWall(cx, cz)) {
          continue;
        }
        float cell_min_x = static_cast<float>(cx) * config::tile_size;
        float cell_max_x = cell_min_x + config::tile_size;
        float cell_min_z = static_cast<float>(cz) * config::tile_size;
        float cell_max_z = cell_min_z + config::tile_size;
        float nearest_x = glm::clamp(pos.x, cell_min_x, cell_max_x);
        float nearest_z = glm::clamp(pos.z, cell_min_z, cell_max_z);
        float dx = pos.x - nearest_x;
        float dz = pos.z - nearest_z;
        if (dx * dx + dz * dz < radius * radius) {
          return true;
        }
      }
    }
    return false;
  }

  // Move when the candidate position is free.
  // - `step` - Axis-separated displacement.
  // - `maze` - Maze to collide against.
  void TryMove(const glm::vec3 &step, const Maze &maze) noexcept {
    glm::vec3 candidate = camera_.position + step;
    if (!CollidesAt(candidate, maze)) {
      camera_.position = candidate;
    }
  }
};

} // namespace flying_rat
