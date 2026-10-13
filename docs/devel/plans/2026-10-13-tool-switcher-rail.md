# Tool Switcher Rail Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the home tool switcher's pills-plus-Park layout with a segmented rail that never draws a target under 48 px, falls back to a mounted-head face plus a sheet when the tile is too small, and reads an empty carriage correctly on the U1.

**Architecture:** The four-layer measured-layout pattern from `docs/devel/PANEL_WIDGET_GUIDE.md` (nozzle-temps is the working instance): a pure header-only decision, measurement in the widget, the verdict published as widget subjects, and all drawing in XML bound to those subjects. Backend park capability and the tool label forms already on this branch are kept.

**Tech Stack:** C++17, LVGL 9.5 + helix-xml, Catch2, `helix-screen ctl` for verification. macOS: build with `gmake`.

**Spec:** `docs/devel/plans/2026-10-13-tool-switcher-rail-design.md` (read it first; this plan argues from it). One deliberate difference: the spec sketched `helix::ui::tool_switcher_layout()` in `include/`; this plan follows the nozzle-temps precedent, `helix::decide_tool_switcher_layout()` header-only in `src/ui/panel_widgets/`, and adds `pair_horizontal` to the verdict.

## Global Constraints

- Branch `feature/tool-switcher-labels-park`, worktree `.worktrees/tool-switcher-labels-park`. Claim it: `scripts/helix-claim take worktree:tool-switcher-labels-park "<task>" --pid $PPID`.
- No tappable target smaller than `#touch_target_min` (48 px) on either axis, at any tile size or breakpoint.
- Rail cells are separated by a 2 px gap; the content box is the tile minus `#space_md` on each side.
- Declarative rules (`.claude/rules/declarative-ui.md`): no `lv_obj_add_event_cb`, no imperative visibility, no `lv_label_set_text`, no C++ styling, no C++ widget-creation loops. Measured layout is the permitted C++.
- Vendor names stay in backends (`.claude/rules/vendor-abstraction.md`); the widget asks capability questions.
- Threading: subject writes on the main thread only (`.claude/rules/threading.md`).
- New user-visible strings go through `lv_tr` and into every `translations/*.yml`; regenerate with `gmake translations`.
- No comment archaeology. Tests tagged `[tool_switcher]` plus the issue (`[1784]` or `[1769]`).
- Commit with explicit pathspecs; let the pre-commit hook run; never `--no-verify`. Do not merge or push.
- Every new header gets `scripts/syntax_check.py <header>` before its task's tests are written.

## Review Focus

1. **Tool count changes after the widget is built** (discovery lands 0 → 4 heads, or a printer switch 4 → 2): the rail must rebuild to the new count, never index a pool past it. Test in Task 3.
2. **A configured tool name** ("Left", "Right Extruder") in a narrow segment: the label must fall back to the short form or ellipsize, never overflow the 48 px cell. Test in Task 3.
3. **No filament colour known** for a head (no AMS slot, or colour unset): the strip is omitted, not drawn black. Test in Task 3.
4. **A print starts while the sheet is open**: picking a head or Park from the stale sheet is refused with the existing toast. Test in Task 4.
5. **A backend that never reports an empty carriage** reports `active_tool` -1 transiently: `ToolState` clamps to T0 as today, and only backends whose carriage can be empty show "Choose head". Test in Task 1.

---

### Task 1: U1 reports an empty carriage when its heads are parked

**Files:**
- Modify: `include/ams_backend.h` (new virtual next to `load_mounts_tool()`)
- Modify: `include/ams_backend_snapmaker.h`, `src/printer/ams_backend_snapmaker.cpp#AmsBackendSnapmaker::apply_active_tool_locked`
- Modify: `src/printer/ams_state_tool_mapping.cpp` (the `topo.allows_empty_carriage` line)
- Test: `tests/unit/test_ams_backend_snapmaker.cpp` (new cases), `tests/unit/test_tool_state*.cpp` if a ToolState-level case fits better there

**Interfaces:**
- Produces: `virtual bool AmsBackend::carriage_can_be_empty() const` — default returns `load_mounts_tool()`; `AmsBackendSnapmaker` overrides to `true`. `ams_state_tool_mapping.cpp` sets `topo.allows_empty_carriage = backend->carriage_can_be_empty()`.
- Rule in `apply_active_tool_locked`: a head whose own `state` is `"PARKED"` is never the active tool, whatever `toolhead.extruder` names. When the named head is parked and no head has its active pin or a non-PARKED state, `current_tool` becomes -1.

