// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "../lvgl_test_fixture.h"
#include "screensaver_maze.h"
#include "screensaver_maze_sim.h"
#include "screensaver_pixel_writer.h"

#include <cmath>
#include <cstring>
#include <deque>
#include <random>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::DirtyRect;
using helix::ui::FrameTarget;
using helix::ui::MAZE_LEVELS;
using helix::ui::MazeArt;
using helix::ui::MazeItem;
using helix::ui::MazeItemKind;
using helix::ui::MazeMap;
using helix::ui::MazePose;
using helix::ui::MazeRenderer;
using helix::ui::MazeScreensaver;
using helix::ui::MazeSim;
using helix::ui::MazeTile;
using helix::ui::MazeWalker;
using helix::ui::PixelFormat;
using helix::ui::PixelWriter;
using helix::ui::Rgb;

namespace {

/// A heap frame of `format`, rows padded past their pixels the way LVGL's strides can be.
struct Frame {
    std::vector<uint8_t> bytes;
    FrameTarget target;

    Frame(uint32_t w, uint32_t h, PixelFormat format) {
        const uint32_t bpp = format == PixelFormat::RGB565 ? 2 : 4;
        const uint32_t stride = w * bpp + 12;
        bytes.assign(static_cast<size_t>(stride) * h, 0);
        target = {bytes.data(), stride, w, h, format};
    }
};

uint64_t frame_hash(const FrameTarget& f) {
    const uint32_t bpp = f.format == PixelFormat::RGB565 ? 2 : 4;
    uint64_t hash = 1469598103934665603ULL;
    for (uint32_t y = 0; y < f.h; y++) {
        const uint8_t* row = f.data + static_cast<size_t>(y) * f.stride;
        for (uint32_t i = 0; i < f.w * bpp; i++) {
            hash = (hash ^ row[i]) * 1099511628211ULL;
        }
    }
    return hash;
}

/// Cells reachable from cell (0, 0) through open passages.
int reachable_cells(const MazeMap& map) {
    const int n = map.cells();
    std::vector<bool> seen(static_cast<size_t>(n * n), false);
    std::deque<int> todo{0};
    seen[0] = true;
    int count = 0;
    while (!todo.empty()) {
        const int c = todo.front();
        todo.pop_front();
        count++;
        for (int d = 0; d < 4; d++) {
            if (!map.passage(c % n, c / n, d)) {
                continue;
            }
            const int nx = c % n + helix::ui::HEADING_DX[d];
            const int ny = c / n + helix::ui::HEADING_DY[d];
            const int next = ny * n + nx;
            if (!seen[static_cast<size_t>(next)]) {
                seen[static_cast<size_t>(next)] = true;
                todo.push_back(next);
            }
        }
    }
    return count;
}

/// Open passages between cells, each counted once.
int open_edges(const MazeMap& map) {
    int edges = 0;
    for (int cy = 0; cy < map.cells(); cy++) {
        for (int cx = 0; cx < map.cells(); cx++) {
            edges += map.passage(cx, cy, 0) ? 1 : 0;
            edges += map.passage(cx, cy, 1) ? 1 : 0;
        }
    }
    return edges;
}

/// Walks until arriving in cell (ex, ey), or `limit` moves; returns the moves taken or -1.
int walk_to(MazeWalker& walker, const MazeMap& map, int ex, int ey, uint32_t limit) {
    while (walker.moves() <= limit) {
        bool arrived = false;
        walker.advance(map, 50, arrived);
        if (arrived && walker.cell_x() == ex && walker.cell_y() == ey) {
            return static_cast<int>(walker.moves());
        }
    }
    return -1;
}

/// A corridor running east: open tiles (1..size-2, mid), brick elsewhere.
MazeMap corridor(int size) {
    std::vector<MazeTile> tiles(static_cast<size_t>(size * size), MazeTile::BRICK);
    const int mid = size / 2;
    for (int x = 1; x < size - 1; x++) {
        tiles[static_cast<size_t>(mid * size + x)] = MazeTile::OPEN;
    }
    MazeMap map;
    map.set_tiles(size, tiles);
    return map;
}

/// An open room: every tile open but the outer ring.
MazeMap room(int size) {
    std::vector<MazeTile> tiles(static_cast<size_t>(size * size), MazeTile::BRICK);
    for (int y = 1; y < size - 1; y++) {
        for (int x = 1; x < size - 1; x++) {
            tiles[static_cast<size_t>(y * size + x)] = MazeTile::OPEN;
        }
    }
    MazeMap map;
    map.set_tiles(size, tiles);
    return map;
}

} // namespace

