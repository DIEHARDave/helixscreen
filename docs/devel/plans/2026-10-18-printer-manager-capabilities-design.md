# Printer Manager: Capabilities Expansion - Design

## Intent

The CAPABILITIES group on the Printer Manager overlay (`ui_xml/printer_manager_overlay.xml`,
`src/ui/ui_printer_manager_overlay.cpp`) shows 18 yes/no chips today. It should become the
one place that lists everything HelixScreen knows this printer can do. It has two jobs:

1. **Detection check.** Preston, a support helper or the user opens it and sees at a glance
   whether HelixScreen detected a piece of hardware, and *as what*. "Probe · Cartographer"
   settles a "my Cartographer isn't working" report; a bare "Probe" does not. So the list is
   complete, and where one value decides whether detection was right, the chip carries it.
2. **Discovery.** A user learns what their printer has and what HelixScreen does with it.
   A chip with a screen behind it opens that screen; a chip without one opens a short
   explainer.

## Decisions (Preston, 2026-10-18)

- **Present only.** A capability that was not detected does not appear. There is no
  "not detected" row.
- **Sub-groups.** The chips are split under small headers instead of one long wrap.
- **Every chip is tappable.** It opens the screen that uses the capability, or a short
  info popup (what it is, what HelixScreen does with it, where it shows up).
- **Detail goes in the chip text** ("Probe · Beacon", "CoreXY", "4 Tools"), not in a
  separate label/value section.
- **Scope = what HelixScreen already detects**, plus a detail value where yes/no can't
  settle detection. New detection is out of scope unless HelixScreen uses the result.
- **Detected but unused still gets a chip** (Shake&Tune, Heat Soak). Its popup says what it
  is and that HelixScreen detected it, and does not claim a HelixScreen feature.
- **Overridden capabilities are marked.** Where the user forced a capability on in
  settings (QGL, Z-Tilt, Bed Mesh, Nozzle Clean, Speaker, Heat Soak...), the chip says so
  ("QGL · set manually"), so a support check can tell "Klipper reported it" from "the user
  turned it on". The capability subjects carry detection plus overrides, so this needs a
  per-capability "overridden" source next to them (from `CapabilityOverrides`); the chip
  component takes it as an input.

## Calls made without Preston (check these)

- **Power-Loss Recovery: dropped.** `plr_select_backend()` answers "is a recovery
  available right now", not "does this printer support it", so the chip would appear only
  after a power cut. A real capability signal is new detection.
- **Adaptive Mesh: dropped.** `PrintStartProfile::adaptive_meshing` is an internal ETA flag
  set per profile, not something detected from the printer.
- **Webcam: info popup, not a camera view.** The fullscreen view belongs to the home
  panel's `CameraWidget` instance; pulling it out is real stream-lifecycle work.
- **Multi-Filament vs Tool Changer.** Tool Changer is its own chip ("4 Tools").
  Multi-Filament names the filament backend(s) other than the toolchanger and hides when
  the toolchanger is the only one. With several such backends, one chip per backend
  (builder's call how: a few fixed chips or a C++-built list).
- **Chamber.** Chamber Sensor binds `printer_has_chamber_sensor` and Chamber Heater
  `printer_has_chamber_heater`; the catch-all `printer_has_chamber` chip goes away.
- **Extra fans.** "Extra" = controllable fans beyond part cooling, as the
  `HasExtraFans` header comment says.

## Constraints

- The details column is **416 px wide at 800x480** (about 3 chips per row) and **234 px at
  480x272** (about 2 per row). Measured with `ctl geom pm_details`. Portrait stacks the
  column under the identity card. Chip text must fit 234 px with the icon; long detail
  strings get shortened, never ellipsized mid-word (`feedback_grow_button_before_ellipsis`).
- XML holds appearance, C++ holds data (`.claude/rules/declarative-ui.md`). Visibility binds
  to the existing capability subjects. No XML in the repo uses formatted `bind_text`, so
  detail text comes from a string subject or is set in C++ by an observer on the capability.
- ESP32 builds: several destination screens don't exist there (bed mesh, input shaper,
  screws tilt, PA, PID). Their handlers already use the
  `#if defined(HELIX_PLATFORM_ESP32) show_feature_unavailable_toast()` pattern; new handlers
  keep it, or fall back to the info popup. Timelapse stays hidden on ESP32 as it is now.
