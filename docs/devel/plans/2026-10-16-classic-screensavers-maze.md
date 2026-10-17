# Maze: open field of view, loops and rooms, distance shading, thumbnail wall art

**Goal:** the 3D Maze stops reading as a tube. It gets the original's 90° vertical field of
view, a braided maze with 2-3 small rooms that the walker still solves, distance shading on
every surface at every ladder level, and picture walls hung with the user's own print
thumbnails, all taken from the local thumbnail cache. It costs no more per second on its
bottom rung than today's bottom rung does.

**Architecture:** all scene work stays in the pure sim (`include/screensaver_maze_sim.h`,
`src/ui/screensaver_maze_sim.cpp`, no LVGL). The `MazeScreensaver` wrapper
(`include/screensaver_maze.h`, `src/ui/screensaver_maze.cpp`) gains the art pipeline: it lists
cached thumbnails from print history, the print-select file list and the printer picture,
decodes them under a time and memory budget, and hands the sim a `MazeArt` with up to 12
pictures. The pool-building logic is in pure free functions so it can be tested without the
app.

**Tech stack:** C++17, Catch2 (`tests/unit/test_screensaver_maze.cpp`), the screensaver
framework (`docs/devel/SCREENSAVERS.md`), `ThumbnailCache` / `ThumbnailProcessor` file naming
(`include/thumbnail_cache.h`, `include/thumbnail_rules.h`).

**Spec:** `docs/devel/plans/2026-10-16-classic-screensavers-design.md`, § "2. Maze", plus
"Constraints" and "Gate bar".

## Global Constraints

Verbatim from the spec:

- The original kept our proportions: a 12x12 maze, 1x1 corridors, walls as tall as a corridor is
  wide, and the eye at mid-height. Widen the vertical FOV to match the original (90°). The
  horizontal FOV follows the screen aspect, so it ends up near 120° on 800x480. If that much
  looks wrong at the edges, the builder may cap the horizontal FOV and must say what they chose.
  Grid, wall height and eye height stay as they are.
- After generating the perfect maze, knock out some interior walls ("braid" it) so there are
  loops and longer sightlines, and carve 2-3 small open rooms (2x2 or 2x3).
- The walker must still reach the exit, and must not circle a loop forever. Keep wall-following
  and the rocks' hand swap. The walker remembers the (cell, heading) states it has seen since the
  last swap. On a repeat it has latched onto an interior island, so it walks the shortest path to
  a wall on the boundary component, grabs it, and carries on. Tests: every generated maze, over
  many seeds, reaches the exit within a bound; a constructed island case recovers.
- Braiding leaves the start and exit cells as dead ends. The start and exit placements still
  hold. The flipping rocks, the Helix-logo sprites and the toolhead still place correctly.
- Distance shading: darken walls, floor and ceiling with distance toward a dark tint, from a
  precomputed table indexed by depth, so it costs no per-pixel divide. Sprites get the same
  factor. It must hold on the flat (untextured) ladder levels too.
- Wall art draws from three pools, mixed: recently printed files (print history), other gcode
  files on the printer, the printer picture. Raise the number of picture walls (6 is too few).
- Only thumbnails already in the local thumbnail cache. It never fetches when it starts: a saver
  start must not cause network traffic. Empty cache: the printer picture, then brick. Decoding
  stays in `on_start`, under a bounded total memory budget.
- Framework: allocate in `on_start`, free in `on_stop`, no static buffers; both depths (RGB565
  and XRGB8888); motion on elapsed time; device budget no wakeup over 20 ms, p99 under 5 ms.

### Decisions this plan makes (the spec's "Claude's call" / "left to the builder")

| Item | Choice |
|---|---|
| Vertical FOV | 90°: `focal = ih / 2` |
| Horizontal cap | **120°** (`tan 60° = 1.7320508`), square pixels kept. `focal = max(ih/2 / 1.0, iw/2 / 1.7320508)`. 800x480: 90° x 118.1° (cap not reached). 1024x600: 90° x 119.3°. 480x272: 88.9° x 120°. A 1920x440 bar display: 43.3° x 120° (today it is 53° x 130°). Portrait 480x800: 90° x 61.9° |
| Braid | `BRAID_SHARE = 0.5` of the perfect maze's dead ends (other than start and exit), plus `EXTRA_OPENINGS = 6` random walls between two cells |
| Rooms | 2, or 3 on a coin flip; each 2x2, 2x3 or 3x2 cells; every tile inside opened, pillars included |
| Picture walls | `PICTURE_WALLS = 16`, up to `MAZE_MAX_PICTURES = 12` distinct pictures |
| Fog curve | `k(d) = round(32 * (0.15 + 0.85 * exp(-max(0, d - 1) / 6)))`, toward tint `{6, 6, 10}`; table of 128 slots at 4 per tile |
| Walker bound | `step_bound() = (ROCK_COUNT + 1) * 12 * CELLS * CELLS` = 5184 cell middles. Per hand phase: one island lap (at most 4·cells² states before a repeat), a recovery path (at most 4·cells², the state count), one boundary lap (at most 4·cells²) |
| Ladder | A fifth rung `{66, 4, false}` |
| Art pools | recent 5, other files 6, printer 1, interleaved; thumbnails over 512 px a side skipped; decode stops starting new images after 120 ms |

### Cost of the wider FOV, measured

Pixel count does not change with FOV; what changes is the mix. Walls stand half as tall, so
more of every column is textured floor and ceiling (about 10 ops a pixel against about 3 for a
wall pixel). Measured with a standalone host bench (the real `screensaver_maze_sim.cpp` and a
patched copy, `g++ -O2`, 800x480, 1500 frames, x86 thelio, microseconds per frame, noise about
±10%):