// ============================================================================
// Maze generation
// ============================================================================

TEST_CASE("a generated maze is perfect: every cell reachable, no loops, closed outside",
          "[screensaver][maze]") {
    for (uint32_t seed = 1; seed <= 50; seed++) {
        std::minstd_rand rng(seed);
        MazeMap map;
        map.generate(MazeSim::CELLS, rng);
        const int cells = MazeSim::CELLS * MazeSim::CELLS;
        REQUIRE(map.size() == 2 * MazeSim::CELLS + 1);
        CHECK(reachable_cells(map) == cells);
        CHECK(open_edges(map) == cells - 1);
        for (int i = 0; i < map.size(); i++) {
            CHECK(map.wall(i, 0));
            CHECK(map.wall(i, map.size() - 1));
            CHECK(map.wall(0, i));
            CHECK(map.wall(map.size() - 1, i));
        }
    }
}

TEST_CASE("a maze hangs pictures only on walls, and out of bounds reads as brick",
          "[screensaver][maze]") {
    std::minstd_rand rng(7);
    MazeMap map;
    map.generate(MazeSim::CELLS, rng);
    int pictures = 0;
    for (int y = 0; y < map.size(); y++) {
        for (int x = 0; x < map.size(); x++) {
            if (map.tile(x, y) == MazeTile::PICTURE) {
                pictures++;
                CHECK(map.wall(x, y));
            }
        }
    }
    CHECK(pictures > 0);
    CHECK(map.tile(-1, 3) == MazeTile::BRICK);
    CHECK(map.tile(map.size(), 3) == MazeTile::BRICK);
}

// ============================================================================
// Walker
// ============================================================================

TEST_CASE("the wall follower reaches every cell of a perfect maze within the step bound",
          "[screensaver][maze]") {
    const uint32_t bound = 4u * MazeSim::CELLS * MazeSim::CELLS;
    for (uint32_t seed = 1; seed <= 50; seed++) {
        std::minstd_rand rng(seed);
        MazeMap map;
        map.generate(MazeSim::CELLS, rng);
        const int last = MazeSim::CELLS - 1;
        for (bool left : {false, true}) {
            MazeWalker walker;
            walker.reset(0, 0, 0, left);
            INFO("seed " << seed << (left ? " left" : " right"));
            CHECK(walk_to(walker, map, last, last, bound) > 0);
        }
    }
}

TEST_CASE("a walker that swaps hands mid-walk still reaches the far corner",
          "[screensaver][maze]") {
    std::minstd_rand rng(11);
    MazeMap map;
    map.generate(MazeSim::CELLS, rng);
    MazeWalker walker;
    walker.reset(0, 0, 0, false);
    while (walker.moves() < 10) {
        bool arrived = false;
        walker.advance(map, 50, arrived);
    }
    walker.set_left_handed(true);
    CHECK(walker.left_handed());
    const int last = MazeSim::CELLS - 1;
    CHECK(walk_to(walker, map, last, last, 8u * MazeSim::CELLS * MazeSim::CELLS) > 0);
}

TEST_CASE("the walk is a function of time: two short advances equal one long one",
          "[screensaver][maze]") {
    std::minstd_rand rng(3);
    MazeMap map;
    map.generate(MazeSim::CELLS, rng);
    MazeWalker a;
    MazeWalker b;
    a.reset(0, 0, 0, false);
    b.reset(0, 0, 0, false);
    for (int i = 0; i < 200; i++) {
        bool arrived = false;
        uint32_t left = 32;
        while (left > 0) {
            left -= a.advance(map, left, arrived);
        }
        for (int half = 0; half < 2; half++) {
            uint32_t part = 16;
            while (part > 0) {
                part -= b.advance(map, part, arrived);
            }
        }
        REQUIRE(a.pose().x == Catch::Approx(b.pose().x).margin(1e-4));
        REQUIRE(a.pose().y == Catch::Approx(b.pose().y).margin(1e-4));
        REQUIRE(a.pose().angle == Catch::Approx(b.pose().angle).margin(1e-4));
    }
    CHECK(a.moves() > 0);
}

