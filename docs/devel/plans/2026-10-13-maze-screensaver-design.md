# 3D Maze screensaver: design

A port of the Windows 95/NT OpenGL "3D Maze" screensaver, drawn as a Wolfenstein-style
raycaster on the existing `SaverBase` canvas, with HelixScreen branding where the original
showed OpenGL and Windows logos. It is the seventh saver, `ScreensaverType::MAZE_3D = 7`,
stable name `"maze"`, label `"3D Maze"`.

Reference: https://www.screensaversplanet.com/screensavers/3d-maze-461/ and
https://en.wikipedia.org/wiki/3D_Maze.

## What the original does, and what each piece becomes

| Original | Behaviour | Here |
|---|---|---|
| Maze | Random, no loops, first person, follows the right-hand wall | Same: recursive-backtracker perfect maze, right-hand follower |
| Start button | Floating sign the walk begins in front of | Floating sign with the Helix icon |
| Exit smiley | Translucent floating smiley; touching it starts a new maze | Same smiley, procedurally drawn |
| OpenGL logos | Spinning logos placed along the paths, decorative | **Helix icon**, spinning about its vertical axis |
| Rocks | Spinning gray polyhedra; touching one flips the view upside down and swaps the follower to the left wall | Same behaviour; gray polyhedra, ~16 rotation frames generated at start |
| Rat | 2D sprite roaming the maze | A tiny toolhead sprite |
| Globe pictures | Decorative pictures on some wall faces | The active printer's image (`PrinterImageManager`), on a few walls per maze |
| Brick / wood / tile textures | Wall, floor, ceiling | Procedural brick, wood, ceiling tile, generated at start |
| Overhead map | Optional vector overlay | Not ported: it needs a setting, and savers have none |

## Rendering: a Wolfenstein raycaster

The whole look is a 2D grid cast one ray per column, the way Wolfenstein 3D ran on a 386.
No polygons, no depth sort beyond a per-column wall distance, no LVGL draw calls.

- **Map**: the maze as a `uint8_t` grid of `(2 * cells + 1)^2` tiles, wall or open. 12 x 12
  cells gives a 25 x 25 map; a right-hand walk of that takes a few minutes.