- [ ] **Step 1: Write the failing tests** (`[snapmaker][tool_switcher][1784]`)
  - Feed the exact frame read from the U1 on 2026-10-13: `extruder`, `extruder1`, `extruder2`, `extruder3` all `{"state": "PARKED"}` and `toolhead: {"extruder": "extruder2"}`. Expect `get_system_info().current_tool == -1` and `current_slot == -1`.
  - Same frame but `extruder2.state` absent (incremental update): `current_tool` follows `toolhead.extruder` (2), as today.
  - A frame with `extruder1` active pin set and `toolhead.extruder` = `extruder1`, state not PARKED: `current_tool == 1`.
  - `AmsBackendSnapmaker::carriage_can_be_empty()` is true; a backend with `load_mounts_tool()` false and no override (e.g. the AFC backend) answers false.
  - ToolState level: with the U1 backend reporting -1, `ToolState::instance().active_tool_index() == -1`; with a backend that answers `carriage_can_be_empty() == false` and reports -1, it stays 0 (Review Focus 5).
- [ ] **Step 2: Run them and watch them fail:** `gmake t F='[1784]'`. Expected: the PARKED-frame case reports 2.
- [ ] **Step 3: Implement** the virtual, the override, the mapping line and the PARKED rule.
- [ ] **Step 4: Run** `gmake t F='[1784]'` and `gmake t F='[snapmaker]'`, then `gmake t F='[tool_state]'`. All pass.
- [ ] **Step 5: Hand-mutate** the PARKED check (remove it) and the override (return false); each must turn a case red. Restore.
- [ ] **Step 6: Commit** `fix(snapmaker): a parked head is not the active tool, so the carriage reads empty (prestonbrown/helixscreen#1784)` with `mutation:` lines.

### Task 2: The layout decision

**Files:**
- Create: `src/ui/panel_widgets/tool_switcher_layout.h` (header-only, beside `nozzle_layout.h`; no LVGL includes)
- Test: `tests/unit/test_tool_switcher_layout.cpp`

**Interfaces:**
- Produces (exact):

```cpp
namespace helix {
enum class ToolSwitcherMode : int { RailWithPark = 0, Rail = 1, Pair = 2, Single = 3 };

struct ToolSwitcherLayout {
    ToolSwitcherMode mode = ToolSwitcherMode::Single;
    int cols = 1;        ///< rail modes: grid columns; Pair: 2 along the long axis, else 1
    int rows = 1;
    int cell_w = 0;      ///< px; >= min_target in every mode
    int cell_h = 0;
    int rail_h = 0;      ///< px of the content box the rail or face occupies
    bool detail = false; ///< a detail block fits under the rail
    bool pair_horizontal = true; ///< Pair: face and Park side by side (else stacked)
};

ToolSwitcherLayout decide_tool_switcher_layout(int tool_count, int content_w, int content_h,
                                               int min_target, int gap, int body_px,
                                               int small_px);
}
```

- Mode selection, first that fits: `count + 1` cells (RailWithPark), `count` cells (Rail), two cells along the longer axis (Pair), else Single. "N cells fit": for rows = 1..N, cols = ceil(N/rows), cell = ((w − gap·(cols−1))/cols, (h − gap·(rows−1))/rows); the first rows whose cell is ≥ min_target on both axes fits; stop when cell height < min_target.
- Detail (rail modes only): rail rows sized at min(body_px·3.4, ...) but ≥ min_target; detail true when the rest of the box holds ≥ 2.5 × small_px; then `rail_h` is the rail's height, else `rail_h == content_h`.
- Pair: Park cell = max(min_target, 30% of the long axis).

- [ ] **Step 1:** Write the header with the declarations above and an empty inline body returning `{}`; run `scripts/syntax_check.py src/ui/panel_widgets/tool_switcher_layout.h`.
- [ ] **Step 2: Write the failing tests** (`[tool_switcher][1784]`):
  - The full table from the spec (content boxes: 800×480 1x1 95×93, 2x1 214×93, 1x2 95×211, 2x2 214×211; 480×272 1x1 58×52, 2x1 130×52, 1x2 58×118, 2x2 130×118; heads 2, 4, 5, 8; min 48, gap 2, body/small 18/16 and 12/10). Each cell asserts the mode.
  - 2x1 at 800×480: 4 heads → Rail (198 of 214 px), 5 heads → Pair.
  - 1 head and 16 heads on 214×211; a 0×0 box → Single; tool_count 0 → Single.
  - Property: for tool_count 1..16 and every box from 40×40 to 260×260 in 6 px steps, `cell_w >= 48 && cell_h >= 48` unless mode is Single on a box smaller than 48 (then the cell is the whole box).
  - Detail: 2x2 at 800×480 with 5 heads → detail true; 2x1 → false.
- [ ] **Step 3: Run** `gmake t F='[tool_switcher]'` — fails.
- [ ] **Step 4: Implement** the body.
- [ ] **Step 5: Run** — passes. Hand-mutate: drop the `+1` for Park; swap the Rail/RailWithPark order; let a cell under 48 count as a fit. Each turns a case red.
- [ ] **Step 6: Commit** `feat(tools): decide the tool switcher's layout from its box and a 48 px touch floor (prestonbrown/helixscreen#1784)`.