TEST_CASE("a walker stops at each new cell so the caller sees every arrival",
          "[screensaver][maze]") {
    // Cell row 2 is tile row 5, the corridor's open row.
    MazeMap map = corridor(11);
    MazeWalker walker;
    walker.reset(0, 2, 0, false);
    bool arrived = false;
    const uint32_t used = walker.advance(map, 100000, arrived);
    CHECK(arrived);
    CHECK(used == MazeWalker::DEFAULT_MOVE_MS);
    CHECK(walker.cell_x() == 1);
    CHECK(walker.moves() == 1);
}

// ============================================================================
// Raycaster
// ============================================================================

TEST_CASE("a wall straight ahead has the slice height its distance gives, with no fisheye",
          "[screensaver][maze]") {
    // A room, facing the east wall from 3.5 tiles off: the wall face is at x = size - 1.
    const int size = 15;
    MazeMap map = room(size);
    MazeRenderer r;
    Frame frame(160, 100, PixelFormat::XRGB8888);
    r.init(frame.target.w, frame.target.h, PixelFormat::XRGB8888, MazeArt{});
    r.set_level(0);
    MazePose pose;
    pose.x = static_cast<float>(size - 1) - 3.5f;
    pose.y = size * 0.5f;
    pose.angle = 0.0f;
    r.render(map, {}, pose, 0.0f, 0, frame.target);

    const uint32_t mid = r.internal_w() / 2;
    CHECK(r.wall_distance(mid) == Catch::Approx(3.5f).margin(0.01f));
    const int expected = static_cast<int>(static_cast<float>(r.internal_h()) / 3.5f);
    CHECK(std::abs((r.wall_bottom(mid) - r.wall_top(mid)) - expected) <= 1);
    // A flat wall square to the view stands the same height across every column that hits it.
    for (uint32_t x = 0; x < r.internal_w(); x++) {
        INFO("column " << x);
        CHECK(r.wall_distance(x) == Catch::Approx(3.5f).margin(0.01f));
    }
}

TEST_CASE("a symmetric scene renders symmetric wall slices", "[screensaver][maze]") {
    MazeMap map = corridor(11);
    MazeRenderer r;
    Frame frame(120, 80, PixelFormat::XRGB8888);
    r.init(frame.target.w, frame.target.h, PixelFormat::XRGB8888, MazeArt{});
    r.set_level(0);
    MazePose pose{2.5f, 5.5f, 0.0f};
    r.render(map, {}, pose, 0.0f, 0, frame.target);
    const uint32_t w = r.internal_w();
    for (uint32_t x = 0; x < w / 2; x++) {
        INFO("column " << x);
        CHECK(r.wall_distance(x) == Catch::Approx(r.wall_distance(w - 1 - x)).margin(0.02f));
    }
}

TEST_CASE("a sprite behind a wall draws nothing; in front of it, it draws", "[screensaver][maze]") {
    // Corridor east along row 5; the camera looks down it at a logo.
    MazeMap map = corridor(11);
    MazeRenderer r;
    Frame frame(120, 80, PixelFormat::XRGB8888);
    r.init(frame.target.w, frame.target.h, PixelFormat::XRGB8888, MazeArt{});
    r.set_level(0);
    const MazePose pose{1.5f, 5.5f, 0.0f};

    r.render(map, {}, pose, 0.0f, 0, frame.target);
    std::vector<uint32_t> bare(r.internal_w() * r.internal_h());
    for (uint32_t x = 0; x < r.internal_w(); x++) {
        for (uint32_t y = 0; y < r.internal_h(); y++) {
            bare[x * r.internal_h() + y] = r.internal_pixel(x, y);
        }
    }
    auto changed = [&]() {
        size_t n = 0;
        for (uint32_t x = 0; x < r.internal_w(); x++) {
            for (uint32_t y = 0; y < r.internal_h(); y++) {
                n += r.internal_pixel(x, y) != bare[x * r.internal_h() + y] ? 1 : 0;
            }
        }
        return n;
    };

    // Beyond the east wall (x = 10) the logo is outside the corridor and hidden.
    std::vector<MazeItem> hidden{{MazeItemKind::LOGO, 12.5f, 5.5f, true}};
    r.render(map, hidden, pose, 0.0f, 0, frame.target);
    CHECK(changed() == 0);

    std::vector<MazeItem> visible{{MazeItemKind::LOGO, 5.5f, 5.5f, true}};
    r.render(map, visible, pose, 0.0f, 0, frame.target);
    CHECK(changed() > 0);

    std::vector<MazeItem> inactive{{MazeItemKind::LOGO, 5.5f, 5.5f, false}};
    r.render(map, inactive, pose, 0.0f, 0, frame.target);
    CHECK(changed() == 0);
}

