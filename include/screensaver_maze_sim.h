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
    {SAVER_FAST_PERIOD, 1, true}, {33, 2, true}, {33, 2, false}, {50, 4, false}, {66, 4, false},
};

inline constexpr size_t MAZE_LEVEL_COUNT = std::size(MAZE_LEVELS);

/// Half the vertical field of view as a tangent: 1 is 90 degrees, the original's.
inline constexpr float MAZE_VFOV_HALF_TAN = 1.0f;
/// Half the widest horizontal field of view as a tangent: tan 60 degrees, 120 across. A frame
/// wider than that ratio narrows its vertical view instead of bending further at the edges.
inline constexpr float MAZE_HFOV_MAX_HALF_TAN = 1.7320508f;

/// Focal length in pixels of a w x h view: square pixels, 90 degrees vertically unless that
/// would see more than 120 degrees across.
inline float maze_focal(uint32_t w, uint32_t h) {
    const float vertical = static_cast<float>(h) * 0.5f / MAZE_VFOV_HALF_TAN;
    const float horizontal = static_cast<float>(w) * 0.5f / MAZE_HFOV_MAX_HALF_TAN;
    return vertical > horizontal ? vertical : horizontal;
}

/// Distance fog: a surface d tiles away keeps k / MAZE_FOG_ONE of its colour and takes the
/// rest from MAZE_FOG_TINT, with k = round(32 * (FLOOR + (1 - FLOOR) * exp(-(d - START) / SCALE))).
inline constexpr uint32_t MAZE_FOG_ONE = 32;
inline constexpr float MAZE_FOG_START = 1.0f;
inline constexpr float MAZE_FOG_SCALE = 6.0f;
inline constexpr float MAZE_FOG_FLOOR = 0.15f;
inline constexpr Rgb MAZE_FOG_TINT{6, 6, 10};
/// The renderer reads k from a table of MAZE_FOG_SLOTS depths, MAZE_FOG_STEPS_PER_TILE a tile;
/// the textured floor and ceiling from MAZE_FOG_BANDS pre-fogged copies of their textures.
inline constexpr int MAZE_FOG_STEPS_PER_TILE = 4;
inline constexpr int MAZE_FOG_SLOTS = 128;
inline constexpr int MAZE_FOG_BANDS = 16;
/// Faces that run east-west keep this much of their light, out of MAZE_FOG_ONE, as
/// Wolfenstein drew them.
inline constexpr uint32_t MAZE_SIDE_K = 21;
/// The floor and ceiling of the flat levels, before fog.
inline constexpr Rgb MAZE_FLAT_FLOOR{130, 86, 48};
inline constexpr Rgb MAZE_FLAT_CEILING{175, 172, 165};

/// Fog brightness, out of MAZE_FOG_ONE, of a surface `depth` tiles away.
uint32_t maze_fog_k(float depth);
/// The fog table slot holding `depth`, clamped to the table.
inline int maze_fog_slot(float depth) {
    const float s = depth * static_cast<float>(MAZE_FOG_STEPS_PER_TILE);
    if (!(s > 0.0f)) {
        return 0;
    }
    return s >= static_cast<float>(MAZE_FOG_SLOTS - 1) ? MAZE_FOG_SLOTS - 1 : static_cast<int>(s);
}
/// The depth at the centre of `depth`'s table slot: the depth the renderer fogs it as.
inline float maze_fog_slot_depth(float depth) {
    return (static_cast<float>(maze_fog_slot(depth)) + 0.5f) /
           static_cast<float>(MAZE_FOG_STEPS_PER_TILE);
}
/// `c` at fog brightness `k`: (c * k + tint * (32 - k) + 16) >> 5 per channel.
Rgb maze_fog(Rgb c, uint32_t k);

/// What a map tile holds.
enum class MazeTile : uint8_t {
    OPEN = 0,
    BRICK = 1,
    /// Brick with a picture hung on it (MazeMap::picture_index says which), or plain brick
    /// when there are no pictures.
    PICTURE = 2,
};

/// The tile map.
class MazeMap {
  public:
    /// Interior walls of a maze that get a picture, each with its own index.
    static constexpr int PICTURE_WALLS = 16;
    /// open_up() opens a wall out of this share of the perfect maze's dead ends...
    static constexpr float BRAID_SHARE = 0.5f;
    /// ...then this many more walls between two cells...
    static constexpr int EXTRA_OPENINGS = 6;
    /// ...and carves this many rooms, or one more on a coin flip.
    static constexpr int ROOMS_MIN = 2;

