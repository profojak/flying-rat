/* Maze. */

module;

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

export module flying_rat.maze;

namespace flying_rat {

// Represent a tile type of the maze.
export enum class Tile : std::uint8_t {
  // Empty tile with no wall, player can move here.
  Empty,
  // Wall tile blocking the player's movement.
  Wall
};

// Maze class generates a random maze.
// NOTE: Call `Generate` after `Resize` to build the maze, `Resize` invalidates
// any previously built maze.
export class Maze {
private:
  // Width and height of the maze in tiles.
  glm::ivec2 size_{0, 0};
  // Start position in the maze.
  glm::ivec2 start_{1, 1};
  // Exit position in the maze.
  glm::ivec2 exit_{1, 1};
  // Maze tiles.
  std::vector<Tile> tiles_;

  // Step size constant.
  static constexpr int STEP{2};
  // Border size constant.
  static constexpr int BORDER{1};

public:
  Maze() = default;

  // - `width`, `height` - Width and height of the maze in tiles.
  // NOTE: Call `Generate` after constructing to build the maze.
  Maze(int width, int height) { Resize(width, height); }

  // Return width and height of the maze in tiles.
  [[nodiscard]] glm::ivec2 Size() const noexcept { return size_; }

  // Return the start tile, always `{BORDER, BORDER}` after `Generate`.
  [[nodiscard]] glm::ivec2 Start() const noexcept { return start_; }

  // Return the exit tile, always `size_ - {STEP, STEP}` after `Generate`.
  [[nodiscard]] glm::ivec2 Exit() const noexcept { return exit_; }

  // Return true when `(x, y)` is a wall.
  // - `x`, `y` - Coordinates of the tile.
  // NOTE: Out-of-bounds counts as wall so player collision keeps the player
  // inside the maze.
  [[nodiscard]] bool IsWall(int x, int y) const noexcept {
    if (x < 0 || y < 0 || x >= size_.x || y >= size_.y) {
      return true;
    }
    auto index =
        static_cast<std::size_t>(y) * static_cast<std::size_t>(size_.x) +
        static_cast<std::size_t>(x);
    return tiles_[index] == Tile::Wall;
  }

  // Tile accessor.
  // - `x`, `y` - Coordinates of the tile.
  template <typename Self>
  [[nodiscard]] decltype(auto) At(this Self &&self, int x, int y) noexcept {
    auto index =
        static_cast<std::size_t>(y) * static_cast<std::size_t>(self.size_.x) +
        static_cast<std::size_t>(x);
    return std::forward_like<Self>(self.tiles_[index]);
  }

  // Resize the maze to the given width and height, force odd dimensions.
  // - `width`, `height` - Width and height of the maze in tiles.
  // NOTE: Call `Generate` after `Resize` to build the maze.  Size is odd and
  // >= 3 after calling `Resize`.  Invalidates any previously built maze.
  void Resize(int width, int height) {
    size_.x = width < 2 * BORDER + 1 ? 2 * BORDER + 1 : (width | 1);
    size_.y = height < 2 * BORDER + 1 ? 2 * BORDER + 1 : (height | 1);
    tiles_.assign(static_cast<std::size_t>(size_.x) *
                      static_cast<std::size_t>(size_.y),
                  Tile::Wall);
  }

  // Build a random maze using a recursive backtracker algorithm, then carve
  // some open rooms.
  // - `rng` - Random number generator to use for maze generation.
  // NOTE: Call `Resize` first, then `Generate`.  Start is `{BORDER, BORDER}`
  // and exit tile is `size_ - {STEP, STEP}`.
  template <std::uniform_random_bit_generator Rng> void Generate(Rng &rng) {
    std::ranges::fill(tiles_, Tile::Wall);
    At(BORDER, BORDER) = Tile::Empty;

    std::vector<glm::ivec2> stack;
    stack.reserve(static_cast<std::size_t>(size_.x) *
                  static_cast<std::size_t>(size_.y) / 4);
    stack.push_back({BORDER, BORDER});

    // Directions to move in, two tiles away to keep walls between corridors.
    static constexpr std::array directions = {
        glm::ivec2{STEP, 0}, glm::ivec2{-STEP, 0}, glm::ivec2{0, STEP},
        glm::ivec2{0, -STEP}};

    while (!stack.empty()) {
      const glm::ivec2 position = stack.back();

      // Visit directions in random order.
      auto shuffled = directions;
      std::ranges::shuffle(shuffled, rng);

      auto it = std::ranges::find_if(shuffled, [&](auto dir) {
        auto p = position + dir;
        return p.x >= BORDER && p.y >= BORDER && p.x < size_.x - BORDER &&
               p.y < size_.y - BORDER && At(p.x, p.y) == Tile::Wall;
      });

      // Backtrack when no unvisited neighbor exists.
      if (it == shuffled.end()) {
        stack.pop_back();
        continue;
      }

      // Knock down the wall between current cell and neighbor.
      auto direction = *it;
      auto neighbor = position + direction;
      At(position.x + direction.x / STEP, position.y + direction.y / STEP) =
          Tile::Empty;
      At(neighbor.x, neighbor.y) = Tile::Empty;
      stack.push_back(neighbor);
    }

    CarveRooms(rng);

    start_ = {BORDER, BORDER};
    exit_ = {size_.x - STEP, size_.y - STEP};
  }

  // Carve a few 3x3 rooms into the maze.
  // - `rng` - Random number generator to use for room placement.
  // NOTE: Rooms avoid the start and exit tiles.
  template <std::uniform_random_bit_generator Rng> void CarveRooms(Rng &rng) {
    if (size_.x < 9 || size_.y < 9) {
      return;
    }
    std::uniform_int_distribution<int> x_dist(BORDER + 2, size_.x - BORDER - 3);
    std::uniform_int_distribution<int> y_dist(BORDER + 2, size_.y - BORDER - 3);
    const glm::ivec2 start{BORDER, BORDER};
    const glm::ivec2 exit{size_.x - STEP, size_.y - STEP};

    int carved = 0;
    for (int attempt = 0; attempt < 20 && carved < 4; ++attempt) {
      const int cx = x_dist(rng);
      const int cy = y_dist(rng);
      if (At(cx, cy) != Tile::Empty) {
        continue;
      }

      // Keep spawn and exit as corridors.
      if (std::abs(cx - start.x) <= 1 && std::abs(cy - start.y) <= 1) {
        continue;
      }
      if (std::abs(cx - exit.x) <= 1 && std::abs(cy - exit.y) <= 1) {
        continue;
      }
      for (int y = cy - 1; y <= cy + 1; ++y) {
        for (int x = cx - 1; x <= cx + 1; ++x) {
          At(x, y) = Tile::Empty;
        }
      }
      ++carved;
    }
  }
};

} // namespace flying_rat
