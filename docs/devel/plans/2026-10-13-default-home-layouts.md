# Default home layouts Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fresh, post-wizard and Reset home layouts pick the widgets each printer can drive (candidate slots resolved from capability gates), stay live until the user changes them, and cover every tier, with xxlarge on a 72px track edge.

**Architecture:** A pure resolver (`resolve_default_layout`) turns `default_layout.json` (fixed anchors plus slots) and a capability snapshot into placements. `PanelWidgetConfig::build_default_grid` becomes snapshot + resolve. A `default` block (`signature`, `hash`) beside the saved pages marks an unedited default; `PanelWidgetManager::populate_widgets` re-resolves it at the top of the populate when the signature changes while CONNECTED and READY.

**Tech Stack:** C++17, LVGL 9.5 + helix-xml, libhv's `hv/json.hpp`, Catch2, `helix-screen ctl`, GNU make.

**Spec:** `docs/devel/plans/2026-10-13-default-home-layouts-design.md` (read it first; the tables live there).

## Global Constraints

- Saved layouts the user has changed are never re-defaulted; fresh, post-wizard and Reset only.
- No default places `toolhead`, `host_health` or `exclude_map`.
- Tier tables are in tracks in JSON (1 cell = 2x2 tracks); odd spans only on widgets with `supports_half_col` / `supports_half_row`.
- Every candidate list ends in an ungated widget; a resolved layout has no dead tiles and leaves no default-enabled widget enabled and unplaced.
- The resolver `.cpp` is compiled into the ESP32 firmware: no exceptions, no RTTI, no `std::regex`; JSON via `json_util::safe_*`; `#include "hv/json.hpp"`; spdlog only.
- Vendor names never appear in the resolver, the tables' condition fields or the manager (`.claude/rules/vendor-abstraction.md`); conditions name capability subjects.
- Threading: re-resolve and gate observers run on the LVGL thread only; gate observers use `observe<>` with the owner's `SubjectLifetime` (`.claude/rules/threading.md`).
- Comments describe the code as it is (no archaeology); commit subjects `feat(home):` / `fix(home):` / `test(home):`; `git commit -- <paths>`, never `git add -A`.
- Inner loop is `make t F='[tag]'`; `make mutate-diff` and `make full-test-run` once at the end of the branch, not per task.
- Every new header gets `scripts/syntax_check.py <header>` before its first use.

## Review Focus

1. **A printer switch while the default is unedited**: printer B must resolve from B's gates after B is READY, never inherit A's snapshot or write A's arrangement into B's node. Test in Task 5.
2. **A filament system that goes away and comes back while READY** (AFC unit unplugged, HH restart): the layout follows the hardware, and N gate flips write settings at most once per distinct signature. Test in Task 5.
3. **Reset after an edit**: the layout becomes live again and follows a later gate change. Test in Task 5.
4. **A user who adds a second page or a plugin widget** to an otherwise untouched default: the hash changes, the layout freezes, and the plugin's `retained` entries survive. Test in Task 5.
5. **Crash-loop safe mode** with an unedited default and a gate change: shows the resolved default, writes nothing. Test in Task 5.

---

## File structure

| File | Responsibility |
|---|---|
| `include/default_layout_resolver.h`, `src/system/default_layout_resolver.cpp` (new) | Pure resolution: table + tier + grid + variant + snapshot to placements, disabled ids, signature; `arrangement_hash` |
| `src/system/panel_widget_config.cpp`, `include/panel_widget_config.h` | Snapshot gathering, resolver call, `default` block stamping, overlay seeds, `is_unedited_default()`, `reresolve_default_if_changed()` |
| `src/ui/panel_widget_manager.cpp` | Re-resolve at the top of `populate_widgets`; observers for `requires` subjects; `gate_subject_lifetime` rows |
| `assets/config/default_layout.json` | The tables from the spec |
| `assets/config/panel_widgets/{cc1,ad5x,ad5x_zmod}/home.json` | Deleted or rewritten as overlays (Task 7) |
| `assets/config/presets/snapmaker_u1.json` | Stale `printer.panel_widgets.home` removed |
| `include/grid_layout.h` | XXLarge track edge 72 |
| `scripts/esp32_stage_assets.py`, firmware `app_srcs.txt` | Stage the tables; compile the resolver |
| `scripts/screenshot-recipes.sh`, `docs/devel/LAYOUT_SYSTEM.md` | Recipes by `ctl geom`; docs |
| `src/ui/panel_widgets/speed_flow_widget.cpp` (+ its XML) | Wide-rung fix |

