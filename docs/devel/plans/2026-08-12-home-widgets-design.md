# Seven new home-panel widgets

Date: 2026-08-12
Status: approved design; size ladders ruled 2026-10-12. Batch 0 (the DRY extractions) is on
main. None of the seven widgets is built, and no branch carries them.

## Goal

Surface seven families of printer data that are live in subjects but have never reached
the home screen. Every widget has a size ladder from half a cell to two cells and beyond,
adapts to print state, and decides its rung with a pure function the tests pin at the real
pixel boundaries.

| # | Widget | Kind | Batch |
|---|--------|------|-------|
| 1 | `speed_flow`: speed / flow override | binding | 1 |
| 2 | `z_offset`: live Z-offset (baby-stepping) | binding | 1 |
| 3 | `dryer`: filament drying | binding | 1 |
| 4 | `host_health`: host and MCU health + sparklines | canvas (reuse) | 2 |
| 5 | `toolhead`: position / homing | canvas (new draw) | 2 |
| 6 | `bed_mesh_tile`: bed mesh heightmap | canvas (reuse) | 2 |
| 7 | `exclude_map`: exclude-objects mini-map | canvas (reuse + mini mode) | 3 |

Plus one carried fix: the pre-print phase is invisible in the print-status widget's
detailed layout.

## Size ladders and state

### Rulings that apply to every widget

- **The floor is half a cell on both axes where the grid offers half cells.** Every
  widget offers the 0.5x0.5, 1x0.5 and 0.5x1 rungs: the registry minimum is `1x1` in
  tracks and each def sets `supports_half_col` and `supports_half_row` (the `half_c` /
  `half_r` columns of `s_widget_defs`, fields in `include/panel_widget_registry.h`). The
  grid honours half-cell spans at medium and large and declines them at micro and tiny,
  where a half-cell placement comes back as 1x1, so on those tiers every ladder starts
  at 1x1.
- **Too small for controls means read-only.** A rung without room for a control draws
  values only, and a tap anywhere opens the widget's full overlay or panel. Controls
  (steppers, Save, Home All, Calibrate) appear only at the rungs named below.
- **Content adapts to print state.** "Printing" is `print_active` (1 while printing or
  paused). Where a widget's idle and printing columns differ, a change in `print_active`
  re-runs the decision, because the content being measured changed.
- **Larger than the top rung adds spacing, not content**, unless a widget's table names a
  rung above 2x2.

### Pixel budgets

