# Fireflies screensaver: implementation plan

**Goal:** a new saver, stable name `fireflies`: 2-5 invisible baits wander a perspective box and
100-175 flies chase them in coloured swarms, each with a glowing wind-bent tail, drawn in additive
light on black. It passes the CPU gate on the weak boards through a ladder whose bottom rung fits a
1-core board's 10% budget.

**Architecture:** a pure `FirefliesSim` (no LVGL, no clock, own `std::minstd_rand` passed in)
that advances the world in fixed 10 ms steps and draws into a `FrameTarget` at either depth, plus a
thin `FirefliesScreensaver : SaverBase` wrapper, the same split as `HyperspaceSim` /
`HyperspaceScreensaver`. The additive-light helpers hyperspace keeps private move into
`screensaver_pixel_writer.h` so both savers share them.

**Tech stack:** C++17, Catch2 (`tests/unit/`), the screensaver framework in `include/screensaver_*.h`.

**Spec:** `docs/devel/plans/2026-10-16-classic-screensavers-design.md` § "3. Fireflies", plus
"Constraints" and "Gate bar".

## Clean-room rule (read first)

The original (github.com/mpcomplete/fireflies) is GPL-2.0-only and cannot be copied into this
GPL-3.0-or-later tree. Build from the behaviour and numbers in this plan and the spec only.
**Do not open the upstream repository, any copy of it (there is one in the session scratchpad's
`dl/ff`; leave it shut), or any translation of it.** Every behaviour below that the spec does not
fix (steering law, mode mechanics, camera, tail profile) is this plan's own design, and is marked
*Claude's call*. The reviewer checks that no structure, naming or constant beyond the spec's
published numbers comes from upstream.

## Global Constraints

Verbatim from the spec:

- Scene: black background, additive blending. 2-5 invisible baits, 100-175 flies that chase them
  in clumps.
- Baits at speed 50 / accel 600, flies at speed 100 / accel 300.
- Each swarm cycles its hue at 15°/s; each fly shifts ±20° of hue with its speed.
- Each fly trails a 2.25 s tail, widest and brightest in the middle, that fades with age and is
  bent by a "wind" that changes every 4-8 tail-lengths.
- Bodies: small dart shapes.
- Space: a world 80 units tall × aspect-scaled wide × 50 deep, seen through a perspective camera.
- Modes: stop, loop, glow, hyperspeed, split, merge, wind gust.
- Cheaper variants (fewer flies, shorter tails, or a framebuffer fade) belong on the ladder, not in
  the default look.
- Framework: `SaverBase` subclass, pure `*_sim` class drawing into a `FrameTarget`, registry row and
  settings option translated in all 9 languages, allocate in `on_start` / free in `on_stop`, no
  static buffers, new sources in `app_srcs_excluded.txt`.
- Both depths (RGB565 and XRGB8888). Ladder via `ladder_size` / `ladder_period_ms` /
  `on_level_request`.
- Device budget: no wakeup over 20 ms, p99 under 5 ms. Motion on elapsed time, never frame count.
- Gate bar: the ladder bottoms out low enough to fit a 1-core board's 10% budget at idle.