    /// Carves a perfect maze (no loops, every cell reachable) of `cells` x `cells` with a
    /// recursive backtracker, and hangs PICTURE_WALLS pictures on interior walls.
    void generate(int cells, std::minstd_rand& rng);

    /**
     * @brief Turns the perfect maze into one with loops and a few small rooms
     *
     * Opens walls out of dead ends ("braids" it), opens a few more at random, and carves
     * 2x2, 2x3 or 3x2 rooms with every tile inside open. Cells `keep_a` and `keep_b`
     * (cy * cells + cx) keep their walls, and any opening that would cut one of their walls
     * off from the outer ring is undone, so a wall follower still passes them.
     */
    void open_up(int keep_a, int keep_b, std::minstd_rand& rng);

    /// Frees the tiles; the map is empty until the next generate().
    void release();

    /// A hand-built map of `size` x `size` tiles in row order, with each tile's picture index
    /// (none: all 0); for tests.
    void set_tiles(int size, std::vector<MazeTile> tiles, std::vector<uint8_t> pictures = {});

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
    /// Which picture hangs on tile (x, y); 0 outside the map and on tiles without one.
    uint8_t picture_index(int x, int y) const;
    /// True for a wall joined to the outer ring through walls (4-connected), and outside the
    /// map. A hand on such a wall follows it round to every cell along it; a wall that is not
    /// is an island a wall follower can circle for ever.
    bool boundary_wall(int x, int y) const;

  private:
    size_t index(int x, int y) const {
        return static_cast<size_t>(y) * static_cast<size_t>(size_) + static_cast<size_t>(x);
    }
    /// Marks the walls joined to the outer ring.
    void label_walls();
    /// Hangs every picture index 0..PICTURE_WALLS-1 the map lacks on a wall facing open floor.
    void hang_pictures(std::minstd_rand& rng);
    /// True when every wall tile round cell `c` is a boundary wall.
    bool walled_by_boundary(int c) const;
    /// Opens the wall from cell `c` toward `heading` unless it is kept or the opening would
    /// cut a kept cell's walls off the boundary; true when it opened.
    bool open_wall(int c, int heading, int keep_a, int keep_b);
    /// Opens every tile of the room of cells [cx0, cx1] x [cy0, cy1] unless that would cut a
    /// kept cell's walls off the boundary; true when it opened.
    bool open_room(int cx0, int cy0, int cx1, int cy1, int keep_a, int keep_b);
    int cell_passages(int c) const;

    int size_ = 0;
    std::vector<MazeTile> tiles_;
    std::vector<uint8_t> pictures_;
    std::vector<uint8_t> boundary_;
    /// Scratch while opening up: dead ends to braid, then a room's tiles to put back.
    std::vector<int> scratch_;
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

/// What a walk reached before the time it was given ran out.
enum class WalkEvent : uint8_t {
    NONE,
    /// Crossed a doorway into a new cell, or stopped at the stop cell's doorway.
    ENTERED,
    /// Passed the middle of the cell it is in.
    ARRIVED,
};

/**
 * @brief Walks the maze by keeping one hand on the wall, without stopping in the cells
 *
 * The walk runs doorway to doorway at a constant speed: straight through a cell, a quarter
 * arc round the inner corner where it turns, and only in a dead end (or to turn back) in to
 * the middle, round on the spot and back out, the one place it faces a wall. Where to leave a
 * cell is decided at the doorway in: toward its hand when that side is open, else on, else
 * away. The pose is a function of the time fed in.
 *
 * In a maze with loops a hand can rest on an island and circle it for ever. While a stop cell
 * is set the walker remembers each (cell, heading) it has entered since its hand last
 * changed; entering one again means it is circling, so it walks the shortest way to a cell
 * whose wall on its hand side is joined to the outer ring, and follows that wall instead.
 */
class MazeWalker {
  public:
    /// One cell, doorway to doorway: two tiles.
    static constexpr uint32_t DEFAULT_MOVE_MS = 650;
    /// A quarter turn on the spot, at the start and in dead ends.
    static constexpr uint32_t DEFAULT_TURN_MS = 420;

