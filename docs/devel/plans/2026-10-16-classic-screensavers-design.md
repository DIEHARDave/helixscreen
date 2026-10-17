# Classic screensavers: faithful Pipes, open Maze, Fireflies, Aqua Surface, High Fly

## Intent

HelixScreen's savers recreate Windows-era classics. Preston wants them to look like he
remembers the originals. Today Pipes reads as thin outlined lines, and Maze feels claustrophobic.
Three new savers join them: Fireflies, Aqua Surface 2 and High Fly. Success means someone who
knew the originals recognises each one on sight, and that each saver still passes the existing
CPU gate on the weak boards.

Reference pages:
- 3D Pipes: https://www.screensaversplanet.com/screensavers/3d-pipes-494/
- Fireflies: https://www.screensaversplanet.com/screensavers/fireflies-283/
- Aqua Surface 2: https://www.screensaversplanet.com/screensavers/aqua-surface-2-1644/
- High Fly: https://www.screensaversplanet.com/screensavers/high-fly-1640/

The Pipes and Maze numbers below come from Microsoft's NT OpenGL SDK sample source
(`samples/WIN32/OPENGL/SCRSAVE/{PIPES,MAZE}` in github.com/DigitalMars/dmc). The Fireflies
numbers come from Matt Perry's source (github.com/mpcomplete/fireflies, `scene.cc`).

## Goals, non-goals, anti-goals

**Goals**
- Pipes looks like the original: fat shaded cylinders, mixed joints, real materials, and a screen
  that fills and then dissolves.
- Maze feels open the way the original did. Wall art draws from the user's own gcode thumbnails,
  and distance shading adds depth.
- Fireflies, Aqua Surface and High Fly each exist as a new saver.

**Non-goals**
- ESP32 / K-Touch support. No screensaver compiles there today, and none will.
- New per-saver settings. Each saver has one fixed look; Pipes gets no joint-style setting.
- Pre-rendered video. The two Flash originals were video loops; ours render live.

**Anti-goals** (any of these fails the work even if it runs)
- A saver that the gate marks too heavy at idle on a board where the current Maze fits. See
  "Gate bar" below.
- Pipes that still read as lines, or a Maze that still reads as a tube.
- The screensaver starting a burst of network requests.

## Constraints

- Follow the existing framework in `docs/devel/SCREENSAVERS.md` § "Adding a saver":
  - a `SaverBase` subclass, with pure `*_sim` classes (no LVGL) that draw into a `FrameTarget`;
  - a registry row and a settings option, translated in all 9 languages;
  - allocate in `on_start`, free in `on_stop`, no static buffers;
  - add each new source to `app_srcs_excluded.txt`.
- Every saver draws at both depths (RGB565 for `SCREENSAVER_16BPP_TARGETS`, 32-bit elsewhere).
  Each also provides a degrade ladder (`ladder_size` / `ladder_period_ms` / `on_level_request`) so
  the gate can step it down rather than mark it too heavy.
- Device budget: no wakeup over 20 ms and p99 under 5 ms (`scripts/screensaver-perf/`).
- Motion runs on elapsed time (`MotionClock` / `StepAccumulator`), never on frame count.
- There is no shared 3D helper today. Aqua and High Fly do share one renderer, the heightfield
  column renderer below, because two savers need it. Nothing else is extracted speculatively.

**Gate bar.** *Claude's call:*
- Each saver's ladder bottoms out low enough to fit a 1-core board's 10% budget at idle. That
  means a low frame rate and a coarse scale. For Aqua and High Fly, the last rung is a static
  scene with only the clock ticking.
- Falling back while a print halves the budget is the gate working as designed, not a failure.
- Measure on the Pi 3B (`pi3b_gate.sh`), plus one 16 bpp embedded board (`embedded_gate.sh`)
  when one is free under a `device:` claim. The headless mock run's `ctl` gate numbers are the
  per-branch check.

## 1. 3D Pipes (rework of `screensaver_pipes.cpp`)

**What Preston decided:** Mixed joints, the 16 original materials, the teapot, multiple pipes,
a dissolve clear, and a screen that actually fills. No settings.

