# Default home layouts by capability and printer

Status: design, awaiting review. Supersedes the "Default layout" section of
`2026-08-12-home-widgets-design.md`, which ruled one re-author once all seven widgets
exist. This ships with the six that exist; `exclude_map` slots in when it lands.

## Goal

A fresh install, a finished wizard or a Reset should land on a home panel that uses the
widgets this printer can actually drive, at sizes where they read well, with no holes.
Saved layouts are never touched: the new defaults reach first run, post-wizard and Reset
only.

## Rulings (Preston, 2026-10-13)

| Question | Ruling |
|---|---|
| Mechanism | Capability rules in `default_layout.json` for every printer, plus hand-tuned preset seeds only where a packaged printer needs one |
| Existing layouts | Fresh and Reset only |
| Timing | Go now with six widgets; reserve a place for `exclude_map` |
| Scope | Every tier (micro through xxlarge, portrait, ultrawide), the U1 and K-Touch gaps, the screenshot recipes |
| Approach | Candidate slots; an unedited default stays live (A) |
| temp_graph | Stays on medium (2x3 cells, unchanged) |
| toolhead | On for toolchangers at every tier; on for everyone at large and up |
| bed_mesh_tile | Large and up only, when the bed-mesh gate is open |
| host_health | Never in a default; catalog only |
| Last 1x1 cells | Notifications is fixed everywhere; LED is a low-priority candidate |

## What is wrong today

Measured on a headless `HELIX_MOCK_PRINTER=k2` at 800x480:

- `build_default_grid` resolved at +0.662s with `ams_slot_count=0`; the CFS backend came
  up at +0.671s. The default stamps once, so a fresh K2 gets the `filament` tile, whose
  gate (`filament_sensor_count`) is closed on a K2: the cell renders as a hole, and the
  CFS never appears. The C++ "AMS replaces filament" swap in `build_default_grid` has this
  race for every late AMS backend.
- A fixed gated anchor whose gate is closed leaves a hole. Nothing back-fills it.
- The six new widgets (speed_flow, z_offset, dryer, host_health, toolhead,
  bed_mesh_tile) appear in no table.
- `assets/config/presets/snapmaker_u1.json` carries `printer.panel_widgets.home` in
  pre-track cell units with no `grid` or `layout_units` tag; at config v27
  `has_uninterpretable_coordinates()` drops it, so the U1 gets the generic tiny table.
  Inferred from code; the implementation confirms it on a mock first.
- `scripts/esp32_stage_assets.py` stages neither `default_layout.json` nor the seeds, so
  the K-Touch falls to the hardcoded three-widget fallback plus auto-placement.
  Confirmed on the device or the ESP32 sim before fixing.
- speed_flow at Wide (2x1 cells) draws its label and value on top of each other
  (seen on the K2 mock, ctl-placed). It must be fixed before speed_flow enters a table.

## Mechanism

### Slots

`default_layout.json` keeps its fixed anchors (`{"id": ..., "placements": ...}`) and
gains a second anchor form, a slot: a region per tier and an ordered list of
alternatives that fill it.

```json
{
  "slot": "spool_env",
  "placements": { "medium": {"col": 8, "row": 0, "colspan": 4, "rowspan": 2} },
  "alternatives": [
    { "cells": [ {"candidates": ["ams"], "at": [0, 0, 4, 2]} ] },
    { "cells": [ {"candidates": ["dryer", "chamber_temperature", "led", "clock"], "at": [0, 0, 2, 2]},
                 {"candidates": ["filament", "active_spool", "led", "clock"],      "at": [2, 0, 2, 2]} ] }
  ]
}
```

- `at` is `[col, row, colspan, rowspan]` in tracks, relative to the region. One
  alternative definition serves every tier whose region has that shape; an alternative
  that does not fit the tier's region is skipped.
- A candidate qualifies when its registry gate is open (the same check
  `compute_visible_widget_ids` makes) and, if it has one, its `when` condition holds.
  `when` names a subject and a comparison, e.g. `{"id": "toolhead", "when": "tool_count gt 1"}`,
  evaluated with the existing subject-expression parser.
