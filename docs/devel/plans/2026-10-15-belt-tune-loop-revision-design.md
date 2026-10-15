# Belt Tension: tuning loop revision after hardware (design)

Revises `2026-10-09-belt-tune-loop-design.md` (#1736) from two Voron 2.4 sessions
(2026-10-14 and 2026-10-15). The raw notes, CSVs and per-run table are in the SDD workspace,
`hardware-2026-10-14.md`. Where this file and the original design disagree, this file wins.

## Rulings (Preston, 2026-10-15)

- Settling laps run automatically before every sweep that follows a belt change.
- The zone is chosen by a calibration turn, not by peak height.
- Screw turns: ½ to 2 turns in ¼ steps (¼ only when nearly matched).
- "What you turned" starts empty each round; an empty Re-measure is a repeat check.

## What the hardware showed

1. **Belts need settling.** After a turn, the change stays mostly in the span beside the
   tensioner until the gantry runs the belt loop through. Without settling laps, readings sit
   3-6 Hz off and do not return when the turn is undone. Three perimeter laps with no turn
   moved both belts' high peaks up 3-6 Hz.
2. **Turns are bigger than designed.** Settled, one full turn moves the A-B gap about 2-3 Hz.
   A full sweep wanders 3-5 Hz from run to run in the broad 105-140 Hz hump. ⅛ and ¼ steps
   (0.3-0.7 Hz) cannot be seen.
3. **Narrow sweeps repeat far better than full sweeps.** Narrow against narrow, with no turn:
   0.3 Hz (10-14) and 0.6 Hz (10-15). Narrow against full: 2.9 Hz, because the narrow sweep's
   abrupt start reshapes the window.
4. **The tallest pair is the wrong zone.** On the Voron it is the 25-45 Hz frame mode, mostly
   Z motion on A. Tension barely moves it. The tuner steered on it twice and gave false advice
   from a tuned state.
5. **The tallest peak in the belt band hops.** The 105-140 Hz band holds several overlapping
   modes. The tallest moved 2-5 Hz between identical runs; a power-weighted centroid moved
   under 1 Hz.
6. **Both belts drift together.** Common-mode drift of up to 5 Hz between runs cancels in the
   A-B gap; per-belt readings do not.
7. **Belt modes are in-plane.** Use psd_x + psd_y. As A slackened, its hump became 78% Z.
8. **Peak height is not tension.** The Voron's B path has a 120-133 Hz mode with 3-10x the
   in-plane power of A's (also in a Sep 29 Shake&Tune run). Pearson similarity, ours and
   Shake&Tune's, is mostly a height comparison, so it caps below 100% however well the
   tension matches.
9. **The stepper default fakes a turn.** "What you turned" defaults to the suggestion, so a
   Re-measure without turning scored as ¼ turn and gave a false "The gap grew after tightening B".

Tuned result for reference: A -1, B +1 turns from the 10-14 start, 82% on our panel
(86.8% by Shake&Tune's formula), top peaks 133.0 / 132.9 Hz, repeated within 0.7 points.

## Changes

### 1. Settling laps everywhere

One shared step runs before every sweep that follows a belt change: Test Both, Re-test A/B
and every tune round. The step is `BeltSettle` in calibration code, a pure planner plus a
gcode emitter:

- Three perimeter laps, inset `max(20 mm, 7% of the axis span)` from `axis_minimum` /
  `axis_maximum`, at the `[resonance_tester]` probe point's Z (the sweep already moves there),
  then back to the probe point's X/Y.
- Speed `min(300 mm/s, 0.6 x max_velocity)`. Accel untouched.
- Refuse (and say why) when not homed in XY or a print is active; the existing gate already
  covers the print case.
- Timed like a sweep in progress text: "Settling belts".

On this Voron it takes 13 s.

### 2. Zone choice by response, not height

Candidate zones come from the **in-plane** curves only, and a peak must be at least half
in-plane power to count. The zone is then confirmed by a **calibration turn**: the user turns
the looser belt one full turn (direction from the in-plane centroid gap of the strongest
candidate), the loop settles and runs a full sweep of both paths, and the candidate whose
gap moved most beyond 1.5 Hz becomes the zone. That move seeds Hz/turn. If nothing moves
past 1.5 Hz, the loop says the turn did not register and asks for another full turn before
any advice.

### 2a. Already matched at entry

Before asking for a calibration turn, the loop checks the in-plane belt band (the strongest
in-plane candidate) on the round-0 curves. If the A-B centroid gap there is within the
single-sweep noise, Tune Belts says the belts already match and offers Good Enough, with
"Calibrate anyway" as the secondary action. No turn is requested from a tuned state.

### 3. Narrow baseline at entry; analyse the zone only

Once the zone is known, the first narrow round is a BASELINE (no verdict), so every verdict
compares narrow against narrow. Each sweep runs `ZONE_SWEEP_MARGIN_HZ` (10) past both ends of
the zone, and the analysis uses the zone bins only, so the start transient falls outside.

### 4. Track the in-plane centroid, steer on the gap

The per-path reading is the power-weighted centroid of psd_x + psd_y over the zone, which
replaces `refine_peak_in_window` as the tracker. The gap is the A-B centroid difference
from the same round, as today. Verdicts read the gap's change only. Sanity bound: a gap
move above `max(6 Hz, 3 x |turns| x hz_per_turn)` is a measurement fault, re-measure with
no verdict and no rate sample.

### 5. Turn sizes

| | Original | Revised |
|---|---|---|
| First suggestion | ¼ | from the calibration turn's rate, else 1 |
| Step | ⅛ | ¼ |
| Smallest | ⅛ | ½ unless the gap is under 1.5 x tolerance, then ¼ |
| Largest | 1 | 2 |
| `MIN_MOVE_HZ` | 0.5 | 0.8 |

### 6. "What you turned" starts empty

The stepper starts at "not turned" each round. Re-measure with it empty records `no_turn`
(a repeatability round). Setting an amount and tapping Re-measure records a turn. No
default amount is ever recorded as a turn.

### 7. Peak-height difference gets its own hint

When the matched zone's in-plane power differs by more than 3x between paths, the results
show "Path B resonates much more than A at N Hz. That points at a mechanical difference
(pulley grub screws, idler bearings, belt tracking), not tension." This sits beside the
existing shape warning, reusing its modal.

### 8. UI fixes carried from 10-14

- Path note: "done" while the other path runs, and the age refreshes on RESULTS.
- "was N%" moves from the path note to the similarity number.
- Ghost curves normalise to their own maximum.
- Title Case on buttons: Good Enough, Next Zone, Keep Tuning, Tune Belts, Re-test.

## What stays

- The panel structure, view states, gauge, chart, Stop and the gate.
- No absolute frequency target.
- The similarity number, with its current 5-140 Hz band, as a summary of curve shape.
- SPRING and UNKNOWN tensioner kinds keep their words-only paths; settling applies to them too.

## Out of scope here

- Live progressive sweep graphs for belt tension and Input Shaper (separate issue).
- A height-insensitive similarity (normalised or log-scale correlation): to be tested
  offline against the 10-15 CSVs first.
- The pluck mode.

## Hardware verification

On the Voron, from a deliberately detuned state (A +1 from the tuned position):
1. Tune Belts picks a zone in 105-140 Hz, not 25-45.
2. The loop recommends loosening A about one turn and reaches a matched gap within 3 rounds.
3. A no-turn Re-measure reports NO_MOVEMENT, never WRONG_WAY or OVERSHOT.
4. The confirming full sweep scores within 5 points of 82%.