| Level | today | 90° FOV | + fog, per-pixel on floor and walls | + fog as planned |
|---|---|---|---|---|
| 0 (1x, textured) | 412 / 431 | 463 / 476 | 674 / 662 | 454 / 456 |
| 1 (2x, textured) | 143 / 140 | 161 / 142 | 201 / 199 | 145 / 165 |
| 2 (2x, flat) | 138 / 121 | 143 / 122 | 157 / 156 | 125 / 141 |
| 3 (4x, flat) | 49 / 50 | 57 / 47 | 57 / 55 | 53 / 57 |

(XRGB8888 / RGB565.) "As planned" is: walls shaded per column through a 64-entry pre-shaded
copy of the texture column when the slice is taller than the texture, per pixel when shorter;
floor and ceiling read pre-fogged texture bands; flat rows bake the fog in. That holds the
whole change to about +10% at level 0 and within noise to +15% at the bottom rung. The new
rung at 66 ms costs `57 / 66 = 0.86` of today's bottom rung per second (`50 / 50`), so the
anti-goal ("too heavy where today's maze fits") holds even if ARM shows the upper end. The
naive per-pixel fog (+45% at level 0) is rejected. Bench sources:
`/tmp/claude-1000/-home-pbrown-Code-Printing-helixscreen/41e9fa45-f64c-4947-864e-448345f897fc/scratchpad/mazebench/`
(gone after the session; rebuild it the same way if a number is disputed). Device numbers come
from Task 11.

### How the saver finds cached thumbnails (investigated)

- The cache names files by hash, never by gcode: `{cache_dir}/{thumbnail_hash(cache_id)}.png`
  for the full PNG and `{hash}_{w}x{h}_ARGB8888.bin` for each pre-scaled copy
  (`src/print/thumbnail_rules.cpp#thumbnail_file_name`). The hash includes the connected
  printer (`http_epoch::printer_key()`), so **a directory listing cannot tell this printer's
  thumbnails from another printer's, a timelapse's or a USB stick's**. The saver therefore
  starts from the app's own lists and asks for each file by name.
- A Moonraker thumbnail path from the gcodes root is its own cache id
  (`thumbnail_cache_id(ThumbnailSource::Moonraker, key)`).
- **Recent prints:** `get_print_history_manager()->get_jobs()` (`include/app_globals.h`),
  newest first, each job with `filename` and `thumbnails` (relative to the gcode's directory;
  resolve with `resolve_thumbnail_path(rel, helix::gcode_dir_of(filename))`, the rule
  `select_and_resolve_thumbnail` uses). Written on the main thread through `token.defer`
  (`src/print/print_history_manager.cpp#on_history_fetched`).
- **Other files:** `PrintSelectPanel::file_list_`, each `PrintFileData::original_thumbnail_url`
  a gcodes-root Moonraker path. Private today; this plan adds a const accessor and a nullable
  panel getter (`get_global_print_select_panel()` logs an error when the panel does not exist).
  Main-thread data.
- **The lookup** must not mutate the cache or touch the network:
  - `ThumbnailCache::get_if_cached(...)` is **not** used: with a `source_modified` it deletes a
    stale entry (`invalidate`), and every hit bumps the LRU (`note_use`). `fetch` downloads.
  - Use `ThumbnailProcessor::instance().get_if_processed(cache_id,
    ThumbnailProcessor::get_target_for_display())` first (the card .bin print select writes; a
    file read with no inflate) and then `get_thumbnail_cache().get_cache_path(cache_id)` +
    `helix::fs::exists` (the full PNG). Two `stat`s per key, nothing written.
- **Threading:** `on_start` runs on the main thread; every source above is main-thread data, and
  `get_if_processed` takes only its own short mutex. Nothing crosses threads, so no
  `queue_update` is involved (`.claude/rules/threading.md` not engaged).
- `LV_CACHE_DEF_SIZE` is 0 (`lv_conf.h`), so `lv_image_decoder_close` frees each decode; nothing
  lingers in LVGL's image cache.

**Memory budget.** Per thumbnail decode: the LVGL buffer (at most 512 x 512 x 4 = 1 MiB at the
side cap) plus its ARGB copy (1 MiB), both freed before the next decode. Held until
`sim_.init`: at most 12 reduced pictures of 64 x 64 x 4 = 16 KiB, 192 KiB. Resident while
running: textures `(3 + 12) x 64 x 64` pixels (240 KiB at 32 bpp, 120 KiB at 16 bpp, against 96 /
48 KiB today) and the fog bands `2 x 16 x 64 x 64` pixels (512 / 256 KiB), the bands freed at a
flat level. The printer picture keeps today's behaviour (no side cap; it is one image and may be
a large asset) but is reduced to 64 px before anything else decodes.

## Review Focus

The five failure modes most likely to survive a green suite, each pinned by a test in the
owning task:

1. **Fog skipped on a path:** flat-level rows, sprites or the RGB565 store drawn unfogged
   (Task 2: flat row exact value, sprite exact value, 565/8888 agreement).
2. **Recovery that never fires or fires forever:** `seen_` not cleared on a hand swap, or a
   repeat on the boundary contour sending the walk round again (Task 6: constructed island
   recovers exactly once; seeds x swaps all reach the exit; `abandoned() == 0`).
3. **Braiding that breaks the start or exit:** either stops being a dead end, or its walls end
   up on an island so the wall-follower never passes it (Task 5: per-seed dead-end and
   `boundary_wall` checks).
4. **A saver start that mutates the cache or the network:** a later edit swaps the lookup for
   `get_if_cached` / `fetch` (Task 9: a stale-dated cached PNG survives the lookup and its mtime
   is unchanged; the art source only sees the lookup).