Decisions this plan makes (*Claude's call*):

| Item | Value |
|---|---|
| Type number / name / label | `ScreensaverType::FIREFLIES = 10`, `"fireflies"`, label key `"Fireflies"`, both depths |
| Fixed step | `FIREFLIES_STEP_MS = 10`; random draws happen only inside a step, so motion is identical at any frame period or level |
| Camera | fixed; eye on the box's centre line `CAM_DIST = 100` units in front of the box centre. `F = (h/2) * CAM_DIST / 40`; a point projects to `sx = w/2 + x * F / (z + CAM_DIST)`, `sy = h/2 + y * F / (z + CAM_DIST)`, so the box's middle depth plane fills the frame height. Size scale `s = CAM_DIST / (z + CAM_DIST)` (1.33 near, 0.8 far). Skip a point with `z + CAM_DIST < 1` |
| World box | x in `[-W/2, W/2]` with `W = 80 * w / h`, y in `[-40, 40]`, z in `[-25, 25]` |
| Steering | "seek": desired velocity = unit(target - pos) * top speed; acceleration = (desired - vel) clamped to the accel length; vel += a * dt, then clamped to top speed. Baits target a waypoint; flies target their bait's position. No arrival slow-down, so flies swing through their bait and the swarm stays a living clump |
| Per-fly pace | `pace` uniform in [0.75, 1.0] scales a fly's top speed and accel, so a swarm does not collapse to one path |
| Bait waypoints | uniform inside the box inset by 10% on every axis; a new one when within 5 units or after `retarget_ms` uniform in [2000, 5000] |
| Swarm hues | initial hue of bait `i` of `n` = `360 * i / n + uniform[0, 30)`; += 15°/s every step |
| Fly hue | swarm hue + (`-20 + 40 * min(1, |v| / 100)`) degrees, wrapped into [0, 360) |
| Tail history | one point per fly every `FIREFLIES_TAIL_SAMPLE_MS = 50` (every 5 steps), ring of `FIREFLIES_TAIL_POINTS = 46` points, int16 fixed point at 1/64 unit (±512 units, enough for a 4:1 panel's box plus overshoot). Tails start collapsed on the fly |
| Tail profile | `u` = age / the level's `tail_ms`. Brightness `sin(pi u) * (1 - 0.5 u)`; width `1 + (max_px - 1) * sin(pi u)`, `max_px = tail_width_px * (h / 480) * s` |
| Wind | horizontal-leaning vector, magnitude uniform [2, 6] units/s, y component scaled by 0.3; a new heading after uniform [9000, 18000] ms (4-8 × 2250), eased linearly from the old over 2000 ms. A tail point of age `a` s is drawn displaced by `wind * a` |
| Body | filled dart triangle along the projected velocity: tip at the head, length `6 px * (h/480) * s`, half-width `2 px * (h/480) * s`, fly colour mixed 50% toward white |
| Mode schedule | NORMAL for uniform [8000, 15000] ms (first one too), then one of the 7 modes drawn uniformly; a refused SPLIT or MERGE re-draws. Durations: STOP [4000, 6000], LOOP [6000, 10000], GLOW [4000, 6000], HYPERSPEED [2000, 4000], WIND_GUST [3000, 5000] ms; SPLIT and MERGE act in one step |
| STOP | each bait's target is its position at the mode's start and its desired velocity 0: it brakes at 600 and holds |
| LOOP | each bait seeks a point circling its `loop_centre` (its position at the mode's start) in the x-z plane, radius 15 units, angular speed 2.67 rad/s (40 units/s tangential) |
| GLOW | tail and body light × 1.6 (saturating); at levels with `halo`, a soft radial halo of radius `8 px * (h/480)` round each body |
| HYPERSPEED | top speed and accel × 3 for baits and flies |
| SPLIT | the alive bait with the most flies divides into a free slot: new bait at the parent's position, velocity negated, a fresh waypoint, hue parent + 120°. Every other fly of the parent (by index order, starting with its second) moves to the new bait |
| MERGE | the alive bait with the fewest flies dies; each of its flies joins the alive bait nearest the dying bait |
| WIND_GUST | wind magnitude × 4, eased in and out over 500 ms |
| Clear + dirty | per bait slot, the box the slot drew last frame is cleared to black before anything draws; `dirty` = one box per slot, last ∪ now. At most `FIREFLIES_MAX_BAITS` = 5 areas a frame. A whole-frame fade was rejected for the ladder: it touches every pixel every frame and invalidates the whole canvas, which costs more than short explicit tails |
| 16 bpp | additive light at RGB565 rounds through `BAYER_4X4`, so faint tail ends show as a sparse pattern instead of truncating to nothing |

The ladder (`FIREFLIES_LEVELS`, most expensive first; levels change only what is drawn):

| Level | Period | Flies drawn | Tail drawn | Tail width at 480 px | GLOW halo |
|---|---|---|---|---|---|
| 0 | 16 ms (`SAVER_FAST_PERIOD`) | 175 | 2250 ms | 3 | yes |
| 1 | 33 ms | 175 | 2250 ms | 3 | yes |
| 2 | 33 ms | 175 | 2250 ms | 1 | no |
| 3 | 50 ms | 120 | 1500 ms | 1 | no |
| 4 | 100 ms | 48 | 750 ms | 1 | no |

The whole population is always simulated (175 flies cost a few microseconds a step); a level draws
the first `max_flies`. That is what keeps a level change free of motion change and allocation.

**Type numbering across the batch.** Pipes, maze, fireflies and the clock/Aqua/High Fly branch land
together. Aqua and High Fly also add types. `ScreensaverType` numbers are in-app identity only
(SCREENSAVERS.md § "Names, not numbers"), and the registry test requires rows in type order from 1,
so whichever branch merges second renumbers its own enumerators, registry rows and the XML
`options` order to follow the first. Stable names never change.

## Review Focus

The five failure modes most likely to ship untested, each pinned by a test in the owning task:

1. **Motion that depends on the frame period.** Integrating per frame instead of per fixed step, or
   drawing from `rng` outside a step, makes a level change visibly jump the swarms. Task 3:
   "fireflies move identically at 16 ms and 48 ms frames" and "a level change leaves the motion
   untouched".
2. **A dirty box that misses what the previous frame drew.** The tails move every frame; a box
   over only the new drawing leaves stale light on screen. Task 6: "every changed pixel lies in a
   dirty box", both depths, top and bottom rung, across a SPLIT and a MERGE.
3. **Faint tails vanishing at RGB565.** Hyperspace's `add_light` truncates each 8-bit add to 5/6
   bits, so a tail end dimmer than 8 never shows on a 16 bpp board and tails look half as long.
   Task 1 pins the dithered add; Task 6 pins "an RGB565 frame lights at least 60% of the pixels
   the XRGB8888 frame does".
4. **Orphaned flies or a bait count outside 2-5.** A MERGE that kills a bait without moving its
   flies, or a SPLIT at 5, leaves flies chasing a dead slot. Task 5: ten simulated minutes, every
   fly's bait alive and 2 ≤ alive ≤ 5 at every frame; refusals tested directly.
5. **A level change or a frame that allocates, and a stopped saver that keeps its buffers.** Task 7
   pins constant `heap_bytes()` across frames and levels; Task 8 pins zero after `stop()`.

## Files

| Action | Path |
|---|---|
| Create | `include/screensaver_fireflies_sim.h`, `src/ui/screensaver_fireflies_sim.cpp` |
| Create | `include/screensaver_fireflies.h`, `src/ui/screensaver_fireflies.cpp` |
| Create | `tests/unit/test_screensaver_fireflies.cpp` |
| Create | `tests/test_helpers/screensaver_cost_model.h` |
| Modify | `include/screensaver_pixel_writer.h`, `src/ui/screensaver_hyperspace_sim.cpp`, `tests/unit/test_screensaver_pixel_writer.cpp` |
| Modify | `tests/unit/test_screensaver_dirty_budget.cpp` (uses the extracted cost model) |
| Modify | `include/screensaver_registry.h`, `src/ui/screensaver_manager.cpp`, `ui_xml/settings_display_overlay.xml` |
| Modify | `translations/{en,de,es,fr,it,ja,pt,ru,zh}.yml`, generated `ui_xml/translations/*.xml` |
| Modify | `firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt`, `scripts/screensaver-perf/perf_env.sh`, `config/settings.json.template` |
| Modify | `docs/devel/SCREENSAVERS.md`, `docs/devel/ENVIRONMENT_VARIABLES.md`, `docs/user/CONFIGURATION.md`, `docs/user/guide/settings/display.md`, `docs/devel/CHANGELOG_1_1_DRAFT.md` |

Candidate headers, syntax-checked with the repo's flags, are in the session scratchpad at
`/tmp/claude-1000/-home-pbrown-Code-Printing-helixscreen/41e9fa45-f64c-4947-864e-448345f897fc/scratchpad/plan_headers/fireflies/`
(`include/screensaver_fireflies_sim.h`, `include/screensaver_fireflies.h`, the pixel-writer
additions in `include/screensaver_pixel_writer.h`, `test_helpers/screensaver_cost_model.h`). Copy
them as the starting point; the signatures below are the contract.

All test cases below go in `tests/unit/test_screensaver_fireflies.cpp` unless a task says
otherwise, inside `#ifdef HELIX_ENABLE_SCREENSAVER`. Sim cases are tagged
`[screensaver][fireflies_sim]`, wrapper cases `[screensaver][screensaver_fireflies]`. Reuse the
hyperspace test file's shapes: a `SimFrame` heap frame with a padded stride, and a `Swarm` struct
(rng + sim + frame) with `step(ms)` and `run_for(ms, frame_ms)`. Default test frame 320 × 200.

---

## Task 1: shared additive light

Hyperspace keeps `hue_color` and `add_light` in its anonymous namespace; fireflies needs both.
Move them, rather than fork a twin, and widen the RGB565 overload with a dither threshold.

**Files:** modify `include/screensaver_pixel_writer.h`, `src/ui/screensaver_hyperspace_sim.cpp`;
test `tests/unit/test_screensaver_pixel_writer.cpp`.

**Interfaces.** Produces, in `namespace helix::ui` in `screensaver_pixel_writer.h`:

```cpp
constexpr Rgb hue_color(uint32_t h);                                   // 256 steps round the wheel
inline void add_light(uint32_t* p, uint32_t r, uint32_t g, uint32_t b);  // XRGB8888, saturating
inline void add_light(uint16_t* p, uint32_t r, uint32_t g, uint32_t b,
                      uint8_t threshold = 0);  // RGB565; adds (c + (t>>1))>>3 to R/B, (c + (t>>2))>>2 to G
```

Bodies move unchanged from hyperspace; the only new behaviour is the threshold bias, and 0 gives
hyperspace's exact current result. Delete hyperspace's copies (both visible at once is an
ambiguous call, checked).

- [ ] Write the failing tests, `[screensaver][pixel_writer]`:
  - `"hue_color walks the wheel in 256 steps"`: `hue_color(0) == Rgb{255,0,0}`,
    `hue_color(128) == Rgb{0,255,255}`, `hue_color(256) == hue_color(0)`,
    `hue_color(43) == Rgb{253,255,0}`.
  - `"add_light saturates each XRGB8888 channel and keeps the X byte"`: from `0xFF0A1420` add
    (250, 1, 2) gives `0xFFFF1522`.
  - `"an RGB565 add finer than one level shows as an ordered pattern"`: over a 4×4 tile of black
    pixels, add (4, 4, 4) with `threshold = BAYER_4X4[y][x]`: the R levels sum to 8, G to 16,
    B to 8. With `threshold = 0` every pixel stays 0. Adding (255, 255, 255) to `0xFFFF` stays
    `0xFFFF`.
- [ ] `make t F='[pixel_writer]'`: FAIL (no such functions).
- [ ] Move the helpers, add the threshold, switch hyperspace to them.
- [ ] `make t F='[pixel_writer]'` and `make t F='[hyperspace_sim]'`: PASS (hyperspace's replay
  and fill tests are the guard that its output did not change).
- [ ] `git commit -m "refactor(screensaver): share additive light and the hue wheel through PixelWriter's header" -- include/screensaver_pixel_writer.h src/ui/screensaver_hyperspace_sim.cpp tests/unit/test_screensaver_pixel_writer.cpp`

## Task 2: the population

**Files:** create `include/screensaver_fireflies_sim.h`, `src/ui/screensaver_fireflies_sim.cpp`,
`tests/unit/test_screensaver_fireflies.cpp`.

**Interfaces.** Consumes `FrameTarget`, `DirtyRect`, `SAVER_FAST_PERIOD`
(`screensaver_frame.h`), `StepAccumulator`, `unit_random`, `random_below`
(`screensaver_motion.h`). Produces the header from the scratchpad candidate. Its contract:

```cpp
namespace helix::ui {
struct FirefliesLevel { uint32_t period_ms; uint16_t max_flies; uint16_t tail_ms;
                        uint8_t tail_width_px; bool halo; };
inline constexpr FirefliesLevel FIREFLIES_LEVELS[] = {
    {SAVER_FAST_PERIOD, 175, 2250, 3, true}, {33, 175, 2250, 3, true},
    {33, 175, 2250, 1, false}, {50, 120, 1500, 1, false}, {100, 48, 750, 1, false}};
inline constexpr size_t FIREFLIES_LEVEL_COUNT = std::size(FIREFLIES_LEVELS);
inline constexpr uint32_t FIREFLIES_STEP_MS = 10, FIREFLIES_TAIL_SAMPLE_MS = 50, FIREFLIES_TAIL_MS = 2250;
inline constexpr size_t FIREFLIES_TAIL_POINTS = FIREFLIES_TAIL_MS / FIREFLIES_TAIL_SAMPLE_MS + 1; // 46
inline constexpr float FIREFLIES_WORLD_H = 80, FIREFLIES_WORLD_D = 50;
inline constexpr float FIREFLIES_BAIT_SPEED = 50, FIREFLIES_BAIT_ACCEL = 600;
inline constexpr float FIREFLIES_FLY_SPEED = 100, FIREFLIES_FLY_ACCEL = 300;
inline constexpr float FIREFLIES_HUE_RATE_DPS = 15, FIREFLIES_HUE_SWING_DEG = 20;
inline constexpr size_t FIREFLIES_MIN_BAITS = 2, FIREFLIES_MAX_BAITS = 5;
inline constexpr size_t FIREFLIES_MIN_FLIES = 100, FIREFLIES_MAX_FLIES = 175;
inline constexpr uint32_t FIREFLIES_WIND_MIN_MS = 4 * FIREFLIES_TAIL_MS, FIREFLIES_WIND_MAX_MS = 8 * FIREFLIES_TAIL_MS;
enum class FirefliesMode : uint8_t { NORMAL, STOP, LOOP, GLOW, HYPERSPEED, SPLIT, MERGE, WIND_GUST };
inline constexpr size_t FIREFLIES_MODE_COUNT = 8;
float fireflies_tail_brightness(float u);
int32_t fireflies_tail_width(float u, float max_px);
struct FireflyVec { float x, y, z; };   // world units, x right, y down, z away from the camera

class FirefliesSim {
  public:
    struct Bait { FireflyVec pos, vel, target, loop_centre; float hue_deg; int32_t retarget_ms; bool alive; };
    struct Fly  { FireflyVec pos, vel; float pace; uint8_t bait; };
    void init(const FrameTarget& frame, std::minstd_rand& rng, size_t level);
    void step(uint32_t dt_ms, FrameTarget& frame, std::minstd_rand& rng, std::vector<DirtyRect>& dirty);
    void request_level(size_t level);
    size_t level() const;
    bool begin_mode(FirefliesMode mode, uint32_t duration_ms, std::minstd_rand& rng);
    FirefliesMode mode() const;
    uint32_t modes_begun(FirefliesMode mode) const;
    const std::vector<Bait>& baits() const;   // FIREFLIES_MAX_BAITS slots
    size_t baits_alive() const;
    const std::vector<Fly>& flies() const;
    float fly_hue_deg(size_t i) const;
    uint32_t tail_age_ms(size_t i) const;
    const FireflyVec& wind() const;
    uint32_t wind_changes() const;
    uint64_t last_wind_change_ms() const;
    uint64_t elapsed_ms() const;
    size_t heap_bytes() const;
    void clear();
};
}
```

`FireflyVec`, not `Vec3`: the pipes branch may add a 3D vector to `helix::ui` in the same batch.

- [ ] Write the failing tests:
  - `"a fireflies population has 2 to 5 baits and 100 to 175 flies, each chasing a live bait"`:
    over seeds 1..200: `2 <= baits_alive() <= 5`, `100 <= flies().size() <= 175`, every
    `baits()[f.bait].alive`, every `f.pace` in [0.75, 1.0], every bait inside the box
    (`|x| <= 40 * W/H`, `|y| <= 40`, `|z| <= 25`). Across the seeds both 2 and 5 baits occur, and
    the fly counts span at least 60 distinct values.
  - `"a fireflies swarm replays exactly from a seed, and another seed differs"`: two sims seeded 7,
    3 s of 33 ms frames: every fly and bait position equal bit for bit
    (`screensaver_same_bits`); seed 8 differs somewhere.
  - `"a cleared fireflies sim holds nothing"`: `heap_bytes() > 0` after `init`, `== 0` after
    `clear()`; `baits().size() == FIREFLIES_MAX_BAITS` after `init`.
- [ ] `make t F='[fireflies_sim]'`: FAIL to compile (header missing), then FAIL on values once a
  stub exists.
- [ ] Implement `init` (population as Global Constraints; `tail_` sized
  `flies × FIREFLIES_TAIL_POINTS × 3`, every point the fly's start position), `clear` (swap every
  vector with an empty one), `heap_bytes`, and `step` advancing only `elapsed_ms_` for now.
- [ ] `make t F='[fireflies_sim]'`: PASS.
- [ ] `git add -N include/screensaver_fireflies_sim.h src/ui/screensaver_fireflies_sim.cpp tests/unit/test_screensaver_fireflies.cpp && git commit -m "feat(screensaver): fireflies population of baits and flies" -- include/screensaver_fireflies_sim.h src/ui/screensaver_fireflies_sim.cpp tests/unit/test_screensaver_fireflies.cpp`

## Task 3: motion

**Files:** `src/ui/screensaver_fireflies_sim.cpp`, test file.

**Interfaces.** Implements `step`'s advance half: `steps_.steps_due(dt_ms, FIREFLIES_STEP_MS, 50)`
whole calls of `void advance_one(std::minstd_rand& rng)`. Each step: baits seek their waypoint
(re-drawing it as Global Constraints), flies seek their bait with `pace`, `elapsed_ms_ += 10`.
`rng` is read only inside `advance_one` (and `begin_mode`), never while drawing.

- [ ] Write the failing tests:
  - `"fireflies move identically at 16 ms and 48 ms frames"`: seed 11; A takes 300 steps of 16 ms,
    B 100 of 48 ms (both 4800 ms): `elapsed_ms() == 4800` for both and every fly and bait
    position matches bit for bit.
  - `"a fireflies level change leaves the motion untouched"`: seed 12; A stays at level 0, B
    `request_level(4)` at 2000 ms; at 6000 ms every position matches bit for bit.
  - `"flies close in on a stopped bait"`: seed 13; `begin_mode(STOP, 10000, rng)` right after
    `init`; record each fly's distance to its bait; after 8000 ms the mean distance is
    `<= 25` units and `<= 0.5 *` the starting mean. Check the precondition first:
    `REQUIRE(start_mean > 30)`.
  - `"baits and flies keep to their top speeds"`: 60 s, every step-sized frame (10 ms):
    every bait speed `<= FIREFLIES_BAIT_SPEED * mult + 1e-3`, every fly speed
    `<= FIREFLIES_FLY_SPEED * pace * mult + 1e-3`, where `mult` is 3 while `mode() ==
    HYPERSPEED` and 1 otherwise; every bait stays within the box plus 1 unit.
  - `"a stall advances the swarm a capped step"`: one `step(5000, ...)` advances
    `elapsed_ms()` by exactly 500.
- [ ] `make t F='[fireflies_sim]'`: FAIL.
- [ ] Implement the seek law and the waypoint rule.
- [ ] `make t F='[fireflies_sim]'`: PASS.
- [ ] `git commit -m "feat(screensaver): fireflies chase their baits in fixed 10 ms steps" -- src/ui/screensaver_fireflies_sim.cpp tests/unit/test_screensaver_fireflies.cpp`

## Task 4: colour, tails and wind

**Files:** `src/ui/screensaver_fireflies_sim.cpp`, test file.

**Interfaces.** `fireflies_tail_brightness(u) = sin(pi u) * (1 - 0.5 u)` on [0, 1], else 0;
`fireflies_tail_width(u, max_px) = max(1, lround(1 + (max_px - 1) * sin(pi u)))` on [0, 1].
`fly_hue_deg`, `tail_age_ms`, the wind fields as the header documents. The tail ring advances
every 5th step, keeping the newest 46 points; `tail_age_ms` = `(tail_count_ - 1) * 50` plus 10 ms
per step since the newest sample. Drawing stops at the level's `tail_ms`, so the visible tail is
exactly 2.25 s at levels 0-2.

- [ ] Write the failing tests:
  - `"each swarm's hue turns 15 degrees a second"`: seed 21; record every alive bait's
    `hue_deg`; after 2000 ms (before any mode can start: the first NORMAL lasts at least 8 s)
    each has advanced 30° mod 360, within 0.01.
  - `"a fly's hue swings 20 degrees either way with its speed"`: after 3 s, for every fly
    `wrap180(fly_hue_deg(i) - bait hue) == -20 + 40 * min(1, |v| / 100)` within 0.01; across the
    population the smallest offset is `< -5` and the largest `> 5` (flies are at mixed speeds).
  - `"a fly's tail grows to 2.25 s of history and stops there"`: `tail_age_ms(0) == 0` after
    `init`; `== 1000` at 1000 ms; in [2250, 2300) at 5000 ms, 5040 ms and 30000 ms, for every fly (46 points 50 ms apart, plus up to 40 ms since the newest).
  - `"a tail is widest and brightest in its middle and dims toward its old end"`:
    `b(0) == 0`, `b(1)` within 1e-6 of 0, `b(0.5) > b(0.1)`, `b(0.5) > b(0.9)`,
    `b(0.9) < b(0.1)`, `b(-0.1) == 0`, `b(1.1) == 0`; `fireflies_tail_width(0.5f, 3.0f) == 3`,
    `fireflies_tail_width(0.0f, 3.0f) == 1`, `fireflies_tail_width(0.5f, 1.0f) == 1`.
  - `"the wind takes a new heading every 4 to 8 tail lengths"`: seed 22, 200 s; note
    `elapsed_ms()` at each increment of `wind_changes()`: the first is in [9000, 18000] and every
    gap between consecutive changes is in [9000, 18000]; `REQUIRE(wind_changes() >= 10)`. Outside
    WIND_GUST, `|wind()| <= 6 + 1e-3`.
- [ ] `make t F='[fireflies_sim]'`: FAIL.
- [ ] Implement hue drift, the tail ring, the wind schedule and easing.
- [ ] `make t F='[fireflies_sim]'`: PASS.
- [ ] `git commit -m "feat(screensaver): fireflies hues, 2.25 s tails and a shifting wind" -- src/ui/screensaver_fireflies_sim.cpp tests/unit/test_screensaver_fireflies.cpp`

## Task 5: modes

**Files:** `src/ui/screensaver_fireflies_sim.cpp`, test file.

**Interfaces.** `begin_mode` and `void schedule_next_mode(std::minstd_rand&)` as Global
Constraints. `begin_mode` draws from `rng` only for SPLIT's waypoint; the scheduler draws the next
gap when a mode ends. `modes_begun` counts accepted starts.

- [ ] Write the failing tests:
  - `"ten minutes of fireflies runs every mode and never orphans a fly"`: seed 31, 160 × 96
    frame, 100 ms frames for 600 s. At every frame `2 <= baits_alive() <= 5` and every fly's bait
    is alive. At the end `modes_begun(m) >= 1` for STOP, LOOP, GLOW, HYPERSPEED, SPLIT, MERGE,
    WIND_GUST.
  - `"a split at five baits and a merge at two are refused"`: drive to 5 baits with repeated
    `begin_mode(SPLIT)`; one more returns false and leaves the count and every fly's bait as they
    were. Same for MERGE at 2.
  - `"a split hands every other fly to a new bait"`: from a state with fewer than 5 baits, note the
    parent (most flies, `c` of them); after SPLIT the parent has `ceil(c/2)`, the new slot
    `floor(c/2)`, the new bait's hue is parent + 120 mod 360 within 0.01, and its position equals
    the parent's.
  - `"a merge moves the smallest swarm to the nearest other bait"`: from more than 2 baits, note
    the smallest swarm and the alive bait nearest it; after MERGE that slot is dead with 0 flies,
    and the nearest bait's count grew by exactly the merged count.
  - `"stop halts the baits, loop circles them, hyperspeed triples them, a gust quadruples the wind"`:
    STOP: after 1000 ms every bait speed `< 0.5`. LOOP for 6000 ms: after its first 1000 ms every
    bait stays within 20 units of its `loop_centre` and travels at least 60 units over the next
    3 s. HYPERSPEED for 3000 ms: some fly exceeds `1.5 * FIREFLIES_FLY_SPEED` within 1000 ms, and
    2000 ms after it ends every fly is back under `FIREFLIES_FLY_SPEED + 1e-3`. WIND_GUST for
    4000 ms: `|wind|` at 1000 ms in is at least 3× `|wind|` just before.
- [ ] `make t F='[fireflies_sim]'`: FAIL.
- [ ] Implement the scheduler and the seven modes.
- [ ] `make t F='[fireflies_sim]'`: PASS.
- [ ] `git commit -m "feat(screensaver): fireflies stop, loop, glow, hyperspeed, split, merge and gust" -- src/ui/screensaver_fireflies_sim.cpp tests/unit/test_screensaver_fireflies.cpp`

## Task 6: drawing at both depths

**Files:** `src/ui/screensaver_fireflies_sim.cpp`, test file.

**Interfaces.** `template <typename Px> void draw(FrameTarget&, std::vector<DirtyRect>&)`
instantiated for `uint16_t` and `uint32_t` (the hyperspace `draw(uint16_t{})` dispatch shape).
Order: clear every `drawn_[slot]` box to black row by row; for each drawn fly (the first
`max_flies`), its tail segments head to oldest within `tail_ms`, then its body, all through
`add_light` (RGB565 with `BAYER_4X4[y & 3][x & 3]`), each clipped to the frame, each grown into
`drawn_[fly.bait]`; then `dirty` = per slot, last ∪ now, non-empty only. A tail segment is a
Bresenham line (`PixelWriter::line`) widened by stepping the minor axis over the segment's width.
`step(0, ...)` redraws without advancing.

- [ ] Write the failing tests:
  - `"every changed fireflies pixel lies in a dirty box"`: both formats × levels {0, 4}, seed 41;
    300 frames of 33 ms with a SPLIT forced at frame 100 and a MERGE at frame 200: copy the frame
    bytes before each step; after it every byte that differs lies in some dirty box;
    `dirty.size() <= FIREFLIES_MAX_BAITS`. `REQUIRE` that some frame changed pixels.
  - `"fireflies draw only light on black, and never past a row"`: both formats, 321 × 203 frame,
    stride `w * bpp + 7`, one sentinel row below, 5 s: no padding byte or sentinel byte changes;
    at XRGB8888 every X byte in the frame is 0xFF (init fills the frame black first, as
    `SaverCanvas` does); some pixel is lit.
  - `"an RGB565 swarm shows at least 60% of the light an XRGB8888 one does"`: same seed, same
    3 s: lit-pixel count at RGB565 `>= 0.6 *` the XRGB8888 count.
  - `"glow brightens the swarm"`: two sims, seed 42, 3 s; one `begin_mode(GLOW, 5000, rng)`;
    `step(0, ...)` both: the glowing frame's channel sum is `>= 1.3 *` the other's.
  - `"a fireflies frame dirties a fraction of the canvas"`: 800 × 480, level 0, 10 s: mean dirty
    pixels per frame under 60% of the canvas (a swarm is a clump; one box over the whole flock
    fails this).
- [ ] `make t F='[fireflies_sim]'`: FAIL.
- [ ] Implement the clear, tails, darts and halo.
- [ ] `make t F='[fireflies_sim]'`: PASS.
- [ ] `git commit -m "feat(screensaver): fireflies draw additive tails and darts at both depths" -- src/ui/screensaver_fireflies_sim.cpp tests/unit/test_screensaver_fireflies.cpp`

## Task 7: the ladder and the 1-core bar

The starfield's budget test carries the only cost model the tree has (K1C load gate, cost
dominated by areas and pixels invalidated). Extract it so fireflies is held to the same model
instead of a copy.

**Files:** create `tests/test_helpers/screensaver_cost_model.h`; modify
`tests/unit/test_screensaver_dirty_budget.cpp`; test file.

**Interfaces.** Produces, in `namespace helix::test`: `SAVER_COST_FIXED = 10.1`,
`SAVER_COST_PER_AREA = 0.244`, `SAVER_COST_PER_PX = 6.7e-5`, and
`struct SaverLoad { double areas, pixels; double cost() const; void add_frame(const std::vector<DirtyRect>&); void average_over(int frames); };`
(`cost()` is per 33 ms frame, percent of a core). The starfield file switches to it with no
change to its numbers.

- [ ] Write the failing tests:
  - `"a fireflies level change applies at the next frame and frames allocate nothing"`: for
    each level 0..`FIREFLIES_LEVEL_COUNT` (the last clamps): `request_level`, one step,
    `level()` is the clamped level; `heap_bytes()` equals its value straight after `init` on
    every one of 20 further frames.
  - `"each fireflies rung lights less than the one above"`: same seed and 4 s for every level at
    800 × 480: lit-pixel count strictly decreases from level 1 to 2, 2 to 3 and 3 to 4.
  - `"the fireflies bottom rung fits a 1-core board's 10% budget"`: 800 × 480 RGB565, level
    `FIREFLIES_LEVEL_COUNT - 1`, 240 frames at its period, `SaverLoad` over the dirty areas:
    `load.cost() * 33.0 / FIREFLIES_LEVELS[last].period_ms < 10.0`. Also level 0's scaled cost
    exceeds the bottom rung's (the ladder is ordered).
- [ ] `make t F='[fireflies_sim]'`: FAIL (no helper).
- [ ] Extract the helper; switch the starfield test; tune nothing in `FIREFLIES_LEVELS` unless the
  budget case fails, and then lower `max_flies` or `tail_ms` of level 4 only.
- [ ] `make t F='[fireflies_sim]'` and `make t F='[starfield]'`: PASS.
- [ ] `git add -N tests/test_helpers/screensaver_cost_model.h && git commit -m "test(screensaver): share the K1C frame cost model; hold the fireflies ladder to a 1-core budget" -- tests/test_helpers/screensaver_cost_model.h tests/unit/test_screensaver_dirty_budget.cpp tests/unit/test_screensaver_fireflies.cpp`

The model counts redraw only, not the sim's own CPU. Task 10's bench and device gate are the
real measure; this test exists to fail a rung that grows an order of magnitude.

## Task 8: the saver, registry, settings and translations

**Files:** create `include/screensaver_fireflies.h`, `src/ui/screensaver_fireflies.cpp`; modify
`include/screensaver_registry.h`, `src/ui/screensaver_manager.cpp`,
`ui_xml/settings_display_overlay.xml`, `translations/*.yml`, `ui_xml/translations/*.xml`.

**Interfaces.** `class FirefliesScreensaver : public SaverBase` exactly as the scratchpad
candidate: `type()` returns `ScreensaverType::FIREFLIES`; `canvas_format()` returns
`SAVER_BUILD_CANVAS_FORMAT`; `on_start()` calls `sim_.init(canvas().frame(), rng(), level())`,
logs `[Screensaver] Starting fireflies` at info and the heap at debug, returns true; `on_frame`
calls `sim_.step`; `on_level_request(l)` calls `apply_level(l)` then
`sim_.request_level(this->level())`; `on_stop()` calls `sim_.clear()`; `ladder_size()` /
`ladder_period_ms()` read `FIREFLIES_LEVELS`. `friend class FirefliesScreensaverTestAccess`
exposes `sim_` to the test, as hyperspace does.

Registry: `FIREFLIES = 10` after `FLIP_CLOCK`, row
`{ScreensaverType::FIREFLIES, "fireflies", "Fireflies", SAVER_DEPTH_16 | SAVER_DEPTH_32}` last.
`LEGACY_SCREENSAVER_TYPE_NAMES` stays as it is. Manager: `#include "screensaver_fireflies.h"` and
`screensavers_.push_back(std::make_unique<helix::ui::FirefliesScreensaver>())` after the flip
clock. XML: append `&#10;Fireflies` to both `options` and `options_tag` of `row_screensaver`.

Translations, in each `translations/<lang>.yml` (then regenerated):

| en | de | es | fr | it | ja | pt | ru | zh |
|---|---|---|---|---|---|---|---|---|
| Fireflies | Glühwürmchen | Luciérnagas | Lucioles | Lucciole | ホタル | Vaga-lumes | Светлячки | 萤火虫 |

- [ ] Write the failing tests (`[screensaver][screensaver_fireflies]`, `LVGLTestFixture` where a
  saver starts):
  - `"the fireflies saver draws on a canvas in the build's format with its ladder"`: started,
    canvas `header.cf == SAVER_BUILD_CANVAS_FORMAT`, `level_count() == FIREFLIES_LEVEL_COUNT`,
    `FIREFLIES_LEVELS[0].period_ms == SAVER_FAST_PERIOD`.
  - `"a fireflies level request applies at once, frame period included"`: fixed seed 4,
    `request_level(last)`: `level() == last`, timer period `== FIREFLIES_LEVELS[last].period_ms`,
    a fired frame keeps it active.
  - `"a stopped fireflies saver holds no buffers"`: `heap_bytes() == 0` before start, `> 0` after
    one frame, `== 0` after stop.
  - `"fireflies is registered by name at both depths"`: `find_screensaver_by_name("fireflies")`
    gives type `FIREFLIES`, label `"Fireflies"`, both depth bits.
  - The existing registry, dropdown and gate cases already fail on a missing piece: the dropdown
    test (`test_screensaver_registry.cpp`) on a missing XML option, the en.xml case on a missing
    translation, `savers_missing_for_build_depth` (`test_screensaver_gate_manager.cpp`) on a
    missing factory, and "every registered saver starts" on a broken start.
- [ ] `make t F='[screensaver_fireflies]'`: FAIL.
- [ ] Add the saver, the registry row, the factory and the XML option. Run `make translation-sync`,
  fill the table's strings into the 8 non-English `.yml` files (sync writes English placeholders),
  `make translations`, then `make regen-text-fonts` if `scripts/check_cjk_font_staleness.sh`
  (run by the sync) reports new CJK glyphs.
- [ ] `make t F='[screensaver_fireflies]'`, `make t F='[screensaver_registry]'`,
  `make t F='[screensaver_gate]'`: PASS.
- [ ] `git add -N include/screensaver_fireflies.h src/ui/screensaver_fireflies.cpp && git commit -m "feat(screensaver): Fireflies saver in the registry and settings" -- include/screensaver_fireflies.h src/ui/screensaver_fireflies.cpp include/screensaver_registry.h src/ui/screensaver_manager.cpp ui_xml/settings_display_overlay.xml translations ui_xml/translations tests/unit/test_screensaver_fireflies.cpp` (plus any regenerated font files the regen produced)

## Task 9: build lists, perf tooling and docs

**Files:** `firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt`,
`scripts/screensaver-perf/perf_env.sh`, `config/settings.json.template`, the five docs.

- [ ] `app_srcs_excluded.txt`: add, in sorted position,
  `src/ui/screensaver_fireflies.cpp  # not in the v1 Core+AMS cut` and the same for
  `screensaver_fireflies_sim.cpp`. `scripts/check_esp32_app_srcs.py` is the test.
- [ ] `perf_env.sh#perf_saver_name`: add `fireflies` to the saver list.
- [ ] `config/settings.json.template`: add `fireflies` to `_screensaver_type_comment`'s names.
- [ ] `docs/user/CONFIGURATION.md` (`screensaver_type` values), `docs/devel/ENVIRONMENT_VARIABLES.md`
  (`HELIX_SCREENSAVER_NOW` values), `docs/user/guide/settings/display.md` (a row:
  "**Fireflies** | Coloured swarms of glowing fireflies chase each other through the dark, trailing
  long tails that drift in the wind; now and then a swarm stops, circles, splits in two or joins
  another"), `docs/devel/CHANGELOG_1_1_DRAFT.md` (a user-voice entry next to the Hyperspace one).
- [ ] `docs/devel/SCREENSAVERS.md`: the savers row of "Key files" gains
  `include/screensaver_fireflies.h`, plus a row for `include/screensaver_fireflies_sim.h`; the
  ladder paragraph gains fireflies per `FIREFLIES_LEVELS` (16 ms, 33 ms, thin tails, 120 flies
  with 1.5 s tails at 50 ms, 48 flies with 0.75 s tails at 100 ms); a short "Fireflies" section:
  fixed 10 ms steps so levels change only drawing, per-swarm clear-and-dirty boxes, dithered
  additive light at RGB565, and the clean-room note.
- [ ] `scripts/check_esp32_app_srcs.py` (fails on a `src/` file in neither list), `make check-doc-anchors`
  and `make t F='[screensaver]'`: PASS.
- [ ] `git commit -m "docs(screensaver): Fireflies in the settings, perf tooling and guides" -- <each path above>`

## Task 10: verify, record, prove

- [ ] Bench (`[.][fireflies_bench]`, copy of the hyperspace bench shape: ms/frame per level at
  800 × 480 XRGB8888, `WARN` report). Run it once on thelio and put the numbers in the final commit
  body. Expected on the host: level 4 well under 0.5 ms/frame.
- [ ] Headless recording, pinned socket (CLAUDE.md recipe):

  ```bash
  TREE=$(basename "$(git rev-parse --show-toplevel)")
  export HELIX_SOCK="/tmp/helix-$TREE.sock" HELIX_CONFIG_DIR="/tmp/helix-config-$TREE"
  mkdir -p "$HELIX_CONFIG_DIR"
  HELIX_HEADLESS=1 SDL_VIDEODRIVER=dummy ./build/bin/helix-screen --test -vv \
      --remote-socket "$HELIX_SOCK" > /tmp/helix-$TREE.log 2>&1 &
  ./build/bin/helix-screen ctl -s "$HELIX_SOCK" screensaver fireflies
  ```

  Take `ctl screenshot` at 2 s, 10 s and 30 s (one shot at least during a mode; the log names
  mode starts at debug), plus one at `HELIX_SCREENSAVER_LEVEL=4`. There is no "before" for a new
  saver: show the after shots and say so. Stop the instance by the PID resolved from your own
  socket, never by name. Preston judges the pixels.
- [ ] `make mutate-diff` once over the branch; one `mutation:` line per notable kill in the last
  commit's body.
- [ ] `make full-test-run`.
- [ ] Device gate: `scripts/screensaver-perf/pi3b_gate.sh` with the `fireflies` workload, and
  `embedded_gate.sh` on one 16 bpp board when free, each under a `device:` claim. Pass: the gate
  settles on a rung rather than marking it too heavy at idle on any board where the current maze
  fits, wakeups under 20 ms, p99 under 5 ms. Record the settled level per board in the commit
  body or the hand-off.

## Self-review

- Coverage: every spec bullet maps to a task: scene and counts (2), speeds (3), hue (4), tails and
  wind (4, 6), darts (6), world and camera (Global Constraints, 2, 6), the seven modes (5),
  cheaper variants on the ladder (7), framework checklist (8, 9), both depths (1, 6), gate bar (7,
  10).
- Types: one name per thing (`FirefliesSim`, `FirefliesLevel`, `FirefliesMode`, `FireflyVec`,
  `FIREFLIES_*`); the wrapper reads only `FIREFLIES_LEVELS` and the sim's public API.
- Proportion: the plan fixes the behaviour and the tests; seek law, mode mechanics and drawing
  order are stated as rules, and the bodies are the implementer's.
