// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_frame.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <random>
#include <vector>

/**
 * @file screensaver_maze_sim.h
 * @brief The 3D Maze: the maze, the walk through it, and the raycaster that draws it
 *
 * No LVGL, so every part steps and renders without a display. Distances are in map tiles;
 * a maze of N x N cells is a (2N + 1) x (2N + 1) tile map whose odd tiles are the cells.
 */

namespace helix::ui {

/// One rung of the maze's quality ladder.
struct MazeLevel {
    uint32_t period_ms;
    /// The scene renders at (canvas / scale) and each pixel fills scale x scale canvas pixels.
    uint8_t scale;
    /// Textured floor and ceiling; otherwise flat vertical gradients.
    bool textured_floor;
};

/// The ladder, most expensive level first.
inline constexpr MazeLevel MAZE_LEVELS[] = {
    {SAVER_FAST_PERIOD, 1, true},
    {33, 2, true},
    {33, 2, false},
    {50, 4, false},
};

inline constexpr size_t MAZE_LEVEL_COUNT = std::size(MAZE_LEVELS);

/// What a map tile holds.
enum class MazeTile : uint8_t {
    OPEN = 0,
    BRICK = 1,
    /// Brick with the printer picture hung on it, or plain brick when there is no picture.
    PICTURE = 2,
};

/// The tile map.
class MazeMap {
  public:
    /// Carves a perfect maze (no loops, every cell reachable) of `cells` x `cells` with a
    /// recursive backtracker, and hangs pictures on a few interior walls.
    void generate(int cells, std::minstd_rand& rng);

    /// Frees the tiles; the map is empty until the next generate().
    void release();

    /// A hand-built map of `size` x `size` tiles in row order; for tests.
    void set_tiles(int size, std::vector<MazeTile> tiles);

    /// Tiles per side.
    int size() const {
        return size_;
    }
    /// Cells per side.
    int cells() const {
        return (size_ - 1) / 2;
    }
    /// The tile at (x, y); anything outside the map is brick.
    MazeTile tile(int x, int y) const;
    bool wall(int x, int y) const {
        return tile(x, y) != MazeTile::OPEN;
    }
    /// True when the wall between cell (cx, cy) and its neighbour in `heading` is open.
    bool passage(int cx, int cy, int heading) const;

  private:
    int size_ = 0;
    std::vector<MazeTile> tiles_;
    /// Cells still to backtrack from while carving, kept to reuse its allocation.
    std::vector<int> stack_;
};

/// Cell step of each heading: 0 east (+x), 1 south (+y), 2 west, 3 north. Map y grows down,
/// so a heading + 1 is a right turn.
inline constexpr int HEADING_DX[4] = {1, 0, -1, 0};
inline constexpr int HEADING_DY[4] = {0, 1, 0, -1};

/// Where the camera is and which way it looks.
struct MazePose {
    float x = 0.0f;     ///< tiles
    float y = 0.0f;     ///< tiles
    float angle = 0.0f; ///< radians, 0 east, pi / 2 south
};

/**
 * @brief Walks the maze by keeping one hand on the wall
 *
 * At each cell it turns toward its hand when that side is open, else goes on, else turns the
 * other way, else turns round. Moves and turns are eased over fixed durations, so the pose is
 * a function of the time fed in.
 */
class MazeWalker {
  public:
    static constexpr uint32_t DEFAULT_MOVE_MS = 650;
    static constexpr uint32_t DEFAULT_TURN_MS = 420;

    /// Stands in cell (cx, cy) facing `heading`.
    void reset(int cx, int cy, int heading, bool left_handed, uint32_t move_ms = DEFAULT_MOVE_MS,
               uint32_t turn_ms = DEFAULT_TURN_MS);

    /**
     * @brief Walks for up to `dt_ms`
     *
     * Stops early on reaching a new cell, so the caller can react to what is there.
     * @param arrived set when the walk reached a new cell
     * @return the milliseconds used
     */
    uint32_t advance(const MazeMap& map, uint32_t dt_ms, bool& arrived);

