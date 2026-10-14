# Default home layouts by capability and printer

Status: design, revised after independent review and a render pass on live mocks, awaiting approval. Supersedes the
"Default layout" section of `2026-08-12-home-widgets-design.md`, which ruled one
re-author once all seven widgets exist. All seven now exist; `exclude_map` (gate
`printer_has_exclude_object`) stays in the catalog this pass, since the freed large-tier
cell went to print_stats.

## Goal

A fresh install, a finished wizard or a Reset should land on a home panel that uses the
widgets this printer can actually drive, at sizes where they read well, with no dead
tiles. Layouts the user has changed are never touched.

## Rulings (Preston, 2026-10-13)

| Question | Ruling |
|---|---|
| Mechanism | Capability rules in `default_layout.json` for every printer; preset seeds only where a packaged printer still needs one |
| Existing layouts | Fresh and Reset only |
| Timing | Go now; `exclude_map` landed meanwhile and stays catalog-only this pass |
| Scope | Every tier (micro through xxlarge, portrait, ultrawide), the U1 and K-Touch gaps, the screenshot recipes |
| Approach | Candidate slots; an unedited default stays live (A) |
| temp_graph | Stays on medium (2x3 cells, unchanged) |
| toolhead | Never in a default (esoteric); catalog only. Toolchangers get the AMS tool tile, which lists every tool's filament |
| active_spool | Ranks after filament and LED: at 1x1 it is a bare spool icon |
| Cell freed by toolhead on large+ | print_stats |
| xxlarge grid | Track edge 96 to 72px (1920x1080: 9x6 to about 12x7 cells), measured before committing |
| bed_mesh_tile | Large and up only, when the bed-mesh gate is open |
| host_health | Never in a default; catalog only |
| Last 1x1 cells | Notifications is fixed everywhere; LED is a low-priority candidate |

## What is wrong today

Measured on a headless `HELIX_MOCK_PRINTER=k2` at 800x480 unless marked inferred:

- `build_default_grid` resolved at +0.662s with `ams_slot_count=0`; the CFS backend came
  up at +0.671s. The default stamps once, so a fresh K2 gets `filament` (gate
  `filament_sensor_count` closed on a K2, so it draws as a dead tile: dimmed, cancel
  badge) and the CFS never appears. The C++ "AMS replaces filament" swap in
  `build_default_grid` has this race for every late AMS backend.
- A fixed gated anchor whose gate is closed draws as a dead tile
  (`src/ui/panel_widget_manager.cpp#place_widget`, the `hardware_gated` block). Nothing
  substitutes for it.
- The six new widgets (speed_flow, z_offset, dryer, host_health, toolhead,
  bed_mesh_tile) appear in no table.
- Inferred: `assets/config/presets/snapmaker_u1.json` carries `printer.panel_widgets.home`
  in pre-track cell units with no `grid` or `layout_units` tag; at config v27
  `has_uninterpretable_coordinates()` drops it. Confirmed on a mock before deleting.
- `scripts/esp32_stage_assets.py#stage_config` copies neither `default_layout.json` nor
  `panel_widgets/`, so the K-Touch falls to the hardcoded three-widget fallback plus
  auto-placement.
- speed_flow at Wide (2x1 cells) draws its label and value on top of each other (K2
  mock, ctl-placed). Fixed before speed_flow enters a table.

## Mechanism

### Slots

`default_layout.json` keeps fixed anchors (`{"id", "placements"}`) and gains slots: a
region per tier and an ordered list of alternatives that fill it.

```json
{
  "slot": "spool_env",
  "placements": { "medium": {"col": 8, "row": 0, "colspan": 4, "rowspan": 2} },
  "alternatives": [
    { "cells": [ {"candidates": ["ams"], "at": [0, 0, 4, 2]} ] },
    { "cells": [ {"candidates": "ENV", "at": [0, 0, 2, 2]},
                 {"candidates": "FIL", "at": [2, 0, 2, 2]} ] }
  ]
}
```

- `at` is `[col, row, colspan, rowspan]` in tracks, relative to the region. Odd spans
  are allowed only for a widget whose registry def has `supports_half_col` /
  `supports_half_row`.
- Named candidate lists live once at the top of the file (`"candidate_lists": {"ENV":
  [...], "FIL": [...]}`) and are referenced by name.
