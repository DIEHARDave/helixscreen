# Screensavers

A screensaver runs when the display dims (`DisplayManager::check_display_sleep`), covers the
panel with a full-screen overlay, and measures its own CPU cost so it never slows the printer.
This doc is the map: which file owns what, how the gate decides, and how to add a saver.

## Key files

| File | Owns |
|---|---|
| `include/screensaver_registry.h` | `ScreensaverType`, the `SCREENSAVERS` rows (type, stable name, label key, color depths), clamp, `HELIX_SCREENSAVER_NOW` names, the fresh-install default. Always compiled |
| `include/screensaver_base.h` | `SaverBase`: start, frame and stop sequence, levels, level requests |
| `include/screensaver_overlay.h` | `SaverOverlay`: the black touch-absorbing overlay on `lv_layer_top()` |
| `include/screensaver_canvas.h` | `SaverCanvas`: stride-sized buffer, format per depth, hide-before-free, dirty areas merged to 32, the dirty-box layer session |
| `include/screensaver_frame_timer.h` | `SaverFrameTimer`: frame timer with a `MotionClock`, period changes without a motion jump |
| `include/refresh_period_hold.h` | The display refresh while a saver runs, at the saver's current period; `period()` holds `HELIX_SCREENSAVER_REFR_PERIOD_MS` |
| `include/screensaver_frame.h`, `include/screensaver_pixel_writer.h` | `DirtyRect`, `FrameTarget`, `PixelWriter` (RGB565 and XRGB8888, ordered dither, max blend, lines). No LVGL |
| `include/screensaver_gate.h`, `include/screensaver_cpu_clock.h` | Budget, decision, idle baseline, windows, environment switches, the CPU clock seam |
| `include/screensaver_level_store.h` | Board fingerprint and the stored level per saver |
| `include/screensaver.h`, `src/ui/screensaver_manager.cpp` | `ScreensaverManager`: factories, holds, the gate, the black-screen fallback |
| `include/ui_screensaver.h`, `include/screensaver_starfield.h`, `include/screensaver_pipes.h`, `include/screensaver_bounce.h`, `include/screensaver_fireworks.h` | The savers |
| `include/screensaver_maze_sim.h`, `include/screensaver_maze.h` | The 3D maze: maze generation, the wall-following walk, and the raycaster that draws it. No LVGL in the sim |
| `scripts/screensaver-perf/` | Measuring a saver on a real board |

## The foundation

Every saver derives from `SaverBase` and implements `canvas_format()`, `on_start()`,
`on_frame()`, `on_stop()` and its ladder (`ladder_size()`, `ladder_period_ms()`). The base
creates the overlay, the canvas when the saver has one, seeds the saver's own random sequence,
starts the frame timer at the level's period, and invalidates the dirty areas each frame
returns. A saver that writes pixels itself returns boxes; a saver drawing with `lv_draw_*`
opens `SaverCanvas::begin_layer()`, marks what it draws with `mark_dirty()` and finishes with
`finish_layer()`. Toasters use no canvas: their sprites are LVGL images.

Every ladder starts at 16 ms (`SAVER_FAST_PERIOD`, one refresh of a 60 Hz panel), or at
`HELIX_SCREENSAVER_REFR_PERIOD_MS` when that is set for manual testing
(`SaverBase::level_period_ms`); levels 1 and up keep their declared periods either way. Starfield
and pipes then run at 33 ms; toasters at 33 ms with every sprite, then 33 ms with ten; the
bouncing printer and bouncing logo (one class; `BouncingLogoScreensaver` swaps the sprite and drops the tint and corner celebration) at 33 ms, then 33 ms with the corner flash but no confetti; fireworks per
`FIREWORKS_LEVELS`; the 3D maze per `MAZE_LEVELS`: full resolution at 16 ms, then half resolution at
33 ms, the same with flat floor and ceiling, and quarter resolution at 50 ms. While a saver runs, `helix::RefreshPeriodHold` refreshes the display at
the saver's current period (`follow`), so the display and the saver stay equal at every level. Device measurements assume the pacing that keeps 16 ms even: EGL vsync and a 1 ms
main-loop floor, which every device run sets explicitly (`scripts/screensaver-perf/arm_measure.sh`).

## The 3D maze

A port of the Windows 95 OpenGL "3D Maze", drawn Wolfenstein-style: one ray per column through
a tile grid, no polygons. Each run is a 12 x 12 perfect maze (no loops) from a recursive
backtracker. The walk starts in a dead end facing out past the start sign, and the exit, a
translucent smiley, is the cell furthest from the start, which in a tree is always a dead end.