---

### Task 1: speed_flow Wide rung draws without overprinting

**Files:** Modify `src/ui/panel_widgets/speed_flow_widget.cpp` and `ui_xml/components/panel_widget_speed_flow.xml` (confirm names with `ls`); Test `tests/unit/test_speed_flow_layout.cpp`.

**Interfaces:** none new.

Repro: `HELIX_MOCK_PRINTER=k2 ... -s 800x480`, `ctl home place speed_flow 4 2 2 1`: "Speed"/"100%" and "Flow"/"100%" draw on top of each other at the top-left. The same tile at 1920x1080 on a 72px edge (134x142 cells) lays out correctly, so the fault depends on the box size (Wide rung at ~233x113px and 1024x600's 2x1).

- [ ] **Step 1:** Write a failing case in `test_speed_flow_layout.cpp`: at the Wide rung with a 233x113 box (and 1024x600's 2x1 box from `ctl geom`), `speed_flow_label` and `speed_flow_value` of each part do not intersect, and the two parts' boxes do not intersect.
- [ ] **Step 2:** `make t F='[speed_flow]'`, expect FAIL on the intersection assertion.
- [ ] **Step 3:** Fix the Wide rung's flex/track placement so each part is label-left, value-right in its own row (follow the Strip/Glance centring fix in the same widget, `39f334816`'s approach is the precedent in-file).
- [ ] **Step 4:** `make t F='[speed_flow]'` PASS; screenshot the K2 mock tile at 2x1 and Read it.
- [ ] **Step 5:** Commit `fix(home): speed_flow lays out label and value side by side at Wide`.

### Task 2: XXLarge track edge 72px

**Files:** Modify `include/grid_layout.h` (`GRID_CELL` last entry 96 to 72), `tests/unit/test_grid_square_cells.cpp`, `tests/unit/test_default_layout.cpp` (`kLandscapeBudget` xxlarge), `docs/devel/LAYOUT_SYSTEM.md` (grid table row).

**Interfaces:** Produces the 12x7-cell (24x14-track) xxlarge grid at 1920x1080 that Task 6 authors against.

- [ ] **Step 1:** Add a pinned row to `test_grid_square_cells.cpp`: XXLarge, the 1920x1080 content box measured by `ctl geom` on the spike build, expects 24x14 tracks; cells within the existing squareness bound. Expect FAIL (today 18x12).
- [ ] **Step 2:** Change the constant; `make t F='[grid]'` PASS (adjust the tag to what the file uses).
- [ ] **Step 3:** Add the 1920x1080 row to the `LAYOUT_SYSTEM.md` measured table; set `kLandscapeBudget` xxlarge to 24x14.
- [ ] **Step 4:** Commit `feat(home): xxlarge grid uses a 72px track edge, 12x7 cells at 1920x1080`.

### Task 3: the pure resolver

**Files:** Create `include/default_layout_resolver.h`, `src/system/default_layout_resolver.cpp`; Test `tests/unit/test_default_layout_resolver.cpp` (tag `[default_layout_resolver]`).

**Interfaces:**
- Consumes: `get_all_widget_defs()` / `PanelWidgetDef` (`hardware_gate_subject`, `default_enabled`, half-cell flags) from `include/panel_widget_registry.h`; `choose_placement_key()` semantics from `panel_widget_config.cpp` (move it into the resolver unit and call it from there).
- Produces:

```cpp
namespace helix::layout {
struct CapabilitySnapshot {
    std::map<std::string, int> subjects; // gate and `requires` subject name -> value; absent reads 0
    int get(const std::string& name) const;
};
struct ResolvedPlacement {
    std::string id;
    int col, row, colspan, rowspan; // tracks
    nlohmann::json config;          // the anchor's `config`, or empty object
};
struct ResolvedDefault {
    std::vector<ResolvedPlacement> placed;
    std::vector<std::string> disabled; // losing candidates + tier `disabled` ids
    std::string signature;             // tier|cols|rows|variant chain|preset|name=value,... for every consulted subject, sorted
    bool ok = false;                   // false: table unusable (missing, no anchors for this tier); caller falls back
};
ResolvedDefault resolve_default_layout(const nlohmann::json& table,
                                       const nlohmann::json* overlay, // preset seed or nullptr
                                       std::string_view tier, int cols, int rows,
                                       const std::vector<std::string>& variant_chain,
                                       std::string_view preset,
                                       const CapabilitySnapshot& caps);
std::vector<std::string> subjects_named_by(const nlohmann::json& table); // registry gates of every id it names + every `requires` subject
std::string arrangement_hash(const nlohmann::json& pages_payload); // id, enabled, col, row, colspan, rowspan per page, page count; ignores `config` and `retained`
}
```

Rules to implement (spec "Slots"): fixed anchors first, slots in file order; a candidate qualifies when its gate is open, every `requires` subject is non-zero, and it is not placed; an alternative wins when its lead qualifies, the last has no lead requirement; `at` is region-relative; an alternative whose cells do not fit the region is skipped; overlay entries replace base entries by `id`/`slot` or remove them with `"remove": true`; `candidate_lists` referenced by name.

- [ ] **Step 1:** `scripts/syntax_check.py include/default_layout_resolver.h`.
- [ ] **Step 2:** Write failing table-driven cases against small inline tables (not the shipped file):
  - fixed anchor placed at its tier key; `tier@COLSxROWS` key preferred; fallback chain micro→tiny→small and xxlarge→xlarge→large;
  - slot: AMS alternative wins when `ams_slot_count=4`; ENV+FIL when 0; a closed gate skips to the next candidate; `requires printer_has_spoolman=0` skips `active_spool`;
  - a widget already placed by a fixed anchor or an earlier slot is skipped by a later cell;
  - every losing candidate appears in `disabled`; the tier `disabled` list is honoured;
  - an alternative that does not fit the region is skipped; last alternative fills;
  - overlay replace and remove;
  - signature changes when any consulted subject changes and does not change for an unconsulted one;
  - `arrangement_hash` stable across `config` and `retained` edits, changes on a move, an enable flip, or a page added;
  - unknown widget ids in a table are ignored; malformed JSON returns `ok=false`.
- [ ] **Step 3:** `make t F='[default_layout_resolver]'`, expect FAIL (no symbol).
- [ ] **Step 4:** Implement. Firmware rules apply (no throw, `json_util::safe_*`).
- [ ] **Step 5:** `make t F='[default_layout_resolver]'` PASS.
- [ ] **Step 6:** Commit `feat(home): pure default-layout resolver with capability slots`.

### Task 4: build_default_grid resolves through it, losers disabled, default block stamped

**Files:** Modify `src/system/panel_widget_config.cpp`, `include/panel_widget_config.h`, `src/ui/ui_wizard.cpp` (delete `set_enabled_by_id("ams", true)`); Test `tests/unit/test_default_layout.cpp`, `tests/unit/test_panel_widget_config.cpp`.

**Interfaces:**
- Consumes: Task 3.
- Produces on `PanelWidgetConfig`:

```cpp
static helix::layout::CapabilitySnapshot snapshot_capabilities(const nlohmann::json& table); // reads each subjects_named_by() subject via lv_xml_get_subject
bool is_unedited_default() const;           // stored `default.hash` == arrangement_hash(current pages payload)
std::string default_signature() const;      // "" when no default block
```

Payload: `serialize_pages()` writes `"default": {"signature": "...", "hash": "..."}` when the arrangement came from the resolver; `restore_pages()` reads it; parked per-grid copies carry their own. `anchors: "pending"` is read as a default block with an empty signature (keep reading the old key; write only the new one).

- [ ] **Step 1:** Failing cases: the C++ ams/filament swap is gone and the K2 snapshot (`ams_slot_count=4`) yields `ams` at the medium SPOOL_ENV region while `filament` is in disabled; no registry widget with `default_enabled=true` is enabled without a position after `build_default_grid`; `mutually_exclusive()` helper deleted with its uses; Reset stamps a default block whose hash matches.
- [ ] **Step 2:** `make t F='[default_layout],[panel_widget_config]'` FAIL.
- [ ] **Step 3:** Implement: snapshot + resolve; map `ResolvedPlacement` to `PanelWidgetEntry`; disabled ids `disable_and_unplace()`; registry widgets the table never names keep today's append-at-`default_enabled` behaviour; keep the bed_temperature reorder and empty-layout fallback.
- [ ] **Step 4:** PASS; run `make t F='[wizard]'` for the wizard change.
- [ ] **Step 5:** Commit `feat(home): default layouts resolve capability slots and disable the candidates they pass over`.

### Task 5: an unedited default stays live

**Files:** Modify `src/ui/panel_widget_manager.cpp` (`populate_widgets`, `setup_gate_observers`, `gate_subject_lifetime`), `src/system/panel_widget_config.cpp`; Test `tests/unit/test_panel_widget_live_default.cpp` (new, tag `[live_default]`).

**Interfaces:**
- Consumes: Task 4's `is_unedited_default()`, `default_signature()`, `snapshot_capabilities()`.
- Produces: `bool PanelWidgetConfig::reresolve_default_if_changed(int cols, int rows);` returns true when it rewrote the arrangement (and saved once).

Placement in `populate_widgets`: first thing after the panel's config is loaded, before enabled ids are collected and before the "Widget list unchanged" early return, using the last stamped grid; only when `printer_connection_state == CONNECTED` and `klippy_state == READY`; only when `is_unedited_default()`; only when the new signature differs. A grid change on an unedited default re-resolves for the new grid instead of parking. `setup_gate_observers` also observes every `requires` subject from `subjects_named_by()`; `gate_subject_lifetime` gets `printer_has_spoolman` (PrinterState lifetime) and any other `requires` subject the tables use.

- [ ] **Step 1:** Failing cases, all through `PanelWidgetManager::populate_widgets` with a measured grid (follow the fixtures in `test_panel_widget_manager*.cpp`):
  - K2 race: populate READY with `ams_slot_count=0`, set it to 4, run the queued rebuild: `ams` placed 2x1, `filament` disabled;
  - after `ctl`-equivalent `place_entry` move, the same flip changes nothing;
  - a `set_widget_config("led", {...})` keeps the layout live and the config survives a re-resolve;
  - a stale `default` block whose hash does not match (simulated older-build edit) is not re-resolved;
  - disconnected or not READY: no re-resolve;
  - ten flips between two values save exactly twice per distinct signature reached (count `Config::save` calls via the fixture's counter or a test hook);
  - Reset after an edit re-arms; a later flip re-resolves;
  - printer switch: printer B resolves from B's snapshot after B is READY; A's node unchanged;
  - adding a page or a plugin widget freezes; `retained` entries survive;
  - crash-loop safe mode: flip re-resolves the displayed layout and writes nothing;
  - rotation on an unedited default re-resolves for the new grid; on an edited one, parks as today.
- [ ] **Step 2:** `make t F='[live_default]'` FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** `make t F='[live_default],[panel_widget_manager],[panel_widget_config]'` PASS.
- [ ] **Step 5:** Mock check: `HELIX_MOCK_PRINTER=k2` fresh config dir, 800x480: `ctl home` shows `ams` 2x1 at (4,0) without any manual step; log shows one re-resolve.
- [ ] **Step 6:** Commit `feat(home): an unedited default layout re-resolves when a capability gate changes`.

### Task 6: the tables

**Files:** Modify `assets/config/default_layout.json`; Test `tests/unit/test_default_layout.cpp`.

**Interfaces:** Consumes Task 3's schema. Tables are copied from the spec's "Tables" section (landscape cells x2 to tracks; portrait and ultrawide already in tracks).

- [ ] **Step 1:** Failing structural cases over the shipped file: every tier and variant, every slot alternative, inside the tier budget and non-overlapping; every candidate list ends in an ungated widget; the last alternative of every slot fits every region its key can inherit to; odd spans only on half-cell widgets; no table names `toolhead`, `host_health` or `exclude_map`.
- [ ] **Step 2:** Failing persona cases (resolver over the shipped file, one snapshot per row): k2 medium (AMS 2x1); ad5m medium no AMS (chamber + filament, LED at most once); ad5m with Spoolman (active_spool never ahead of filament or LED); cc1 micro; qidi_q2 micro (AMS); u1 tiny (AMS tool tile 2x1); voron_24 large (print_stats at (12,2) tracks, bed_mesh_tile) and without a mesh (job_queue); voron_24 xxlarge 24x14 (the spec's table, FILLER = clock); k1 portrait medium. Each asserts ids and track rectangles.
- [ ] **Step 3:** `make t F='[default_layout]'` FAIL, author the JSON, PASS.
- [ ] **Step 4:** Commit `feat(home): default layout tables for every tier with capability slots`.

### Task 7: seeds and the U1 preset

**Files:** `assets/config/panel_widgets/{cc1,ad5x,ad5x_zmod}/home.json`, `assets/config/presets/snapmaker_u1.json`, `src/system/panel_widget_config.cpp#try_populate_from_preset_seed`; Test `tests/unit/test_panel_widget_config.cpp`, `tests/unit/test_config_preset*.cpp`.

- [ ] **Step 1:** Confirm the U1 claim on a mock: fresh config dir, `HELIX_MOCK_PRINTER=snapmaker_u1 -s 480x320`, log shows the preset layout dropped by `has_uninterpretable_coordinates`. Record the log line in the commit body.
- [ ] **Step 2:** For each of cc1, ad5x, ad5x_zmod: render the seed and the base table on the matching mock and compare. Delete the seed where the base covers it; otherwise rewrite it as an overlay (`anchors` replace/remove only).
- [ ] **Step 3:** `try_populate_from_preset_seed` passes the overlay to the resolver instead of adopting a page list. Failing case first: an overlay that removes `tips` and replaces SPOOL_ENV resolves as expected; a missing seed falls back to the base.
- [ ] **Step 4:** Delete `printer.panel_widgets.home` from `presets/snapmaker_u1.json`; case in `test_config_preset*.cpp` that the applied preset carries no panel_widgets node.
- [ ] **Step 5:** `make t F='[panel_widget_config],[config_preset]'` PASS. Commit `feat(home): preset seeds are default-layout overlays; the U1 preset drops its stale layout`.

### Task 8: K-Touch staging

**Files:** `scripts/esp32_stage_assets.py#stage_config`, the firmware `app_srcs.txt` (path per `docs/devel/ESP32_PORT.md`); Test the script's pytest under `scripts/tests/` if it has one, else add one.

- [ ] **Step 1:** Confirm today's behaviour on the ESP32 sim or device memory notes: no `default_layout.json` staged.
- [ ] **Step 2:** Failing pytest: staging writes a minified `config/default_layout.json` (and `panel_widgets/` when present) and the result parses.
- [ ] **Step 3:** Implement; add `src/system/default_layout_resolver.cpp` to `app_srcs.txt`; run `scripts/check_esp32_app_srcs.py`.
- [ ] **Step 4:** Commit `fix(esp32): stage the default layout tables and compile the resolver`.

### Task 9: recipes, goldens, docs

**Files:** `scripts/screenshot-recipes.sh` (`callouts-4x2`, `widget-catalog-category`), `tests/ui/test_screens.py` goldens, `docs/devel/LAYOUT_SYSTEM.md`.

- [ ] **Step 1:** Replace hardcoded coordinates in the two recipes with `ctl geom <name>` lookups; run both recipes headless and Read the images.
- [ ] **Step 2:** Re-accept home goldens: `tests/ui/test_screens.py -k home --accept-goldens`, Read the new images.
- [ ] **Step 3:** `LAYOUT_SYSTEM.md`: slots, candidate lists, `requires`, the `default` block and when it re-resolves, overlays. Delete both plan files in this commit (`docs/CLAUDE.md` convention).
- [ ] **Step 4:** Commit `docs(home): default layout slots and live defaults`.

### Task 10: render pass and gates

- [ ] **Step 1:** Render every shipped persona at its own resolution idle and printing (`HELIX_MOCK_AUTO_PRINT=1 --sim-speed 6`): k2, ad5m, ad5x, cc1, qidi_q2, snapmaker_u1, creator5, k1 portrait 480x800, voron_24 at 1024x600, 1280x720, 1920x1080, ultrawide 1920x440. `ctl overflow` on each; contact sheet to the scratchpad; show Preston.
- [ ] **Step 2:** `make mutate-diff` once; add tests for survivors.
- [ ] **Step 3:** `make full-test-run`.
- [ ] **Step 4:** Independent review of the branch (Opus), fixes, then land per CLAUDE.md (batch with any ready peer branch).