- **A candidate qualifies** when its registry gate is open (the check
  `compute_visible_widget_ids` makes), every subject in its `requires` is non-zero, and
  it has not already been placed.
- **Resolution order:** fixed anchors first, then slots in file order. Within a slot,
  the first alternative whose lead (first cell's first candidate) qualifies wins; the
  last alternative has no lead requirement. Each cell takes its first qualifying
  candidate.
- **No dead tiles:** every candidate list ends in an ungated widget (`clock` unless a
  table says otherwise). Fixed anchors are only ever ungated widgets, or gated widgets
  whose gate is open on every printer that can reach that tier; otherwise they become
  a one-cell slot.
- **Losers are disabled:** every widget named in any candidate list that the resolver
  did not place is written `enabled: false`, so auto-placement never tries to fit it
  into a full grid. The existing `disabled` maps stay (tips on micro, tiny and medium;
  temp_graph on micro and tiny). `ui_wizard.cpp`'s `set_enabled_by_id("ams", true)` is
  deleted; the slot decides.
- **Regions inherit** through `choose_placement_key()`'s fallback chain (micro to tiny to
  small; xxlarge to xlarge to large). The last alternative must fit every region it can
  inherit to (tested).
- Seed overlays may replace a base anchor or slot by `id`/`slot`, or remove one with
  `{"id": "tips", "remove": true}`.

The one condition subject is `printer_has_spoolman` (`printer_capabilities_state.cpp`),
existing. It gets an entry in `gate_subject_lifetime()`, and `setup_gate_observers()` observes every `requires`
subject named in the loaded table as well as the registry gates.

### Resolution is a pure function

```cpp
// include/default_layout_resolver.h
struct CapabilitySnapshot {
    std::map<std::string, int> subjects; // every gate and requires subject the table names
};
struct ResolvedDefault {
    std::vector<WidgetPlacement> placed;  // tracks, enabled
    std::vector<std::string> disabled;    // losing candidates and tier `disabled`
    std::string signature;                // tier, cols, rows, variant, preset, each consulted subject
};
ResolvedDefault resolve_default_layout(const json& table, std::string_view tier,
                                       int cols, int rows, LayoutVariant variant,
                                       const CapabilitySnapshot& caps);
std::string arrangement_hash(const json& pages); // id, enabled, col, row, spans, per page; page count. No per-widget config.
```

`build_default_grid()` becomes snapshot + resolve. The ams-for-filament swap moves into
data and its C++ is deleted. The bed_temperature reorder and the empty-layout fallback
stay.

### An unedited default stays live

The layout stores `"default": {"signature": ..., "hash": ...}` beside its pages, inside
the `serialize_pages()` payload so parked per-grid copies carry their own.

- **Unedited** means the stored pages still hash to `default.hash`. No write path needs
  to know about this: Edit Mode, the catalog, `ctl home`, plugin RPCs, page add/remove,
  migrations and an older build that preserves unknown keys all change the hash by
  changing the arrangement, and any of them makes the layout the user's. Per-widget
  `config` is outside the hash, so assigning an LED strip or a print_status style keeps
  the layout live; a re-resolve carries `config` across for ids that survive.
- **When it re-resolves:** at the top of `PanelWidgetManager::populate_widgets`, before
  enabled ids are collected and before the "Widget list unchanged" early return, using
  the last stamped grid. Only while CONNECTED and READY (a teardown or retarget zeroes
  `ams_slot_count` and must not reshuffle the panel), only if unedited, and only if the
  new signature differs. A changed signature saves once; an unchanged one writes
  nothing.
- **Grid change** (rotation, UI scale) on an unedited layout re-resolves for the new
  grid instead of parking and porting.
- `anchors: "pending"` folds into this: pending is a `default` block with an empty
  signature, resolved at the first measured READY populate.
- Reset to defaults resolves and stamps a fresh `default` block.
- Crash-loop safe mode keeps its rule: show defaults, write nothing.

### Preset seeds

A seed is a `default_layout.json` overlay at `assets/config/panel_widgets/<preset>/home.json`,
loaded by `try_populate_from_preset_seed()` and resolved by the same function.

- `cc1`, `ad5x`, `ad5x_zmod`: deleted if the base tables reproduce their intent (AD5X
  IFS reaches SPOOL_ENV as an AMS). The render pass shows each persona before and after;
  a seed survives only where the base table loses something.
