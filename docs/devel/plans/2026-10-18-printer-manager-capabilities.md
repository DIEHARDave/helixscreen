# Printer Manager Capabilities Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. § Verification Cadence in the root CLAUDE.md overrides any "verify" step: run the tags named here, nothing broader.

**Goal:** The Printer Manager's CAPABILITIES section lists everything HelixScreen detects, grouped, with detail text, override marking, and a tap action on every chip.

**Architecture:** One `capability_chip` XML component replaces the copy-pasted chips. Chip *text* is composed by a pure function (`capability_chip_text`) from a `CapabilityChipFacts` snapshot and published through per-chip string subjects the overlay owns; chip *visibility* binds to the app's existing capability subjects (plus a few new ones). Every chip shares one click callback that looks the chip up by name in one spec table and opens its destination or an info popup.

**Tech Stack:** C++17, LVGL 9.5 + helix-xml, Catch2, spdlog.

**Spec:** `docs/devel/plans/2026-10-18-printer-manager-capabilities-design.md` (read it first; its group/chip tables are the chip list).

## Global Constraints

- Present only: a chip whose capability is absent is hidden. No "not detected" UI.
- Chip text fits 234 px (480x272 details column) including icon; no mid-word ellipsis.
- Declarative UI rules (`.claude/rules/declarative-ui.md`): no `lv_label_set_text`, no imperative visibility, no `lv_obj_add_event_cb`, no C++ create loops. Compound visibility = `<subject_expr>` / inline `cond`.
- Vendor names only come from their owning module (`ams_system_name`/`AmsBackend::get_system_info`, `probe_type_to_display_string`, `pacal::provider_name`).
- `src/ui/ui_printer_manager_overlay.cpp` is in the ESP32 image: every new `.cpp` it needs goes in `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt`; no exceptions, no `std::regex`, fmt via `<spdlog/fmt/fmt.h>`. Destination screens absent on ESP32 fall back to the info popup.
- Every user-facing string is translatable (`lv_tr` in C++, `label_tag` in XML); finish with `make translation-sync` + `make translations` and commit the YAMLs and `ui_xml/translations/*.xml`.
- Separator in chip text is ` · ` (space, U+00B7, space). Dimensions use `×` (U+00D7).
- No em-dashes in any copy. No comment archaeology.

## Review Focus

1. Printer switch while the overlay is closed, then reopened: every chip text must reflect the new printer (texts recompute on `on_activate`, never cached from a previous printer). Test in Task 4.
2. A group whose chips are all hidden must collapse fully, header included, at every breakpoint (a padded wrapper keeps height). Test in Task 4.
3. Capability answers that arrive while the overlay is open (Spoolman, timelapse, webcams, power devices are async): visibility follows live because it binds subjects; text for count chips must not show a stale count. Texts recompute when the count subject changes. Test in Task 4.
4. A user override set to DISABLE hides the chip (the capability subject is 0); ENABLE on a detected capability does NOT say "set manually". Test in Task 1.
5. A toolchanger printer with no other filament system shows "Tool Changer · 4 Tools" and no Multi-Filament chip. Test in Task 2.

---

### Task 1: Capability sources (new subjects, Fans fix, forced-on query)

**Files:**
- Modify: `include/printer_capabilities_state.h`, `src/printer/printer_capabilities_state.cpp` (`Capability` enum + `CAPABILITY_DEFS`, `set_hardware`)
- Modify: `include/capability_overrides.h`, `src/system/capability_overrides.cpp`
- Test: `tests/unit/test_printer_capabilities_state.cpp` (existing file; add cases), `tests/unit/test_capability_overrides.cpp` (existing or create)

**Interfaces:**
- Produces: `Capability::HasShakeTune` (`printer_has_shaketune`), `Capability::HasHeatSoak` (`printer_has_heat_soak`, from `overrides.has_heat_soak()`), `Capability::HasFanFeedback` (`printer_has_fan_feedback`), `Capability::HasLedEffects` (`printer_has_led_effects`), all initial 0, appended before `Count`, defs in enum order.
- Produces: `HasExtraFans` is now set in `set_hardware`: 1 when `hardware.fans()` holds any entry other than `"fan"` (part cooling).
- Produces: `[[nodiscard]] bool CapabilityOverrides::is_forced_on(const std::string& name) const;` true iff the override is `ENABLE` and auto-detection (`get_auto_value`) is false.