5. **The memory budget not holding:** an oversized thumbnail decoded, more than 12 pictures
   kept, fog bands kept at a flat level (Tasks 2, 7 and 9).

## Task 0: before screenshots

**Files:** none in the tree; images in the session scratchpad.

- [ ] Build the app at the branch's starting commit (`make`, check for a running build first per
  CLAUDE.md) and capture "before" frames, headless, pinned socket:
  ```bash
  TREE=$(basename "$(git rev-parse --show-toplevel)")
  export HELIX_SOCK="/tmp/helix-$TREE.sock" HELIX_CONFIG_DIR="/tmp/helix-config-$TREE"
  mkdir -p "$HELIX_CONFIG_DIR"
  HELIX_HEADLESS=1 SDL_VIDEODRIVER=dummy ./build/bin/helix-screen --test -vv \
      --remote-socket "$HELIX_SOCK" > /tmp/helix-$TREE.log 2>&1 &
  ./build/bin/helix-screen ctl -s "$HELIX_SOCK" click btn_done   # plugin-install modal, if shown
  ./build/bin/helix-screen ctl -s "$HELIX_SOCK" screensaver maze
  # three screenshots a few seconds apart: ctl screenshot <scratchpad>/maze-before-{1,2,3}.png
  ```
  Stop the instance by the PID resolved from your own socket (CLAUDE.md), never by name.

## Task 1: 90° vertical FOV, 120° horizontal cap

**Files:** modify `include/screensaver_maze_sim.h`, `src/ui/screensaver_maze_sim.cpp`; test
`tests/unit/test_screensaver_maze.cpp`.

**Interfaces.** Produces (header, after `MAZE_LEVEL_COUNT`):
```cpp
inline constexpr float MAZE_VFOV_HALF_TAN = 1.0f;
inline constexpr float MAZE_HFOV_MAX_HALF_TAN = 1.7320508f;
inline float maze_focal(uint32_t iw, uint32_t ih);   // max(ih/2 / VFOV, iw/2 / HFOV_MAX)
```
`MazeRenderer` gains `float focal_` set in `set_level` from `maze_focal(iw_, ih_)`.

- [ ] Tests, `[screensaver][maze]`:
  ```cpp
  TEST_CASE("the maze sees 90 degrees vertically, capped at 120 across") {
      CHECK(maze_focal(800, 480) == Approx(240.0f));
      CHECK(maze_focal(400, 240) == Approx(120.0f));
      CHECK(maze_focal(480, 272) == Approx(138.564f).margin(0.01f));   // cap binds
      CHECK(maze_focal(1920, 440) == Approx(554.256f).margin(0.01f));
      CHECK(maze_focal(480, 800) == Approx(400.0f));
      CHECK(maze_focal(1, 1) == Approx(0.5f));
  }
  ```
  Edit "a wall straight ahead has the slice height its distance gives": expected becomes
  `int(maze_focal(r.internal_w(), r.internal_h()) / 3.5f)` (160x100: focal 50, 14 rows).
  New: "a sprite stands the height its distance gives": a LOGO at 4 tiles down `corridor(11)`
  in a 160x100 frame; the changed rows of the centre column span
  `0.55 * focal / 4 = 6.875` rows, within 1.
  New: "the wall top is where the focal puts it": the room's east wall at d = 3.5,
  `r.wall_top(mid) == floor((ih - focal / 3.5) / 2)`.
- [ ] `make t F='[maze]'`: FAIL (no `maze_focal`; slice 28 rows, not 14).
- [ ] Implement: `cast_walls` uses `plane_len = 0.5f * iw_ / focal_` and `line_h = focal_ / perp`;
  `set_level` computes floor-row distance as `focal_ / max(|2y + 1 - ih|, 1)` (the old `ih_`
  numerator becomes `focal_`), capped at 1000 as today; `draw_sprites` uses the same
  `plane_len`, `h = style.size * focal_ / ty`, `centre_y = fh / 2 + (0.5 - center_z) * focal_ /
  ty`. One `plane_len` expression: compute it once in `set_level` (`plane_len_`) so walls and
  sprites cannot drift apart.
- [ ] `make t F='[maze]'`: PASS (including the unchanged symmetric-slice, no-fisheye-across and
  sprite-occlusion tests).
- [ ] `git commit -m "feat(maze): 90 degree vertical field of view, horizontal capped at 120" -- include/screensaver_maze_sim.h src/ui/screensaver_maze_sim.cpp tests/unit/test_screensaver_maze.cpp`

## Task 2: distance shading on walls, floor, ceiling and sprites, at every level

**Files:** as Task 1.

**Interfaces.** Produces (header; values as in the candidate header, checked to compile):
```cpp
inline constexpr uint32_t MAZE_FOG_ONE = 32;
inline constexpr float MAZE_FOG_START = 1.0f, MAZE_FOG_SCALE = 6.0f, MAZE_FOG_FLOOR = 0.15f;
inline constexpr Rgb MAZE_FOG_TINT{6, 6, 10};
inline constexpr int MAZE_FOG_STEPS_PER_TILE = 4, MAZE_FOG_SLOTS = 128, MAZE_FOG_BANDS = 16;
inline constexpr uint32_t MAZE_SIDE_K = 21;                 // replaces DARK_SIDE 0.65
inline constexpr Rgb MAZE_FLAT_FLOOR{130, 86, 48};
inline constexpr Rgb MAZE_FLAT_CEILING{175, 172, 165};
uint32_t maze_fog_k(float depth);
inline float maze_fog_slot_depth(float depth);              // centre of depth's table slot
Rgb maze_fog(Rgb c, uint32_t k);  // (c*k + tint*(32-k) + 16) >> 5 per channel
// MazeRenderer
uint32_t wall_fog_k(uint32_t x) const;   // last render's brightness for column x, side included
```
Private renderer state: `fog_k_` (128 slots, built in `init`), `row_band_` (per internal row),
`wall_k_` (per column), `Store::fog_bands` (16 floor then 16 ceiling textures, pre-fogged),
`Store::fog_tint` (`tint * (32 - k)` per k in the working form of the format). The
`TEX_BRICK_DARK` and `TEX_PICTURE_DARK` slots go: a column's brightness is
`k = fog_k_[slot(perp)]`, times `MAZE_SIDE_K / 32` (rounded) on a y-side face.