    MazePose pose() const;
    int cell_x() const {
        return cx_;
    }
    int cell_y() const {
        return cy_;
    }
    int heading() const {
        return heading_;
    }
    bool left_handed() const {
        return left_handed_;
    }
    /// Swaps hands from the next decision on; a move or turn under way finishes first.
    void set_left_handed(bool left) {
        left_handed_ = left;
    }
    /// Cells entered since reset().
    uint32_t moves() const {
        return moves_;
    }

  private:
    enum class Action : uint8_t { MOVE, TURN_RIGHT, TURN_LEFT };

    void plan(const MazeMap& map);

    int cx_ = 0;
    int cy_ = 0;
    int heading_ = 0;
    bool left_handed_ = false;
    uint32_t move_ms_ = DEFAULT_MOVE_MS;
    uint32_t turn_ms_ = DEFAULT_TURN_MS;
    Action queue_[3]{};
    int queue_len_ = 0;
    int queue_pos_ = 0;
    uint32_t progress_ms_ = 0;
    uint32_t moves_ = 0;
};

enum class MazeItemKind : uint8_t { START, EXIT, LOGO, ROCK, TOOLHEAD };

/// A sprite standing in the maze.
struct MazeItem {
    MazeItemKind kind = MazeItemKind::LOGO;
    float x = 0.0f; ///< tiles
    float y = 0.0f; ///< tiles
    bool active = true;
};

/// An image handed to the maze, as 0xAARRGGBB pixels in row order.
struct MazeImage {
    int w = 0;
    int h = 0;
    std::vector<uint32_t> argb;

    bool empty() const {
        return w <= 0 || h <= 0 || argb.size() < static_cast<size_t>(w) * static_cast<size_t>(h);
    }
};

/// Artwork from outside the simulation. Either may be empty: the logo then falls back to a
/// drawn mark and picture walls show plain brick.
struct MazeArt {
    MazeImage logo;
    MazeImage picture;
};

/**
 * @brief Draws the maze from a pose with one ray per column
 *
 * Renders at the level's internal resolution into a column-major buffer of pixels already
 * packed in the frame's format, then replicates it into the frame row by row. Textures and
 * sprites are built once by init(); nothing is allocated per frame.
 */
class MazeRenderer {
  public:
    static constexpr int TEX = 64;
    /// A sprite texel that draws nothing. No packed RGB565 or XRGB8888 value equals it.
    static constexpr uint32_t SPRITE_KEY = 0x00FFFFFFu;
    static constexpr int ROCK_FRAMES = 16;

    /// Sizes every buffer for a w x h frame of `format` drawn from `level` down, and builds
    /// the textures and sprites.
    void init(uint32_t w, uint32_t h, PixelFormat format, const MazeArt& art, size_t level = 0);
    void set_level(size_t level);
    size_t level() const {
        return level_;
    }
    /// Frees every buffer init() allocated; render() draws nothing until the next init().
    void release();
    /// Heap bytes the renderer holds.
    size_t buffer_bytes() const;
    uint32_t internal_w() const {
        return iw_;
    }
    uint32_t internal_h() const {
        return ih_;
    }

    /**
     * @brief Draws one frame into `out`
     * @param flip 0 upright, 1 upside down; between them the view squashes through zero
     * @param elapsed_ms time since the saver started, for spinning sprites
     */
    void render(const MazeMap& map, const std::vector<MazeItem>& items, const MazePose& pose,
                float flip, uint32_t elapsed_ms, FrameTarget& out);

    /// Perpendicular wall distance of internal column `x` in the last render.
    float wall_distance(uint32_t x) const {
        return zbuf_[x];
    }
    /// First and one-past-last internal row of the wall slice in column `x`, unclipped.
    int wall_top(uint32_t x) const {
        return wall_top_[x];
    }
    int wall_bottom(uint32_t x) const {
        return wall_bottom_[x];
    }
    /// Packed pixel at internal (x, y) of the last render, before the flip.
    uint32_t internal_pixel(uint32_t x, uint32_t y) const {
        return buf_[static_cast<size_t>(x) * ih_ + y];
    }
    /// Whether picture walls show a picture.
    bool has_picture() const {
        return has_picture_;
    }