- [ ] **Step 1: Failing tests.** `[printer_capabilities]`: a discovery with `has_klippain_shaketune`, `has_fan_feedback`, `led_effects` non-empty, and fans `{"fan","heater_fan hotend_fan"}` sets the four new subjects and `printer_has_extra_fans` to 1; fans `{"fan"}` alone gives `printer_has_extra_fans == 0`; heat soak follows an ENABLE override with no macro. `[capability_overrides]`: `is_forced_on("qgl")` is true for ENABLE + undetected, false for ENABLE + detected, false for AUTO, false for DISABLE.
- [ ] **Step 2:** `make t F='[printer_capabilities],[capability_overrides]'` fails (unknown enumerators / member).
- [ ] **Step 3:** Implement. Fold the new flags into `set_hardware` next to the existing ones.
- [ ] **Step 4:** Same command passes. Break the `HasExtraFans` line once, see red, restore.
- [ ] **Step 5:** Commit `feat(capabilities): subjects for Shake&Tune, heat soak, fan tach, LED effects; set extra fans`.

### Task 2: Chip text (pure)

**Files:**
- Create: `include/capability_chip_text.h`, `src/printer/capability_chip_text.cpp` (+ `app_srcs.txt` line)
- Test: `tests/unit/test_capability_chip_text.cpp`