    /// Stands in the middle of cell (cx, cy) facing `heading`.
    void reset(int cx, int cy, int heading, bool left_handed, uint32_t move_ms = DEFAULT_MOVE_MS,
               uint32_t turn_ms = DEFAULT_TURN_MS);

    /// Eases to a stop at the doorway into cell (cx, cy) and stays there; (-1, -1) for none.
    void set_stop_cell(int cx, int cy) {
        stop_x_ = cx;
        stop_y_ = cy;
    }

    /**
     * @brief Walks for up to `dt_ms`
     *
     * Stops early at a doorway and at the middle of a cell, so the caller can react to what is
     * there. Once stopped at the stop cell it uses all the time it is given and stays put.
     * @return the milliseconds used
     */
    uint32_t advance(const MazeMap& map, uint32_t dt_ms, WalkEvent& event);

    MazePose pose() const;
    /// The cell the walk is in, from the doorway in to the doorway out.
    int cell_x() const {
        return cx_;
    }
    int cell_y() const {
        return cy_;
    }
    bool left_handed() const {
        return left_handed_;
    }
    /// Swaps hands from the next doorway on, and forgets the states it has seen.
    void set_left_handed(bool left);
    /// True once the walk has stopped at the stop cell's doorway.
    bool stopped() const {
        return stopped_;
    }
    /// Cell middles passed since reset().
    uint32_t moves() const {
        return moves_;
    }
    /// Times since reset() the walk found itself circling and walked off to the boundary.
    uint32_t recoveries() const {
        return recoveries_;
    }
    /// Frees the circling memory; reset() and a walk size it again.
    void release();

  private:
    enum class Segment : uint8_t { START, STRAIGHT, TURN, DEAD_END };

    /// Plans the way through the cell just entered: the way out and how long it takes.
    void plan_segment(const MazeMap& map);
    void plan_start(const MazeMap& map);
    /// Records the state just entered; the heading a recovery walk takes out of it, or -1 to
    /// follow the wall.
    int track_state(const MazeMap& map);

    int cx_ = 0;
    int cy_ = 0;
    int d_in_ = 0;  ///< heading the walk entered the cell with (at the start, the one it faces)
    int d_out_ = 0; ///< heading it leaves the cell by
    int stop_x_ = -1;
    int stop_y_ = -1;
    bool left_handed_ = false;
    bool planned_ = false;
    bool stopped_ = false;
    bool arrived_ = false;  ///< the middle of this segment has been passed
    bool ease_out_ = false; ///< the segment ends at the stop cell's doorway
    Segment kind_ = Segment::START;
    int spin_quarters_ = 0; ///< signed quarter turns on the spot (START and DEAD_END)
    uint32_t move_ms_ = DEFAULT_MOVE_MS;
    uint32_t turn_ms_ = DEFAULT_TURN_MS;
    uint32_t progress_ms_ = 0;
    uint32_t duration_ms_ = 1;
    uint32_t arrive_ms_ = 0;
    uint32_t moves_ = 0;
    uint32_t recoveries_ = 0;

    // Per state (cell * 4 + heading in): entered since the hand last changed, and the search's
    // predecessor. Sized from the map on first use and kept, so a frame never allocates.
    std::vector<uint8_t> seen_;
    std::vector<int16_t> prev_;
    std::vector<int16_t> queue_;
    /// Headings of the recovery walk under way, and the next one to take.
    std::vector<uint8_t> path_;
    size_t path_pos_ = 0;
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
 * packed in the frame's format, at that format's width (2 bytes for RGB565), then replicates
 * it into the frame row by row. Textures and sprites are built once by init(); nothing is
 * allocated per frame.
 */
class MazeRenderer {
  public:
    static constexpr int TEX = 64;
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
    uint32_t internal_pixel(uint32_t x, uint32_t y) const;
    /// Fog brightness, out of MAZE_FOG_ONE, of the wall in internal column `x` in the last
    /// render, the darker east-west side included.
    uint32_t wall_fog_k(uint32_t x) const {
        return wall_k_[x];
    }
    /// Whether picture walls show a picture.
    bool has_picture() const {
        return has_picture_;
    }