Tile sizes the decision functions are designed and tested against, measured with
`ctl geom` on a live grid (the content box is smaller by the widget's chrome):

| Screen | 0.5x0.5 | 0.5x1 | 1x0.5 | 1x1 |
|--------|---------|-------|-------|-----|
| 800x480 (medium) | 55 x 54 | 55 x 113 | 114 x 54 | 114 x 113 |
| 1024x600 (large) | 51 x 53 | 51 x 112 | 108 x 53 | 108 x 112 |
| 480x272 (micro) | n/a | n/a | n/a | 70 x 64 |

The large tier's half cells are smaller than medium's, so every half-cell rung is sized to
fit 51 x 53 / 51 x 112 / 108 x 53. The shipped tiles already set the shape of these rungs:
a 0.5-wide tile draws its glyph alone, and a 1x0.5 tile lays icon and value in a row
(`TileDirection::Row`, `src/ui/panel_widgets/tile_layout.h`).

### Spans in the registry

Registry spans are in tracks (`GridLayout::TRACKS_PER_CELL == 2`; `LAYOUT_SYSTEM.md`
§ "Widget span authoring"): one track is half a cell, so 0.5x0.5 is `1x1` and a 2x1-cell
widget is `4x2`. Every widget below has `min_colspan = min_rowspan = 1`.

| Widget | Default | Min | Max |
|--------|---------|-----|-----|
| `speed_flow` | 4x2 | 1x1 | 8x4 |
| `z_offset` | 4x2 | 1x1 | 8x4 |
| `dryer` | 2x2 | 1x1 | 6x8 |
| `host_health` | 4x2 | 1x1 | 8x8 |
| `toolhead` | 4x4 | 1x1 | 8x8 |
| `bed_mesh_tile` | 4x4 | 1x1 | 12x8 |
| `exclude_map` | 4x4 | 1x1 | 8x6 |

Defaults are whole cells, so auto-placement seats each widget on cell boundaries.

### Measure, decide, publish, bind

Every widget follows `PANEL_WIDGET_GUIDE.md` § "The reference pattern: measure, decide,
publish, bind":

1. **A pure decision function, no LVGL**, header-only beside the widget
   (`src/ui/panel_widgets/<id>_layout.h`). Input: the measured content box (width,
   height), the widths of the strings the rung would draw, and the state that changes
   content (`print_active`, plus each widget's own inputs listed below). Output: a verdict
   struct carrying the rung, `fits`, and the widget's own flags.
2. **Measurement in the widget.** `on_size_changed()` measures the content box with the
   chrome subtracted the way `TileSizing::set_content_root()` does it
   (`src/ui/panel_widgets/tile_sizing.h#set_content_root`); if that measurement is private to
   TileSizing, lift it into a shared helper rather than copying it. Strings are measured
   from the same composer the labels render.
3. **Publish the verdict as subjects**, one int per axis of the verdict. These widgets are
   single-instance, so subjects register through `register_widget_subjects()` at
   registration; a widget that becomes multi-instance registers per-instance subjects in
   its constructor (never `attach()`), as `TiledPanelWidget` does.
4. **XML binds everything else**: visibility, fonts, flow and colour off the verdict
   subjects (`bind_flag_if_*`, `bind_style_if_*`). Enum values are the subject ints.

**One rung chooser.** The rung enum and the chooser live once, in
`src/ui/panel_widgets/size_rung.h`:

```cpp
enum class SizeRung : int { Glance = 0, Strip = 1, Column = 2, Cell = 3, Wide = 4, Full = 5, Large = 6 };
```

| Rung | Nominal size (cells) |
|------|----------------------|
| `Glance` | 0.5x0.5 |
| `Strip` | 1x0.5 (wide, short) |
| `Column` | 0.5x1 (narrow, tall) |
| `Cell` | 1x1 |
| `Wide` | 2x1 |
| `Full` | 2x2 and larger |
| `Large` | 3x3 and larger (`bed_mesh_tile` only) |

`helix::choose_size_rung(content_w, content_h, const RungNeeds& needs)` takes, per rung
the widget offers, the minimum box that rung's content needs (measured, per state) and
returns the largest rung whose box fits on both axes, in the order Large, Full, Wide,
Cell, then Strip or Column, then Glance. A box between rungs (1.5x1, 1x2) takes the largest
rung it holds and spends the rest on spacing. Each widget's decision function calls the
chooser and adds its own flags.

Rung needs come from measured content. Where a boundary is a size class rather than a
content fit (how many MCU or dryer rows a tall tile holds, whether the bed mesh has room
for a colour scale), decide it from the per-tier bands in `include/panel_widget_size.h`
(`w_normal()`, `w_wide()`, `h_tall()`, `h_taller()`; 174 / 264 / 168 / 253 px at
medium, scaled per tier) rather than new constants.

**`fits_at()` refuses what the widget cannot draw.** Each widget overrides
`PanelWidget::fits_at()` with its decision's `fits`: false when the `Glance` content does
not fit the content box. It must be monotonic (fits at a size means fits at every larger
size); `helix::grow_span_to_fit()` (`include/grid_layout.h`) then lifts the span to the
nearest size that draws, in edit mode and on load.

### Status colour

Widgets that judge a value share one classification, in `include/value_status.h`
(namespace `helix::status`, pure functions, no LVGL):

```cpp
enum class ValueStatus : int { Normal = 0, Warn = 1, Danger = 2 };
```

The ints match the existing `ams_env_ind_<n>_humidity_status` encoding (0 ok, 1 warn,
2 danger). Each function returns a classification; each caller maps it to its own
appearance through `bind_style_if_eq` (`#warning` for Warn, `#danger` for Danger, and for
Normal either the default text colour or `#success` where the ruling asks for green).

| Function | Rule |
|----------|------|
| `override_status(int pct)` | Warn when `pct != 100`, else Normal |
| `z_offset_status(bool save_available)` | Warn when `z_offset_save_available`, else Normal |
| `mesh_flatness_status(float range_mm)` | Normal below 0.15, Warn below 0.30, else Danger (Normal draws `#success`) |
| `host_health_status(const HostHealthInput&)` | Worst of the host and MCU rules below; returns the status and the metric that caused it |

`host_health_status` rules, as fixed defaults:

| Metric | Warn | Danger |
|--------|------|--------|
| Host CPU load | >= 70% | >= 90% |
| Each MCU load | >= 70% | >= 90% |
| Host CPU temperature | >= 70 C | >= 80 C |
| Throttle (`perf_host_throttle_state`, Pi bitmask) | an "occurred since boot" bit set (bits 16-19) | a "now" bit set (bits 0-3) |
| MCU retransmits | count rose since the previous sample | |
| Disk free | below `kDiskLowFreeMb` (500 MB) | |

The Performance overlay can adopt the same function later; this design does not change
the overlay.

## Batch 0: DRY extractions (done)

On main, under these names:

- **D1.** `helix::tune::set_speed_percent` / `set_flow_percent` (`include/tune_controller.h`),
  clamps speed 50..200 and flow 75..125; `volumetric_flow_mm3_s` and
  `status_speed_flow_text` share the readout.
- **D2.** `helix::zoffset::adjust(api, ps, session_base_mm, current_offset_mm, delta_mm)`
  (`include/z_offset_utils.h#"AdjustResult adjust("`), +/-2 mm session travel, absolute `Z=` where the
  firmware persists the offset; step table `kZStepAmountsMm` with
  `persisted_step_index()` / `set_persisted_step_index()`, persisted per printer.
- **D3.** `helix::BedCoordMapper` (`include/bed_coord_mapper.h`).
- **D4.** `helix::bed_dimensions(api, ps)` (`include/bed_dimensions.h#bed_dimensions`),
  centre-origin beds included.
- **D5.** `helix::axis_is_homed(ps, Axis)` and `toolhead_is_homed(ps)`
  (`include/toolhead_homing.h`).
- **D6 (open, not on the widget path).** `dryer_current_temp_text`,
  `dryer_target_temp_text` and `dryer_time_text` have no writer, and the scalar `dryer_*`
  subjects follow `dryer_mirror_unit_`, which tracks whichever unit the environment overlay
  last browsed. The `dryer` widget does not read them: it reads the per-unit
  `ams_env_ind_<i>_*` subjects and `get_environment_zones()`. D6 is cleanup for whoever
  next touches `src/printer/ams_state_dryer.cpp`: write or delete the three text subjects
  and the stale `AmsDryerCard` comment.

## Batch 1: binding widgets

### 1. Speed / flow override (`speed_flow`)

| Rung | Idle | Printing |
|------|------|----------|
| 0.5x0.5 | Speedometer icon + speed % | Same; with physical units on, the measured speed replaces the % |
| 1x0.5 / 0.5x1 | Speed and flow percentages, "%" dropped | Same; with physical units on, measured values replace the percentages |
| 1x1 | Speed and flow percentages | Percentages, each with its measured value under it (mm/s, mm³/s) |
| 2x1 | Labelled rows: "Speed" / "Flow" + % | % primary, measured value at the right |
| 2x2+ | Read-only labelled values, no steppers | Inline `-`/`+` steppers at 5% per row; long-press a value resets that axis to 100%; measured value in the row header |

- **Colour:** each percentage draws through `override_status(pct)`: amber when off 100,
  at every rung.
- **Physical units.** `DisplaySettingsManager::get_speed_flow_physical_units()`
  (`include/display_settings_manager.h`) applies only to the rungs below 1x1-printing,
  and only while printing: those show the measured value instead of the %. Rungs with
  room show both, % primary, whatever the setting.
- **Data:** `speed_factor` and `flow_factor` (int %, default 100, from
  `src/printer/printer_motion_state.cpp#update_from_status`; flow is Klipper's
  `extrude_factor`). Measured speed is `live_velocity` (mm/s); volumetric flow is
  `helix::tune::volumetric_flow_mm3_s(live_extruder_velocity, filament diameter)` with the
  diameter from `PrinterDiscovery::filament_diameter_mm()`. Text composes through
  `helix::tune::status_speed_flow_text()`, the composer the print-status readout uses
  (`PrintProgressText::refresh_speed_flow`). Measured values are drawn only while printing,
  where a 0 means stopped rather than unknown.
- **Actions:** steppers call `helix::tune::set_speed_percent` / `set_flow_percent` with
  the current value +/-5; long-press calls them with 100.
- **Tap** outside the steppers: `get_print_tune_overlay().show(parent_screen, api,
  printer_state)` (`include/ui_print_tune_overlay.h#get_print_tune_overlay`), idle or
  printing.
- **Gate:** none.
- **Verdict:** rung, `show_measured` (printing and the rung has room
  or physical units apply), `show_steppers` (Full and printing).

### 2. Live Z-offset (`z_offset`)

| Rung | Idle and printing |
|------|-------------------|
| 0.5x0.5 | Icon + value, 2 decimals |
| 1x0.5 / 0.5x1 | Icon + value, 3 decimals |
| 1x1 | Icon, value, and a caption: "mm Z-offset", or "<delta> unsaved" while a session delta is pending |
| 2x1 | 1x1 content + a `<z_offset_indicator>` cross-section, and a Save button |
| 2x2+ | Up/down steppers at the persisted step, a step chip, Save under the steppers; "Tool N" label on a tool changer |

- **Value:** `helix::zoffset::displayed_z_offset_microns(PrinterState&)`
  (`include/z_offset_utils.h#displayed_z_offset_microns`), published into a widget-owned
  subject. Never bind `gcode_z_offset`: firmware that persists the offset outside
  `gcode_move` reads 0 there at idle (`Z_OFFSET_PERSISTENCE.md`). On a tool changer the
  value is the active tool's offset.
- **Caption delta:** `pending_z_offset_delta`.
- **Colour:** the value draws through `z_offset_status(z_offset_save_available)`: amber
  while there is something to save.
- **Save:** from 2x1 up. Hidden unless `z_offset_save_available`
  (`helix::zoffset::init_save_available_subject`, the whole save rule including dirty
  per-tool offsets), disabled while `print_active`. Calls
  `helix::zoffset::save_dirty_offsets_shared()`.
- **Steppers:** 2x2+, live both idle and printing. Each press calls
  `helix::zoffset::adjust(api, ps, session_base_mm, current_offset_mm, +/-step)`. The step
  is `kZStepAmountsMm[persisted_step_index()]`; the chip cycles the table and writes
  `set_persisted_step_index()`, shared with the tune overlay.
- **Indicator:** `<z_offset_indicator>` (`include/ui_z_offset_indicator.h`), auto-ranging.
  Microns convert with `std::lround(mm * 1000.0)`; the tune overlay's indicator feed in
  `src/ui/ui_print_tune_overlay.cpp` truncates with `static_cast<int>`, and this widget
  fixes that call site rather than copying it.
- **Tap** outside the steppers and Save: the tune overlay.
- **Gate:** none; adjusting works on any Klipper printer.
- **Verdict:** rung, decimals (2 or 3), `show_save_row`,
  `show_steppers`, `show_tool_label`.

### 3. Filament drying (`dryer`)

Three independent drying sources, read per source:

| Source | Availability | Live state | Opens |
|--------|--------------|------------|-------|
| Box dryers, per unit | a zone with `dryer.supported` in `backend->get_environment_zones(unit)` | per-unit `ams_env_ind_<i>_*` (`AmsState::get_env_ind_drying_active_subject(unit)`, `_drying_text_`, `_temp_text_`, `_humidity_text_`); `ams_units_dryer_version` bumps when the set of running dryers changes; `EnvironmentZone::dryer` (`DryerInfo`) carries remaining and duration minutes and temps | `open_environment_for_unit(unit)` (`include/ui_ams_environment_overlay.h`) |
| Chamber-heater drying | `printer_has_chamber_dryer` | `chamber_dryer_active`, `chamber_dryer_text`; `TemperatureController::chamber_dryer()` returns its `DryerInfo` | `ChamberDryerModal::show_owned()` |
| Bed drying | `printer_can_bed_dry` | `bed_drying_state` (0 idle), `bed_drying_text`, from `BedDryingController` | `BedDryingModal::show_owned()` |

The widget never reads the scalar `dryer_*` subjects (D6): they follow the overlay's
browsing.

| Rung | Nothing drying | Drying |
|------|----------------|--------|
| 0.5x0.5 | Icon + highest box RH | Progress ring + time left |
| 1x0.5 / 0.5x1 | Highest RH | Time left + temperature |
| 1x1 | "NN% RH" / "Not drying" | Ring + time left + current/target temperature |
| 2x1 | "NN% RH" / "Not drying" | One source: ring + source name (+ material) + temps + RH + end time. Two or more: "N drying" with each source's time left |
| 2x2+ | One row per source (idle boxes included): name, progress bar, time, temperature, RH; more rows when taller | Same rows |

Print state does not change this layout.

- **Highest RH** is the maximum `EnvironmentData::humidity_pct` across every unit's zones
  that report humidity. A tile on a printer with no humidity source shows "Not drying"
  alone.
- **Progress** is `1 - remaining / duration` from each source's `DryerInfo`. A source
  whose progress is unknown publishes -1 (the convention `dryer_progress_pct` uses) and
  draws its time text with no ring, never a ring at 0.
- **No start or stop on the tile.** Below 2x2 a tap opens the environment overlay
  (`open_environment_for_unit`, which picks the zone overview when there are several);
  with only chamber or bed drying available it opens that source's modal. At 2x2+ tapping
  a row opens that row's source.
- **New prerequisites:**
  - `helix::DryerSummary`, a pure function over the box zones, the chamber `DryerInfo`
    and active flag, and the bed state, returning the source rows, the drying count, the
    highest RH and availability. A small model observes the three sources'
    subjects and `ams_units_dryer_version`, runs it, and publishes the widget's subjects.
  - `home_dryer_available`, published by that model (any box zone with a dryer, or
    `printer_has_chamber_dryer`, or `printer_can_bed_dry`), as the registry's
    `hardware_gate_subject`. It registers before the panel widget manager resolves gates.
  - A bed drying progress value: `BedDryingController` tracks the run in `tick()` but
    publishes only state and text. It gains a remaining/duration accessor (or a
    `bed_drying_progress_pct` subject) so the bed row can draw a ring.
- **Verify** with `HELIX_MOCK_DRYER` for box dryers, and a mock persona with a chamber
  dryer and with bed drying for the other two sources (`MOCK_ENVIRONMENT_VARIABLES.md`).
- **Verdict:** rung, `show_ring`, `row_count` (rows that fit at 2x2+).

## Batch 2: canvas widgets

### 4. Host health (`host_health`)

| Rung | Idle | Printing |
|------|------|----------|
| 0.5x0.5 | Chip icon tinted by the worst status + the metric that caused it (CPU % when Normal) | Same |
| 1x0.5 | CPU %, temperature | Same |
| 0.5x1 | CPU %, temperature, memory | Same |
| 1x1 | CPU % + 60 s sparkline, "temp · mem" line | Same |
| 2x1 | CPU and Mem sparkline rows | CPU row + busiest MCU (load, retransmits) |
| 2x2+ | Throttle warning line (hidden when clear); host block: CPU and Mem sparklines, temperature, memory free, disk free; then one row per MCU (sparkline, load, retransmits); more MCUs when taller | Same |

- **Data** (`include/performance_state.h`, namespace `helix::perf`):
  `perf_host_cpu_pct`, `perf_host_mem_pct_used`, `perf_host_cpu_temp_c10` with
  `perf_host_cpu_temp_present`, `perf_host_mem_free_mb`, `perf_host_throttle_state`,
  `perf_host_throttle_text`, `perf_host_cpu_pct_text`, `perf_host_mem_free_text`,
  `perf_mcu_names` and per-MCU `perf_mcu_<name>_{load_pct,retrans,present,text}`. Source:
  `MoonrakerPerformanceSource` at about 1 Hz; the mock source in `--test`.
- **History:** `PerformanceState::read_history(name)`, 60 samples, for `host_cpu_pct`,
  `host_mem_pct_used` and `mcu_<name>_load_pct`.
- **Throttle:** bind `perf_host_throttle_text`; never re-decode the bitmask.
- **Status colour:** `host_health_status()` over the latest sample plus the previous MCU
  retransmit counts. The verdict publishes the status and a cause enum; the glance binds
  the cause to pick which value it shows.
- **Sparklines:** `<helix_sparkline>` (`src/ui/helix_sparkline.cpp`), which draws on
  `LV_EVENT_DRAW_MAIN_END` with no buffer and takes its colour from the line style, so
  `bind_style` drives the tint.
- **Lifetime:** the `perf_*` subjects are torn down and recreated on reconnect and printer
  switch. Every observer passes `PerformanceState::instance().subjects_lifetime()`.
- **New prerequisite: disk free.** `PerfSample` (`include/performance_source.h`) gains
  disk total and free; `apply_sample` publishes `perf_host_disk_free_mb` and
  `perf_host_disk_free_text`. Moonraker's `/machine/system_info` carries no free-space
  figure; the source is the `disk_usage` block of `server.files.get_directory` on the
  `gcodes` root, fetched at a slow cadence (once a minute), not per sample. The mock
  source synthesizes it.
- **Gate:** `perf_available`. It reads 0 until the first sample, so the widget can appear
  a second after connect.
- **Tap:** the Performance overlay, via `get_system_settings_overlay().open_performance()`
  (`include/ui_settings_system.h`).
- **Verdict:** rung, `show_mcu_row` (2x1 printing), `mcu_rows`
  (2x2+).

### 5. Toolhead position / homing (`toolhead`)

| Rung | Idle | Printing |
|------|------|----------|
| 0.5x0.5 | Homed letters XYZ, green when homed, red when not | Z height |
| 1x0.5 | Homed letters | Z height |
| 0.5x1 | X / Y / Z values, a dash placeholder for an unhomed axis | Same |
| 1x1 | Three axis rows with a homed dot (the Controls card's language) | Same |
| 2x1 | Top-down plate + crosshair + Z bar + XYZ values | Same |
| 2x2+ | Homed: plate, crosshair, Z bar, XYZ. Unhomed: dashed plate, "Position unknown", red letters, Home All button | Larger plate, XYZ row, active tool + live speed |

- **Positions** (`PrinterMotionState`, centimillimetres): `live_position_*` while the
  toolhead moves, `position_*` otherwise. `gcode_z_offset` is microns; never mix scales.
- **Homing:** `helix::axis_is_homed(ps, Axis)` / `toolhead_is_homed(ps)`
  (`include/toolhead_homing.h`). Never bind ControlsPanel's `x_homed` family; another
  panel's `SubjectManager` owns those.
- **Plate geometry:** `helix::bed_dimensions(api, ps)` mapped through
  `helix::BedCoordMapper`, re-resolved when `api->get_build_volume_version_subject()`
  bumps. `get_axis_bounds()` bounds the Z bar. Kinematics decide the drawing:
  `printer_bed_moves` (bed vs gantry Z), and a delta draws a round plate.
- **Home All:** 2x2+ only, hidden when homed and while printing; sends `G28` with no
  confirmation prompt.
- **Live speed** (2x2+ printing): `live_velocity`. **Active tool:** `ToolState`.
- **Tap:** `get_global_motion_panel().show(...)`, as `MotionWidget::handle_click` does.
- **Gate:** none.
- **Drawing.** A plain `lv_obj` with an `LV_EVENT_DRAW_MAIN_END` handler, following
  `src/ui/helix_sparkline.cpp`: the impl pointer as per-callback `user_data`, an
  `LV_EVENT_DELETE` callback freeing it, `lv_draw_*` into `lv_event_get_layer(e)` in
  absolute coordinates. Not an `lv_canvas`. Use `DRAW_MAIN_END` or `DRAW_POST`, never
  `LV_EVENT_DRAW_TASK_ADDED`: in LVGL 9.5 it fires after those, and `lv_draw_*` calls from
  it draw nothing.
- **Redraw cap, about 4 Hz.** Position subjects can update far faster than that during a
  move. Every position and homing observer funnels into one `CoalescedTimer` member
  calling `schedule_once([this]{ lv_obj_invalidate(obj_); })` with a 250 ms period.
  `schedule_once` is leading-edge and bounded; `schedule()` is trailing-edge and starves
  under a continuous producer (`include/ui_coalesced_timer.h`). Never `lv_async_call`
  from a draw path, and never `memset` a struct holding a `CoalescedTimer`.
- **Verdict:** rung, `show_plate`, `show_home_button`.

### 6. Bed mesh heightmap (`bed_mesh_tile`)

| Rung | Idle and printing (static during a print) |
|------|-------------------------------------------|
| 0.5x0.5 | Grid icon + range, tinted; grid-off icon when no mesh is loaded |
| 1x0.5 / 0.5x1 | Small heatmap + range |
| 1x1 | Heatmap + range, or "No mesh loaded" |
| 2x1 | Heatmap + profile name, range, min..max, grid size |
| 2x2 | 2D heatmap + colour scale + profile name + profile count. No mesh and idle: Calibrate and Load profile buttons |
| 3x3+ (`Large`) | Static, non-interactive 3D render on full-tier boards; 2D elsewhere |

- **Colour:** the range draws through `mesh_flatness_status(range_mm)`: green below
  0.15 mm, amber below 0.30 mm, red above.
- **New prerequisite: mesh stats in shared state.** The `bed_mesh_*` subjects
  (`bed_mesh_available`, `_profile_name`, `_dimensions`, `_max_value`, `_min_value`,
  `_variance`, `_profile_count`) are owned by `BedMeshPanel` and written only from
  `BedMeshPanel::on_mesh_update_internal`, whose subscription starts when the panel is
  created, so a home tile reads stale values until the user opens the panel. Move them
  into a state owner that exists from connect:
  - A pure `helix::compute_mesh_stats(const BedMeshProfile&)` returning min, max, range,
    grid size and whether a mesh is loaded (`probed_matrix_has_mesh()`,
    `include/bed_mesh_presence.h`, since Klipper reports `[[]]` for none). The panel's
    update path calls the same function.
  - The state owner subscribes to `bed_mesh` notifications through the API (the cache
    behind `api->advanced().get_active_bed_mesh()` is warm from connect because
    `bed_mesh` rides the union subscription), publishes the subjects, and bumps a
    `bed_mesh_version` subject on every profile or matrix change. The panel binds the
    moved subjects unchanged.
- **Rendering.** `<bed_mesh>` (`include/ui_bed_mesh.h`) renders on
  `LV_EVENT_DRAW_POST` and has a 2D heatmap path. Below `Large`, and on boards that are
  not full-tier, the tile sets `BedMeshRenderMode::Force2D` through
  `ui_bed_mesh_set_render_mode`, leaves `ui_bed_mesh_set_async_mode` off, and installs no
  touch-drag handlers. Feed it with `ui_bed_mesh_set_data` and `ui_bed_mesh_set_bounds`,
  mean-normalized and offset with `ui_bed_mesh_set_z_display_offset` as the panel does,
  and only when `bed_mesh_version` bumps. The renderer governs its own frame rate; wrap it
  in no extra timer.
- **3D cost.** The 3D rung renders synchronously. Measure its frame time on a full-tier
  board before enabling it; if the synchronous render does not hold the frame rate, the
  rung draws 2D and the async render thread stays off the tile.
- **Calibrate** homes first through `helix::ensure_homed_then()`
  (`include/toolhead_homing.h`), no prompt, then sends `BED_MESH_CALIBRATE`. **Load
  profile** opens the Bed Mesh panel's profile list. Both hidden while printing.
- **Tap:** `get_global_bed_mesh_panel().show(screen)`.
- **Gate:** `printer_has_bed_mesh` (a configured `[bed_mesh]`, not a loaded mesh).
- **Verdict:** rung, `has_mesh`, `show_actions`, `render_3d`.

## Batch 3: exclude-objects mini-map (`exclude_map`)

| Rung | Printing, two or more objects |
|------|-------------------------------|
| 0.5x0.5 | Icon + remaining/total ("5/6") |
| 1x0.5 | "N objects · M skipped" |
| 0.5x1 | Tiny map + count |
| 1x1 | Outlines only (current object bright, excluded hatched) + "5 of 6 printing" |
| 2x1 | Map + "Now printing <name>" + counts |
| 2x2+ | Larger map + name + count + a "Skip objects..." button that navigates only |

Not printing, or fewer than two objects (`defined_objects_count < 2`): a quiet
placeholder at every rung, icon + "No objects" or "Single object".

- **Read-only at every size.** The tile never excludes anything. A tap, or "Skip
  objects...", opens the full map in print status, where the confirmation modal and the
  5-second undo window guard the irreversible RPC
  (`src/ui/ui_print_exclude_object_manager.cpp`). At tile size the minimum touch rects
  (`BedCoordMapper::MIN_TOUCH_TARGET_PX`) inflate and overlap, so per-object hit testing
  would be ambiguous, which is the wrong property for an irreversible action.
- **Data:** `PrinterExcludedObjectsState` via `excluded_objects_state()`:
  `defined_objects_count`, `defined_objects_version`, `excluded_objects_version`,
  `get_defined_objects()`, `get_excluded_objects()`, `get_current_object()` (no subject;
  read on a version bump and on each status update), `get_object_geometry(name)`.
  Remaining = defined minus excluded.
- **Outlines:** parsed G-code first-layer outlines when the active print has a parse,
  else Klipper's polygon, else its bbox: the same rule as the full map.
- **Gate:** `printer_has_exclude_object`. Never `exclude_objects_available` or
  `exclude_map_active`: both are `PrintStatusPanel`-owned.
- **New prerequisites:**
  1. **A public entry into the full map.** `PrintStatusPanel::show_exclude_map_view()` is
     private. Add a public method that navigates to print status and opens exclude mode
     through its `ExcludeModeController`, and call it from the tile.
  2. **An outline source independent of print status.** The parse reaches the map today
     only through print status's G-code viewer (`ui_gcode_viewer_get_parsed_file`). The
     tile needs an accessor for the active print's parse that does not require the print
     status panel to exist; without one it draws Klipper geometry.
  3. **A resize path in `ExcludeObjectMapView`.** `create()` sizes the mapper and canvas
     from the plate area once; there is no `LV_EVENT_SIZE_CHANGED` handling. A tile whose
     size arrives through `on_size_changed` needs a rebuild on resize.
  4. **A normal layout slot.** `ui_xml/components/exclude_object_map.xml` roots the view
     with `ignore_layout="true"` at 100%/100% to cover the print-status card. The mini
     mode takes a normal slot in the tile.
  5. **A mini render mode:** outlines only, no number badges, no dims label, no close
     button, hatching for excluded objects.
- **Verdict:** rung, `placeholder` (not printing or fewer than two
  objects), `show_skip_button`.

## Carried fix: pre-print in the detailed layout

`ui_xml/components/panel_widget_print_status.xml` binds `print_start_phase`,
`print_start_progress`, `print_start_message` and `print_start_time_left` in the compact
card's preparing block, and `PrintStartCollector` drives those subjects app-globally. The
detailed active layout, `ui_xml/components/print_status_detailed_active.xml`, binds none
of them, so during the preparing window it shows an arc near 0% and zero times with no
phase. Add the preparing block there, bound to the same subjects and hidden on
`print_start_phase == 0`.

## Per-widget authoring checklist

1. `src/ui/panel_widgets/<id>_layout.h`: the pure decision function and verdict struct,
   calling `choose_size_rung()`. Its tests come first (see Testing).
2. `src/ui/panel_widgets/<id>_widget.{h,cpp}`: subclass `PanelWidget`; `attach()`,
   `detach()`, `on_size_changed()` (measure and publish only), `fits_at()`. Instances are
   recycled across rebuilds, so the publish runs from `attach()` too.
3. `register_<id>_widget()`: forward-declare it in `src/ui/panel_widget_registry.cpp` and
   call it from `init_widget_registrations()`; register the verdict subjects there.
4. Registry row in `s_widget_defs` with the spans from the table above, both half-cell
   flags set, and the gate subject.
5. `ui_xml/components/panel_widget_<id>.xml` (the name must be exactly
   `panel_widget_<id>`), registered in `src/xml_registration.cpp`. Mark every
   non-scrolling container `scrollable="false"`.
6. A row for the id in `expected` in `tests/unit/test_registry_span_bands.cpp`. Add it
   with placeholder bands, run, read the `INFO` output, pin the real values.
7. `make t F='[content_fits]'`. The sweep picks up the registry row on its own. A widget
   that clips at a size `fits_at()` accepts is a bug in the decision function; a
   `kKnownClipping` entry needs a written reason.
8. Regenerate derived artifacts: `make regen-tokens` for every new or edited `ui_xml`
   file; `make translation-sync` then `make translations` for new strings (wrap them in
   `lv_tr()` or `label_tag`); `make regen-fonts` only for a new icon codepoint, added to
   both `include/ui_icon_codepoints.h` and `scripts/regen_mdi_fonts.sh`.
9. Docs and screenshots (below).

`test_panel_widget_manager.cpp` requires every `hardware_gate_subject` to be
XML-registered, which covers `home_dryer_available`, `perf_available`,
`printer_has_bed_mesh` and `printer_has_exclude_object`.

## Sequencing

1. **Shared pieces first**, one commit each with their tests: `size_rung.h` with
   `choose_size_rung()`, `value_status.h`, and the content-box measurement helper if it
   has to be lifted out of TileSizing.
2. **Batch 1:** `speed_flow`, `z_offset`, then `dryer` (after `DryerSummary`,
   `home_dryer_available` and the bed drying progress value).
3. **Batch 2:** `host_health` (after disk free), `toolhead`, then `bed_mesh_tile` (after
   the mesh-stats move, which is the largest prerequisite and lands as its own commit with
   the panel converted).
4. **Batch 3:** `exclude_map`, after its five map-view prerequisites.
5. **Default layout** as the last task, below.

Every widget edits the same three shared files (`panel_widget_registry.cpp`,
`xml_registration.cpp`, `test_registry_span_bands.cpp`), so widgets are implemented
serially in one worktree. Prerequisites that touch none of those (disk free, mesh stats,
`DryerSummary`, the map-view changes) can run in parallel worktrees and merge ahead of the
widget that needs them.

Before Batch 1, run `[content_fits]` unfiltered and confirm it reports no
`[ISOLATION-LEAK]` thread leak: all seven widgets enter that sweep.

## Testing

- **Decision functions, table-driven,** one `[<id>][layout]` case set per widget. Each
  rung boundary is pinned exactly (one pixel either side) at the measured content boxes of
  the medium tier (the four sizes in the pixel-budget table), the large tier's half cells
  (51x53, 51x112, 108x53) and the micro 1x1 (70x64),
  for each state the ladder distinguishes (idle, printing, physical units, mesh or no
  mesh, nothing drying / one / several, placeholder). Each also pins `fits = false` below
  the `Glance` content and monotonic `fits` across a sweep of sizes.
- **`choose_size_rung()` and every `helix::status` function**, table-driven at their
  thresholds (`[size_rung]`, `[value_status]`).
- **Widget tests** with `PanelWidgetHarness<W>` (`tests/test_helpers/panel_widget_size_harness.h`)
  asserting the bound outcome per rung (which parts carry HIDDEN, which style is bound),
  and a `print_active` transition re-deciding the rung.
- **Prerequisite tests:** `compute_mesh_stats` on a real matrix and on `[[]]`; the stats
  owner publishing before the panel exists; `DryerSummary` across the three sources;
  disk-free parsing of a `get_directory` response; the map view rebuilding on resize.
- **Gates per widget:** `make t F='[span_bands]'`, `[content_fits]`, `[widget_def]`,
  `[panel_widget_config]`, `[panel_widget_manager]`, `[grid_edit]`, `[card_merge]`.
  `make mutate-diff` once per branch. `make full-test-run` once, on the batch landing.
- **Screenshots per rung.** In a `--test` run with a pinned socket and config dir and
  `SDL_VIDEODRIVER=dummy`, place the widget at each rung's span, capture with
  `ctl screenshot`, crop with `ctl geom`, at 800x480 and 480x272, idle and printing
  (`--sim-speed 4-10`). The dryer needs `HELIX_MOCK_DRYER`.
- **Expected red:** `test_default_layout.cpp`'s "every non-multi-instance def appears once
  in the default grid" fails from the first widget row until the default layout task.
  Record it in the batch ledger.

## Documentation and screenshots

**User docs** go in `docs/user/guide/home-panel.md`, one guide page for every home
widget. Per widget:

1. A row in the right `## Available Widgets` category table. Sizes there are whole cells,
   so the registry's track spans are halved (a `1x1` track minimum is "0.5x0.5").
2. A row in `## Widget Interactions` (tap action).
3. A row in `### Hardware-Gated Widgets` for the gated four: `dryer`, `host_health`,
   `bed_mesh_tile`, `exclude_map`.
4. An optional deep-dive `##` section with its own screenshot, as `## Clog Detection
   Widget` has, for widgets with non-obvious state: `speed_flow`, `z_offset`, `dryer`,
   `exclude_map`.

Cross-link both ways with the existing feature sections: `guide/printing.md` § "Print
Tune Overlay", § "Z-Offset / Baby Steps", § "Exclude Object"; `guide/filament.md`
§ "Filament Drying and Humidity"; `guide/settings/system.md` § "Performance";
`guide/calibration.md` § "Bed Mesh"; `guide/motion.md` for the toolhead. User-doc style
rules apply (`docs/user/CLAUDE.md`): no source files or class names, exact UI paths.

**Screenshots:** 800x480, dark mode, `helixscreen` theme, committed to
`docs/images/user/<name>.png`. A widget is illustrated by the overlay it opens (a recipe in
`scripts/screenshot-recipes.sh` plus a `PANELS` entry in `scripts/screenshot-all.sh`), by
the home screen when the widget is in the default grid, or by a hand-crop of that shot.
No gate checks doc screenshots, and `home-panel.md` HTML-comment placeholders let the
docs land ahead of the images.

**Other artefacts:** `CHANGELOG.md` `## [Unreleased] / ### Added`; the widget-ID list in
`docs/user/CONFIGURATION.md` ("Available widget IDs"); the "30+ widgets" counts in
`README.md`; `docs/devel/PANEL_WIDGET_GUIDE.md` gains a section on `SizeRung`,
`choose_size_rung()` and `helix::status`, since they become the
pattern for new widgets. The widget catalog's `display_name` / `description` /
`hardware_gate_hint` strings go through `lv_tr()` but the extractor scans only XML, so
the catalog stays English-only; these seven inherit that.

## Default layout

Ruling (Preston, 2026-08-12): a new default layout is authored once all seven widgets
exist.

- Do not edit `assets/config/default_layout.json` per widget.
- The default-layout test is red from the first widget row until this task (see Testing).
- As the final task of Batch 3, author the default layout with all seven, the anchor
  tables for every breakpoint tier, and the per-printer preset seeds
  (`assets/config/panel_widgets/{cc1,ad5x,ad5x_zmod}/home.json`). Spans are in tracks;
  `check_anchor_table()` enforces per-tier budgets and non-overlap.

## Out of scope

- The calibration-status tile.
- A history rung for `speed_flow` (no 4x2 rung; larger tiles add spacing).
- Start or stop drying from the tile.
- Excluding an object from the tile.
- Moving the Performance overlay onto `host_health_status()`.
- Finishing the `homed_axes` consolidation: ControlsPanel and MotionPanel still decode
  `homed_axes` into subjects that XML binds, which is its own task with its own tests.
- The write-only `controls_speed_pct` subject in `src/ui/ui_panel_controls.cpp`.