- A cell takes its first qualifying candidate not already placed elsewhere in the layout.
- An alternative qualifies when its first cell's first candidate qualifies (the lead).
  The last alternative has no lead requirement. So "AMS present" picks the 2x1 AMS
  tile; otherwise the region splits into two 1x1 cells.
- Every candidate list ends in an ungated widget (`clock` unless the table says
  otherwise), so a resolved layout has no holes. A test enforces it.

Fixed anchors stay for widgets that are always present (printer_image, print_status,
temperature, bed_temperature, notifications, fan_stack, temp_graph, tips).

### Resolution is a pure function

```cpp
// include/default_layout_resolver.h
struct CapabilitySnapshot {             // read from gate subjects at resolve time
    std::map<std::string, int> subjects; // gate name or `when` subject -> value
};
struct ResolvedDefault {
    std::vector<WidgetPlacement> placed; // tracks
    std::string signature;               // tier, grid, variant, preset, every consulted subject
};
ResolvedDefault resolve_default_layout(const json& table, std::string_view tier,
                                       int cols, int rows, LayoutVariant variant,
                                       const CapabilitySnapshot& caps);
```

`build_default_grid()` becomes snapshot + `resolve_default_layout()`. The ams-for-filament
swap moves into data (the `spool_env` slot) and its C++ is deleted. The bed_temperature
reorder and the empty-layout fallback stay where they are.

### An unedited default stays live

- The resolved layout is saved with `"origin": "default"` and the resolver's `signature`.
- `setup_gate_observers()` already queues a coalesced rebuild when any gate fires. On that
  rebuild, a layout whose origin is `default` is re-resolved if the signature changed, so
  a late AMS, dryer or tool count lands in its slot.
- Any user edit (Edit Mode drop or resize, catalog add or remove, `ctl home`, a page
  added) clears `origin`. From then on the layout is the user's and is never re-resolved.
- Reset to defaults writes `origin: default` again.
- Crash-loop safe mode keeps its current rule: show defaults, write nothing.

### Preset seeds

A seed becomes a `default_layout.json` overlay in the same schema:
`assets/config/panel_widgets/<preset>/home.json` with `anchors` entries that replace the
base entry of the same `id` or `slot` per tier. Loaded by
`try_populate_from_preset_seed()` and resolved by the same function, so seeds get slots
and liveness for free.

- `cc1`, `ad5x`, `ad5x_zmod`: deleted if the base tables plus slots reproduce their
  intent (AD5X IFS reaches the `spool_env` slot as an AMS). The implementation renders
  each persona before and after and keeps a seed only where the base table loses
  something.
- `snapmaker_u1`: new seed, toolhead-first. The stale `printer.panel_widgets.home` block
  in `presets/snapmaker_u1.json` is deleted.

## Tables

Cells below (1 cell = 2x2 tracks; the JSON is in tracks). `(col,row,w,h)`.
SPOOL_ENV is the slot above. ENV = `[toolhead when tool_count gt 1, dryer,
chamber_temperature, led, clock]`. FIL = `[filament, active_spool, led, clock]`.

### medium 6x4 (800x480: AD5M, AD5X, K1/K1C/K2, Creator 5, Max 4, K-Touch)

| Widget | Cells |
|---|---|
| printer_image | (0,0,2,2) |
| temperature | (2,0,1,1) |
| bed_temperature | (3,0,1,1) |
| SPOOL_ENV | (4,0,2,1): AMS 2x1, else ENV + FIL |
| fan_stack | (2,1,1,1) |
| notifications | (3,1,1,1) |
| temp_graph | (4,1,2,3) |
| print_status (detailed) | (0,2,4,2) |

### micro 6x4 (480x272: CC1, QIDI Q2)

printer_image (0,0,3,2); temperature (3,0); bed_temperature (4,0); notifications (5,0);
fan_stack (3,1); SPOOL_ENV (4,1,2,1); print_status (0,2,6,2).

### tiny 5x4 (480x320: U1) and small 5x5 (480x400)