**Interfaces:**
- Produces:
```cpp
namespace helix {
enum class CapChip : uint8_t {
    Kinematics, BuildVolume, Probe, BedMesh, Qgl, ZTilt, ScrewsTilt, Accelerometer,
    BeltTension, ShakeTune, HeatedBed, ChamberSensor, ChamberHeater, ChamberDryer,
    FilterFan, BedDrying, Fans, FanTach, FilamentSystem, ToolChanger, FilamentSensors,
    WidthSensors, Retraction, PaCal, ToolOffsets, NozzleClean, PurgeLine, HeatSoak,
    ExcludeObject, LayerPause, JobQueue, Mcus, Power, Leds, LedEffects, Speaker, Webcam,
    Spoolman, Timelapse, HelixMacros, HelixPlugin, Count
};
struct FilamentSystemFact { std::string name; int slots = 0; bool is_tool_changer = false; };
struct CapabilityChipFacts {
    std::string kinematics;                 // raw Klipper string, "" if unknown
    BuildVolume build_volume;               // printer_detector.h; zero = unknown
    std::string probe_type;                 // probe_type_to_display_string(), "" if none
    int accel_count = 0, fan_count = 0, tool_count = 0;
    std::vector<FilamentSystemFact> filament_systems;
    int filament_sensor_count = 0, width_sensor_count = 0;
    std::string pa_provider;                // pacal::provider_name(), may be ""
    std::vector<std::string> mcus;          // PrinterDiscovery::mcu_list()
    int power_device_count = 0, led_count = 0, led_effect_count = 0, webcam_count = 0;
    std::set<CapChip> forced_on;            // from CapabilityOverrides::is_forced_on
};
std::string capability_chip_text(CapChip chip, const CapabilityChipFacts& facts);
std::string kinematics_display_name(const std::string& kinematics);
}
```
- Text rules (the spec's detail column): base label + ` · ` + detail when there is a detail, else the base label. Counts show only when > 1 (accel, webcam) or always for counted things (sensors, power, LEDs, LED effects, fans). `forced_on` appends ` · ` + "set manually" (translated) to Qgl, ZTilt, BedMesh, NozzleClean, Speaker, HeatSoak, ChamberSensor, ChamberHeater (chamber override). Kinematics chip text = `kinematics_display_name` alone; BuildVolume = `"%d×%d×%d"` of x span, y span, z_max (rounded, mm unit omitted), "" when any span is 0. FilamentSystem = systems with `!is_tool_changer`, each `name` or `name · slots`, joined `", "`; "" when none. ToolChanger = "Tool Changer · N Tools". Mcus = the single MCU's chip name when one (`mcus[0]`), else "N MCUs".
- `kinematics_display_name`: corexy→CoreXY, limited_corexy→CoreXY, hybrid_corexy→Hybrid CoreXY, corexz→CoreXZ, cartesian→Cartesian, limited_cartesian→Cartesian, delta→Delta, rotary_delta→Rotary Delta, polar→Polar, winch→Winch, anything else returned unchanged, "" stays "".

- [ ] **Step 1: Failing tests `[capability_chip_text]`:** `Probe` with "Beacon" → "Probe · Beacon"; `Probe` with "" → "Probe"; `Qgl` with forced → "QGL · set manually"; `Accelerometer` count 1 → "Accelerometer", 2 → "Accelerometer · 2"; `BuildVolume` {0,350,0,350,340} → "350×350×340", zero z → ""; `FilamentSystem` [{Happy Hare,8,false},{Tool Changer,4,true}] → "Happy Hare · 8"; only the tool changer → ""; two systems → "Happy Hare · 8, ACE · 4"; `ToolChanger` 4 → "Tool Changer · 4 Tools"; `Mcus` ["stm32h723"] → "stm32h723", 2 entries → "2 MCUs"; every kinematics mapping above; every `CapChip` below `Count` returns non-empty for a fully populated facts value (loop).
- [ ] **Step 2:** `make t F='[capability_chip_text]'` fails to link.
- [ ] **Step 3:** Implement. Base labels and units via `lv_tr`.
- [ ] **Step 4:** Passes. Break the toolchanger exclusion once, see red, restore.
- [ ] **Step 5:** Commit `feat(printer-manager): chip text model`.

### Task 3: `capability_chip` component and the grouped layout

**Files:**
- Create: `ui_xml/components/capability_chip.xml` (register it the way `components/activity_chip.xml` is registered)
- Modify: `ui_xml/printer_manager_overlay.xml` (SECTION 3 replaced)

**Interfaces:**
- Produces component `capability_chip`, props: `name` (chip name, `pm_chip_<snake chip>` e.g. `pm_chip_belt_tension`), `icon` (string), `visible` (subject, hidden when 0), `text` (subject, string), `callback` (default `pm_chip_clicked`). Appearance copied from today's chips (card_bg, radius 16, xs/sm pads, `icon size="xs" variant="success"`, `text_small`, `styles.press_wash` pressed, clickable).
- Text subjects are named `pm_chip_text_<snake chip>` (owned by Task 4).
- Six `setting_group`s in this order with `setting_group_header` titles: MOTION & LEVELING, THERMAL & AIR, FILAMENT & TOOLHEAD, PRINTING, ELECTRONICS & ACCESSORIES, ADD-ONS; chips in each group in the spec's table order. The chip-wrap container inside each group has zero padding so a group of hidden chips measures 0 height and `setting_group` collapses it; spacing comes from the group/header.
- Visibility sources per chip: the spec table's subject. Compound ones as `<subject_expr>` in the overlay: Belt Tension = `printer_supports_belt_compare and printer_has_accelerometer`; HelixScreen Macros = `helix_macros_status ge 1`; HelixPrint Plugin = `helix_plugin_installed eq 1`; Filament Sensors / Width Sensors = `<count> gt 0`; Tool Changer = `ams_is_tool_changer`. Overlay-owned int subjects (Task 4) for facts with no app subject: `pm_has_kinematics`, `pm_has_build_volume`, `pm_has_filament_system`, `pm_has_mcus`.
- The catch-all `printer_has_chamber` chip is gone; Chamber Sensor and Chamber Heater replace it. The timelapse ESP32 hide in C++ stays as is.

- [ ] **Step 1:** Write the component and the six groups. XML needs no rebuild.
- [ ] **Step 2:** `make lint-xml` (or the xml schema check `land-gate` uses) passes.
- [ ] **Step 3:** Commit with Task 4 (the layout references subjects Task 4 creates).

### Task 4: Overlay wiring: text subjects, click dispatch, info popups

**Files:**
- Modify: `include/ui_printer_manager_overlay.h`, `src/ui/ui_printer_manager_overlay.cpp`
- Create: `src/ui/printer_manager_chips.cpp` + `include/printer_manager_chips.h` (+ `app_srcs.txt`)
- Test: `tests/unit/test_printer_manager_overlay.cpp`

**Interfaces:**
- Consumes: Task 1 subjects and `is_forced_on`; Task 2 `CapChip`, `CapabilityChipFacts`, `capability_chip_text`; Task 3 names.
- Produces:
```cpp
namespace helix {
struct CapChipSpec {
    CapChip chip;
    const char* name;          // "pm_chip_probe"
    const char* info_title;    // untranslated key, lv_tr at use
    const char* info_message;  // one or two sentences: what it is, what HelixScreen does with it, where
    void (*open)();            // nullptr = info popup
};
std::span<const CapChipSpec> cap_chip_specs();            // one entry per CapChip, enum order
const CapChipSpec* find_cap_chip_spec(std::string_view name);
CapabilityChipFacts gather_capability_chip_facts();      // reads PrinterState, discovery, AmsState, managers; main thread
}
```
- Overlay owns `std::array<lv_subject_t, CapChip::Count>` string subjects `pm_chip_text_<snake>` plus the four `pm_has_*` ints, registered in `init_subjects`, recomputed in `on_activate` and whenever a count subject the texts read changes (`power_device_count`, `webcam_count`, `filament_sensor_count`, `width_sensor_count`, `accel_count`, `ams_slot_count`, `tool_count`) via `observe<int>` with proper `SubjectLifetime`.
- One registered callback `pm_chip_clicked`: resolves the clicked chip's name, `find_cap_chip_spec`, calls `open` or shows `helix::ui::modal_alert(lv_tr(title), lv_tr(message), ModalSeverity::Info, ...)`. The existing per-chip callbacks fold into `open` functions in the spec table (keep their ESP32 guards; on ESP32 a screen-less chip's `open` is nullptr so the popup shows).
- Destinations are the spec table's "Tap" column; openers named in the design's recon (e.g. `get_global_belt_tension_panel()`, `helix::open_led_control_overlay`, `open_job_queue_modal`, `helix::settings::get_sensor_settings_overlay()`, `get_global_pa_cal_panel()`, `get_global_tool_offset_cal_panel()`, `get_global_temp_graph_overlay().open(Mode::Bed/Chamber, ...)`, ChamberDryerModal, the bed-drying modal, AdvancedPanel for the two HelixScreen add-ons, system settings for MCUs).
- Info copy tone: present tense, plain, no em-dashes, what HelixScreen does with it. Shake&Tune and Heat Soak say they were detected and that HelixScreen does not use them; Webcam's says where the camera view lives (the home camera widget).
- Klipper SOFTWARE row: when `is_kalico`, its label reads "Kalico" (label subject, not C++ text set).

