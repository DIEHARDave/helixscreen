# Classic screensavers: shared clock, heightfield renderer, Aqua Surface, High Fly

**Goal:** One shared clock-readout decision (flip clock, Aqua, High Fly), one pure voxel-space
heightfield column renderer, and two new savers on it: `aqua` (Aqua Surface) and `highfly`
(High Fly).

**Architecture:** Pure, LVGL-free `*_sim` classes draw into a `FrameTarget`; thin `SaverBase`
wrappers read the clock and the print and draw the few text lines with `lv_draw_label`. Both
new scenes render at canvas/2 (400x240 on 800x480) through `HeightfieldRenderer`, then
upscale, then draw output-resolution overlays (rim, droplets, bubbles, clock).

**Tech stack:** C++17, Catch2 (`tests/unit/`), `PixelWriter`, `MotionClock` /
`StepAccumulator`, `locale_formats`, `lv_tr`.

**Spec:** `docs/devel/plans/2026-10-16-classic-screensavers-design.md`, sections "What the two
Flash originals actually look like", "Prototypes", "Shared renderer", "4. Aqua Surface",
"5. High Fly", "Shared clock face". Framework: `docs/devel/SCREENSAVERS.md` § "Adding a saver".

**Prototypes (read before Tasks 3-5):** main tree, gitignored:
`/home/pbrown/Code/Printing/helixscreen/docs/superpowers/spikes/screensavers/water/aqua2.py`
(+ `water2.mp4`), `clouds/highfly.py` (deck, sky, field), `clouds/highfly2.py` (layers, band,
towers, fly-through; it imports highfly.py and overrides `shafts` and `haze`),
`reference_originals.png`. Every value in "Global Constraints" was copied from them.

**Candidate headers (syntax-checked, see end):**
`/tmp/claude-1000/-home-pbrown-Code-Printing-helixscreen/41e9fa45-f64c-4947-864e-448345f897fc/scratchpad/plan_headers/sky-water/`
holds `screensaver_clock_reading.h`, `screensaver_stroke_digits.h`, `screensaver_clock.h`,
`screensaver_heightfield.h`, `screensaver_aqua_sim.h`, `screensaver_aqua.h`,
`screensaver_highfly_sim.h`, `screensaver_highfly.h`, and the revised flip clock headers
`flip_clock_sim_revised.h` / `flip_clock_revised.h`. Copy each into `include/` as the task's
starting header; the signatures below are quoted from them.

## Dependencies on the other branches

- **Enum values are pre-assigned:** `FIREFLIES = 10` (fireflies branch), `AQUA = 11`,
  `HIGHFLY = 12` (this branch). `SCREENSAVERS` rows must run in type order from 1
  (`test_screensaver_registry.cpp` "registry rows run in type order from 1"), so this branch's
  registry commit (Task 4.1) **stacks on the fireflies branch's registry commit**. Rebase onto
  it before Task 4.1, or the contiguity test fails on a gap at 10. The batch landing merges
  the fireflies branch first.
- The fireflies branch moves `hue_color` and `add_light` into `include/screensaver_pixel_writer.h`
  (the RGB565 `add_light` gains an optional Bayer dither). Nothing here needs hue cycling. Aqua's
  droplets, rim and bubbles use `PixelWriter::blend_max`. If an additive splat looks better on
  bubbles during tuning, consume `add_light` from `screensaver_pixel_writer.h`; do not write
  another copy.

## Decisions made here (the spec's "Claude's call" / "left to the builder")

1. **Shared names use a `Clock` prefix.** Moved out of the flip clock: `FlipPrintPhase` →
   `ClockPrintPhase`, `FlipClockPrint` → `ClockPrint`, `FlipClockInput` → `ClockInput`
   (it gains `ClockDate date`), `FlipMode` → `ClockMode`, `FlipMeridiem` → `ClockMeridiem`,
   `FLIP_DONE_HOLD_MS` / `FLIP_COUNTDOWN_RESYNC_S` / `FLIP_MAX_COUNTDOWN_S` /
   `FLIP_DRIFT_STEP_MS` → `CLOCK_*`, `flip_drift_at` → `clock_drift_at`, `flip_clock_now` →
   `saver_clock_now`, `flip_print_phase` → `clock_print_phase`. No aliases are left behind.
   `test_screensaver_flip_clock.cpp` changes **by identifier rename only**. Its assertions and
   values are untouched, and that is the proof the behaviour did not change (Review Focus 1).
2. **The countdown state machine is shared.** The anchor, resync, pause and done-hold logic
   moves from `FlipClockState::step` into `ClockState::step` verbatim. `FlipClockState` becomes
   a `ClockState` plus `flip_face(reading)`. Aqua and High Fly then show the same smoothed
   remaining time as the flip clock.
3. **Print text.** Two new keys, `"ETA {}"` and `"{} left"` (duration from
   `helix::format::duration`, e.g. "1h 12m"). Aqua joins them as `"ETA 9:41 PM · 1h 12m left"`;
   High Fly puts them on two lines. PAUSED reads `"PAUSED"` + left; DONE reads `"DONE"`
   (existing keys).