printer_image (0,0,2,2); temperature (2,0); bed_temperature (3,0); notifications (4,0);
fan_stack (2,1); SPOOL_ENV (3,1,2,1); print_status (0,2,5,2). Small adds tips (0,4,5,1).
U1 seed: SPOOL_ENV alternatives reordered so `toolhead when tool_count gt 1` leads at 2x1.

### large and xlarge 8x5 (1024x600, 1280x720)

| Widget | Cells |
|---|---|
| printer_image | (0,0,2,2) |
| temperature, bed_temperature | (2,0), (3,0) |
| SPOOL_ENV | (4,0,2,1) |
| ENV2 = `[chamber_temperature, dryer, led, clock]` | (6,0,1,1) |
| notifications | (7,0,1,1) |
| fan_stack | (2,1,1,1) |
| FIL2 = `[filament, active_spool, led, clock]` | (3,1,1,1) |
| z_offset | (4,1,2,1) |
| toolhead | (6,1,2,2) |
| print_status (detailed) | (0,2,4,2) |
| speed_flow | (4,2,2,1) |
| temp_graph | (4,3,2,2) |
| MESH = `[bed_mesh_tile, print_stats]` | (6,3,2,2) |
| tips | (0,4,4,1) |

`exclude_map` joins MESH's candidate list ahead of `print_stats` when it lands.

### xxlarge 12x7 (1920x1080)

The large set plus room: printer_image (0,0,3,3); print_status (3,0,4,3); small readouts
and SPOOL_ENV across (7,0)-(8,2); toolhead (9,0,3,3); MESH at Large rung (9,3,3,3);
temp_graph (0,4,9,2); tips (0,3,7,1) and the remaining row for z_offset and speed_flow.
Exact cells settle in the render pass; budgets checked by `check_anchor_table()`.

### Portrait (micro 4x6, tiny 4x5, medium 4x6 and up)

Keep the current band structure: printer_image beside a 2x2 block, tips, print_status,
bottom row. The 2x2 block becomes temperature, bed_temperature, notifications on top
and SPOOL_ENV beside them where it fits 2x1, else ENV. The bottom row takes fan_stack and
FIL.

### Ultrawide (tiny 36x8 tracks, small 46x10)

Unchanged left-to-right order; the right-hand block of small readouts gains SPOOL_ENV
(2x1) and ENV; tips stays the footer.

## Gaps fixed in the same change

- K-Touch: `scripts/esp32_stage_assets.py` stages `default_layout.json` and
  `panel_widgets/`. Size checked against the firmware image budget.
- U1: the seed above; the dead preset block removed.
- `scripts/screenshot-recipes.sh`: `callouts-4x2` and `widget-catalog-category` locate
  widgets through `ctl geom <name>`, not coordinates of the old default.
- speed_flow Wide rung layout fixed (own commit, own test).

## Testing

- `test_default_layout.cpp`, extended:
  - every tier and variant table, slots included, within budget and non-overlapping for
    every alternative;
  - every candidate list ends in an ungated widget;
  - every alternative fits its region on every tier it is authored for.
- Resolver, table-driven, one row per persona snapshot: k2 (AMS 2x1), ad5m (ENV + FIL,
  LED placed once), u1 (toolhead leads), cc1, qidi_q2 (AMS), voron_24 large (toolhead,
  bed_mesh_tile), multi_extruder tiny (toolhead in ENV). Asserts ids and cells.
- Liveness: resolve with `ams_slot_count=0`, fire the gate with 4, the rebuild places
  `ams` 2x1. Same sequence after a user edit leaves the layout untouched. Reset
  re-arms it.
- Race regression: the K2 mock sequence above, from config load through the first
  READY populate.
- Seeds: each surviving seed resolves within the medium/micro/tiny budgets.
- Render pass: every shipped persona at its own resolution, idle and printing
  (`--sim-speed 6`), `ctl overflow` clean, screenshots for review. Goldens in
  `tests/ui/test_screens.py` re-accepted for home.

## Out of scope

- Re-defaulting saved layouts, migration offers, "new layout available" prompts.
- host_health in any default.
- New gates beyond `when` on an existing subject (tool count is the only new input).