- U1: no seed needed. Its Snapmaker backend publishes `ams_slot_count`, so SPOOL_ENV shows
  the AMS tool tile (all four tools and their filament) on every tier.
  The stale `printer.panel_widgets.home` block in `presets/snapmaker_u1.json` is deleted.

## Tables

Landscape tables in cells (1 cell = 2x2 tracks; the JSON is in tracks): `(col,row,w,h)`.
Portrait and ultrawide in tracks, matching the existing variant tables.

Candidate lists:

| Name | Candidates |
|---|---|
| ENV | dryer, chamber_temperature, led, clock |
| FIL | filament, led, active_spool `requires printer_has_spoolman`, clock |
| ENV2 | chamber_temperature, dryer, led, clock |
| MESH | bed_mesh_tile, job_queue |

SPOOL_ENV is the slot above: AMS (any filament system, toolchangers included) at 2x1,
else ENV + FIL. A second ENV or FIL cell elsewhere on the page takes the next candidate
not already placed.

### medium 6x4 (800x480: AD5M, AD5X, K1/K1C/K2, Creator 5, Max 4, K-Touch)

printer_image (0,0,2,2); temperature (2,0); bed_temperature (3,0); SPOOL_ENV (4,0,2,1);
fan_stack (2,1); notifications (3,1); temp_graph (4,1,2,3); print_status detailed
(0,2,4,2).

### micro 6x4 (480x272: CC1, QIDI Q2)

printer_image (0,0,3,2); temperature (3,0); bed_temperature (4,0); notifications (5,0);
fan_stack (3,1); SPOOL_ENV (4,1,2,1); print_status (0,2,6,2).

### tiny 5x4 (480x320: U1) and small 5x5 (480x400)

printer_image (0,0,2,2); temperature (2,0); bed_temperature (3,0); notifications (4,0);
fan_stack (2,1); SPOOL_ENV (3,1,2,1); print_status (0,2,5,2). Small adds tips (0,4,5,1).

### large and xlarge 8x5 (1024x600, 1280x720)

printer_image (0,0,2,2); temperature (2,0); bed_temperature (3,0); SPOOL_ENV (4,0,2,1);
ENV2 (6,0); notifications (7,0); fan_stack (2,1); FIL (3,1); z_offset (4,1,2,1);
print_stats (6,1,2,2); print_status (0,2,4,2); speed_flow (4,2,2,1); temp_graph (4,3,2,2);
MESH (6,3,2,2); tips (0,4,4,1). 40 of 40 cells.

### xxlarge 12x7 (1920x1080 at a 72px track edge)

The track edge drops from 96 to 72px (`include/grid_layout.h#GRID_CELL`), taking 1920x1080
from 9x6 to 12x7 cells (measured on a spike build: 1x1 cell 134x142px; every 1x1 and
2x1 widget used here reads without clipping; temp_graph needs 3 rows to keep its axes
and legend; print_status needs 5 columns, as 4x3 clips its idle data column by 14px).

| Widget | Cells |
|---|---|
| printer_image | (0,0,3,3) |
| print_status detailed | (3,0,5,3) |
| temperature, bed_temperature | (8,0), (9,0) |
| SPOOL_ENV | (10,0,2,1) |
| fan_stack, notifications, ENV, FIL | (8,1), (9,1), (10,1), (11,1) |
| z_offset, speed_flow | (8,2,2,1), (10,2,2,1) |
| temp_graph | (0,3,8,3) |
| tips | (0,6,8,1) |
| MESH | (8,3,4,2) |
| print_stats | (8,5,3,2) |
| FILLER = `[clock, network]` | (11,5,1,2) |