- [ ] Tests:
  ```cpp
  TEST_CASE("the fog curve and blend") {
      CHECK(maze_fog_k(0.5f) == 32); CHECK(maze_fog_k(1.0f) == 32);
      CHECK(maze_fog_k(2.0f) == 28); CHECK(maze_fog_k(4.0f) == 21);
      CHECK(maze_fog_k(7.0f) == 15); CHECK(maze_fog_k(13.0f) == 8);
      CHECK(maze_fog_k(100.0f) == 5);
      for (float d = 0; d < 40; d += 0.25f) CHECK(maze_fog_k(d + 0.25f) <= maze_fog_k(d));
      CHECK(maze_fog({200, 100, 50}, 32) == Rgb{200, 100, 50});
      CHECK(maze_fog({255, 255, 255}, 15) == Rgb{123, 123, 125});
      CHECK(maze_fog({0, 0, 0}, 5) == Rgb{5, 5, 8});
      CHECK(maze_fog_slot_depth(3.5f) == Approx(3.625f));
      CHECK(maze_fog_slot_depth(1000.0f) == Approx(31.875f));
  }
  ```
  - "a wall's fog follows its distance, darker on the y side": `room(15)` facing east at 3.5
    tiles: `wall_fog_k(mid) == maze_fog_k(maze_fog_slot_depth(3.5f))` (23); facing north at
    3.5 tiles from the north wall: `== (23 * 21 + 16) >> 5` (15).
  - "a picture wall is drawn at its fogged colour": art with one solid `{0, 255, 0}` picture,
    a 15-tile room whose east wall centre tile is PICTURE, facing it from 3.5 tiles, 160x100
    XRGB8888, level 0: `internal_pixel(mid, ih / 2) == PixelWriter::pack({2, 185, 3}, XRGB8888)`.
  - "a sprite takes the fog of its distance": `corridor(11)`, a LOGO from a solid white 8x8
    `art.logo` at 4.0 tiles, elapsed 0: every internal pixel that differs from the bare render
    equals `pack(maze_fog({255,255,255}, maze_fog_k(maze_fog_slot_depth(4.0f))))`
    (`{169, 169, 171}`), and at least 20 differ.
  - "flat levels bake the fog into each row": `room(15)`, 128x96, level 2 (internal 64x48,
    focal 24), pose `{7.5, 7.5, 0}`: every pixel of internal row 30 (floor, distance 24/13)
    equals `pack(maze_fog(MAZE_FLAT_FLOOR, maze_fog_k(maze_fog_slot_depth(24.0f / 13.0f))))`;
    row 17 equals the same with `MAZE_FLAT_CEILING`; row 47 (distance 24/47) equals
    `pack(MAZE_FLAT_FLOOR)`. The existing "one colour per row" test still passes.
  - "a textured floor darkens with distance": `room(31)`, pose `{1.5, 15.5, 0}` (the far wall
    under one row tall), 128x96 at level 1 (internal 64x48, focal 24): the mean luminance of
    internal row 25 (distance 8, k 13) is under 0.6x that of row 47 (distance 0.51, k 32).
  - "a flat level gives the fog bands back": `init(800, 480, RGB565, {}, 1)`, then
    `set_level(2)`: `buffer_bytes()` drops by at least `2 * 16 * 64 * 64 * 2`.
    `init(..., 2)` never allocates them (bytes equal to the post-drop figure).
  - Edit "RGB565 and XRGB8888 renders agree": tolerance becomes 16 / 8 / 16 (two 565 LSBs:
    shading quantizes a second time in 565's working form). Keep `lit > half`.
- [ ] `make t F='[maze]'`: FAIL.
- [ ] Implement in `screensaver_maze_sim.cpp`:
  - `maze_fog_k` / `maze_fog` as their comments say; the renderer's tables are built from these
    two functions only, so a test of the functions is a test of the tables.
  - The shade: XRGB8888 `((p & 0xFF00FF) * k >> 5 & 0xFF00FF) | ((p & 0xFF00) * k >> 5 & 0xFF00)`
    plus `fog_tint[k]`, X byte kept 0xFF; RGB565 in the expanded `0x07E0F81F` form, multiply,
    `>> 5`, add tint, fold. Round so that `k == 32` is the identity in both (the 565/8888 test
    and the `maze_fog(c, 32) == c` test both rely on it).
  - Walls: when `y1 - y0 > TEX`, shade the 64-texel column once into a stack `Px[TEX]` and copy;
    otherwise shade each pixel. `wall_k_[x]` records k.
  - Textured floor and ceiling: `row_band_[y]` from the row's distance at `set_level`; the inner
    loop reads `fog_bands + band offset` with no shading. Band b holds the texture fogged at
    `k_b = 32 - round(b * (32 - maze_fog_k(1e9)) / 15)`; a row takes the band whose `k_b` is
    nearest its `maze_fog_k`.
  - Flat levels: `apply_level` fills `ceil_rows` / `floor_rows` with
    `maze_fog(MAZE_FLAT_*, k(row distance))`; the old horizon-to-edge gradient goes (the fog is
    the gradient now). Fog bands are built only when the level is textured, and freed
    (`free_vector`) when a flat level is applied.
  - Sprites: per sprite `k` from `ty`, shade each drawn texel.
- [ ] `make t F='[maze]'`: PASS.
- [ ] `git commit -m "feat(maze): distance fog on walls, floor, ceiling and sprites at every level" -- include/screensaver_maze_sim.h src/ui/screensaver_maze_sim.cpp tests/unit/test_screensaver_maze.cpp`

## Task 3: the fifth ladder rung

**Files:** as Task 1.

- [ ] Edit "the maze ladder runs 16, 33, 33, 50 ms" into "...16, 33, 33, 50, 66 ms":
  `MAZE_LEVEL_COUNT == 5`, `MAZE_LEVELS[4] == {66, 4, false}`, `set_level(4)` gives 200x120,
  `set_level(99)` clamps to 4. The saver ladder test is unchanged and covers the new rung.
- [ ] FAIL, then add `{66, 4, false}` to `MAZE_LEVELS`; PASS.
- [ ] `git commit -m "feat(maze): a 66 ms bottom rung" -- include/screensaver_maze_sim.h tests/unit/test_screensaver_maze.cpp`

## Task 4: map knows its boundary walls and which picture hangs where

**Files:** as Task 1.

**Interfaces.** Produces on `MazeMap`:
```cpp
static constexpr int PICTURE_WALLS = 16;           // moves from the .cpp, raised from 6
void set_tiles(int size, std::vector<MazeTile> tiles, std::vector<uint8_t> pictures = {});
uint8_t picture_index(int x, int y) const;         // 0 outside the map / on non-PICTURE tiles
bool boundary_wall(int x, int y) const;            // true outside the map
// private
void label_walls();                                // flood fill from the outer ring, 4-connected
void hang_pictures(std::minstd_rand& rng);         // fills missing indices 0..PICTURE_WALLS-1
std::vector<uint8_t> pictures_, boundary_;
```
`generate` and `set_tiles` call `label_walls()` last; `release()` frees both new vectors.

- [ ] Tests:
  - "a perfect maze has no islands": seeds 1..50, every wall tile is `boundary_wall`.
  - "an island is not on the boundary": `room(15)` with tiles x,y in [4, 10] set BRICK:
    `boundary_wall(4, 4)` false, `boundary_wall(0, 7)` true, `boundary_wall(-1, 3)` true,
    `boundary_wall(7, 1)` false (open tile).
  - "a generated maze hangs 16 pictures, indices 0..15 once each": seeds 1..50.
  - "set_tiles carries picture indices": a 15x15 map with two PICTURE tiles given indices 3
    and 7 reads them back; an empty `pictures` reads 0.
- [ ] FAIL, implement, PASS.
- [ ] `git commit -m "feat(maze): boundary walls and per-wall picture index" -- include/screensaver_maze_sim.h src/ui/screensaver_maze_sim.cpp tests/unit/test_screensaver_maze.cpp`

## Task 5: braid, extra openings and rooms

**Files:** as Task 1.

**Interfaces.** Produces:
```cpp
// MazeMap
static constexpr float BRAID_SHARE = 0.5f;
static constexpr int EXTRA_OPENINGS = 6;
static constexpr int ROOMS_MIN = 2;
void open_up(int keep_a, int keep_b, std::minstd_rand& rng);   // cells as cy * cells + cx
```
Consumes `label_walls`, `hang_pictures`. `MazeSim::new_maze` changes to: `generate` → start
(a dead end, facing out, as today) → exit (furthest by BFS in the perfect tree, as today) →
`map_.open_up(start, exit, rng)` → items. Item placement stays as it is.

Algorithm (the parts the signature does not fix):
- An *opening* turns one wall tile between two cells OPEN. It is refused when either cell is a
  kept cell or the tile is on the outer ring. After each accepted opening (and after each
  room), `label_walls()`; if any wall tile of either kept cell is no longer `boundary_wall`,
  undo it. A flood fill of 625 tiles a few dozen times is the whole cost, once per maze.
- Braid: shuffle the perfect maze's dead ends (excluding kept cells); for the first
  `round(BRAID_SHARE * n)` that are still dead ends, open the wall toward a neighbour cell,
  preferring one that is itself a dead end.
- Extra openings: random closed walls between two cells until `EXTRA_OPENINGS` are accepted or
  200 attempts.
- Rooms: `ROOMS_MIN + random_below(rng, 2)` rooms, each 2x2, 2x3 or 3x2 (random), placed at
  random so they contain no kept cell and overlap no other room, up to 50 attempts each. Every
  tile strictly inside the room's tile rectangle `[2*cx0+1, 2*cx1+1] x [2*cy0+1, 2*cy1+1]` opens,
  interior pillars included, so a room holds no one-tile island.
- `hang_pictures` last, refilling any picture an opening or room removed.

- [ ] Tests (a helper builds the sim's map: `MazeSim sim; sim.init(8x8 frame, {}, rng, 4)`, then
  reads `sim.map()` and the START/EXIT items' cells):
  - "an opened maze keeps every cell, closes its outside and has loops": seeds 1..200:
    `reachable_cells == 144`, outer ring all wall, `open_edges - 143 >= 6`; and the mean of
    `open_edges - 143` over the 200 seeds is at least 10. If the mean misses, raise
    `BRAID_SHARE`, never the bound.
  - "an opened maze has 2 or 3 rooms": count interior OPEN tiles at (even, even) (a 2x2 room
    opens 1 pillar, a 2x3 opens 2): between 2 and 6 for every seed, and a 2x2 block of cells
    with all four interior walls open exists at least twice.
  - "start and exit stay dead ends on the boundary": per seed, the START and EXIT cells have
    exactly one passage, and every wall tile around each is `boundary_wall`.
  - "items still stand in distinct open cells": per seed, LOGO, ROCK, TOOLHEAD, START and EXIT
    cells are open tiles, logos and rocks each in a different cell, none in the start or exit
    cell; counts as in the existing scene test.
  - Unchanged and still passing: "a generated maze is perfect" (that is `generate` alone).
- [ ] FAIL, implement, PASS.
- [ ] `git commit -m "feat(maze): braid the maze and carve rooms, keeping start and exit as dead ends" -- include/screensaver_maze_sim.h src/ui/screensaver_maze_sim.cpp tests/unit/test_screensaver_maze.cpp`

## Task 6: the walker walks off islands

**Files:** as Task 1.

**Interfaces.** Produces:
```cpp
// MazeWalker
uint32_t recoveries() const;
void set_left_handed(bool left);     // now out of line: also clears seen_ and path_
// private
int track_state(const MazeMap& map); // called from plan_start and plan_segment
std::vector<uint8_t> seen_, path_; size_t path_pos_; std::vector<int16_t> prev_, queue_;
// MazeSim
uint32_t exits_reached() const;
uint32_t abandoned() const;
uint32_t step_bound() const;         // (ROCK_COUNT + 1) * 12 * CELLS * CELLS = 5184
```
Behaviour:
- Only while a stop cell is set (the toolhead wanders and may circle an island for ever; that
  is fine for it and saves a search per lap).
- On entering a cell (and at the start), state `s = (cy * cells + cx) * 4 + d_in`. With a path
  pending, the next heading is `path_[path_pos_++]`, and when the path is used up `seen_` is
  cleared. Otherwise, if `seen_[s]` is set the walk is on an island: breadth-first search over
  states from `s` (a move to an open neighbour in heading e reaches `(neighbour, e)`), goal = a
  state other than `s` whose hand-side tile (`(d + hand) & 3`) is `boundary_wall`; record the
  headings, `recoveries_++`, take the first. Otherwise set `seen_[s]` and follow the wall.
- A heading opposite `d_in` plans a `DEAD_END` segment, which already walks in, turns round on
  the spot and walks out; nothing new is needed for a U-turn mid-corridor.
- Buffers sized from `map.cells()` on first use and reused (`assign` keeps capacity), so a
  frame never allocates; `MazeSim::clear` frees them through the walker.
- `MazeSim`: `exits_reached_++` when the walk stops at the exit; `abandoned_++` when
  `on_arrival` replaces a maze for passing `step_bound()`.

- [ ] Tests:
  ```cpp
  TEST_CASE("a walker latched onto an island walks off it and reaches the exit") {
      // 15x15 tiles: room(15) with tiles x, y in [4, 10] brick (cells 2..4 square is an island).
      MazeWalker w;
      w.reset(1, 3, 3 /*north*/, false);  // right hand on the island's west face
      w.set_stop_cell(0, 0);
      // advance in 50 ms steps until stopped() or moves() > 12 * 49
      CHECK(w.stopped());
      CHECK(w.recoveries() == 1);
  }
  ```
  - "a walker never recovers on a perfect maze": seeds 1..50, start dead end to the farthest
    cell, both hands: stops, `recoveries() == 0`.
  - "every opened maze reaches its exit, rocks and all": seeds 1..100, `MazeSim` at 16x12,
    level 4, `step(100)` until `exits_reached() == 1` or 60000 steps: `exits_reached() == 1`,
    `abandoned() == 0`, and `walker().moves() <= step_bound()` at the exit hold.
  - "extra hand swaps still reach the exit": seeds 1..100, a walker on `sim.map()` from the
    START cell facing out with the EXIT as stop cell, `set_left_handed(!left_handed())` at
    moves 17, 34 and 51: stops within `4 * 12 * 144` moves.
  - "a hand swap forgets the states seen": in the island map, swap hands once mid-lap; the walk
    still stops at (0, 0) and `recoveries() <= 1`.
  - Unchanged: the existing perfect-maze wall-follower and swap tests, and the rock/exit test.
- [ ] FAIL (the island test runs to its bound), implement, PASS.
- [ ] `git commit -m "feat(maze): the walker leaves an island it has latched onto" -- include/screensaver_maze_sim.h src/ui/screensaver_maze_sim.cpp tests/unit/test_screensaver_maze.cpp`

## Task 7: many pictures, one texture each

**Files:** as Task 1.

**Interfaces.** Produces:
```cpp
inline constexpr size_t MAZE_MAX_PICTURES = 12;
struct MazeArt { MazeImage logo; std::vector<MazeImage> pictures; };   // `picture` goes
MazeImage reduce_maze_image(const MazeImage& src, int max_side);
size_t MazeRenderer::picture_count() const;                             // has_picture() goes
```
Texture slots become `TEX_BRICK = 0, TEX_FLOOR = 1, TEX_CEIL = 2, TEX_PICTURE0 = 3`, one slot per
non-empty picture up to 12. A PICTURE tile draws
`TEX_PICTURE0 + map.picture_index(x, y) % picture_count_`, brick when the count is 0. Each
picture texture is built the way `TEX_PICTURE` is today (frame, matte, `fit_image`).
`reduce_maze_image` reuses the file's `sample_box` (alpha-weighted box), so the reduction and
the texture fit agree on what an average is.

- [ ] Tests:
  - "reduce_maze_image fits a box and keeps aspect": 128x64 all `0xFFFF0000` → 64x32 all red;
    10x10 at max 64 → identical; 2x1 `{red, blue}` at max 1 → 1x1, r and b both 127 or 128;
    a 4x4 image whose left half is alpha 0 white and right half opaque blue, at max 2 → right
    texel blue, left texel alpha 0; empty → empty.
  - "each picture wall shows its own picture": 15-tile room, east wall centre PICTURE index 0,
    north wall centre PICTURE index 1, art pictures `{solid red, solid blue}`: facing east the
    centre pixel is red-dominant, facing north blue-dominant (thresholds as the existing
    four-sides test).
  - "picture indices wrap by the count": index 5 with 2 pictures shows picture 1.
  - "pictures past twelve are ignored": 14 pictures → `picture_count() == 12`; empty images in
    the list are skipped, not counted.
  - Edit "picture walls show the picture only when there is one" and "read the right way round"
    to `art.pictures = {img}` and `picture_count()`.
- [ ] FAIL, implement, PASS.
- [ ] `git commit -m "feat(maze): one texture per picture, up to twelve" -- include/screensaver_maze_sim.h src/ui/screensaver_maze_sim.cpp tests/unit/test_screensaver_maze.cpp`

## Task 8: picking the art (pure)

**Files:** modify `include/screensaver_maze.h`, `src/ui/screensaver_maze.cpp`; test
`tests/unit/test_screensaver_maze.cpp`.

**Interfaces.** Produces (candidate header checked to compile):
```cpp
inline constexpr size_t MAZE_ART_RECENT = 5, MAZE_ART_FILES = 6, MAZE_ART_FILE_PROBES = 48;
inline constexpr int MAZE_ART_MAX_SOURCE_SIDE = 512, MAZE_ART_PICTURE_SIDE = 64;
inline constexpr uint32_t MAZE_ART_DECODE_BUDGET_MS = 120;
struct MazeArtSources { std::vector<std::string> recent, files; std::string printer; };
using MazeArtCandidate = std::vector<std::string>;
using MazeArtLookup = std::function<std::string(const std::string& key)>;
std::vector<MazeArtCandidate> history_art_candidates(const std::vector<PrintHistoryJob>& jobs,
                                                     size_t max_jobs);
std::vector<std::string> cached_art_paths(const std::vector<MazeArtCandidate>& candidates,
                                          const MazeArtLookup& lookup, size_t max,
                                          size_t max_probes,
                                          const std::vector<std::string>& taken = {});
std::vector<std::string> mix_maze_art(const MazeArtSources& sources, size_t max);
```
`PrintHistoryJob` is forward-declared in the header (it is a global-namespace struct in
`include/print_history_data.h`); the .cpp includes `print_history_data.h`,
`moonraker_types.h` (`resolve_thumbnail_path`) and `ui_filename_utils.h` (`gcode_dir_of`).

- [ ] Tests:
  - `history_art_candidates`: jobs `{"sub/a.gcode", thumbs {".thumbs/a-32.png" 32x32,
    ".thumbs/a-300.png" 300x300}}`, `{"b.gcode", {}}`, `{"c.gcode", {".thumbs/c.png" 300x300}}`,
    `{"sub/a.gcode", ...}` again; `max_jobs = 5` →
    `{{"sub/.thumbs/a-300.png", "sub/.thumbs/a-32.png"}, {".thumbs/c.png"}}`; `max_jobs = 1` →
    only the first.
  - `cached_art_paths` with a recording lookup `{"k1" → "A:/c/1.bin", "k3" → "A:/c/3.png"}` over
    `{{"k0", "k1"}, {"k2"}, {"k3"}}`: max 5 → `{"A:/c/1.bin", "A:/c/3.png"}`; max 1 →
    `{"A:/c/1.bin"}` and "k2", "k3" never looked up; `max_probes = 2` → `{"A:/c/1.bin"}`, "k3"
    never looked up; `taken = {"A:/c/1.bin"}` → `{"A:/c/3.png"}`.
  - `mix_maze_art`: `{r1, r2, r3}`, `{f1, f2}`, `"p"`, max 12 → `{r1, f1, p, r2, f2, r3}`;
    `{a}`, `{a, b}`, `""` → `{a, b}`; max 3 → `{r1, f1, p}`; `{}, {}, "p"` → `{p}`; all empty →
    `{}`.
- [ ] FAIL, implement, PASS.
- [ ] `git commit -m "feat(maze): choose wall art from history, files and the printer picture" -- include/screensaver_maze.h src/ui/screensaver_maze.cpp tests/unit/test_screensaver_maze.cpp`

## Task 9: the saver loads art from the cache, never the network

**Files:** modify `include/screensaver_maze.h`, `src/ui/screensaver_maze.cpp`,
`include/ui_panel_print_select.h`, `src/ui/ui_panel_print_select.cpp`; test
`tests/unit/test_screensaver_maze.cpp`.

**Interfaces.** Produces:
```cpp
// screensaver_maze.h
std::string maze_cached_thumbnail(const std::string& key);   // card .bin, else full PNG, else ""
MazeArtSources maze_art_sources_from_app(int screen_w, std::minstd_rand& rng);
void MazeScreensaver::set_art_source(std::function<MazeArtSources()> source);
const MazeSim& MazeScreensaver::sim() const;
// ui_panel_print_select.h (checked to compile)
PrintSelectPanel* find_print_select_panel();                  // nullptr before creation
std::vector<std::string> PrintSelectPanel::printer_thumbnail_keys() const;  // empty on USB
```
- `maze_cached_thumbnail(key)`: `cache_id = helix::thumbnail_cache_id(ThumbnailSource::Moonraker,
  key)`; return `ThumbnailProcessor::instance().get_if_processed(cache_id,
  ThumbnailProcessor::get_target_for_display())` if non-empty, else
  `ThumbnailCache::to_lvgl_path(get_thumbnail_cache().get_cache_path(cache_id))` when
  `helix::fs::exists`, else empty. No `get_if_cached`, no `fetch`, nothing that writes.
- `maze_art_sources_from_app`: recent = `cached_art_paths(history_art_candidates(jobs, 20),
  maze_cached_thumbnail, MAZE_ART_RECENT, 20)` (`get_print_history_manager()` may be null:
  empty pool); files = the panel's keys shuffled with `rng`, as one-key candidates, through
  `cached_art_paths(..., MAZE_ART_FILES, MAZE_ART_FILE_PROBES, recent)`; printer =
  `PrinterImageManager::instance().get_displayed_image_path(screen_w)` (today's source).
- `on_start`: `sources = art_source_ ? art_source_() : maze_art_sources_from_app(...)`;
  `order = mix_maze_art(sources, MAZE_MAX_PICTURES)`; decode in order with `load_image`, which
  now (a) reads the header with `lv_image_decoder_get_info` and skips a thumbnail with a side
  over `MAZE_ART_MAX_SOURCE_SIDE` (the printer path is exempt), (b) reduces through
  `reduce_maze_image(..., MAZE_ART_PICTURE_SIDE)` before the next decode; stop starting decodes
  once `MAZE_ART_DECODE_BUDGET_MS` has passed on `std::chrono::steady_clock`. The logo also goes
  through `reduce_maze_image(..., 64)`. Log at debug:
  `"[Screensaver] Maze art: {} recent, {} files, printer {}, {} decoded in {} ms"`.
- Fallback: no decodable art means `pictures` is empty and the walls are brick, as today; a
  missing printer picture with thumbnails present just leaves it out.

- [ ] Tests (`LVGLTestFixture`, run from the repo root like the existing saver test):
  - "the maze saver hangs the art its source gives": source `{recent: {"A:assets/images/helix-icon-64.png",
    "A:assets/images/benchy_thumbnail_white.png"}, files: {"A:assets/images/folder.png"},
    printer: "A:assets/images/printer_200.png"}` → after `start()`,
    `sim().renderer().picture_count() == 4`.
  - "it skips missing and oversized thumbnails": recent `{"A:/nonexistent.png",
    "A:assets/images/printers/snapmaker-u1.png" (1929x1869), "A:assets/images/folder.png"}` →
    count 1.
  - "it keeps twelve at most": the nine `assets/images/flags/flag_*.png` plus the three PNGs
    above as files, printer as above → count 12.
  - "no art means brick": empty sources → count 0, saver active.
  - "the cache lookup reads, never writes": write a PNG (copy `helix-icon-64.png`) to
    `get_thumbnail_cache().get_cache_path(thumbnail_cache_id(Moonraker, "maze-test/x.png"))`,
    set its mtime to 2001-01-01; `maze_cached_thumbnail("maze-test/x.png")` returns its LVGL
    path, the file still exists with the same mtime; an unknown key returns empty. Remove the
    file afterwards.
  - "the print-select keys skip USB and empty entries": through `PrintSelectPanelTestAccess` if
    one exists (`tests/test_helpers/`), else a small friend accessor in the same style; a list
    `{url "a.png"}, {url ""}, {url "sub/b.png"}` returns `{"a.png", "sub/b.png"}`, and nothing
    while `usb_source_` is active. If building that access costs more than the test, cover the
    accessor with the review and say so in the commit body.
- [ ] FAIL, implement, PASS.
- [ ] `git commit -m "feat(maze): wall art from cached print thumbnails, no fetch at start" -- include/screensaver_maze.h src/ui/screensaver_maze.cpp include/ui_panel_print_select.h src/ui/ui_panel_print_select.cpp tests/unit/test_screensaver_maze.cpp`
  (`ui_panel_print_select.h` is a heavy header: expect a long commit-hook build.)

## Task 10: docs

**Files:** modify `docs/devel/SCREENSAVERS.md` § "The 3D maze" and the ladder sentence in "The
foundation".

- [ ] Rewrite the maze section as the code now is: a braided 12 x 12 maze with 2-3 rooms
  (start and exit kept as dead ends on the boundary), 90° vertical / at most 120° horizontal
  field of view, the walker's island recovery, distance fog (table by depth, banded floor,
  baked flat rows, per-column walls), picture walls from cached thumbnails (the three pools,
  cache-only, the budget numbers), and the ladder's 66 ms rung. No history narration.
- [ ] `git commit -m "docs(screensavers): the open maze" -- docs/devel/SCREENSAVERS.md`

## Task 11: verification and evidence

- [ ] No new sources: `app_srcs_excluded.txt` already lists `screensaver_maze*.cpp`; confirm with
  `grep screensaver_maze firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt`.
  `ui_panel_print_select.cpp` gains only exception-free code. No new user-facing strings, so no
  translation work (the spec's 9-language rule applies to new savers).
- [ ] `size build/obj/ui/screensaver_maze*.o`: `.data` / `.bss` not grown (the fog and slot
  tables live in the renderer's heap vectors, not statics).
- [ ] After screenshots, same recipe as Task 0, but first `ctl navigate` to print select and to
  history so the mock fills the thumbnail cache, then `ctl screensaver maze`; capture three
  frames and a short recording of a recovery if one appears in the log. Check the log for the
  `Maze art:` line with a non-zero count. Show before and after to Preston; he judges the look
  (fisheye at the edges, fog strength, room feel). If he finds 118° too wide, the knob is
  `MAZE_HFOV_MAX_HALF_TAN`.
- [ ] CPU: the headless mock run's `ctl` gate numbers at levels 0 and 4
  (`HELIX_SCREENSAVER_LEVEL=0` / `4`), and `scripts/screensaver-perf/pi3b_gate.sh maze` on the
  Pi 3B under a `device:` claim; one 16 bpp board with `embedded_gate.sh` when free. Record the
  wakeup max and p99 (20 ms / 5 ms limits).
- [ ] `make mutate-diff` once for the branch (on the test host, branch pushed).
- [ ] `make full-test-run`; the batch `make land-gate` runs with the other branches.