- **The walk** (`MazeWalker`) keeps a hand on the wall and never stops in a cell: doorway to
  doorway at a constant speed, straight through or round a quarter arc about the inner corner,
  so the wall ahead stays at least about three quarters of a tile away. Only a dead end walks in
  to the middle, spins round and walks out, the one place the view faces a wall. It eases to a
  stop in the exit's doorway and holds there with the smiley a tile ahead, then a new maze
  starts. A walk past `MazeSim::step_bound()` cells starts a new maze instead of walking on.
- **Items**: spinning Helix icons where the original had OpenGL logos, a start sign carrying the
  icon, gray icosahedron rocks that flip the view upside down (a squash through zero) and swap
  the walk to the other hand, a toolhead that wanders on its own walk, and the active printer's
  image hung on a few walls, plain brick when there is none.
- **Cost**: it repaints every pixel every frame, so it is the costliest saver per frame and the
  one the gate is likeliest to mark too heavy on a small board. `MAZE_LEVELS` pulls the
  internal resolution (each pixel replicated `scale x scale`) and swaps textured floor and
  ceiling for flat gradients. It renders into its own column-major frame, then copies that into
  the canvas row by row. Frame, textures and sprites are packed at the display's own pixel width
  (2 bytes on RGB565), so the inner loops copy pixels and never convert colour.

Motion is a function of elapsed time (`include/screensaver_motion.h`), so a level that changes
the frame period changes nothing on screen but the frame rate, and at the toasters' lowest rung
the sprite count.

## The gate

`ScreensaverManager::on_idle_check_tick` runs on the display manager's idle-check tick and
samples `CLOCK_PROCESS_CPUTIME_ID` (every thread, LVGL's draw thread included) at most every
250 ms.

- While no saver runs, samples feed `IdleBaseline`: the CPU rate over the last idle stretch of
  up to 10 s, and 0 under 3 s of samples.
- A run ignores its first second, then closes a 5 s window after another for as long as it runs:
  `(cpu delta - baseline * wall) / wall` as a share of one core.
- The budget is `saver_budget_share`: 4 or more cores 50%, 3 cores 37%, 2 cores 25%, 1 core 10%,
  halved while `SaverHost::is_printing` says a print holds the machine.
- `decide_saver_level`: within budget the level stays (a run never steps up); over budget the
  saver steps down one level at its next natural break; over budget at the bottom level the run
  becomes a static black `SaverOverlay` with no frame timer.

Each step down and each too-heavy mark is stored at `/display/screensaver_levels/<name>` as
`{"level", "too_heavy", "version", "board"}`. The next run of that saver starts there, unless
`helix_version_full()` or the board fingerprint (display path, cores, bogomips to the nearest
100, resolution, color depth) differs, which starts again at level 0.

`HELIX_SCREENSAVER_BUDGET_PCT` replaces the budget and `HELIX_SCREENSAVER_LEVEL` forces a level
with the gate off (`docs/devel/ENVIRONMENT_VARIABLES.md`).

## Color depth and builds

The registry row lists the depths a saver draws at. Every saver draws both RGB565 and
XRGB8888 through `PixelWriter`, with `SAVER_BUILD_CANVAS_FORMAT` picking the display's depth
at compile time, so a 16 bpp build compiles all of them (Makefile `SCREENSAVER_16BPP_TARGETS`
is the depth-16 target list; `SCREENSAVER_32BPP_ONLY_SRCS` is the exclusion list a saver that
cannot draw 16 bpp would land on, currently empty). The two lists and the registry's depth
bits must agree.

The fresh-install default is depth-aware (`default_screensaver_type`): Flying Toasters when the
build draws at its depth, otherwise the first registered saver that does, and Off when none do.

## Adding a saver

1. Add the type to `ScreensaverType` and a row to `SCREENSAVERS` with a stable name and depths.
2. Add the label to `row_screensaver` in `ui_xml/settings_display_overlay.xml` (a test
   compares it with the registry), run `make translation-sync`, translate the new key in every
   `translations/*.yml`, then `make translations` (and `make regen-text-fonts` for new CJK text).
3. Derive from `SaverBase` in `include/screensaver_<name>.h` and `src/ui/screensaver_<name>.cpp`;
   keep the scene in a pure simulation class when it writes pixels, so it is testable without a
   display. Register the factory in `ScreensaverManager::ScreensaverManager`.
4. Add the new sources to `firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt`
   (the Makefile screensaver filter picks up `src/ui/screensaver_*.cpp` by itself).
5. Test determinism per seed, that dirty areas cover every changed pixel, and the ladder, then
   measure the saver on a board with `scripts/screensaver-perf/`.
6. A saver costs no RAM while it is not running. Nothing sizable goes in `.data` or `.bss`: no
   static or global textures, frames, sprite sheets or lookup tables (small `constexpr` tables in
   `.rodata` are fine). Everything is allocated in `on_start()` and released, capacity included,
   in `on_stop()`; `clear()` on a vector keeps its capacity, so swap it with an empty one. Check
   with `size build/obj/ui/screensaver_<name>*.o`.