### Task 3: Measure and publish; per-tool subject pools

**Files:**
- Modify: `src/ui/panel_widgets/tool_switcher_widget.h`, `src/ui/panel_widgets/tool_switcher_widget.cpp`
- Test: `tests/unit/test_tool_switcher_widget.cpp`, `tests/unit/test_widget_size_tool_switcher.cpp`

**Interfaces:**
- Consumes: `decide_tool_switcher_layout()` (Task 2); `ToolLabelForm`, `tool_label_in_form()`, `pick_tool_label_form()` (`include/display_numbering.h`, on this branch); `active_tool`, `tool_count` subjects (ToolState).
- Produces (widget subjects, registered with `register_widget_subjects("tool_switcher", ...)` as `nozzle_temps_widget.cpp` does, so they exist before the XML parses):
  - int `tool_switcher_mode` (= `ToolSwitcherMode` value), `tool_switcher_cols`, `tool_switcher_rows`, `tool_switcher_cell_w`, `tool_switcher_cell_h`, `tool_switcher_rail_h`, `tool_switcher_detail` (0/1), `tool_switcher_label_form` (= `ToolLabelForm` value), `tool_switcher_park_supported` (0/1, existing).
  - per-tool pools for i in 0..15 (pool sized before `tool_count` is published, like `tool_cal_*_${i}`): string `tool_switcher_seg_${i}_label`, string `tool_switcher_seg_${i}_sub`, color `tool_switcher_seg_${i}_color`, int `tool_switcher_seg_${i}_has_color`.
  - detail pools in display order (mounted head first), i in 0..15: color `tool_switcher_detail_${i}_color`, string `tool_switcher_detail_${i}_text`, int `tool_switcher_detail_${i}_hot`, int `tool_switcher_detail_${i}_main`; int `tool_switcher_detail_rows` (how many fit).
- `on_size_changed()` and `attach()` measure the content box, call the decision, publish. No widget calls there.
- `fits_at()` returns true for every size.
- Remove: pill building (`rebuild_pills`, `pill_buttons_`, grid descriptors) and the Park pill path.

- [ ] **Step 1: Write the failing tests** (`[tool_switcher][1784]`):
  - Publishing: for 214×93 and 5 heads, `tool_switcher_mode` == Pair; for 4 heads, Rail and `tool_switcher_cols` == 4.
  - Pools: after a ToolState topology with 4 tools, `seg_0_label` / `seg_0_sub` follow the label form; a tool with no AMS colour has `seg_i_has_color` == 0 (Review Focus 3).
  - Count change: build at 4 tools, then republish ToolState with 2 → `tool_count` 2, pools 2..3 untouched by rendering (no XML row exists for them) and no out-of-range access under ASAN-style bounds checks; then grow to 6 → pools 4..5 filled (Review Focus 1).
  - Configured name: tool names "Left Extruder"/"Right Extruder" in a 48 px cell → label form falls back so the measured label width ≤ cell width (Review Focus 2).
  - Detail order: mounted head 2 of 4 → `detail_0_*` describes head 2, `detail_0_main` == 1; with no head mounted, the first detail row is the empty-carriage line.
- [ ] **Step 2: Run** `gmake t F='[tool_switcher]'` — fails.
- [ ] **Step 3: Implement** measure/publish and the pools; delete the pill code.
- [ ] **Step 4: Run** the tag plus `[widget_size]` — passes. Hand-mutate: skip republish on size change; publish the label form from the plain label instead of the measured one; leave the detail order by index. Each must turn a case red.
- [ ] **Step 5: Commit** `feat(tools): the tool switcher publishes its layout and per-head data as subjects (prestonbrown/helixscreen#1784)`.

### Task 4: Draw it in XML; events; the sheet

**Files:**
- Modify: `ui_xml/components/panel_widget_tool_switcher.xml` (replace the pill container)
- Create: `ui_xml/components/tool_switcher_segment.xml`, `ui_xml/components/tool_switcher_face.xml`
- Delete: `ui_xml/components/tool_switcher_park_pill.xml`
- Modify: `ui_xml/components/tool_picker_button.xml`, `ui_xml/tool_switcher_picker.xml` (sheet rows ≥ `#touch_target_min`, Park row)
- Modify: `src/ui/panel_widgets/tool_switcher_widget.cpp` (event callbacks registered with `lv_xml_register_event_cb`)
- Test: `tests/unit/test_tool_switcher_widget.cpp`, `tests/unit/test_tool_switcher_print_gate.cpp`