- **Walls**: per column, DDA through the grid (Lode Vandevenne's formulation), perpendicular
  distance to avoid fisheye, wall slice height `h / dist`, texture column from the hit
  fraction. Texture rows step in 16.16 fixed point. N/S faces drawn at a darker shade than
  E/W, as Wolfenstein did; no per-pixel distance fog at any level.
- **Floor and ceiling**: at the top level, textured per row (floor casting: one
  world-space step per screen row, sampled per pixel). Below it, flat fills with a vertical
  gradient, which costs a `memset`-class write per row.
- **Sprites** (logos, start sign, exit, rocks, rat): camera-space transform, sorted far to
  near, drawn as scaled columns tested against a per-column wall z-buffer, colour-keyed
  transparency. A spinning logo is the sprite scaled horizontally by `|cos(angle)|`,
  mirrored when back-facing, which is how a flat sprite reads as spinning about its axis.
- **Internal resolution**: the scene renders at `canvas / scale` and is replicated
  `scale x scale` into the canvas. Scale is the main lever the ladder pulls.
- **Every frame changes every pixel**, so `on_frame()` returns one full-canvas `DirtyRect`
  (the `SAVER_WHOLE_CANVAS_PERCENT` rule already says full beats partial here).
- **The upside-down flip**: a raycaster cannot roll the camera. The flip is a vertical squash
  through zero over ~600 ms (scale rows by `cos(t * pi)`, then render rows mirrored), applied
  in the replication step, so it costs nothing extra.

### Constraints the implementation must hold

- No allocation after `on_start()`. Map, z-buffer, the internal frame, textures and sprite
  frames are sized once.
- Textures and sprite frames are pre-packed at start into the canvas's own format, so the
  inner loops copy pixels and never convert colour or branch on `PixelFormat` per pixel. The
  hot loops are templated on the format.
- Inner loops are integer. Floats are fine per column and per row (every target has an FPU),
  not per pixel.
- Motion is a function of elapsed time (`screensaver_motion.h`): walk and turn speeds are in
  cells per second and radians per second, so a level change alters only the frame rate.
- Columns write down a row-major buffer, which strides badly through cache at 480 rows. The
  renderer draws into its own internal buffer **transposed** (column-major) and the
  replication step writes the canvas row by row. Keep this unless a board measurement says the
  plain layout is no slower.

## Quality ladder

Most expensive first, per the `SaverBase` contract. `MAZE_LEVELS` is a table of
`{scale, period_ms, floor}`:

| Level | Scale | Period | Floor / ceiling | Pixels cast per frame at 800x480 |
|---|---|---|---|---|
| 0 | 1 | 16 ms | textured | 384,000 |
| 1 | 2 | 33 ms | textured | 96,000 |
| 2 | 2 | 33 ms | flat gradient | 96,000 (walls only textured) |
| 3 | 4 | 50 ms | flat gradient | 24,000 |

Below level 3 the gate marks it `TOO_HEAVY` and shows black, as for every saver. The
replication write is the full canvas at every level and is the floor on its cost; the
perf run will show whether level 3 needs a coarser scale too.

`on_level_request()` applies at once: changing scale mid-walk shows nothing but the new
resolution.

Expected outcome, to confirm with `scripts/screensaver-perf/`: Pi 4/5 hold level 0 or 1,
Pi 3 and the quad A53 boards land on 2 or 3, single-core A7 and MIPS boards mark it too heavy.

## Walk

- Right-hand rule on the cell graph. One cell move is a smooth translation; a turn is a
  smooth 90 degree yaw; both eased, durations from constants.
- Touching a rock: flip animation, follower switches hand, the rock is consumed. A second
  rock flips back.
- Reaching the exit: brief hold facing the smiley, then a new maze from the next seed draw.
- A safety cap on steps per maze (a perfect maze's wall walk visits each edge at most twice,
  so `4 * cells^2` is a provable bound) regenerates the maze rather than walking forever if
  the invariant ever breaks.

## Code shape

Pure simulation, no LVGL, testable without a display (the starfield/fireworks pattern):

```
include/screensaver_maze_sim.h      src/ui/screensaver_maze_sim.cpp
  struct MazeMap      { int cells; std::vector<uint8_t> tiles; bool wall(int x, int y) const; }
  MazeMap generate_maze(int cells, std::minstd_rand& rng);
  struct MazeItems    { start, exit, std::vector<GridPos> logos, rocks; }   // placed on the solution path and dead ends
  MazeItems place_items(const MazeMap&, std::minstd_rand& rng);
  class MazeWalker    { void reset(const MazeMap&, GridPos start);
                        WalkEvent advance(uint32_t dt_ms);           // NONE, ROCK, EXIT
                        Pose pose() const; bool left_handed() const; }
  class MazeRenderer  { bool init(uint32_t w, uint32_t h, PixelFormat, const SpriteSet&);
                        void set_level(const MazeLevel&);
                        void render(const MazeMap&, const MazeItems&, Pose, float flip, FrameTarget out); }

include/screensaver_maze.h           src/ui/screensaver_maze.cpp
  class MazeScreensaver : public SaverBase   // canvas format SAVER_BUILD_CANVAS_FORMAT
```

`SpriteSet` holds the pre-packed sprite frames. `MazeScreensaver::on_start()` builds it:
the Helix icon decoded once from `assets/images/helix-icon-128.png` through
`lv_image_decoder_open` (as `BouncingPrinterScreensaver::decode_sprite` does) and
downsampled to 64 px; the smiley, start sign and rock frames are procedural. A failed
decode falls back to a procedural mark rather than refusing to start.

Reuse, not twins:

- `PixelWriter` already owns the RGB565 and XRGB8888 packing. Add a
  `static uint32_t pack(Rgb, PixelFormat)` to it and use that to pre-pack textures; do not
  write a second packer.
- `screensaver::random_below` / `unit_random` for every random draw.
- `StepAccumulator` / `MotionClock` from `screensaver_motion.h` for time-based motion.

## Registration (the SCREENSAVERS.md checklist)

1. `MAZE_3D = 7` and a registry row `{MAZE_3D, "maze", "3D Maze", SAVER_DEPTH_16 | SAVER_DEPTH_32}`.
2. Dropdown label in `ui_xml/settings_display_overlay.xml`, `make translation-sync`,
   translate in every `translations/*.yml`, `make translations`.
3. Factory in `ScreensaverManager::ScreensaverManager`.
4. Sources in `firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt`.
5. `docs/devel/SCREENSAVERS.md`: key files row, ladder sentence.

Not the default saver. Toasters stays the fresh-install default.

## Tests (`tests/unit/test_screensaver_maze.cpp`, tag `[screensaver][maze]`)

- **Perfect maze**: for 50 seeds, every cell reachable from the start, open edges = cells - 1
  (no loops), the outer wall is closed.
- **Determinism**: same seed, same map, same items, same rendered frame hash.
- **Walker**: for 50 seeds it reaches the exit within the step bound; after a ROCK event it
  is left-handed and still reaches the exit; time-based (two 16 ms advances equal one 32 ms).
- **Raycaster geometry**: a hand-built corridor map, camera facing a wall at distance d:
  the centre column's slice height is `h / d`; columns are symmetric about the centre in a
  symmetric scene; no fisheye (a flat wall straight ahead gives one height across columns).
- **Occlusion**: a sprite behind a wall writes no pixel; the same sprite in front of it does.
- **Both formats**: render the same scene into RGB565 and XRGB8888 frames; colours agree
  to 565 precision.
- **Ladder**: `ladder_size() == 4`, periods 16/33/33/50, level 3 renders a frame whose
  internal buffer is `w/4 x h/4`.
- **Flip**: at flip = 1 the rendered frame is the vertical mirror of flip = 0.
- **Registry**: the existing registry/dropdown test picks up the new row.

Then `make mutate-diff` once on the branch, and a `scripts/screensaver-perf/` run on a Pi to
confirm the expected landing levels.

## Rulings

- Rocks stay gray polyhedra, procedurally rendered into ~16 rotation frames at start.
- The rat becomes a tiny toolhead.
- Wall pictures show the active printer's image, a few walls per maze; when no image
  resolves, those walls stay plain brick.
- Textures are faithful: brick walls, wood floor, ceiling tiles, generated in code.
- The Helix icon replaces both the OpenGL logos and the start sign's logo; the exit stays a
  smiley.
