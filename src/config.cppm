/* Global configuration. */

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

} // namespace flying_rat::config