84 of 84 cells, rendered on the spike build with voron_24 (`ctl overflow` reports only the
idle print card's buttons 7px past the bottom, which the 96px build shows too).
Consequences handled in the same task: `tests/unit/test_grid_square_cells.cpp` and the
`LAYOUT_SYSTEM.md` grid table gain the 1920x1080 row; `kLandscapeBudget` follows; an
existing xxlarge layout meets a new grid once and is parked and ported by
`switch_to_grid`, as on a UI Scale change.

### Portrait (tracks)

The existing band structure stays; only the small-widget positions change.

- **micro, medium** (8x12): printer_image (0,0,4,4); temperature (4,0); notifications
  (6,0); bed_temperature (4,2); fan_stack (6,2); tips (0,4,8,2); print_status (0,6,8,4);
  SPOOL_ENV (0,10,4,2); temp_graph (4,10,4,2).
- **tiny** (8x10): as micro with no tips; print_status (0,4,8,4); SPOOL_ENV (0,8,4,2);
  temp_graph (4,8,4,2).
- **small, large, xlarge, xxlarge** (10 wide): printer_image (0,0,4,4); temperature
  (4,0); fan_stack (6,0); notifications (8,0); bed_temperature (4,2); SPOOL_ENV
  (6,2,4,2); tips (0,4,10,2); print_status as today; temp_graph as today (large and up).
- **xxlarge@6x14**: as today, with its `ams` (0,12,6,2) becoming a 6x2 SPOOL_ENV region
  whose last alternative is three 2x2 cells: ENV, FIL, `[led, clock]`.

### Ultrawide (tracks)

- **tiny** (36x8): left side unchanged; right block temperature (28,0), bed_temperature
  (30,0), fan_stack (32,0), notifications (34,0), SPOOL_ENV (28,2,4,2), ENV2 (32,2),
  motion (34,2).
- **small** (46x10): temperature (36,0), bed_temperature (38,0), fan_stack (40,0), ENV2
  (42,0), notifications (44,0), SPOOL_ENV (36,2,4,2), FIL (40,2), motion (42,2), macros
  (44,2). The tiny `disabled` list (active_spool, macros) stays.

## Gaps fixed in the same change

- K-Touch: `esp32_stage_assets.py` stages `default_layout.json` (minified, ~11 KB) and
  `panel_widgets/`. The resolver `.cpp` joins the firmware `app_srcs.txt`, written to the
  no-exceptions rules and checked by `scripts/check_esp32_app_srcs.py`.
- U1: the dead preset block removed (see Preset seeds).
- `scripts/screenshot-recipes.sh`: `callouts-4x2` and `widget-catalog-category` find
  widgets with `ctl geom <name>`, not coordinates from the old default.
- speed_flow Wide rung layout fixed (own commit, own test).
- Docs when it ships: `LAYOUT_SYSTEM.md` (the `default_layout.json` section and the
  defaults-builder paragraph); this plan file deleted.

## Testing

Each of these fails if its feature is removed.

- `test_default_layout.cpp`, extended:
  - every tier and variant, every slot alternative, within budget and non-overlapping;
  - every candidate list ends in an ungated widget;
  - the last alternative of every slot fits every region it can inherit to;
  - odd `at` spans only on half-cell widgets;
  - `mutually_exclusive()` retired with the C++ swap.
- Resolver, table-driven, one row per persona snapshot:
  - k2: AMS 2x1;
  - ad5m: ENV + FIL, no active_spool without Spoolman, LED placed once;
  - u1 at tiny and a voron toolchanger at medium: AMS tool tile 2x1;
  - no table ever places toolhead or host_health;
  - cc1, qidi_q2 (AMS);
  - voron_24 at large (print_stats, bed_mesh_tile) and without a mesh (job_queue);
  - ad5m with Spoolman: active_spool never leads filament or LED.
  Each row asserts ids and cells, and that no default-enabled widget is left enabled
  and unplaced.
- Liveness, through `populate_widgets` (not only the resolver):
  - `ams_slot_count` 0 then 4 while READY places `ams` 2x1;
  - the same sequence after an arrangement edit leaves the layout alone;
  - a per-widget `config` write keeps the layout live and its config survives the
    re-resolve;
  - a stale `default` block whose hash no longer matches (an older build's edit) is
    not re-resolved;
  - no re-resolve while disconnected or not READY; an unchanged signature writes
    nothing;
  - Reset re-arms it.
- Race regression: the K2 mock sequence, from config load through the CFS backend
  arriving after the first READY populate.
- Seeds: each surviving seed resolves within its tier budgets.
- Render pass: every shipped persona at its own resolution, idle and printing
  (`--sim-speed 6`), `ctl overflow` clean, screenshots for review. The home goldens in
  `tests/ui/test_screens.py` re-accepted.

## Out of scope

- Re-defaulting layouts users have changed; migration offers or prompts.
- host_health in any default.
- New capability subjects; the two `requires` subjects already exist.