TEST_CASE("RGB565 and XRGB8888 renders of one scene agree to 565 precision",
          "[screensaver][maze]") {
    std::minstd_rand rng(5);
    MazeMap map;
    map.generate(MazeSim::CELLS, rng);
    const MazePose pose{1.5f, 1.5f, 0.3f};
    Frame f32(96, 64, PixelFormat::XRGB8888);
    Frame f16(96, 64, PixelFormat::RGB565);
    MazeRenderer r32;
    MazeRenderer r16;
    r32.init(96, 64, PixelFormat::XRGB8888, MazeArt{});
    r16.init(96, 64, PixelFormat::RGB565, MazeArt{});
    r32.render(map, {}, pose, 0.0f, 0, f32.target);
    r16.render(map, {}, pose, 0.0f, 0, f16.target);
    PixelWriter p32(f32.target);
    PixelWriter p16(f16.target);
    size_t lit = 0;
    for (int32_t y = 0; y < 64; y++) {
        for (int32_t x = 0; x < 96; x++) {
            const Rgb a = p32.get(x, y);
            const Rgb b = p16.get(x, y);
            REQUIRE(std::abs(a.r - b.r) <= 8);
            REQUIRE(std::abs(a.g - b.g) <= 4);
            REQUIRE(std::abs(a.b - b.b) <= 8);
            lit += (a.r | a.g | a.b) != 0 ? 1 : 0;
        }
    }
    CHECK(lit > 96 * 64 / 2);
}

TEST_CASE("an XRGB8888 maze frame keeps every X byte opaque", "[screensaver][maze]") {
    std::minstd_rand rng(9);
    MazeSim sim;
    Frame frame(128, 80, PixelFormat::XRGB8888);
    sim.init(frame.target, MazeArt{}, rng, 0);
    std::vector<DirtyRect> dirty;
    for (int i = 0; i < 20; i++) {
        sim.step(33, frame.target, rng, dirty);
    }
    for (uint32_t y = 0; y < frame.target.h; y++) {
        for (uint32_t x = 0; x < frame.target.w; x++) {
            REQUIRE(frame.bytes[y * frame.target.stride + x * 4 + 3] == 0xFF);
        }
    }
}

TEST_CASE("upside down is the vertical mirror of upright", "[screensaver][maze]") {
    std::minstd_rand rng(13);
    MazeMap map;
    map.generate(MazeSim::CELLS, rng);
    const MazePose pose{1.5f, 1.5f, 0.0f};
    for (size_t level = 0; level < helix::ui::MAZE_LEVEL_COUNT; level++) {
        INFO("level " << level);
        Frame up(64, 48, PixelFormat::XRGB8888);
        Frame down(64, 48, PixelFormat::XRGB8888);
        MazeRenderer r;
        r.init(64, 48, PixelFormat::XRGB8888, MazeArt{});
        r.set_level(level);
        r.render(map, {}, pose, 0.0f, 0, up.target);
        r.render(map, {}, pose, 1.0f, 0, down.target);
        for (uint32_t y = 0; y < 48; y++) {
            REQUIRE(std::memcmp(up.bytes.data() + y * up.target.stride,
                                down.bytes.data() + (47 - y) * down.target.stride, 64 * 4) == 0);
        }
    }
}

TEST_CASE("mid-flip the view squashes to black at the horizon", "[screensaver][maze]") {
    std::minstd_rand rng(13);
    MazeMap map;
    map.generate(MazeSim::CELLS, rng);
    Frame frame(64, 48, PixelFormat::XRGB8888);
    MazeRenderer r;
    r.init(64, 48, PixelFormat::XRGB8888, MazeArt{});
    r.render(map, {}, MazePose{1.5f, 1.5f, 0.0f}, 0.5f, 0, frame.target);
    PixelWriter p(frame.target);
    for (int32_t x = 0; x < 64; x++) {
        CHECK(p.get(x, 0) == Rgb{0, 0, 0});
        CHECK(p.get(x, 47) == Rgb{0, 0, 0});
    }
}

// ============================================================================
// Ladder and the whole scene
// ============================================================================