- All user-facing strings are translatable (`lv_tr` / `label_tag`, then
  `make translation-sync` + `make translations`).
- Vendor names (Snapmaker, QIDI, AFC, Happy Hare) appear only in display text that comes
  from the module that owns that capability (`ams_system_name`,
  `probe_type_to_display_string`, `pacal::provider_name`). No vendor branching in the
  overlay (`.claude/rules/vendor-abstraction.md`).

## The chip

Today each chip is a copy-pasted ~10-line `lv_obj`. Make one `capability_chip` XML
component first: icon, label text, visibility subject, click callback. Every chip, old
and new, uses it. Detail text works through the same component (a named label C++ can set,
or a bound string subject; builder's call). Tappable chips keep the `press_wash` pressed
style. Since every chip is now tappable, there's no separate look for "info only".

## Groups and chips

Group headers use `setting_group_header` (or a lighter sub-header, builder's call). A
group whose chips are all hidden collapses entirely, header included. Note
`setting_group` only auto-collapses when no visible child has height, so a padded wrapper
around hidden chips keeps it open; handle that.

"New" = no chip today. "Detail" = the value shown after " · ". Destinations name the
opener the code already uses.

### Motion & Leveling
| Chip | Subject / source | Detail | Tap |
|---|---|---|---|
| Kinematics (new) | `PrinterDiscovery::kinematics` | "CoreXY", "Cartesian", "Delta"... as the chip text itself | info |
| Build volume (new) | `hardware().build_volume()` | "350×350×340" as chip text | info |
| Probe | `printer_has_probe` | type via `ProbeSensorManager` + `probe_type_to_display_string` | ProbeOverlay |
| Bed Mesh | `printer_has_bed_mesh` | - | BedMeshPanel (existing) |
| QGL | `printer_has_qgl` | - | info |
| Z-Tilt | `printer_has_z_tilt` | - | info |
| Screws Tilt | `printer_has_screws_tilt` | - | ScrewsTiltPanel (existing) |
| Accelerometer ("ADXL" today) | `printer_has_accelerometer` | count when > 1 | InputShaperPanel (existing) |
| Belt Tension (new) | `printer_supports_belt_compare` AND accelerometer | - | BeltTensionPanel |
| Shake&Tune (new) | `has_klippain_shaketune` (needs a subject) | - | info (detected, not used by HelixScreen) |

### Thermal & Air
| Chip | Subject / source | Detail | Tap |
|---|---|---|---|
| Heated Bed | `printer_has_heater_bed` | - | TempGraphOverlay Bed |
| Chamber Sensor ("Chamber" today) | `printer_has_chamber_sensor` | - | TempGraphOverlay Chamber |
| Chamber Heater (new) | `printer_has_chamber_heater` | - | TempGraphOverlay Chamber |
| Chamber Dryer (new) | `printer_has_chamber_dryer` | - | ChamberDryerModal |
| Filter Fan (new) | `printer_has_chamber_filter_fan` | - | TempGraphOverlay Chamber |
| Bed Drying (new) | `printer_can_bed_dry` | - | bed drying modal |
| Fans | `printer_has_extra_fans` | fan count | fan control overlay (existing) |
| Fan Tach (new) | `has_fan_feedback` (needs a subject; Creality `fan_feedback` only) | - | fan control overlay |

**The Fans chip never appears today:** nothing calls
`set_capability(Capability::HasExtraFans, ...)`, so `printer_has_extra_fans` stays 0. This
is in scope; it's exactly what the detection check exists to catch.

### Filament & Toolhead
| Chip | Subject / source | Detail | Tap |
|---|---|---|---|
| Multi-Filament | `ams_type` | `ams_system_name` + slot count ("Happy Hare · 8") | AMS detail (existing) |
| Tool Changer (new) | `ams_is_tool_changer` / `tool_count` | "4 Tools" | AMS detail |
| Filament Sensors (new) | `filament_sensor_count` > 0 | count | sensor settings overlay |
| Width Sensors (new) | `width_sensor_count` > 0 | count | sensor settings overlay |
| Retraction | `printer_has_firmware_retraction` | - | retraction settings (existing) |
| PA Calibration (new) | `printer_has_pa_cal` | `pacal::provider_name` if it reads well | PA cal panel |
| Tool Offsets (new) | `printer_has_tool_offset_cal` | - | ToolOffsetCalibrationPanel |
| Nozzle Clean | `printer_has_nozzle_clean` | - | info |
| Purge Line (new) | `printer_has_purge_line` | - | info |
| Heat Soak (new) | `has_heat_soak_macro` (needs a subject) | - | info (detected, not used by HelixScreen) |

### Printing
| Chip | Subject / source | Detail | Tap |
|---|---|---|---|
| Exclude Object (new) | `printer_has_exclude_object` | - | info |
| Layer Pause (new) | `printer_has_layer_pause` | - | info |
| Job Queue (new) | `printer_has_job_queue` | - | job queue modal |

### Electronics & Accessories
| Chip | Subject / source | Detail | Tap |
|---|---|---|---|
| MCUs (new) | `PrinterDiscovery::mcu_list` | "2 MCUs", or the primary chip name when there's only one | system settings (MCU load) |
| Power | `power_device_count` | count | Power panel (existing) |
| LEDs | `printer_has_led` | count | LED settings (existing) |
| LED Effects (new) | `has_led_effects` (needs a subject) | count | LED control overlay |
| Speaker | `printer_has_speaker` | - | sound settings (existing) |
| Webcam | `printer_has_webcam` | count when > 1 | info |

### Add-ons
| Chip | Subject / source | Detail | Tap |
|---|---|---|---|
| Spoolman | `printer_has_spoolman` | - | Spoolman panel (existing) |
| Timelapse | `printer_has_timelapse` | - | timelapse settings (existing) |
| HelixScreen Macros (new) | `helix_macros_status` >= 1 | - | Advanced panel |
| HelixPrint Plugin (new) | `helix_plugin_installed` == 1 | - | Advanced panel |

**Klipper fork:** when `is_kalico`, the SOFTWARE group's Klipper row says "Kalico" instead
of being a chip. It's software identity, and that row is where someone checks it.

Group names, chip order and exact chip labels are the builder's call within this shape.

## The info popup

`helix::ui::modal_alert(title, message, ModalSeverity::Info, ...)`, as InputShaperPanel's
help popup uses it. One or two sentences per capability: what it is, what HelixScreen does
with it, and where in the app that happens ("Exclude Object lets you skip a failed part
mid-print from the print status screen."). The strings live in one table keyed by chip, not
scattered through handlers. The "No X detected" strings in
`src/ui/panel_widget_registry.cpp` are prior art for tone. Every chip gets an entry, so a
chip that has a screen but runs on ESP32 without it falls back to its popup.

## Non-goals

- Showing undetected capabilities.
- Power-loss recovery and adaptive mesh chips (see calls above).
- New hardware detection: CAN toolhead, TMC driver model, sensorless homing,
  skew_correction, z_thermal_adjust, gcode_arcs, input shaper values. Revisit any of these
  when HelixScreen starts using it.
- Changing what any destination screen does.
- Redesigning the identity column or the SOFTWARE group (except the Kalico label).

## Anti-goals

- A chip whose visibility disagrees with what the rest of the app believes. Chips bind to
  the same subjects the app's own gating uses, never a re-derived check.
- Text that overflows or ellipsizes at 480x272.
- An info popup that describes a feature HelixScreen doesn't actually have.

## Left to the builder

Component API, how detail text is delivered, the mechanism for new subjects (extend
`CAPABILITY_DEFS` in `src/printer/printer_capabilities_state.cpp` where it fits), icon
choice per chip (add to both icon lists if a new one is needed), group and chip labels,
popup copy, tests (each new subject's set/clear path; the group-collapse behaviour; one
handler per destination kind), and screenshot verification at 800x480, 480x272 and
portrait.