4. **Date text.** `locale_formats` gains `format_localized_long_date(const tm*)` ("October 21,
   2019") and `format_localized_weekday(const tm*)` ("Monday"), with full-name fallback tables
   for all 9 languages. Aqua's line is `"<long date> · <weekday>"`. High Fly's left block is
   the long date over the weekday. Text is drawn in the language's natural case: uppercasing
   non-ASCII UTF-8 is not available, so the reference's small caps is dropped.
5. **Stroke digits shared.** `glyph_distance` and the glyph table move to
   `screensaver_stroke_digits.{h,cpp}`; `build_stroke_digits(unit, stroke, out)` is
   parameterised by stroke width. **Feasibility of thin High Fly digits: yes.** The distance
   field is stroke-independent. Only the box padding (`1 + stroke`) and the coverage threshold
   (`stroke / 2`) use the stroke, and both are parameters. Thin strokes stay inside the box.
   Cost is one distance per glyph pixel per digit at `on_start`: about 0.15 M evaluations for
   150 px numerals, a few ms, never per frame.
6. **Text over repainting scenes.** A shared `SaverLabel` (lifted out of
   `FlipClockScreensaver::draw_text` together with its `font_fitting`, renamed
   `clock_font_fitting`) redraws a line when its text or box changes or when the frame's
   dirty boxes cross it. Both new canvases use `PIPES_CANVAS_FORMAT`, for the reason the flip
   clock does.
7. **Ring readings:** hour ring = (h mod 12 + m/60 + s/3600) / 12, or h/24 in 24-hour mode;
   minute ring = (m + s/60) / 60; top ring = s/60, or print progress in
   COUNTDOWN/PAUSED, and 1.0 in DONE.
8. **Gerstner evaluation:** one fixed-point inversion pass instead of the prototype's two,
   with sin/cos from a 1024-entry table. The crests must stay peaked, which a test pins
   (Review Focus 4).
9. **Fresnel:** `F = max(f0 + (1 - f0)(1 - cos)^5, AQUA_FRESNEL_FLOOR)` with
   `AQUA_FRESNEL_FLOOR = 0.12`, a starting value between the two passes (0.35 too shiny, none
   too dark). Preston tunes it by eye from the screenshots.
10. **Droplets:** emission is evaluated on a fixed 50 ms real-time step (`StepAccumulator`),
    not per frame. A droplet's z is reflected back into the tank `[z_near, z_near + D]`, never
    culled. Gravity is 0.35 g in sim time, per the spec (aqua2 used 0.45). There are no lip
    sheets, because the spec rejects them as "hairy tufts".
11. **Upscale:** scale 2 is bilinear, any other scale replicates. A 16 bpp target dithers.
12. **Text layout sizes** (all × canvas height/480, then rounded) are listed in Tasks 3.4 and 4.4.
13. **Memory budget:** Aqua ≤ 1.5 MB and High Fly ≤ 4.5 MB at 800x480, all freed in
    `on_stop`.

## Spec errors found against the prototypes

- **Aqua focal length.** The spec says "focal length 500 px at 800 wide". In aqua2.py the
  focal is 500 px at the **400-px internal width** (`cols = arange(W) - W/2` with W = 400, and
  the overlay uses `f = focal * 2` at 800). This plan uses 500 × (internal_w / 400).
- **Aqua horizon.** The spec says "horizon about 45 px above centre". That is pass 1
  (aqua.py `pitch_px=-45`). The tuned pass 2 has `pitch_px=-92` at 240 rows, which with
  focal 500 puts the near edge about 59% down the screen. This plan uses −92 × (internal_h /
  240). Tell Preston if the screenshot looks wrong.
- **Impulse placement.** aqua2.py spawns impulses at z0 + U(0.8, 2.4). The spec says "at least
  ~1.2 units behind the near edge", so this plan uses z0 + U(1.2, 2.4).
- **aqua2.py breaks Preston's droplet rule.** It removes spray on a lifetime (`sp[:,6] > 0`)
  and culls to the last 1800 (`[-1800:]`). Do not port either.
- **Fly-through clear.** The prototype clears over 9 units at speed 10 (0.9 s). The spec says
  about 1.3 s, which this plan uses.
- **Softbox bands.** The spec says "about 4"; aqua2 has 3 plus a ceiling ramp. This plan adds
  a 4th starting band (below).

## Global Constraints (verbatim values)

**Framework:** `SaverBase` wrapper + pure sim; allocate in `on_start`, free (swap with empty)
in `on_stop`; no static buffers (`constexpr` tables only); both depths; ladder level 0 at
`SAVER_FAST_PERIOD`; motion on elapsed time only; device budget no wakeup > 20 ms, p99 < 5 ms;
each new `src/ui/screensaver_*.cpp` listed in
`firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt`.

**Renderer:** front-to-back slices, `dz *= grow`; bilinear sampling; per-slice tables
(z, 1/z, dz, sink, fog, world x of column 0, world x per column) built once per view, no
divide in the inner loop; cost = columns × slices samples + fills.
- Span gradient from this slice's colour (top) to the previous slice's (bottom), only where
  the previous slice drew the column.
- Clouds: SOFT_UNDER, F = 10 px at 240 rows, y-buffer rises only to row + F.
- Water: FEATHERED_SILHOUETTE: fractional coverage on the top pixel = (r0 + 1) − top_f, then
  [1 2 1]/4 horizontal smoothing over rows r0−1..r0+1.
- Water grid indexed by (u = x/z, z), not world x.
- Clouds sink: h × smoothstep((z − z_near)/40), sink_min 0.

**Aqua (aqua2.py, at internal 400x240 unless scaled):**
- Grid 160 × 64 over u ∈ ±(W/2 + 8)/focal, z ∈ [3.5, 7.5]. 110 slices, z_near 3.5, depth
  4.0, grow 1.015 (so dz0 = 4·0.015/(1.015¹¹⁰ − 1) ≈ 0.01448).
- focal 500 (× w/400). cam_h 0.80. horizon row = h/2 − 92·(h/240). Time scale 0.25.
  g = 9.81, depth 1.2, L = 7.0.
- Tank modes n = 1, 2, 3, A = 0.34, 0.20, 0.10, k = nπ/L, ω = √(g k tanh(k·1.2)), phase
  U(0, 2π). Term: A cos(k(x + L/2)) cos(ωt + φ).
- Gerstner (λ, A, angle rad, Q): (2.6, 0.13, 0.15, 0.9), (1.7, 0.08, −0.5, 0.9),
  (1.1, 0.045, 0.9, 0.85), (0.75, 0.02, −1.2, 0.8). k = 2π/λ, ω as above. Inversion:
  p0 = p − Σ Q A d cos(k d·p − ωt + φ), then height += A sin(k d·p0 − ωt + φ).
- Impulses: first at t = 0.6, then every U(2.0, 4.0) sim s, x U(−2.5, 2.5), z z0 + U(1.2, 2.4),
  A U(0.5, 0.9). Removed at age 12. Contribution: env = A e^(−age/3),
  rise = min(1, age/0.6), σ = 0.22 + 0.18·age, c = 1.3·age,
  gz = e^(−((z − ze)/1.0)²), term env·rise·0.5·gz·(e^(−((x − xe − c)/σ)²) + e^(−((x − xe + c)/σ)²)).
- Normals: Gx = ∂H/∂u / z, Gz = ∂H/∂z − (x/z)·Gx. Thin = clamp((H − box9_x(H))/0.12, 0, 1).
- Wrinkle: one octave. A 64² slope table from band-limited noise (spectrum k⁻², anisotropy
  kx × 6, band 0.03 < k < 0.3, normalised to max |slope| 1), sampled bilinearly at
  (x·4 + t·0.4, z·14 − t·0.6). Amplitude 0.7 × clamp(1.4 − 1.6(z − z0)/D, 0.1, 1.0).
- Shading: v = norm(x, h − cam_h, z), cos = clamp(−v·n), r = v + 2 cos n, f0 = 0.02 (floor
  per decision 9). Environment table of 512 over elevation [−1, 1]: smoothstep plateau bands
  box(c, w, a, soft) = a·smooth(clamp((w − |e − c|)/soft)) at (0.07, 0.022, 0.9, 0.025),
  (0.22, 0.026, 1.0, 0.025), (0.45, 0.035, 0.9, 0.03), plus a 4th starting band
  (0.70, 0.03, 0.7, 0.03), plus 0.05·clamp(e, 0, 1).
- Pre-blurred environment levels σ = 0, 0.008, 0.016, 0.032, 0.064, 0.128 (elevation units).
  level = clamp(log2(max(foot, 1e-4)/0.02) + 1, 0, 5), linear between levels, where
  foot = max(|Δel| to the next column, |Δel| to the next slice).
- Sun Ld = norm(−0.3, 0.75, −0.6), spec = clamp(r·Ld)^120.
- trans = (1 − F)(0.05 + 0.22·thin); shade = F·refl·4.0 + 1.5·spec + trans; × fog with
  fog = (1 − (z − z0)/D)^0.6 (renderer fog falling, power 0.6).
- Output grey g^1.05 × tint (0.94, 0.98, 1.03), dithered at 16 bpp (tint table of 256
  entries built at start).
- Below the edge: first slice not filled. Pixel = box15(front shade)·0.25·e^(−d/18) + 0.008,
  d = rows below the edge (internal px).
- Rim (output res): brightness clamp(0.40 + 1.2·box5(front shade), 0, 1). Max-blend rows at
  offsets −1, 0, 1, 2, 3, 4 from the edge with weights 0.4, 1.0, 0.7, 0.4, 0.2, 0.1 (× h/480),
  bilinear across columns. Present every frame.
- Bubbles: 300. x U(0, W), y U(0, H), speed U(0.15, 0.6), phase U(0, 2π). Rise speed·24 px/s
  (× h/480). x wobble 3·sin(phase) px, phase += 2 rad/s. Respawn at H + U(0, 40) when
  y < 0. Drawn only where y > edge + 6, brightness 0.10 + 0.40·speed.
- Droplets: candidates are grid cells with z < z0 + 3.0 and score = dH/dt + 2·max(H, 0) > 2.6.
  Per 50 ms emission step, pick ≤ 3; each pick throws 1-3 droplets with probability 0.15.
  lean = −sign(Gx)·0.35. v = (1.5·lean + N(0, 0.3), 0.6·max(dH/dt, 0) + U(0.8, 1.5),
  N(−0.1, 0.1)). Gravity 0.35 g, drag v *= (1 − 0.3·dt), sim time.
- Droplet draw: streak back along velocity (× 0.035 s of travel), 10 segments, taper
  (1 − u)^1.5, depth fade clamp(1.2 − (z − z0)/D, 0.3, 1), blend_max. Landing glint lasts
  250 ms real (a glint is not a droplet). Cap AQUA_MAX_DROPLETS = 48 live: emission refuses,
  nothing is culled.
- Shimmer check: mean |Δgrey| between consecutive 50 ms frames ≈ 4 grey levels on the water
  body.

**High Fly (highfly.py P + highfly2.py Q, at internal 400x240):**
- Horizon 0.56·H, cam_h 15.0, focal_y 150 (× h/240), half-width tan fov 1.0 (focal_x = (w/2)/1.0).
- Slices z_near 10, z_far 350, grow 1.025, 117 slices (dz0 ≈ 0.501). Feather 10 px.
  Speed 10 units/s. Fog start 25, end 300, power 0.8. near_white 40, near_k 0.7. Sink 40.
- Field: xs = 2.0, so texel = world × 2 and height = sample / 2. Column world x = 64 + cols·z
  with cols = (x − w/2 + 0.5)/(w/2)·fov; world y = −t·speed − z.
- Field 256², tileable, floor 0. Built in this order:
  - 30 big puffs: r U(18, 34), sphere centre height U(−16, 8);
  - 170 lumps: r U(8, 16), base = h(c) − r·U(0.55, 0.85);
  - 1500 small: r U(2.5, 5.5), base = h(c) − r·U(0.5, 0.8);
  - value-noise octaves 16/32/64, amplitude 1/0.5/0.25, normalised by std, scaled by
    1.5 × clamp(h/10, 0, 1);
  - talus: 20 iterations of h = max(h, max4(neighbours) − 2.0);
  - box blur radius 1, 3 passes, wrapping.
- Field shade:
  - lam = clamp(n·norm(0.3, 0.3, 1.0)); ao = clamp(0.5 + (h − blur6(h))/10);
    ao2 = clamp(0.5 + (h − blur2(h))/3); hnorm = (h − min)/range; noise = blur1(N(0, 1))/std;
  - shade = clamp(−0.05 + 0.40 lam + 0.75 ao + 0.15 hnorm + 0.03 noise + 0.5(ao2 − 0.5)).
- Colours:
  - C_SHADOW (45, 130, 182), C_MIDTONE (150, 212, 235), C_LIT (252, 254, 255),
    C_FOG (30, 110, 162);
  - shade→rgb: below 0.55, SHADOW→MIDTONE over s/0.55; above, MIDTONE→LIT over
    (s − 0.55)/0.45;
  - per slice: rgb += (LIT − rgb)·0.7·clamp(1 − z/40), then rgb += (FOG − rgb)·fog.
- Sky (static, `on_start`):
  - gradient top (40, 128, 180) → bottom (12, 78, 124) over clamp(y/(0.56H))^0.8;
  - glow centre (0.5W, −0.12H), d = √((dx/W)² + 1.6(dy/H)²),
    glow = 0.8 e^(−(d/0.22)²) + 0.3 e^(−(d/0.55)²) toward (215, 242, 252);
  - shafts (angle a0 rad from atan2(x − cx, y − cy), width, strength): (−0.30, 0.17, 0.085),
    (−0.02, 0.22, 0.10), (0.27, 0.19, 0.085), (0.55, 0.16, 0.05), × fall
    clamp(1 − (y − cy)/(0.8H))^1.6.
- Deck composite: out = col + (1 − acc)·sky. Static white haze over the bottom 22%:
  t = clamp((y − 0.78H)/(0.22H))^1.5 toward C_LIT, applied after the layers.
- Billboard draw (texel u, v → world xw0 + u·q, hb + (th − v)·q):
  - bilinear texture sampling;
  - near-white push nw = 0.7·clamp(1 − z/40);
  - fog premultiplied by fog(z)·fogmul;
  - soft depth test vis = clamp((deck_z − z)/(0.25 z) + 0.5) (`highfly_layer_visibility`);
  - "over" composite.
- Band: 1024 × 90, z 260, q 0.55, xw0 = −1.5·t, hb −6, wraps.
  - Blobs r U(14, 30), top U(10, 50), each with 4 sub-blobs (r·U(0.3, 0.55), amplitude 0.8),
    x step U(18, 40);
  - rows 55+ += linspace(0, 2.5).
- Layers: 3 textures 512 × 128, q 0.16, z cycles 170 → 8 staggered by span/3, hb −7,
  alpha 0.85·fin·fout with fin = (170 − z)/30 and fout = (z − 8)/22, x offset
  (i·97 + cycle·61) mod 300 − 150.
  - Texture: 70 blobs r U(8, 26) at y U(30, 115), amplitude U(0.5, 1); rows 80+ +=
    linspace(0, 2); d *= clamp(0.8 + 0.25·wisp); dens_k 1.8.
- shade_cloud:
  - d += 0.45·wisp·dc(1 − dc)·4 with dc = clamp(d); blur(smooth); α = 1 − e^(−dens_k d);
  - od = cumsum_down(d)/h·18, blur 2; lit = e^(−absorb·od); relief = clamp(6·∂y blur3(d));
    edge = 1 − α;
  - s = clamp(bias + 0.50 + 0.55 lit + 0.30 edge + 0.12 relief + 0.03 wisp − 0.25(1 − lit)²);
  - defaults: dens_k 2.2, absorb 0.9, smooth 1, bias 0.
- Towers: texture 256 × 300 (shape below), q 0.22·size, size U(0.95, 1.2), visible for z in
  [20, 330], fin (330 − z)/50, hb −8, xw = lateral·z·fov − tw·q/2, lateral ±U(0.40, 0.60)
  alternating sides. A new tower every U(13, 14) s of travel, at most 2 live.
  - Shape: 14 tiers, tier t = i/13 at y = h − 40 − t(h − 90), radius 62(1 − 0.25t)·U(0.85, 1.1),
    x cx ± U(14).
  - Each tier: one main blob r·0.8 at amplitude 1.3, plus 7 sub-blobs at angle
    U(0.95π, 2.05π), offset (0.75r cos, 0.55r sin), radius r·U(0.28, 0.45).
  - Base rows h−50+ += linspace(0, 2.5); d *= clamp(0.75 + 0.3 wisp); sides
    × clamp(1.6 − 1.6·xn²).
  - Shade: dens_k 2.0, smooth 3, bias 0.22, absorb 0.6, prototype fogmul 0.45.
  - Spec fix, starting values: 3 variants from distinct seeds; fogmul 0.25; crown boost
    s += 0.25·clamp(1 − od/2) (sunlit tops white, not bluish).
- Fly-through: every U(60, 120) s.
  - Envelope: smoothstep in over 1.0 s, hold 0.5 s, smoothstep out over 1.3 s.
  - out += ((200, 216, 230) − out)·amt·0.62.
  - Wisps: noise 64 × 128 from octaves (8, 1.0), (32, 0.6), × 0.6, blurred r3 along the
    log-radius axis; lookup row = log(r)·40 − t·120, column = angle.
  - a = clamp(1.5n)·clamp(1.2r)·amt·0.7 toward C_LIT. r is normalised by the half-size, from
    the centre (0.5W, 0.5H).
  - Polar lookup table precomputed per internal pixel, wisp sampled bilinearly.
  - The cloud body approaching: tower-shaped texture q 0.09, centred, hb = cam_h − th·q·0.6,
    alpha fin·(1 − amt)·0.9.

## Review Focus (most likely untested failure modes, each tested in its task)

1. **The flip clock silently changes** during the extraction. This is guarded by a rename-only
   diff of `test_screensaver_flip_clock.cpp` (Task 1.2 step 5) plus its full tag passing.
2. **A droplet disappears in mid-air, or the cap culls.** Tested in Task 3.2: removal only
   below the surface, no timer, emission refused at the cap, emitted == live + landed.
3. **Icicles / streaks.** The gradient is applied where the previous slice did not draw, or
   not at all. Tested in Task 2.2 (gated test) and 2.2 (gradient test).
4. **The cheaper Gerstner flattens the crests** into sine humps. Tested in Task 3.1
   (crest width < trough width).
5. **The last rung isn't static** (repaints the scene, or the clock never ticks), or the dirty
   boxes miss pixels the frame changed (rim, droplets, labels). Tested in Tasks 3.4 and 4.4
   (still rung; every changed pixel lies in a dirty box).

---

## Part 1: Shared clock face (stacked commit 1)

### Task 1.1: `ClockState`, `ClockReading`, `clock_time`, `clock_drift_at`

**Files:** create `include/screensaver_clock_reading.h` (candidate copy),
`src/ui/screensaver_clock_reading.cpp`, `tests/unit/test_screensaver_clock_reading.cpp`.

**Produces:** `ClockTime clock_time(int32_t seconds_of_day, bool use_24h)`;
`class ClockState { void step(uint32_t, const ClockInput&); ClockMode mode() const;
int32_t countdown_s() const; const ClockReading& reading() const; void reset(); }`;
`void clock_drift_at(uint32_t, int32_t, int32_t, int32_t&, int32_t&)`. Constants
`CLOCK_DONE_HOLD_MS = 60000`, `CLOCK_COUNTDOWN_RESYNC_S = 10`, `CLOCK_MAX_COUNTDOWN_S`,
`CLOCK_DRIFT_STEP_MS = 60000`.

- [ ] Write failing tests, tag `[screensaver][clock_reading]`:
  - `"clock_time shows 12-hour and 24-hour time with seconds and the marker"`:
    `clock_time(21*3600+28*60+7, false)` → hour 9, hour_of_day 21, minute 28, second 7, PM;
    `clock_time(0, false)` → hour 12, AM; `clock_time(12*3600, false)` → 12, PM;
    `clock_time(9*3600, true)` → hour 9, NONE; `clock_time(-1, true)` → 23:59:59 (wraps).
  - `"a reading carries the date it was given"`: input date {2019, 10, 21, 1}, step(0) →
    `reading().date` equal field by field.
  - `"a print with an estimate reads a countdown with its end time and progress"`: 21:28:00,
    PRINTING, remaining 4320, progress 37 → mode COUNTDOWN, countdown_s 4320,
    eta_minute_of_day 1360, progress_pct 37, time still 9:28 PM. step(1000, same) →
    countdown 4319. Remaining 4319 + 5 (jitter ≤ 10) is ignored; remaining 4000 resyncs.
  - `"paused freezes, done holds then returns to the clock, cancelled clears"`: PAUSED for
    5000 ms keeps countdown_s; COMPLETE after COUNTDOWN → DONE with progress_pct 100, and
    after CLOCK_DONE_HOLD_MS → CLOCK; NONE → CLOCK with countdown_s 0.
  - `"the drift steps once per CLOCK_DRIFT_STEP_MS within range"`: the existing flip drift
    test body moved here under the new name.
- [ ] `make t F='[clock_reading]'`, expect FAIL (no symbols).
- [ ] Implement: `ClockState::step` is `FlipClockState::step` moved verbatim (FlipMode →
  ClockMode), and builds `reading_` (time via `clock_time`, date and use_24h copied,
  eta/progress as `FlipClockState::build_face` computes them today, DONE progress 100).
  `clock_drift_at` is `flip_drift_at` verbatim.
- [ ] `make t F='[clock_reading]'`, expect PASS.
- [ ] Commit after Task 1.2 (one commit; the flip clock cannot compile half-moved).

### Task 1.2: Flip clock on the extracted code; `saver_clock_now`; `SaverLabel`

**Files:** modify `include/screensaver_flip_clock_sim.h` (= candidate
`flip_clock_sim_revised.h`), `src/ui/screensaver_flip_clock_sim.cpp`,
`include/screensaver_flip_clock.h` (= `flip_clock_revised.h`),
`src/ui/screensaver_flip_clock.cpp`, `include/screensaver.h` (`SaverHost::print_status`
returns `ClockPrint`; include `screensaver_clock_reading.h` instead of the flip sim),
`src/application/display_manager.cpp#screensaver_host`, `src/ui/screensaver_manager.cpp`.
Create `include/screensaver_clock.h` (candidate copy) and `src/ui/screensaver_clock.cpp`.
Test: `tests/unit/test_screensaver_flip_clock.cpp` (renames only).

**Produces:**
- `FlipFace flip_face(const ClockReading&)`;
- `FlipClockState { step(uint32_t, const ClockInput&); ClockMode mode(); countdown_s(); face(); reset(); }`;
- `ClockInput saver_clock_now()` (fills `date` from `localtime_r`: tm_year + 1900,
  tm_mon + 1, tm_mday, tm_wday);
- `ClockPrintPhase clock_print_phase(PrintState)`;
- `const lv_font_t* clock_font_fitting(int32_t)`;
- `class SaverLabel { bool draw(SaverCanvas&, const std::string&, const DirtyRect&, Rgb, lv_opa_t, lv_text_align_t, const std::vector<DirtyRect>& under, std::vector<DirtyRect>& dirty); const DirtyRect& box() const; const std::string& text() const; void reset(); }`.

**ScreensaverManager:** a private `ClockInput ScreensaverManager::clock_input()` (the current
lambda body: `saver_clock_now()` plus `host_.print_status()`) feeds the flip clock, Aqua and
High Fly sources. This is one function, not three lambdas.

- [ ] In the test file, apply exactly the renames in decision 1 (sed list in the commit body).
  Add `#include "screensaver_clock_reading.h"` and `using` lines for the new names. Nothing else
  changes.
- [ ] `make t F='[screensaver_flip_clock]'`, expect FAIL (compile).
- [ ] Implement:
  - `FlipClockState::step` = `state_.step(dt, in); face_ = flip_face(state_.reading());`.
  - `flip_face` is today's `build_face` switch, reading from `ClockReading`. `clock_face()`
    stays and uses `clock_time`.
  - `FlipClockSim::build_glyphs` calls `build_stroke_digits(layout_.unit, 0.2f, glyphs_)`
    (Task 1.3; do 1.3 first if you prefer). `glyph_coverage` → `glyphs_.at`.
  - `FlipClockScreensaver::draw_text` keeps its erase branch (black fill or `redraw_card`),
    then calls `label.draw(...)` with TEXT_COLOR, `LV_OPA_COVER`, the same alignment and
    `card_dirty` as `under`.
  - `SaverLabel::draw` returns false without drawing when the text and box are unchanged and
    no `under` box intersects the box. Otherwise it does what `draw_text` does today
    (`clock_font_fitting`, vertical centring, `begin_layer` / `mark_dirty` / `finish_layer`)
    and records the text and box. An empty text or box draws nothing and records them.
- [ ] `make t F='[screensaver_flip_clock]'` and `make t F='[clock_reading]'`, expect PASS.
- [ ] Verify the test diff is rename-only:
  `git diff --word-diff=porcelain -- tests/unit/test_screensaver_flip_clock.cpp | grep '^[-+][^-+]'`
  must list only the renamed identifiers and the added include/using lines.
- [ ] `make t F='[screensaver]'` (registry, manager, gate tests compile against the new
  SaverHost).
- [ ] Commit:
  `git add -N include/screensaver_clock_reading.h src/ui/screensaver_clock_reading.cpp include/screensaver_clock.h src/ui/screensaver_clock.cpp tests/unit/test_screensaver_clock_reading.cpp include/screensaver_stroke_digits.h src/ui/screensaver_stroke_digits.cpp`
  then `git commit -m "refactor(screensaver): share the clock readout, stroke digits and text label between clock savers" -- <all paths above> include/screensaver.h include/screensaver_flip_clock*.h src/ui/screensaver_flip_clock*.cpp src/application/display_manager.cpp src/ui/screensaver_manager.cpp tests/unit/test_screensaver_flip_clock.cpp firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt`.
  Add the three new `.cpp` files to `app_srcs_excluded.txt` in this commit, in sorted order.

### Task 1.3: Shared stroke digits

**Files:** create `include/screensaver_stroke_digits.h` (candidate copy) and
`src/ui/screensaver_stroke_digits.cpp` (the `Stroke`/`GlyphDef`/`GLYPHS`/`dist_to_*` code moved
from the flip sim's anonymous namespace). Test: `tests/unit/test_screensaver_clock_reading.cpp`.

**Produces:** `float stroke_digit_distance(int, float, float)`;
`void build_stroke_digits(int32_t unit, float stroke, StrokeDigitSet&)`;
`StrokeDigitSet::{at, heap_bytes, clear}`.

- [ ] Tests `[screensaver][clock_reading]`:
  - `"stroke digits size their box from the stroke"`: unit 50, stroke 0.2 → w 60, h 90;
    stroke 0.06 → w 53, h 83.
  - `"a thin stroke covers fewer pixels than a bold one"`: for every digit, Σ coverage at
    0.06 < 0.5 × Σ at 0.2, and > 0.
  - `"a stroke digit's centre line is fully covered"`: digit 1 at unit 50, stroke 0.06, the
    pixel on x = 0.55 + 0.03 units at mid-height has coverage ≥ 250.
  - `"nine is six turned half a turn"`:
    `stroke_digit_distance(9, x, y) == stroke_digit_distance(6, 1 - x, 1.6 - y)` on a grid.
  - `"clear frees the coverage"`: heap_bytes 0 after clear.
- [ ] FAIL, implement, PASS (`make t F='[clock_reading]'`, then `F='[screensaver_flip_clock]'`
  pixel tests unchanged).
- [ ] Committed with Task 1.2.

### Task 1.4: Localized long date, weekday and print text

**Files:** modify `include/locale_formats.h`, `src/ui/locale_formats.cpp`; implement
`clock_print_text`, `clock_date_text` and `clock_meridiem_text` in
`src/ui/screensaver_clock.cpp`. Test: `tests/unit/test_screensaver_clock_reading.cpp`
(LVGL fixture cases).

**Produces:**
- `std::string helix::ui::format_localized_long_date(const struct tm*)`;
- `std::string helix::ui::format_localized_weekday(const struct tm*)`;
- `ClockPrintText clock_print_text(const ClockReading&)`;
- `ClockDateText clock_date_text(const ClockDate&)`;
- `std::string clock_meridiem_text(ClockMeridiem)`.

Patterns, keyed on the existing `DatePatternGroup` where it fits:
- en `"{Month} {d}, {yyyy}"`;
- de `"{d}. {Month} {yyyy}"`;
- fr / it / ru `"{d} {month} {yyyy}"`;
- es / pt `"{d} de {month} de {yyyy}"`;
- zh / ja `"{yyyy}年{m}月{d}日"`.

A system locale uses strftime `%B` / `%A`, as `get_month_name` does with `%b`. Fallback
tables `k_month_full` / `k_day_full`:
- en: January..December / Sunday..Saturday;
- de: Januar, Februar, März, April, Mai, Juni, Juli, August, September, Oktober, November,
  Dezember / Sonntag, Montag, Dienstag, Mittwoch, Donnerstag, Freitag, Samstag;
- fr: janvier, février, mars, avril, mai, juin, juillet, août, septembre, octobre, novembre,
  décembre / dimanche, lundi, mardi, mercredi, jeudi, vendredi, samedi;
- es: enero, febrero, marzo, abril, mayo, junio, julio, agosto, septiembre, octubre,
  noviembre, diciembre / domingo, lunes, martes, miércoles, jueves, viernes, sábado;
- it: gennaio, febbraio, marzo, aprile, maggio, giugno, luglio, agosto, settembre, ottobre,
  novembre, dicembre / domenica, lunedì, martedì, mercoledì, giovedì, venerdì, sabato;
- pt: janeiro, fevereiro, março, abril, maio, junho, julho, agosto, setembro, outubro,
  novembro, dezembro / domingo, segunda-feira, terça-feira, quarta-feira, quinta-feira,
  sexta-feira, sábado;
- ru (genitive months): января, февраля, марта, апреля, мая, июня, июля, августа, сентября,
  октября, ноября, декабря / воскресенье, понедельник, вторник, среда, четверг, пятница,
  суббота;
- zh: weekdays 星期日..星期六 (星期日, 星期一, 星期二, 星期三, 星期四, 星期五, 星期六);
- ja: weekdays 日曜日..土曜日 (日曜日, 月曜日, 火曜日, 水曜日, 木曜日, 金曜日, 土曜日);
- zh / ja months are numeric through the pattern.

- [ ] Tests:
  - `"the long date and weekday read in the active language"` (`[clock_reading][locale]`),
    for tm 2019-10-21 (Monday):
    - `locale_set_language("en")` → "October 21, 2019" / "Monday";
    - "de" → "21. Oktober 2019" / "Montag";
    - "fr" → "21 octobre 2019" / "lundi";
    - "es" → "21 de octubre de 2019";
    - "ja" → "2019年10月21日" / "月曜日";
    - "ru" → "21 октября 2019".
    - With a system locale installed, the strftime path may differ: assert the fallback by
      the same means `test_clock_widget.cpp` does (copy its locale guard).
  - `"the print readout says when it ends and how long is left"` (LVGL fixture, en, 12-hour
    via `DisplaySettingsManager::instance().set_time_format(TimeFormat::HOUR_12)`, restored
    after):
    - COUNTDOWN reading, eta 1360, countdown 4320 → eta "ETA 10:40 PM", left "1h 12m left";
    - PAUSED → eta "PAUSED", left "1h 12m left";
    - DONE → "DONE", "";
    - CLOCK → "", "".
  - `"the date text joins nothing and uses locale_formats"`: `clock_date_text({2019,10,21,1})`
    in en → {"October 21, 2019", "Monday"}.
- [ ] FAIL → implement → PASS (`make t F='[clock_reading]'`).
- [ ] `make translation-sync`; translate `"ETA {}"` and `"{} left"` in every `translations/*.yml`:

  | lang | `ETA {}` | `{} left` |
  |---|---|---|
  | de | `Ende {}` | `noch {}` |
  | fr | `Fin {}` | `{} restant` |
  | es | `Fin {}` | `quedan {}` |
  | it | `Fine {}` | `{} rimanenti` |
  | pt | `Término {}` | `{} restantes` |
  | ru | `Окончание {}` | `осталось {}` |
  | zh | `预计 {}` | `剩余 {}` |
  | ja | `終了予定 {}` | `残り {}` |

  Then `make translations`; `make regen-text-fonts` if translation-sync reports new CJK glyphs.
- [ ] Commit: `git commit -m "feat(screensaver): localized long date, weekday and print readout for clock savers" -- include/locale_formats.h src/ui/locale_formats.cpp src/ui/screensaver_clock.cpp tests/unit/test_screensaver_clock_reading.cpp translations/ <generated translation paths make translations touched>`.

---

## Part 2: Heightfield column renderer (stacked commit 2)

### Task 2.1: Slices, fog, bilinear

**Files:** create `include/screensaver_heightfield.h` (candidate copy),
`src/ui/screensaver_heightfield.cpp`, `tests/unit/test_screensaver_heightfield.cpp`; add the
.cpp to `app_srcs_excluded.txt`.

**Produces:** `HeightfieldFog::at`; `build_heightfield_slices(const HeightfieldView&,
std::vector<HeightfieldSlice>&)`; `bilinear_sample(const float*, int32_t, int32_t, float, float,
bool)`.

- [ ] Tests `[screensaver][heightfield]`:
  - `"slices run from near to far, each thicker by the grow factor"`: water view (3.5, 7.5,
    1.015, 110): z[0] == 3.5; |z[109] + dz[109] − 7.5| < 1e-3; dz[0] ≈ 0.01448 (±1e-4);
    dz[i+1]/dz[i] == 1.015 (±1e-5); inv_z·z == 1 (±1e-6). Clouds view (10, 350, 1.025, 117):
    dz[0] ≈ 0.501 (±0.002).
  - `"each slice knows the world x of its columns"`: w 400, focal_x 200 → x_step = z/200,
    x_left = (0.5 − 200)·z/200.
  - `"sink lowers near heights smoothly to full height"`: sink 40: slice at z_near → 0,
    z_near + 20 → 0.5, ≥ z_near + 40 → 1; 0 sink → all 1.
  - `"fog rises or falls with depth"`: {25, 300, 0.8, false}.at(25) = 0, at(300) = 1,
    at(162.5) = 0.5^0.8; falling {3.5, 7.5, 0.6, true}.at(3.5) = 1, at(7.5) = 0.
  - `"bilinear sampling interpolates between cells and wraps or clamps"`: 2×2 grid {0, 1, 2, 3}:
    (0.5, 0.5, clamp) → 1.5; (1.5, 0, wrap) → 0.5; (5, 0, clamp) → 1.
- [ ] FAIL → implement → PASS (`make t F='[heightfield]'`).

### Task 2.2: `HeightfieldRenderer::render`, edges, gradient, depth

**Produces:** `init`, `set_view`, `render(HeightfieldScene&)`, `color_at`, `coverage_at`,
`depth_slice_at`, `top_at`, `front_top_at`, `front_color_at`, `samples`, `span_pixels`,
`heap_bytes`, `clear`.

Tests use a test-file `HeightfieldScene` that returns a configured height and colour per
(slice, column).

- [ ] Tests `[screensaver][heightfield]`:
  - `"samples cost columns times slices"`: 40×30 view, 20 slices → samples() == 800, and the
    scene's `sample_slice` was called 20 times with w = 40.
  - `"a near tall surface hides a far low one"`: heights 1.0 for slices < 5, 0.2 after; the
    column's colour above the near span top comes from no far slice (coverage 0) and the far
    colour never appears; `top_at` equals the near projected row.
  - `"a span shades from this slice's colour at its top to the last slice's at its bottom"`:
    two slices both drawing a column, colours (200,0,0) then (0,0,200) → the top pixel of the
    second span is ≈ (0,0,200) and its bottom ≈ (200,0,0) (± 8).
  - `"no gradient where the previous slice did not draw the column"`: slice 1 is hidden behind
    slice 0 in column x and slice 2 rises above → slice 2's span is flat (every pixel ± 2 of
    its colour). This is the icicle case.
  - `"span_gradient off draws flat spans"`.
  - `"the feathered silhouette gives the top pixel fractional coverage and smooths it"`:
    FEATHERED, a top at row 10.25 → coverage at row 10 ≈ 191 (0.75·255, ± 3) before
    smoothing; with a neighbouring column at 20.0, rows 9..11 of the step column average
    [1 2 1].
  - `"soft-under edges fade in over F rows and keep the far surface showing"`: SOFT_UNDER,
    F = 10, one flat slice at row 50 → coverage rises across rows 50..59 (strictly increasing,
    row 50 < 30, row 59 ≥ 230); a farther, higher slice whose row is 55 still adds colour at
    rows 55..59 (y-buffer rose only to 60).
  - `"first_slice_fills false leaves the rows below the near edge uncovered"`.
  - `"depth records the slice where coverage first reached half"`.
  - `"render allocates nothing and heap stays constant"`: heap_bytes equal before and after
    10 renders; `clear()` → 0.
- [ ] FAIL → implement → PASS.

### Task 2.3: `resolve` and `upscale_frame`

**Produces:** `HeightfieldRenderer::resolve(const Rgb*, Rgb, int32_t, int32_t,
std::vector<Rgb>&) const`; `DirtyRect upscale_frame(const std::vector<Rgb>&, int32_t, int32_t,
int32_t, int32_t, int32_t, FrameTarget&)`.

- [ ] Tests:
  - `"resolve composes under coverage over the background"`: coverage 128, colour (100,0,0)
    premultiplied, background (0,0,200) → (100, 0, ~100).
  - `"a 2x upscale interpolates and a 4x one replicates, in either format"`: 2×1 frame
    {black, white} at scale 2 → output columns 0..3 rise monotonically and the end pixels are
    black / white; scale 4 → 4×4 blocks equal. XRGB8888 and RGB565 (RGB565 dithered: the mean
    of a 4×4 block of mid grey equals 128 ± 2).
  - `"upscale writes only the rows asked for and returns that box"`: rows outside
    [y_from·scale, y_to·scale) are untouched; the returned box matches.
  - `"an odd-sized canvas never writes past a row"` (stride padding pattern, as
    `test_screensaver_hyperspace.cpp` "odd-sized").
- [ ] FAIL → implement → PASS.
- [ ] Commit: `git add -N include/screensaver_heightfield.h src/ui/screensaver_heightfield.cpp tests/unit/test_screensaver_heightfield.cpp` then `git commit -m "feat(screensaver): voxel-space heightfield column renderer for grazing-angle surfaces" -- include/screensaver_heightfield.h src/ui/screensaver_heightfield.cpp tests/unit/test_screensaver_heightfield.cpp firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt`.

---

## Part 3: Aqua Surface (`aqua`)

All values in Global Constraints › Aqua. Scale every pixel constant given "at 400x240" by
internal w/400 or h/240, and every output-resolution constant by canvas h/480.

### Task 3.1: `AquaWater`

**Files:** create `include/screensaver_aqua_sim.h` (candidate copy),
`src/ui/screensaver_aqua_sim.cpp`, `tests/unit/test_screensaver_aqua.cpp`.

**Produces:** `AquaWater::{init, set_grid, evaluate, height_at, exact_height, rise_at,
grid_x, grid_z, heights, impulses, heap_bytes, clear}`; `float aqua_gerstner_height(const
AquaWave&, float x, float z, float t)` (one wave with its one-pass inversion: the function
`AquaWater` sums, not a test hook).

- [ ] Tests `[screensaver][aqua]`:
  - `"the tank modes keep the dispersion relation"`: after init, each mode's
    ω² == 9.81·k·tanh(1.2k) (± 1e-4), k = nπ/7.
  - `"Gerstner crests stay peaked"`: `aqua_gerstner_height({0.13, 2π/2.6, ω, 1, 0, 0.9, 0}, x, 5, 0)`
    sampled at 2000 points over one wavelength → the width where h > A/2 is < 0.8 × the width
    where h < −A/2; with steep 0 the two widths are equal (± 2%).
  - `"the grid samples the analytic surface"`: `height_at(x, z)` within 0.01 of
    `exact_height(x, z, t)` at 50 random points inside the grid.
  - `"impulses start behind the near edge and expire after 12 sim seconds"`: over 120 sim s,
    every impulse z ∈ [4.7, 5.9], start gaps ∈ [2.0, 4.0], none older than 12 in `impulses()`.
  - `"evaluate allocates nothing"`: heap constant across 100 evaluations.
- [ ] FAIL → implement → PASS (`make t F='[aqua]'`).

### Task 3.2: `AquaSplashes`, Preston's droplet rule

**Produces:** `AquaSplashes::{init, emit, step, droplets, emitted, heap_bytes, clear}`.
Add to the header: `bool emit(const AquaDroplet&)` (true when stored, false at the cap; the
crest path in `step` uses it too).

- [ ] Tests `[screensaver][aqua][droplets]`:
  - `"a droplet is removed only once it is below the surface at its own position"`: real
    water, seed 3, 300 sim s at dt 0.0125. Track every `AquaLanding` and assert
    `landing.y < landing.surface` for each. Also assert that every droplet still in
    `droplets()` after a step has y ≥ water.height_at(x, z).
  - `"a droplet in flight never expires on a timer"`: `emit({0, 0.5, 5.0, 0, 5.0, 0})` (vy 5,
    so flight ≈ 2·5/(0.35·9.81) ≈ 2.9 sim s, longer than any lifetime aqua2 used). Step dt
    0.01: it is present at every step while y ≥ surface (check at 2.5 s) and absent within one
    step after y < surface.
  - `"the cap limits emission and never culls"`: emit 48 → all true, the 49th → false, size 48.
    Step 0.01 once with a calm surface (all droplets high) → size stays 48 and
    `emitted() == 48`. Run 200 sim s → `emitted() == live + Σ landings` at every step.
  - `"droplets stay inside the tank depth"`: z ∈ [3.5, 7.5] always.
  - `"emission follows time, not frame rate"`: one AquaSim-level check, done in Task 3.4
    (same emitted count ± 10% for 20 s at 16 ms vs 50 ms periods, same seed).
- [ ] FAIL → implement → PASS (`make t F='[droplets]'`).

### Task 3.3: Rings and clock layout

**Produces:** `AquaRings aqua_rings(const ClockReading&)`;
`AquaClockLayout layout_aqua_clock(int32_t w, int32_t h, int32_t dx, int32_t dy)`.

Layout at 800x480, scaled by h/480:
- ring_r 36, top_r 22, ring_width 2;
- hour centre (w/2 − 45, 0.80h), minute centre (w/2 + 45, 0.80h), top centre
  (w/2, 0.80h − 68);
- digit_unit = round(0.8·ring_r / 1.66) for stroke 0.06 (two digits side by side inside a ring);
- text line: y from 0.80h + 48 to 0.80h + 70, x from w/2 − 220 to w/2 + 220;
- bounds = union.

- [ ] Tests `[screensaver][aqua][clock]`:
  - `"the rings fill through the day, the hour and the minute"`:
    - 10:30:15 12h → hour 0.87535, minute 0.50417, top 0.25 (± 1e-4);
    - 22:30:15 24h → hour 0.93767;
    - 22:30:15 12h → 0.87535;
    - 00:00:00 → all 0.
  - `"during a print the top ring is progress"`: COUNTDOWN progress 42 → top 0.42, hour and
    minute still the time; PAUSED 42 → 0.42; DONE → 1.0.
  - `"the clock fits in the water at every drift and screen size"`: for 800×480, 480×320,
    1024×600, 480×800 and the drift range, bounds lie inside the canvas, and bounds.y1 is
    below the median near-edge row of a fresh sim (checked in 3.4).
- [ ] FAIL → implement → PASS.

### Task 3.4: `AquaSim`: shading, rim, bubbles, droplets drawn, clock drawn, ladder

**Produces:** `AquaSim::{init, step, request_level, level, water, splashes, renderer,
clock_layout, sim_time, rim_brightness, heap_bytes, clear}`.

Per frame:
1. Advance sim time by dt·0.25.
2. `water.evaluate`.
3. Emission steps: the `StepAccumulator` at 50 ms, cap 4.
4. `splashes.step`.
5. Render through a private `HeightfieldScene` in the .cpp (FEATHERED_SILHOUETTE,
   `first_slice_fills = false`, span gradient on).
6. Resolve over black, plus the below-edge smear.
7. Upscale rows [min silhouette − 2, h).
8. Rim, bubbles, droplets, glints at output resolution.
9. The rings and digits (stroke 0.06, colour (230, 236, 242) at 0.9 alpha; ring track
   (40, 42, 46), fill (220, 226, 232); antialiased arcs from the distance |d − r| and
   end-angle caps, fill clockwise from 12 o'clock).

Dirty:
- the water box from the silhouette row (min over this and the last frame, − rim extent) to
  the bottom;
- each droplet/glint box above it, this and last frame;
- the clock bounds when the reading changes.
- All are merged with `merge_dirty_areas(…, SAVER_MAX_DIRTY_AREAS)`. The whole canvas once
  `covers_whole_canvas` says so.

Still rung: draw everything once at init or level entry, then a frame redraws only the clock
bounds (restoring the saved water pixels beneath it from a copy of that region taken at
entry), and only when `reading.time` or the readout changes.

- [ ] Tests `[screensaver][aqua]`:
  - `"an aqua run replays exactly from a seed, and another seed differs"` (both formats).
  - `"every pixel an aqua frame changes lies in one of its dirty boxes"`: 60 frames at 33 ms,
    snapshot diff, as the flip clock's test of the same name.
  - `"the near-edge rim is always lit"`: every frame for 20 s, every column inside the water,
    `rim_brightness(x) ≥ 0.4`, and the canvas pixel at the rim row is ≥ 0.4·255·0.94 grey.
  - `"bubbles never rise above the surface"`: level 3 (splashes off), 10 s of frames: every
    output pixel more than 1 row above the rim (rim row − 2 and up), outside the clock bounds,
    is black.
  - **Shimmer** `"the water shimmers no more than about four grey levels a frame"`
    (`[screensaver][aqua][shimmer]`): canvas 800×480, level 1 (internal 400×240), seed 3.
    - Warm up 2 s at 50 ms frames, then 20 frame pairs at 50 ms.
    - grey = (r + g + b)/3 at output pixels whose internal coverage is 255 in both frames,
      more than 6 output rows below the silhouette, and outside droplet, bubble and clock
      boxes.
    - Mean |Δgrey| over all those samples ≤ 5.0 and ≥ 0.3 (the water moves). Print the
      value with `INFO`/`WARN` so the measured number is in the log.
    - If its runtime exceeds 10 s in the test build, retag `[slow]` and keep the measurement
      as the final verification step below.
  - `"the ladder steps to coarser water and ends standing still"`:
    - periods non-decreasing; level 0 == SAVER_FAST_PERIOD; last level `still`;
    - at the still level: 10 frames with the time unchanged → dirty empty and pixels
      unchanged;
    - a frame where the minute changes → dirty ⊆ clock bounds and only the minute ring / its
      digits changed.
  - `"emission follows time, not frame rate"` (from 3.2).
  - `"frames allocate nothing and clear frees everything"`: heap constant over 100 frames and
    a level change, ≤ 1.5 MB at 800×480, `clear()` → 0.
- [ ] FAIL → implement → PASS (`make t F='[aqua]'`).

### Task 3.5: Wrapper, registry, settings, translations, manager

**Files:**
- create `include/screensaver_aqua.h` (candidate copy) and `src/ui/screensaver_aqua.cpp`
  (pattern: `screensaver_hyperspace.cpp` + flip clock's input source);
- modify `include/screensaver_registry.h`: `AQUA = 11`, row
  `{ScreensaverType::AQUA, "aqua", "Aqua Surface", SAVER_DEPTH_16 | SAVER_DEPTH_32}`, after
  the fireflies row (see Dependencies);
- modify `ui_xml/settings_display_overlay.xml` `row_screensaver` `options` and `options_tag`
  (append `&#10;Aqua Surface` after Fireflies);
- modify `src/ui/screensaver_manager.cpp` (push `AquaScreensaver` with
  `set_input_source([this]{ return clock_input(); })`);
- modify `app_srcs_excluded.txt` (`screensaver_aqua.cpp`, `screensaver_aqua_sim.cpp`);
- translations.

`on_frame`:
1. `clock_.step(dt, read_input())`;
2. `sim_.step(...)`;
3. the line text = `clock_print_text` joined `"{eta} · {left}"` while printing, else
   `clock_date_text` joined `"{date} · {weekday}"`;
4. when the text changes, `sim` repaints the old line box first (expose
   `AquaSim::restore(FrameTarget&, const DirtyRect&, std::vector<DirtyRect>&)`, which writes
   the scene pixels for that box: underwater pixels are redrawn each frame anyway, so restore
   is only needed at the still rung);
5. `line_.draw(canvas(), text, sim_.clock_layout().text, {200, 206, 212}, LV_OPA_90,
   LV_TEXT_ALIGN_CENTER, dirty, dirty)`.

The clock drifts with `clock_drift_at` (range from the layout's fit, as `flip_drift_range`
computes it) at `CLOCK_DRIFT_STEP_MS`. `on_level_request` → `apply_level` +
`sim_.request_level`.

- [ ] Tests (`LVGLTestFixture`, `[screensaver][aqua]`):
  - `"the aqua saver draws on a pipes-format canvas with the aqua ladder"` (as the flip clock
    case);
  - `"the aqua saver shows the print its source reports"`: source PRINTING 4320 s → after
    start, `clock().mode() == COUNTDOWN` and the label text starts "ETA ";
  - `"a stopped aqua saver holds no buffers"`;
  - `"aqua is registered by name at both depths"` (as hyperspace's).
  - The registry tests (`test_screensaver_registry.cpp`) cover the XML list and the en
    translation once the row exists.
- [ ] FAIL → implement → PASS: `make t F='[aqua]'`, `make t F='[screensaver]'`.
- [ ] `make translation-sync`; translate `Aqua Surface`: de `Wasseroberfläche`, fr
  `Surface aquatique`, es `Superficie acuática`, it `Superficie dell'acqua`, pt
  `Superfície da água`, ru `Водная гладь`, zh `水面`, ja `水面`; then `make translations`
  (`make regen-text-fonts` if new CJK glyphs are reported).
- [ ] `size build/obj/ui/screensaver_aqua*.o`: `.data` + `.bss` have no sizable entries.
- [ ] Commit: `git add -N` the four new files, then `git commit -m "feat(screensaver): Aqua Surface, glassy slow-motion water with a ring clock" -- <new files> include/screensaver_registry.h ui_xml/settings_display_overlay.xml src/ui/screensaver_manager.cpp firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt translations/ <generated translation files>`.

---

## Part 4: High Fly (`highfly`)

All values in Global Constraints › High Fly.

### Task 4.1: Field, sky, textures

**Files:** create `include/screensaver_highfly_sim.h` (candidate copy),
`src/ui/screensaver_highfly_sim.cpp`, `tests/unit/test_screensaver_highfly.cpp`.

**Produces:** `build_highfly_field(std::minstd_rand&, HighFlyField&)`;
`highfly_layer_visibility(float, float)`; `highfly_whiteout(float)`;
`HighFlyTexture::heap_bytes`. The texture builders (`make_band`, `make_layer`, `make_tower`,
shade_cloud, fbm, blur) are `.cpp`-local and run in `init()`. Value noise uses `unit_random`,
never `<random>` distributions.

- [ ] Tests `[screensaver][highfly]`:
  - `"the cloud field tiles and never steps more than the talus slope"`: for every texel and
    its right / down neighbour, wrapping across the seam, |Δh| ≤ 2.0 + 1e-4.
  - `"the cloud field has billows tall enough to break the skyline"`: max height / 2 (world) >
    cam_h 15 and min < cam_h. Camera height sits below the tallest tops.
  - `"crevices shade darker than tops"`: mean shade over the top 10% of heights > mean over
    the bottom 10% + 0.15.
  - `"a field replays from its seed"`.
  - `"billboards hide behind nearer deck and show in front of it"`:
    `highfly_layer_visibility(50, 100)` == 0, `(200, 100)` == 1, `(100, 100)` == 0.5,
    `(INFINITY, 100)` == 1, and monotonic in deck_z.
  - `"a fly-through eases in, holds and clears"`:
    - `highfly_whiteout(-1)` == 0, `(0.5)` == 0.5, `(1.0)` == 1, `(1.4)` == 1, `(1.5 + 0.65)`
      == 0.5, `(2.8)` == 0, `(5)` == 0;
    - continuous: |w(t + 0.001) − w(t)| < 0.01.
- [ ] FAIL → implement → PASS (`make t F='[highfly]'`).

### Task 4.2: Clock layout

**Produces:** `layout_highfly_clock(int32_t, int32_t, int32_t, int32_t)`.

At 800×480, scaled by h/480:
- numerals 110 px tall, so digit_unit = round(110 / 1.66) for stroke 0.06; centred on
  x = w/2 with baseline at 0.60h;
- colon dot radius 0.05 units;
- seconds unit = 0.35·digit_unit, top-aligned with the numerals, right of the minutes
  + 0.15 units;
- meridiem box under the seconds, 0.3·numeral height;
- date_line / day_line at x 0.05w..0.30w, y 0.47h and 0.53h, height 0.05h each.

- [ ] Tests `[screensaver][highfly][clock]`:
  - `"the high fly clock fits every screen at every drift"` (same sizes as Aqua);
  - `"digit boxes run left to right without overlapping, seconds sit up and to the right"`;
  - `"the clock sits at the cloud line"`: the numeral boxes straddle 0.56h ± 0.1h.
- [ ] FAIL → implement → PASS.

### Task 4.3: `HighFlySim` scene and ladder

**Produces:** `HighFlySim::{init, step, request_level, level, renderer, clock_layout,
elapsed_s, fly_since_s, next_fly_s, next_tower_s, towers_visible, band_top, heap_bytes,
clear}`.

Per frame:
1. Advance t.
2. Deck scene (a private `HeightfieldScene`, SOFT_UNDER F = 10·h/240, sink 40, span gradient,
   first slice fills with C_LIT behind it, fog {25, 300, 0.8}).
3. Resolve over the cached sky+band rows.
4. Layers (level's count) and towers, back to front, depth-tested via `depth_slice_at` →
   slice z.
5. Haze.
6. Fly-through when live.
7. Upscale rows [band_top, h), or the whole frame while `fly_since_s() >= 0` and whiteout > 0.
8. Numerals: thin stroke digits (0.06), colour (226, 238, 250) blended at 0.85 over the
   pixels just written.

The band is redrawn into the sky cache every frame only over its own rows.

- `band_top` = the band's top row at init (the highest band pixel row − 1).
- Tower boxes are dirty this and last frame.
- Towers: schedule `next_tower_s` from U(13, 14); variants chosen round-robin without
  immediate repeat; alternate sides.
- Fly-through: schedule from U(60, 120); its approaching body draws while z > 0.8.

- [ ] Tests `[screensaver][highfly]`:
  - `"a high fly run replays exactly from a seed, and another seed differs"`.
  - `"the sky above the cloud band never repaints outside a tower or a fly-through"`: 30 s at
    50 ms; on frames with `towers_visible() == 0` and no fly-through, every dirty box has
    y1 ≥ band_top·scale.
  - `"every pixel a high fly frame changes lies in one of its dirty boxes"`.
  - `"the sun glows at the top centre"`: after init, pixel (w/2, 2) luminance > (4, 2) + 30.
  - `"towers come every 13 to 14 seconds, at most two at once, in more than one shape"`:
    over 120 s: entry gaps ∈ [13, 14], max towers_visible ≤ 2, ≥ 2 distinct variants used,
    and the 3 baked tower textures differ pairwise (> 5% of texels by > 16 alpha).
  - `"a fly-through comes every one to two minutes and caps its whiteout"`: over 300 s at
    100 ms, starts gaps ∈ [60, 120]; at the hold, mean frame colour is within 62% ± 3% of the
    way from the pre-fly mean to (200, 216, 230) (wisps excluded by comparing the centre
    pixel).
  - `"wisps sample smoothly, without radial seams"`: during the hold, across any internal row
    through the centre, adjacent-pixel |Δgrey| ≤ 24. The prototype's nearest sampling shows
    larger jumps.
  - `"the ladder drops layers before coarsening the deck and ends standing still"`:
    - the first level with `layers == 0` precedes the first with scale 4;
    - the last level is `still`;
    - at the still level, 10 unchanged-time frames → dirty empty; a minute change → dirty ⊆
      clock bounds.
  - `"frames allocate nothing and clear frees everything"`: ≤ 4.5 MB at 800×480.
- [ ] FAIL → implement → PASS.

### Task 4.4: Wrapper, registry, settings, translations, manager

As Task 3.5 with:
- `HIGHFLY = 12`, row `{ScreensaverType::HIGHFLY, "highfly", "High Fly", SAVER_DEPTH_16 | SAVER_DEPTH_32}`;
- XML `&#10;High Fly` appended;
- `include/screensaver_highfly.h` (candidate copy), `src/ui/screensaver_highfly.cpp`, and the
  sim .cpp in `app_srcs_excluded.txt`.

The wrapper draws:
- `date_line_` / `day_line_`: `clock_date_text`, or `clock_print_text` (eta / left) while
  printing; colour (226, 238, 250), `LV_OPA_80`, left-aligned;
- `meridiem_`: `clock_meridiem_text(reading.time.meridiem)`, left-aligned.

All are drawn with `under = dirty`, so they redraw when the clouds repaint beneath them.
Clock drift with `clock_drift_at`.

- [ ] Tests (`LVGLTestFixture`, `[screensaver][highfly]`):
  - canvas format and ladder;
  - "the high fly saver shows the print its source reports" (date line starts "ETA ", day
    line ends " left");
  - no buffers after stop;
  - registered at both depths.
- [ ] FAIL → implement → PASS: `make t F='[highfly]'`, `make t F='[screensaver]'`.
- [ ] Translations of `High Fly`: de `Über den Wolken`, fr `Au-dessus des nuages`, es
  `Sobre las nubes`, it `Sopra le nuvole`, pt `Acima das nuvens`, ru `Над облаками`, zh
  `云端飞翔`, ja `雲の上`; `make translations` (+ `make regen-text-fonts` for new CJK).
- [ ] `size build/obj/ui/screensaver_highfly*.o` check.
- [ ] Commit: `git add -N` the new files, then `git commit -m "feat(screensaver): High Fly, a flight over sunlit cloud tops with a thin clock" -- <paths>`.

---

## Part 5: Branch finish

- [ ] `make t F='[clock_reading],[heightfield],[aqua],[highfly],[screensaver_flip_clock],[screensaver]'`,
  then `make full-test-run`.
- [ ] **Shimmer measurement** (whether or not the test went `[slow]`): run
  `./build/bin/helix-tests '[shimmer]' -s` and record the mean |Δgrey| in the branch report.
  The target is about 4.
- [ ] Headless screenshots / recording. Pinned socket per CLAUDE.md:
  `TREE=$(basename "$(git rev-parse --show-toplevel)")`, `HELIX_SOCK=/tmp/helix-$TREE.sock`,
  `HELIX_CONFIG_DIR=/tmp/helix-config-$TREE`, then
  `HELIX_HEADLESS=1 SDL_VIDEODRIVER=dummy ./build/bin/helix-screen --test -vv --remote-socket "$HELIX_SOCK"`.
  - `ctl -s "$HELIX_SOCK" screensaver aqua`, and screenshots at 0, 3, 6 and 9 s.
  - `ctl ... screensaver highfly`, and screenshots at 0, 5 and 10 s. Use
    `HELIX_SCREENSAVER_LEVEL=0`. Force a fly-through by waiting, or by a run whose seed puts
    one early: log `next_fly_s` at `-vv` start.
  - Repeat both with `HELIX_MOCK_AUTO_PRINT=1 --sim-speed 6` for the print readout.
  - "Before" for the flip clock: the same `screensaver flipclock` shot on main and on the
    branch must match.
  - Show them to Preston with exactly what to judge: the water's glassiness against the two
    prototype passes, the fresnel floor 0.12, the 4th softbox band, the tower crowns, and the
    two horizon / focal values flagged in "Spec errors".
- [ ] Gate numbers from the headless run (`ctl` gate output) for both savers at each level.
  Pi 3B (`scripts/screensaver-perf/pi3b_gate.sh`) and one 16 bpp board (`embedded_gate.sh`)
  under a `device:` claim when free.
- [ ] `make mutate-diff` once, at the end.
- [ ] Delete nothing from `docs/devel/plans/` here (the design ships with the batch). Add an
  Aqua / High Fly / shared-clock / heightfield section to `docs/devel/SCREENSAVERS.md` (key
  files rows, ladders), committed with Part 4.
