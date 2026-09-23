/* Global configuration. */

module;

#include <glm/glm.hpp>

#include <cstddef>

export module flying_rat.config;

export namespace flying_rat::config {

// Maze dimensions in tiles.  Maze generator forces odd sizes.
inline constexpr int maze_width = 21;
inline constexpr int maze_height = 21;

// World scale.
inline constexpr float tile_size = 2.0f;
inline constexpr float wall_height = 2.5f;

// Player.
inline constexpr float player_radius = 0.30f;
inline constexpr float player_eye_height = 1.6f;
inline constexpr float move_speed = 4.0f; // Units per second.

// Camera and input.
inline constexpr float mouse_sensitivity = 0.0025f; // Radians per pixel.
inline constexpr float fov_degrees = 75.0f;
inline constexpr float near_plane = 0.05f;
inline constexpr float far_plane = 100.0f;

// Fraction of the smaller window dimension used for the minimap.
inline constexpr float minimap_scale = 0.6f;

// Lighting.
inline constexpr glm::vec3 light_direction{-0.6f, -1.0f, -0.4f};
inline constexpr float ambient_strength = 0.35f;

// Clear color (RGBA).
inline constexpr glm::vec4 clear_color{0.08f, 0.09f, 0.11f, 1.0f};

// Maze room carving.
inline constexpr int room_attempts = 20;
inline constexpr int max_rooms = 4;

// Frames that can be in flight at once.
inline constexpr std::size_t max_frames_in_flight = 2;

} // namespace flying_rat::config