**Target behaviour, from the original's source:**
- **Grid:** 16 nodes along the long screen axis. The other axes scale by aspect, and depth
  matches the long axis, so 800x480 gives about 15x9x15.
- **Pipe size:** radius = 1/7 of the cell pitch. A ball joint is √2·r. An elbow is a quarter
  torus whose bend radius equals the pipe radius.
- **Joints:** Mixed. At each turn, a ball 1/3 of the time and an elbow otherwise. A teapot
  replaces a joint 1 time in 1000. Each pipe also gets an end cap at its start and its end.
- **Growth:** pipes prefer to go straight, weighted `1 + rand(4)`, and 1 time in 20 much more
  strongly. A turn picks the direction with the most empty nodes, looking up to 2 nodes ahead.
  A pipe that gets stuck ends, and a new one starts at a random empty node.
- **Pipes at once:** 2-4, always multiple-pipe mode.
- **Per screen:** about 7-8 pipes in total (5 × 1.5), or until no empty nodes are left.
- **Materials:** each pipe takes one of the 16 SGI materials, with their real
  ambient/diffuse/specular/shininess from `COMMON/MATERIAL.C`:
  - emerald, jade, pearl, ruby, turquoise
  - brass, bronze, copper, gold, silver
  - cyan, white and yellow plastic
  - cyan, green and white rubber
- **Lighting:** one fixed directional light, with a specular highlight that sits consistently
  across pipes and joints. The highlight is not a screen-space stripe.
- **Camera:** perspective, about a 90° FOV, looking straight at the grid. Each new screen rotates
  the view a further ~9.7° around Y.
- **Clear:** a blocky digital dissolve, random-order black blocks over about 2 s, then a new
  screen.
- **Depth:** a near piece always covers a far one. The current saver paints in creation order and
  gets this wrong.

**Left to the builder:** how to draw the shaded cylinders cheaply (per-material pre-shaded
gradient spans, cached elbow and ball sprites, a z-buffer, or painter's order by node depth),
and whether to keep `lv_draw_*` or move to direct `PixelWriter` rendering.

## 2. Maze (changes to `screensaver_maze_sim.cpp` / `screensaver_maze.cpp`)

**What Preston decided:** match the original's field of view, and also add loops and small rooms.
Wall art draws from recent print thumbnails, any file thumbnails, and the printer picture. He
also wants distance shading, if it is cheap.

**Field of view.** The original kept our proportions: a 12x12 maze, 1x1 corridors, walls as tall
as a corridor is wide, and the eye at mid-height. What made it feel open was a 90° vertical FOV;
ours is about 53°. Widen the vertical FOV to match the original. The horizontal FOV follows the
screen aspect, so it ends up near 120° on 800x480. If that much fisheye looks wrong at the edges,
the builder may cap the horizontal FOV and must say what they chose. Grid, wall height and eye
height stay as they are.

**Loops and rooms.** After generating the perfect maze, knock out some interior walls ("braid" it)
so there are loops and longer sightlines, and carve 2-3 small open rooms (2x2 or 2x3).
- The walker must still reach the exit, and must not circle a loop forever. *Claude's call:* keep
  wall-following, which is the original's look. Keep the rocks' hand swap. The walker remembers
  the (cell, heading) states it has seen since the last swap. On a repeat it has latched onto an
  interior island, so it walks the shortest path to a wall on the boundary component, grabs it, and
  carries on. Tests:
  - every generated maze, over many seeds, reaches the exit within a bound;
  - a constructed island case recovers.
- Braiding leaves the start and exit cells as dead ends.
- The start and exit placements still hold.
- The flipping rocks, the Helix-logo sprites and the toolhead still place correctly.

The exact counts are the builder's call. They should make it noticeably more open without losing
the corridor character.

**Distance shading.** Darken walls, floor and ceiling with distance: a linear or exponential fall
toward a dark tint, from a precomputed table indexed by depth, so it costs no per-pixel divide.
Sprites get the same factor. It must hold on the flat (untextured) ladder levels too.

**Wall art.** The picture walls take images from three pools, mixed:
- thumbnails of recently printed files (print history);
- thumbnails of other gcode files on the printer;
- the printer picture (the existing source).

Raise the number of picture walls so the art is noticeable; the count is the builder's call, and
6 today is too few.

*Claude's call:* the saver uses only thumbnails already in the local thumbnail cache
(`ThumbnailCache` / the cache dir). It never fetches when it starts, because a saver start must not
cause network traffic. If the cache is empty, it falls back to the printer picture, and then to
brick, as today. Decoding stays in `on_start`, under a bounded total memory budget.

## 3. Fireflies (new saver, `fireflies`)

*Claude's call, not discussed:* recreate Matt Perry's Fireflies, which the store page describes
(flies in coloured swarms chasing points). His repo is GPL-2.0 with no "or later" grant, so it
cannot be copied into this GPL-3.0-or-later tree. Build it **as a clean reimplementation from the
behaviour and the published numbers only**. No code is translated from `scene.cc` and the other
files. The reviewer checks this.

- **Scene:** a black background with additive blending. 2-5 invisible baits, and 100-175 flies
  that chase them in clumps.
- **Speeds:** baits at speed 50 / accel 600, flies at speed 100 / accel 300.
- **Colour:** each swarm cycles its hue at 15°/s, and each fly shifts ±20° of hue with its speed.
- **Tails:** each fly trails a 2.25 s tail, widest and brightest in the middle, that fades with age
  and is bent by a "wind" that changes every 4-8 tail-lengths.
- **Bodies:** small dart shapes.
- **Space:** a world 80 units tall × aspect-scaled wide × 50 deep, seen through a perspective
  camera.
- **Modes:** a subset of the original's random modes: stop, loop, glow, hyperspeed, split, merge,
  wind gust.

Cheaper variants (fewer flies, shorter tails, or a framebuffer fade in place of explicit tails)
belong on the ladder, not in the default look.

## What the two Flash originals actually look like

These come from the store's own screenshots (`img/screenshots/screensavers/aqua-surface-2-{1,2,3}.png`,
`high-fly-{1,2,3}.png` on screensaversplanet.com).