**Interfaces:**
- Consumes: Task 3's subjects; `handle_tool_selected(int)` and `handle_park_requested()` (existing).
- Produces: event callbacks `tool_switcher_segment_cb` (user_data is the segment index as a string, `"${i}"`: event_cb user_data is a `char*`), `tool_switcher_face_cb`, `tool_switcher_park_cb`. Segment tap: non-mounted → `handle_tool_selected(i)`; mounted → open the sheet.
- XML: four branches with `bind_flag_if_not_eq subject="tool_switcher_mode" ref_value="N" flag="hidden"`; the rail is a wrapping flex row; segments from `<repeat count="tool_count">`, sized with `bind_style_prop prop="width" subject="tool_switcher_cell_w"` / height (verify `bind_style_prop` accepts width/height on an int subject; if it does not, size them in C++ under `// DECLARATIVE_OK: measured layout` and say so in the commit); mounted look via `bind_style_if ... cond="active_tool eq ${i}"`; colour strip via `bind_style_prop prop="bg_color" subject="tool_switcher_seg_${i}_color"` and hidden when `_has_color` is 0; Park segment hidden unless mode is RailWithPark, disabled when `active_tool lt 0`; detail list a second `<repeat count="tool_switcher_detail_rows">`, rows centred (`style_flex_main_place="center"`).

- [ ] **Step 1: Write the failing tests:**
  - Rail at 4 heads builds 4 segments + no Park (Rail) and every segment's measured size ≥ 48×48 (`lv_obj_update_layout` first).
  - Tapping segment 1 when 0 is mounted requests T1; tapping the mounted segment opens the picker (`has_overlay_open()` true).
  - Pair face tap opens the picker; Park in Pair is disabled with no head mounted.
  - Sheet: one row per head plus Park, each row ≥ 48 px tall; Park row disabled with no head mounted.
  - Gating (Review Focus 4): open the sheet, move the print lifecycle to Printing, tap a head row → no tool change requested and the existing "Cannot park while a print is active" / tool-change refusal path runs.
- [ ] **Step 2: Run** `gmake t F='[tool_switcher]'` — fails.
- [ ] **Step 3: Implement** the XML and callbacks; delete the Park pill component and its registration.
- [ ] **Step 4: Run** `[tool_switcher]`, `[tool-switcher]`, `[widget_size]`, `[job_holds_machine]`; then `make test-xml`-equivalent gates via the commit hook. Hand-mutate: drop the mounted-segment sheet branch; drop the Park `active_tool lt 0` binding; make sheet rows `#button_height_sm_micro`. Each must turn a case red.
- [ ] **Step 5: Commit** `feat(tools): the tool switcher is a segmented rail that keeps every target finger-sized (prestonbrown/helixscreen#1784)`.

### Task 5: Filament panel closed dropdown reads the G-code name

**Files:**
- Modify: `src/ui/ui_panel_filament.cpp` (the `tool_short_label(...)` call for the closed control)
- Test: `tests/unit/test_display_numbering.cpp` or the filament panel dropdown test

**Interfaces:**
- Consumes: `tool_label_in_form(noun, name, index, ToolLabelForm::Code)`.

- [ ] **Step 1: Failing test** (`[1769]`): closed dropdown label for a generated name at index 0 reads "T0"; a configured name keeps its physical label.
- [ ] **Step 2: Run** — fails ("1").
- [ ] **Step 3: Implement**; remove `tool_short_label()` if nothing else calls it.
- [ ] **Step 4: Run** `[1769]` and `[filament]` — passes.
- [ ] **Step 5: Commit** `feat(filament): the closed tool dropdown names the tool as G-code does (prestonbrown/helixscreen#1769)`.

### Task 6: Verify in the app and ship the docs

**Files:**
- Modify: `docs/devel/TOOL_ABSTRACTION.md` (empty carriage on the U1), `docs/devel/PANEL_WIDGET_GUIDE.md` (tool switcher as a second measured-layout instance, one paragraph)
- Delete: `docs/devel/plans/2026-10-13-tool-switcher-rail-design.md`, this plan

- [ ] **Step 1:** Launch with the pinned-socket recipe (CLAUDE.md), `HELIX_MOCK_AMS=toolchanger HELIX_AMS_GATES=N` for N in 2, 4, 5, 8, at 800×480 and `-s micro`; and the U1 persona. For 1x1, 2x1, 1x2, 2x2: `ctl geom` every segment/face/Park ≥ 48×48; the mode per size matches the spec table; screenshots into the scratchpad.
- [ ] **Step 2:** U1 persona: mount, then Park; `active_tool` reads -1 and the face shows "Choose head".
- [ ] **Step 3:** `gmake unit-sweep` (macOS: compare failures against #1776's known set), then hand the screenshots to Preston for the look.
- [ ] **Step 4:** Move the durable facts into the two docs; delete the spec and this plan.
- [ ] **Step 5: Commit** `docs(tools): tool switcher rail and the U1 empty carriage` and kill the instance by PID from its own socket.