- [ ] **Step 1: Failing tests `[printer_manager]`** (LVGLUITestFixture creating the overlay):
  - every `cap_chip_specs()` entry's `name` resolves with `lv_obj_find_by_name` in the created overlay, and the overlay has exactly `CapChip::Count` `capability_chip` instances (no chip without a spec);
  - with all capability subjects 0 and facts empty, every group is collapsed (height 0, header hidden); setting `printer_has_probe` to 1 makes the MOTION & LEVELING group and the probe chip visible;
  - after a facts change and a second `on_activate`, `pm_chip_text_probe` reads the new text (Review Focus 1);
  - changing `power_device_count` from 2 to 3 while open updates `pm_chip_text_power` (Review Focus 3);
  - clicking a chip whose spec has `open == nullptr` puts one modal on `ModalStack`;
  - `pm_chip_text_filament_system` is "" and `pm_has_filament_system` is 0 when the only system is a tool changer.
- [ ] **Step 2:** `make t F='[printer_manager]'` fails.
- [ ] **Step 3:** Implement; delete the old per-chip callbacks and their registrations.
- [ ] **Step 4:** Passes. Break the recompute-on-count line once, see red, restore.
- [ ] **Step 5:** `make translation-sync && make translations`; commit code, XML, YAMLs, `ui_xml/translations/*.xml`: `feat(printer-manager): grouped capability chips with details and info popups`.

### Task 5: Screens, docs, cleanup

**Files:**
- Modify: `docs/devel/PRINTER_MANAGER.md` (capabilities section: component, text model, spec table, how to add a chip)
- Delete: both `docs/devel/plans/2026-10-18-printer-manager-capabilities*.md`
- Goldens: `tests/ui/test_screens.py -k printer_manager --accept-goldens` if a golden covers it

- [ ] **Step 1:** Headless screenshots (`HELIX_HEADLESS=1 SDL_VIDEODRIVER=dummy`, pinned socket) of the overlay at 800x480, 480x272 and a portrait size, scrolled through the capabilities; a mock printer with many capabilities (Voron mock + `HELIX_MOCK_*` for AMS/toolchanger if needed). Check `ctl geom` that no chip is wider than its column.
- [ ] **Step 2:** Update the doc, delete the plan files, commit `docs(printer-manager): capability chips`.

## Finish (orchestrator, once)

One review of the branch, `make mutate-diff MUTATE_ARGS="--tests '[printer_capabilities],[capability_overrides],[capability_chip_text],[printer_manager]'"`, one screenshot per changed screen shown to Preston, one `make land-gate`, then land.