**Aqua Surface 2 is not a top-down pond.** It is a macro *side view* of a sloshing water surface
at eye level, like a high-speed camera looking through the wall of a tank.
- The scene is monochrome: silver and white on pure black.
- A crumpled, bright, streaky band of surface snakes across the screen. In places it rears up into
  tall crests and curling lips, and in others it dips into deep troughs.
- Above the band is black, with the odd flung droplet.
- Below the band is near-black water with scattered tiny specks (bubbles and particles).
- **The clock** sits in the lower middle, in the water:
  - three thin ring gauges in a triangle. The top ring is small. The bottom left ring holds the
    hour ("01") and the bottom right ring the minute ("09"). Each ring's arc fills partway.
  - under the rings, a small caps date: "21 OCTOBER 2019, MONDAY".

**High Fly** sits just above a cloud deck.
- **Sky:** a deep, saturated blue at the top, with a bright soft glow and light shafts pouring down
  from the top centre (the sun is above).
- **Clouds:** a bank of billowy cumulus fills the bottom ~40%. Its top edge is a soft, lumpy
  silhouette, its crevices are blue-grey, and the near clouds glow almost pure white at the bottom
  of the screen.
- **Clock:** large, thin, pale blue-white numerals ("10:40") sit at the cloud line, slightly
  translucent. Small seconds ("29") sit superscript to the right, with a tiny "PM". The date sits
  on the left in two small lines: "OCTOBER 14, 2019" and "MONDAY".

## Prototypes

Both scenes were prototyped in Python, with the algorithms below, at the device's internal
resolution of 400x240 upscaled to 800x480. Preston watched the results and made two decisions:
- **Water:** go with the tuned glassy water.
- **Clouds:** lock the approach and polish it in the build.

The scripts and videos are in the main tree, at `docs/superpowers/spikes/screensavers/` (gitignored,
local only):
- `water/aqua2.py` and `water/water2.mp4`;
- `clouds/highfly2.py` and `clouds/clouds2.mp4`;
- `reference_originals.png`, the store's screenshots.

**Read the scripts before writing the C++.** Every parameter called out below was found there by
iterating against the reference. Nothing was timed on ARM; the per-frame counts quoted are from the
prototypes.

## Shared renderer: the heightfield column renderer (Aqua + High Fly)

