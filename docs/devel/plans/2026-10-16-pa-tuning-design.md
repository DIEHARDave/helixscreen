# Pressure advance tuning by eye (#982): design

Scaffolding for review; the controller deletes it at landing. Durable facts live in
`docs/devel/PRESSURE_ADVANCE_TUNING.md`.

## Shape

AUTO RAMP + LIVE OVERRIDE, during a test print HelixScreen generates.

1. **Setup** (Advanced > Pressure Advance Tuning, beta): tool (multi-tool only),
   material preset (nozzle/bed temps via `presets::slot_temps()`), extruder kind
   (direct drive 0 -> 0.1, bowden 0 -> 1.0), editable start/end K.
2. **Print**: `pa::generate_test_gcode()` writes a 40 mm hollow square tower (2 walls,
   0.2 mm layers, 1 mm base + 10 bands of 3 mm, 4-loop brim, prime line), uploads it as
   `helix_pa_tuning.gcode` and starts it. It starts and ends through the printer's own
   start/end macros (temperatures passed under the macro's parameter names), falling back
   to G28 + heat. The file reports layers with `SET_PRINT_STATS_INFO` and carries no PA
   commands.
3. **Ramp**: HelixScreen sends `SET_PRESSURE_ADVANCE EXTRUDER=<tuned> ADVANCE=k` when a
   layer enters a new band (`print_layer_current` subject).
4. **Override**: the full-screen tuner shows the reported K, layer and height, and -/+
   with a step of 0.001/0.005/0.01/0.05. A nudge **stops the ramp for the rest of the
   print**: the user is holding a value to judge it, and the ramp moving it away would
   invalidate what they are looking at.
5. **Record**: every layer logs the K the printer *reported* in
   `extruder*.pressure_advance` (new subscription field), whoever set it.
6. **Pick**: on any end (complete, cancel via End Test, error) the band log is shown
   (height range -> K, consecutive layers with one K merged); pick one or keep current.
7. **Result**: applied live; note tells the user to enter it in the slicer filament
   profile; Save to Spool writes the Spoolman filament extra field `pressure_advance`
   (defining the field first); the result is always published to Moonraker DB
   `helix-screen` / `pressure_advance`.

Tune overlay gets a plain live PA row (readout + -.005/-.001/+.001/+.005) for any
print, through the same `tune::set_pressure_advance()` and the same readback subject,
plus a Tuner button while a tuning print runs.

## Interfaces

- `include/pa_tuning.h` (pure): `Ramp`, `TowerGeometry`, `TestPrintParams`,
  `generate_test_gcode()`, `estimate_print_seconds()`, `layer_count/layer_z/
  band_for_layer/ramp_k_for_layer()`, `BandLog`, `TuningSession` (state machine
  below), `merge_moonraker_record()`, `should_auto_apply_on_activation()`.
- `helix::tune::set_pressure_advance(api, extruder, k)`,
  `pressure_advance_from_subject()`, `pressure_advance_text()`.
- `PrinterTemperatureState`: `ExtruderInfo::pressure_advance/smooth_time`,
  `get_active_extruder_pressure_advance_subject()` (K x 10000, -1 unknown, XML
  `extruder_pressure_advance`), `extruder_pressure_advance(name)`.
- `ISpoolmanAPI::set_spoolman_filament_extra_number(filament_id, key, display_name,
  value, ok, err)`; `SpoolInfo::pressure_advance`; `spoolman_detail::
  parse_extra_number/encode_extra_number`.
- `spoolman_sync::apply_saved_pressure_advance(spool)` on `notify_active_spool_set`.
- `presets::slot_temps(slot)`.

## State machine (`TuningSession`)

```
Idle --start--> Starting --print_started / first layer--> Printing
Printing --print_ended, log non-empty--> Picking --pick/keep--> Done
Starting|Printing --print_ended, nothing logged / abandon--> Idle (original K restored)
```

Mode Ramp | Manual inside Printing; `nudge()` sets Manual.

## Tests

`[pa_tuning]` geometry, gcode contents, BandLog, session transitions, DB record;
`[pa_tune]` tune controller send/readback, mock SET_PRESSURE_ADVANCE, temperature
state parse, Spoolman extra field round trip.