  private:
    /// Everything held at the frame's pixel width. Only the store for the frame's format is
    /// filled.
    template <typename Px> struct Store {
        std::vector<Px> frame;      ///< column-major internal frame
        std::vector<Px> textures;   ///< each TEX x TEX, column-major: [u * TEX + v]
        std::vector<Px> sprites;    ///< laid out the same; sprite_key<Px>() draws nothing
        std::vector<Px> ceil_rows;  ///< flat ceiling colour per internal row, fogged
        std::vector<Px> floor_rows; ///< flat floor colour per internal row, fogged
        std::vector<Px> row;        ///< one output row, built once per internal row
        /// MAZE_FOG_BANDS fogged floor textures, then as many ceilings; textured levels only.
        std::vector<Px> fog_bands;
        /// Per k, the tint's share in the format's working form: two words, see shade().
        std::vector<uint32_t> fog_tint;
    };
    struct SpriteDraw {
        float depth; ///< squared distance, for drawing far to near
        uint16_t item;
    };

    template <typename Px> Store<Px>& store() {
        if constexpr (sizeof(Px) == 2) {
            return s16_;
        } else {
            return s32_;
        }
    }
    template <typename Px> const Store<Px>& store() const {
        if constexpr (sizeof(Px) == 2) {
            return s16_;
        } else {
            return s32_;
        }
    }
    template <typename Px> void build(const MazeArt& art);
    template <typename Px> void apply_level();
    template <typename Px>
    void render_as(const MazeMap& map, const std::vector<MazeItem>& items, const MazePose& pose,
                   float flip, uint32_t elapsed_ms, FrameTarget& out);
    template <typename Px> void cast_walls(const MazeMap& map, const MazePose& pose);
    template <typename Px>
    void draw_sprites(const std::vector<MazeItem>& items, const MazePose& pose,
                      uint32_t elapsed_ms);
    template <typename Px> void blit(float flip, FrameTarget& out);
    template <typename Px> void build_fog_bands();
    /// `p` at fog brightness `k`, as maze_fog() computes it, in the store's format.
    template <typename Px> Px shade(Px p, uint32_t k) const;
    /// Distance to the floor (or ceiling) point the centre of internal row `y` sees.
    float row_distance(uint32_t y) const;
    uint32_t fog_k_at(float depth) const {
        return fog_k_[static_cast<size_t>(maze_fog_slot(depth))];
    }

    PixelFormat format_ = PixelFormat::XRGB8888;
    uint32_t w_ = 0;
    uint32_t h_ = 0;
    uint32_t iw_ = 0;
    uint32_t ih_ = 0;
    /// Focal length in internal pixels, and the camera plane's half width it gives.
    float focal_ = 1.0f;
    float plane_len_ = 0.0f;
    size_t level_ = 0;
    bool has_picture_ = false;

    Store<uint16_t> s16_;
    Store<uint32_t> s32_;
    std::vector<float> zbuf_;
    std::vector<int> wall_top_;
    std::vector<int> wall_bottom_;
    std::vector<int32_t> row_dist_q16_; ///< floor distance of each internal row, 16.16
    std::vector<uint8_t> fog_k_;        ///< fog brightness of each depth slot
    std::vector<uint8_t> row_band_;     ///< fog band of each internal row
    std::vector<uint8_t> wall_k_;       ///< fog brightness of each internal column's wall
    std::vector<int> src_rows_;         ///< internal row each output row reads, -1 for black
    std::vector<SpriteDraw> sprite_order_;
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
    /// True while the walk waits at the exit's doorway before the next maze.
    bool holding_exit() const {
        return exit_left_ms_ > 0;
    }
    /// New mazes started since init().
    uint32_t mazes() const {
        return mazes_;
    }
    /// Walks that reached their exit since init().
    uint32_t exits_reached() const {
        return exits_reached_;
    }
    /// Mazes replaced since init() for running past step_bound().
    uint32_t abandoned() const {
        return abandoned_;
    }
    /// Most cell middles one maze's walk may pass before the maze is replaced.
    uint32_t step_bound() const;
    const MazeRenderer& renderer() const {
        return renderer_;
    }

  private:
    void new_maze(std::minstd_rand& rng);
    void advance(uint32_t dt_ms, std::minstd_rand& rng);
    void on_arrival(std::minstd_rand& rng);

    // Scratch for laying out a maze, kept to reuse its allocation.
    std::vector<uint8_t> taken_;
    std::vector<int> dist_;
    std::vector<int> queue_;

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
    uint32_t exits_reached_ = 0;
    uint32_t abandoned_ = 0;
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
