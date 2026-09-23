/* Camera. */

module;

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

export module flying_rat.camera;

import flying_rat.config;

export namespace flying_rat {

// First-person camera.  Use right-handed Y-up coordinates, matching GLM and
// Vulkan conventions.
struct Camera {
  // Position in world space.
  glm::vec3 position{0.0f};
  // Rotation around Y axis, 0 faces -Z.
  float yaw = 0.0f;
  // Rotation around X axis, clamped to +/- 90 degrees.
  float pitch = 0.0f;

  // Return the forward direction of the camera.
  [[nodiscard]] glm::vec3 Forward() const noexcept {
    const float yaw_cos = glm::cos(yaw);
    const float yaw_sin = glm::sin(yaw);
    const float pitch_cos = glm::cos(pitch);
    const float pitch_sin = glm::sin(pitch);
    return glm::vec3(-yaw_sin * pitch_cos, pitch_sin, -yaw_cos * pitch_cos);
  }

  // Return the right direction of the camera.
  [[nodiscard]] glm::vec3 Right() const noexcept {
    const float yaw_cos = glm::cos(yaw);
    const float yaw_sin = glm::sin(yaw);
    return glm::vec3(yaw_cos, 0.0f, -yaw_sin);
  }

  // Rotate the camera by the given delta angles.
  // - `yaw_delta` - Rotation around Y axis, in radians.
  // - `pitch_delta` - Rotation around X axis, in radians.
  void Rotate(float yaw_delta, float pitch_delta) noexcept {
    static constexpr float two_pi = 2.0f * std::numbers::pi_v<float>;
    yaw = std::remainder(yaw + yaw_delta, two_pi);

    // Avoid gimbal flip at the poles.
    static constexpr float max_pitch = std::numbers::pi_v<float> / 2.0f - 0.05f;
    pitch = std::clamp(pitch + pitch_delta, -max_pitch, max_pitch);
  }

  // Return the view matrix of the camera.
  [[nodiscard]] glm::mat4 ViewMatrix() const noexcept {
    return glm::lookAt(position, position + Forward(),
                       glm::vec3(0.0f, 1.0f, 0.0f));
  }

  // Return the projection matrix of the camera.
  // - `aspect` - Aspect ratio of the viewport.
  [[nodiscard]] static glm::mat4 ProjectionMatrix(float aspect) noexcept {
    glm::mat4 proj =
        glm::perspectiveZO(glm::radians(config::fov_degrees), aspect,
                           config::near_plane, config::far_plane);

    // Vulkan NDC has flipped Y axis, correct the GLM output.
    proj[1][1] *= -1.0f;
    return proj;
  }
};

} // namespace flying_rat