Both scenes are a *surface seen at a grazing angle from just above it*: water in one, cloud tops
in the other. One renderer draws both: a **voxel-space (Comanche-style) column renderer** over a
heightfield, as a pure `FrameTarget` sim class.

**How it works:**
- The camera sits just above the surface's mean height, looking almost horizontally.
- Depth slices run front to back, with the step size growing with distance (`dz *= ~1.02`).
- For each slice and each screen column:
  - sample the surface's height and shade at that world point (**bilinear**: nearest aliases badly
    at this scale);
  - project the height to a screen row;
  - if that row is above the column's current top (a per-column y-buffer), fill the span between
    them, then raise the top.
- The cost is bounded by `columns × slices` samples plus the fills, and does not depend on how busy
  the scene is.
- Per-slice constants go in tables built once per frame: the inverse depth, the fog factor, and the
  world-space left and right ends. The inner loop has no divide.
- The prototypes used **400 columns × 110-118 slices**.

**Lessons from the prototypes that the renderer must support:**
- **Shade gradient along each span.** Interpolate from the previous slice's colour to this slice's
  colour along the span. Flat spans streak on steep faces. Interpolate *only* when the previous
  slice also drew that column; otherwise near colours smear upward as "icicles".
- **Soft top edge, two ways:**
  - *Clouds* use front-to-back "under" compositing for the top F px of each new span (F ≈ 10 px at
    400x240), with the y-buffer rising only to row+F. This writes about 2.7× the pixels of a hard
    edge (about 255k blends a frame), and it is what makes cloud edges soft.
  - *Water* feathers once over the final silhouette instead: fractional coverage on the top pixel
    plus a 3 px horizontal smoothing of that edge. Feathering every slice put a dark stripe on thin
    spans.
- **Height-grid indexing:** for water, the grid is indexed by screen column (x/z) and depth, not by
  world x. World-x indexing left the near cells about 13 px wide, and they showed facets.
- **Near-plane handling:** for clouds, heights are scaled down by a smoothstep over the first ~40
  depth units ("sink"), so near clouds pass under the camera instead of being cut into walls. Water
  avoids the problem with a telephoto camera (below).

*Builder's hint, not a rule:* write spans into a column-major scratch buffer so that each span is
contiguous, and transpose during the blit or the upscale.

**Rejected alternatives:**
- A top-down height-field ripple sim: the wrong picture.
- Ray-marched volumetric clouds: several times the budget.
- A flat floor-cast cloud texture: a carpet, with no silhouette.
- Sprites alone for the clouds: overdraw has no bound. The clouds do use a few *full-width* layers
  on top of the deck, which are bounded.
- Live spray and procedurally pre-rendered splash sprites for the water crash: both prototyped.
  They read as "hairy tufts" and "shark fins" respectively.
- Shipping pre-rendered splash footage: Preston declined it, for the install size.

## 4. Aqua Surface (new saver, `aqua`)

**Preston's call:** the original's chaotic slow-motion splash is almost certainly filmed footage,
and the live approach cannot reproduce it. Aqua is therefore **our own take: calm, dark, glassy
sloshing water with crisp, sparse specular highlights**, tuned *between* the two prototype passes.
Pass 1 was too shiny all over, like chrome satin. Pass 2 was realistic but a bit dark and oily,
with highlights forming smooth isophote rings.

**Camera:** telephoto, which is what removed the "black hump" where the near edge towered over the
surface behind it. The prototype settings were:
- near edge at depth 3.5, 4.0 units deep;
- focal length 500 px at 800 wide;
- camera height 0.8;
- horizon about 45 px above centre.