TEST_CASE("the maze ladder runs 16, 33, 33, 50 ms and shrinks the internal frame",
          "[screensaver][maze]") {
    REQUIRE(helix::ui::MAZE_LEVEL_COUNT == 4);
    CHECK(MAZE_LEVELS[0].period_ms == 16);
    CHECK(MAZE_LEVELS[1].period_ms == 33);
    CHECK(MAZE_LEVELS[2].period_ms == 33);
    CHECK(MAZE_LEVELS[3].period_ms == 50);
    CHECK(MAZE_LEVELS[1].textured_floor);
    CHECK_FALSE(MAZE_LEVELS[2].textured_floor);

    MazeRenderer r;
    r.init(800, 480, PixelFormat::RGB565, MazeArt{});
    r.set_level(0);
    CHECK(r.internal_w() == 800);
    CHECK(r.internal_h() == 480);
    r.set_level(3);
    CHECK(r.internal_w() == 200);
    CHECK(r.internal_h() == 120);
    r.set_level(99);
    CHECK(r.level() == helix::ui::MAZE_LEVEL_COUNT - 1);

    // A run that starts low holds only the frame that level draws.
    MazeRenderer low;
    low.init(800, 480, PixelFormat::RGB565, MazeArt{}, 3);
    CHECK(low.internal_w() == 200);
    CHECK(r.buffer_bytes() - low.buffer_bytes() >= (800u * 480u - 200u * 120u) * 4u);
}

TEST_CASE("the flat-floor level draws the floor as one colour per row", "[screensaver][maze]") {
    MazeMap map = room(15);
    MazeRenderer r;
    Frame frame(128, 96, PixelFormat::XRGB8888);
    r.init(128, 96, PixelFormat::XRGB8888, MazeArt{});
    const MazePose pose{7.5f, 7.5f, 0.0f};
    auto row_is_flat = [&](uint32_t y) {
        for (uint32_t x = 1; x < r.internal_w(); x++) {
            if (r.internal_pixel(x, y) != r.internal_pixel(0, y)) {
                return false;
            }
        }
        return true;
    };
    r.set_level(2);
    r.render(map, {}, pose, 0.0f, 0, frame.target);
    CHECK(row_is_flat(r.internal_h() - 1));
    r.set_level(1);
    r.render(map, {}, pose, 0.0f, 0, frame.target);
    CHECK_FALSE(row_is_flat(r.internal_h() - 1));
}

TEST_CASE("a maze run replays exactly from a seed", "[screensaver][maze]") {
    auto run = [](uint32_t seed) {
        std::minstd_rand rng(seed);
        MazeSim sim;
        Frame frame(96, 64, PixelFormat::RGB565);
        sim.init(frame.target, MazeArt{}, rng, 1);
        std::vector<DirtyRect> dirty;
        for (int i = 0; i < 120; i++) {
            sim.step(33, frame.target, rng, dirty);
        }
        return frame_hash(frame.target);
    };
    CHECK(run(42) == run(42));
    CHECK(run(42) != run(43));
}

TEST_CASE("every maze frame dirties the whole canvas", "[screensaver][maze]") {
    std::minstd_rand rng(2);
    MazeSim sim;
    Frame frame(96, 64, PixelFormat::XRGB8888);
    sim.init(frame.target, MazeArt{}, rng, 0);
    std::vector<DirtyRect> dirty;
    sim.step(16, frame.target, rng, dirty);
    REQUIRE(dirty.size() == 1);
    CHECK(dirty[0] == DirtyRect{0, 0, 95, 63});
}

TEST_CASE("a cleared maze holds no render buffers", "[screensaver][maze]") {
    std::minstd_rand rng(4);
    MazeSim sim;
    Frame frame(160, 96, PixelFormat::XRGB8888);
    sim.init(frame.target, MazeArt{}, rng, 0);
    REQUIRE(sim.renderer().buffer_bytes() > 160u * 96u * 4u);
    sim.clear();
    CHECK(sim.renderer().buffer_bytes() == 0);
    CHECK(sim.items().capacity() == 0);
    CHECK(sim.map().size() == 0);
}

TEST_CASE("a scene places start, exit, logos, rocks and a toolhead", "[screensaver][maze]") {
    std::minstd_rand rng(21);
    MazeSim sim;
    Frame frame(64, 48, PixelFormat::XRGB8888);
    sim.init(frame.target, MazeArt{}, rng, 3);
    int counts[5] = {};
    for (const MazeItem& item : sim.items()) {
        counts[static_cast<int>(item.kind)]++;
        CHECK_FALSE(sim.map().wall(static_cast<int>(item.x), static_cast<int>(item.y)));
    }
    CHECK(counts[static_cast<int>(MazeItemKind::START)] == 1);
    CHECK(counts[static_cast<int>(MazeItemKind::EXIT)] == 1);
    CHECK(counts[static_cast<int>(MazeItemKind::LOGO)] == MazeSim::LOGO_COUNT);
    CHECK(counts[static_cast<int>(MazeItemKind::ROCK)] == MazeSim::ROCK_COUNT);
    CHECK(counts[static_cast<int>(MazeItemKind::TOOLHEAD)] == 1);
}