  private:
    struct Texture {
        uint32_t px[TEX * TEX]; ///< column-major: px[u * TEX + v]
    };
    struct SpriteDraw {
        float depth; ///< squared distance, for drawing far to near
        uint16_t item;
    };

    void build_textures(const MazeArt& art);
    void build_sprites(const MazeArt& art);
    void cast_walls(const MazeMap& map, const MazePose& pose);
    void draw_sprites(const std::vector<MazeItem>& items, const MazePose& pose,
                      uint32_t elapsed_ms);
    void blit(float flip, FrameTarget& out);
    uint32_t pack(Rgb c) const;

    PixelFormat format_ = PixelFormat::XRGB8888;
    uint32_t w_ = 0;
    uint32_t h_ = 0;
    uint32_t iw_ = 0;
    uint32_t ih_ = 0;
    size_t level_ = 0;
    bool has_picture_ = false;

    std::vector<uint32_t> buf_; ///< column-major internal frame
    std::vector<float> zbuf_;
    std::vector<int> wall_top_;
    std::vector<int> wall_bottom_;
    std::vector<int32_t> row_dist_q16_; ///< floor distance of each internal row, 16.16
    std::vector<uint32_t> ceil_rows_;   ///< flat ceiling colour per internal row
    std::vector<uint32_t> floor_rows_;  ///< flat floor colour per internal row
    std::vector<int> src_rows_;         ///< internal row each output row reads, -1 for black
    std::vector<uint16_t> row16_;       ///< one RGB565 output row, built once per internal row
    std::vector<uint32_t> row32_;       ///< one XRGB8888 output row, likewise
    std::vector<SpriteDraw> sprite_order_;

    std::vector<Texture> textures_; ///< see the TEX_* indices in the source
    std::vector<Texture> sprites_;  ///< see the SPR_* indices in the source
};

/**
 * @brief The whole saver's scene: maze, items, walker, toolhead and renderer
 *
 * The walk pauses while the view flips after a rock, and holds at the exit before a new
 * maze. A walk that runs past its step bound starts a new maze rather than walking forever.
 */
class MazeSim {
  public:
    static constexpr int CELLS = 12;
    static constexpr int LOGO_COUNT = 5;
    static constexpr int ROCK_COUNT = 2;
    static constexpr uint32_t FLIP_MS = 600;
    static constexpr uint32_t EXIT_HOLD_MS = 1200;

    void init(const FrameTarget& frame, const MazeArt& art, std::minstd_rand& rng, size_t level);
    void set_level(size_t level) {
        renderer_.set_level(level);
    }
    size_t level() const {
        return renderer_.level();
    }
    /// Advances by `dt_ms`, draws the frame, and sets `dirty` to the whole frame.
    void step(uint32_t dt_ms, FrameTarget& frame, std::minstd_rand& rng,
              std::vector<DirtyRect>& dirty);
    /// Frees the maze, the items and every render buffer.
    void clear();

    const MazeMap& map() const {
        return map_;
    }
    const std::vector<MazeItem>& items() const {
        return items_;
    }
    const MazeWalker& walker() const {
        return walker_;
    }
    /// 0 upright, 1 upside down, between them mid-flip.
    float flip() const;
    /// True while the view is turning over.
    bool flipping() const {
        return flip_left_ms_ > 0;
    }
    /// New mazes started since init().
    uint32_t mazes() const {
        return mazes_;
    }
    /// Most cells one maze's walk may enter before the maze is replaced.
    uint32_t step_bound() const;
    const MazeRenderer& renderer() const {
        return renderer_;
    }

  private:
    void new_maze(std::minstd_rand& rng);
    void advance(uint32_t dt_ms, std::minstd_rand& rng);
    void on_arrival(std::minstd_rand& rng);

    MazeMap map_;
    std::vector<MazeItem> items_;
    MazeWalker walker_;
    MazeWalker toolhead_;
    size_t toolhead_item_ = 0;
    MazeRenderer renderer_;
    bool upside_down_ = false;
    uint32_t flip_left_ms_ = 0;
    uint32_t exit_left_ms_ = 0;
    uint32_t elapsed_ms_ = 0;
    uint32_t mazes_ = 0;
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