**Water surface:** the height is an analytic sum, evaluated each frame into a grid (the
prototype's grid was 160 × 64), with a sin lookup table. It sums three things:
- 2-3 **tank sloshing modes** along x, `A·cos(kπx/L)·cos(ωt+φ)`, with ω from `ω² = g·k·tanh(k·depth)`;
- 3-5 **steep Gerstner waves** at mixed angles. The prototype inverted their horizontal
  displacement with two fixed-point passes so they evaluate on a regular grid. That triples their
  cost and makes them the biggest per-frame item, about 150k trig lookups. A cheaper evaluation is
  the builder's call, as long as the crests stay peaked;
- occasional **impulse events**, a Gaussian bump that splits into two travelling pulses, kept at
  least ~1.2 units behind the near edge so they don't hide behind the front face.

Two more settings apply to the surface:
- **Slow motion** comes from a time scale. The prototype used 0.25, so one big slosh takes several
  seconds.
- **Fine wrinkle** is *one gentle octave*, on sim time and anisotropic, faded with distance and
  sampled bilinearly. More than that is the main source of shimmer.

**Shading:**
- **Reflectance:** real water fresnel, F0 ≈ 0.02 rising toward grazing. The prototypes span the
  range: pass 1 put a 0.35 floor under fresnel and was too shiny, and pass 2 used pure F0 0.02 and
  was a bit dark. Tune between them.
- **Reflection:** an environment lookup, a 1D table indexed by the reflected ray's elevation, with
  about 4 narrow "softbox" bands on a black ceiling.
- **Anti-shimmer:** the environment table is **pre-blurred at about 6 levels**. Each sample picks a
  level by how fast the reflection angle changes across neighbouring pixels. This, plus the single
  wrinkle octave, cut the frame-to-frame change from 10-14 to about 4 grey levels. Keep that
  measure as the shimmer check.
- **Colour:** greyscale, expanded through a near-neutral, very slightly cool tint table. Use
  dithered writes at 16 bpp.
- **Translucency:** thin, grazing sheets render a little greyer and darker, as if seen through. It
  was subtle in the prototype.

**Near-edge rim:** a bright antialiased meniscus line 3-5 px wide follows the near-edge silhouette
**at all times**. Its brightness follows the surface just behind it, with a floor of 0.4.

**Below the near edge:**
- the fill is near-black;
- a faint, downward-fading smear of the edge's brightness;
- ~300 bubble specks drifting up.

**Splash events:** modest. Occasional droplets and short streaks flung from fast-rising crests,
following slow, low-gravity (~0.35 g) ballistic paths and drawn with max-blend. They are an
accent, not the main event. Keep them sparse enough that they never read as hair.

**Preston's rule: a droplet never vanishes in mid-air.** Every particle lives until it falls back
to the water surface:
- it is removed only when its height drops below the surface height at its own (x, depth);
- it never expires on a lifetime timer;
- it makes a small, brief glint or ripple speck where it lands.

Capping the particle budget therefore means limiting how many droplets are *emitted*, never
culling live ones. A test pins this: simulate many splashes, and no particle may be removed while
it is above the surface.

**Repaint:** only the water band and the clock region change each frame.

**Clock:** the reference layout.
- Three ring gauges in a triangle, with the hour inside the bottom-left ring and the minute inside
  the bottom-right ring.
- *Claude's reading of the rings:*
  - the hour ring fills through the 12 h or 24 h day;
  - the minute ring fills through the hour;
  - the small top ring fills through the minute (seconds).
- A localised date line under the rings, through `locale_formats`.
- Antialiased arcs.

## 5. High Fly (new saver, `highfly`)

**Preston's call:** lock the prototype's approach, and add his request for **occasional tall,
fluffy clouds and an occasional fly-through a cloud**. The remaining polish happens in the build.

The approach is a hybrid of three parts, drawn back to front:

1. **Static sky**, rendered once in `on_start` and never repainted:
   - a vertical gradient from deep blue down to a paler band at the horizon;
   - a radial glow at the top centre;
   - **4 wide, faint shafts** (0.16-0.22 rad wide, strength 0.05-0.10). Seven narrow ones read as
     a sunburst.
2. **Far billow band:** a tileable strip (the prototype's was 1024 × 90) composited into the sky
   behind the deck and drifting sideways. It is what **guarantees the skyline is never flat**.
3. **Cloud deck:** the shared renderer over a precomputed, tileable cloud-top heightfield (about
   256²).
   - **Building it:** splat soft puffs at 2-3 scales, then run a **talus pass**. Sphere caps leave
     vertical cliffs, which the renderer draws as curtains.
   - **Shading**, baked per texel from three parts:
     - Lambert from a sun above and slightly behind;
     - **ambient occlusion**: height minus blurred height, plus a fine AO term at blur radius 2.
       This is what gives the blue-grey crevices;
     - noise.
   - **Camera height sits below the tallest tops.** This is what makes the skyline lumpy.
   - **Fog:** near clouds go to near-white, far ones to the horizon blue. A static white haze
     gradient covers the bottom ~22%.
   - **Motion:** the deck scrolls toward the camera.
4. **Translucent layers:** three procedural tileable textures (512 × 128 in the prototype), built
   from gaussian blobs with noise-torn edges. They are lit by light accumulated from above, with a
   glow on thin edges.
   - Each is a full-width billboard that recycles from far to near, fading at both ends.
   - Each is depth-tested per pixel against the deck, so the layers sit between billows.
   - They give the soft, wispy look the voxel deck can't.
5. **Towers:** tall cumulus billboards that rise well above the deck. The prototype spawned one
   every 13-14 s of travel, with 1-2 on screen at a time, sliding out sideways as they near. The
   build must improve them:
   - bake **2-3 shape variants** at start, because one texture repeats visibly;
   - give them **crisp, sunlit white crowns** with strong top lighting and less fog. The prototype
     towers came out mottled and bluish, like lumpy snowmen.
6. **Fly-through:** about once every 60-120 s, the camera flies into a centred cloud.
   - The screen eases toward a blue-grey interior, capped at about 62% opacity so a pale clock stays
     readable, with radial wisps streaming outward.
   - It holds for about 0.5 s and clears over about 1.3 s.
   - The build fixes two prototype faults:
     - the wisps need **bilinear sampling or a precomputed polar lookup table**, because nearest
       sampling showed blocky radial seams;
     - the cloud body looks flat at close range and needs more depth.

**Cost per frame, from the prototype:**

| Part | Ordinary frame | Fly-through peak |
|---|---|---|
| Deck samples | 47k | 47k |
| Deck span blends | ~255k | ~255k |
| Layer, band and tower blends | ~116k | ~316k |

On top of that comes the upscale to 800x480. Ladder levels drop the layers before the deck. The
last rung is the static scene with the clock ticking.

**Repaint:** the cloud band and the clock region. The fly-through repaints the whole screen while
it lasts.

**Clock:** the reference layout.
- Big thin numerals. Draw them with the flip clock's stroke digits at a thin stroke width, which
  scales to any size and matches the light weight; that is a builder check.
- Seconds superscript, and a small AM/PM.
- A two-line localised date on the left.

## Shared clock face (Aqua + High Fly + Flip Clock)

The three savers make one decision: what the readout says. They each draw it differently. That
decision moves into one pure function or class that all three call, extracted from
`FlipClockState` / `clock_face` / `flip_clock_now`. It returns these fields:
- the primary time (12 h or 24 h, with AM/PM);
- seconds;
- localised date parts;
- the print state: idle or printing, with progress, time remaining and ETA.

`FlipClockInput` has no date today, so the extraction adds one. The flip clock's tests must still
pass once the flip clock is on the extracted code.

**During a print**, *Claude's call:* the time of day stays the main readout in the two new savers.
The print shows as secondary content:
- in **Aqua**, the small top ring becomes print progress, and the date line becomes
  "ETA 9:41 PM · 1h 12m left";
- in **High Fly**, the date block on the left becomes the same two lines.

The flip clock keeps its current behaviour of replacing the time with the countdown. Burn-in drift
(`flip_drift_at`) applies to the new clocks too.

## Delivery shape

*Claude's call:* this is four parallel branches. Each gets its own review, and they land as a
batch with one land gate:
- pipes;
- maze;
- fireflies;
- the shared clock extraction, the heightfield renderer, Aqua Surface and High Fly. These go on
  one branch, since both savers depend on the clock and the renderer. This branch is the largest;
  its plan may split it into stacked commits, clock and renderer first.

Each branch shows before and after screenshots, or a short recording, from a headless mock run
(`ctl screensaver <name>`). Preston judges how the pixels look.

## Left to the builder

- Rendering technique and data structures inside each sim.
- The exact ladder levels and their periods.
- Braid percentage, room count and size, the number of picture walls, and the fog curve and
  colour.
- Cloud and water texture generation.
- Fonts and sizes for the two new clocks, within the existing `font_fitting` machinery.