TEST_CASE("touching a rock flips the view and swaps hands; reaching the exit starts a new maze",
          "[screensaver][maze]") {
    std::minstd_rand rng(17);
    MazeSim sim;
    Frame frame(48, 32, PixelFormat::XRGB8888);
    sim.init(frame.target, MazeArt{}, rng, 3);
    std::vector<DirtyRect> dirty;
    bool saw_flip = false;
    bool saw_left = false;
    // Every cell of a perfect maze is on the wall walk, so both rocks and the exit come up.
    for (int i = 0; i < 40000 && sim.mazes() < 2; i++) {
        sim.step(50, frame.target, rng, dirty);
        const float flip = sim.flip();
        saw_flip = saw_flip || (flip > 0.0f && flip < 1.0f);
        saw_left = saw_left || sim.walker().left_handed();
        if (!sim.flipping()) {
            // Upside down exactly while following the left wall, once the flip settles.
            REQUIRE((flip == 1.0f) == sim.walker().left_handed());
        }
    }
    CHECK(saw_flip);
    CHECK(saw_left);
    CHECK(sim.mazes() >= 2);
}

TEST_CASE("a helix logo from outside replaces the drawn mark", "[screensaver][maze]") {
    std::minstd_rand rng(3);
    MazeMap map = corridor(11);
    const MazePose pose{1.5f, 5.5f, 0.0f};
    std::vector<MazeItem> logo{{MazeItemKind::LOGO, 4.5f, 5.5f, true}};

    auto render_with = [&](const MazeArt& art) {
        Frame frame(96, 64, PixelFormat::XRGB8888);
        MazeRenderer r;
        r.init(96, 64, PixelFormat::XRGB8888, art);
        r.render(map, logo, pose, 0.0f, 0, frame.target);
        return frame_hash(frame.target);
    };
    MazeArt red;
    red.logo.w = 8;
    red.logo.h = 8;
    red.logo.argb.assign(64, 0xFFFF0000u);
    CHECK(render_with(red) != render_with(MazeArt{}));
}

TEST_CASE("picture walls show the picture only when there is one", "[screensaver][maze]") {
    MazeArt with;
    with.picture.w = 4;
    with.picture.h = 4;
    with.picture.argb.assign(16, 0xFF00FF00u);
    MazeRenderer a;
    MazeRenderer b;
    a.init(32, 32, PixelFormat::XRGB8888, with);
    b.init(32, 32, PixelFormat::XRGB8888, MazeArt{});
    CHECK(a.has_picture());
    CHECK_FALSE(b.has_picture());
}

TEST_CASE("PixelWriter::pack writes what put() writes", "[screensaver][maze]") {
    for (PixelFormat fmt : {PixelFormat::RGB565, PixelFormat::XRGB8888}) {
        Frame frame(2, 1, fmt);
        PixelWriter w(frame.target);
        const Rgb c{200, 100, 50};
        w.put(0, 0, c);
        const uint32_t packed = PixelWriter::pack(c, fmt);
        if (fmt == PixelFormat::RGB565) {
            uint16_t v;
            std::memcpy(&v, frame.bytes.data(), 2);
            CHECK(v == packed);
        } else {
            uint32_t v;
            std::memcpy(&v, frame.bytes.data(), 4);
            CHECK(v == packed);
        }
    }
}

// ============================================================================
// The saver
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "the maze saver declares the maze ladder",
                 "[screensaver][maze]") {
    MazeScreensaver saver;
    REQUIRE(saver.level_count() == helix::ui::MAZE_LEVEL_COUNT);
    CHECK(saver.type() == ScreensaverType::MAZE_3D);
    for (size_t i = 1; i < helix::ui::MAZE_LEVEL_COUNT; i++) {
        CHECK(saver.level_period_ms(i) == MAZE_LEVELS[i].period_ms);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "the maze saver starts, runs and stops", "[screensaver][maze]") {
    MazeScreensaver saver;
    saver.start();
    REQUIRE(saver.is_active());
    saver.stop();
    CHECK_FALSE(saver.is_active());
}

#endif // HELIX_ENABLE_SCREENSAVER
