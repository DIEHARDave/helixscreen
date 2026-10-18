# Plugin Menu Items Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Plugins declare `menu_items` in their manifest and get rows in four app menus (print details, filament slot, Spoolman spool, their own tile), each tap running a `helix.ui.on` handler with a context table, gated by an optional side-effect-free check.

**Architecture:** The manifest parser grows `MenuItemDecl`. `PluginHost` keeps the decls of loaded plugins and answers two questions through one small app-facing header (`include/plugin_menu.h`): "does any plugin declare items for menu X" and "resolve the items for menu X against this context" (which runs checks through a new synchronous, check-mode `LuaRuntime::call_sync`). App menus build plain JSON contexts with pure builders, append rows through one shared helper, and call `plugin_menu::activate` on tap; they never see Lua or plugin internals.

**Tech Stack:** C++17, LVGL 9.5 + helix-xml, Lua 5.4 runtime (`src/plugin/`), Catch2.

**Spec:** `docs/devel/plans/2026-10-18-plugin-menu-items-design.md` (approved). Guide section already written: `docs/devel/PLUGIN_DEVELOPMENT.md` § "Menu items".

## Global Constraints

- App menus must not name plugins or know Lua: they include `plugin_menu.h` only (no `lua_*.h`, no `plugin_host.h`).
- `handler` and `check` are LOCAL handler names, as passed to `helix.ui.on` (validated with `is_valid_local_name`: 1-48 of `[a-z0-9_-]`).
- `menu` is one of `print_file`, `filament_slot`, `spool`, `tile`. A `tile` item in a plugin with no `widgets` fails the parse.
- Limits: at most 4 items per menu per plugin; at most 16 `menu_items` per manifest. Overall plugin rows shown per opening: `print_file` 8, `spool` 6, `tile` 4, `filament_slot` = the number Task 7 measures (fits at 480x272 with the tallest app content, no scroll). Rows past the cap are dropped in load order, logged once at debug.
- Check budget: 10 ms CPU, 100 ms wall, separate from the 50 ms entry budget. A check that overruns, raises, yields, or returns anything other than a boolean or `{visible, enabled, reason}` hides its item and calls `report_error`; never `fault()` directly.
- Check mode: every async binding (through `await_async`) and every side-effecting sync binding raises `"<call> is not allowed in a menu check"` before acting.
- Labels and reasons pass through the plugin's i18n (`lv_tr` of the plugin-namespaced key, as widget labels do today).
- The "⋯" button uses the existing `dots_vertical` icon.
- No comment archaeology; spdlog only; no RTTI/exceptions in shared code; XML appearance, C++ data (`.claude/rules/declarative-ui.md`).
- Verification cadence: while building, `make t F='[tags]'` for the touched tags only, each new test proven red once. No sweep, gate, mutation or review per task.

## Review Focus

1. A plugin unloaded or faulted while its rows are on screen: a later tap must do nothing (gen check in `activate`). Pinned in Task 3.
2. A check that calls `helix.ui.dialog`/`gcode`/`storage_set`: no side effect lands, the plugin stays loaded. Pinned in Task 2.
3. Print details opened on a USB file, and opened before metadata arrives: context has `source="usb"`, `path=nil`, missing metadata as `nil`, never 0. Pinned in Task 4.
4. Slot menu at 480x272 with every app row visible plus the cap of plugin rows: card does not scroll. Pinned in Task 7.
5. Two plugins declaring items for one menu: grouped under each plugin's name in load (alphabetical) order, cap applied across both. Pinned in Task 3/5.

---

### Task 1: Manifest `menu_items`

**Files:**
- Modify: `include/plugin_manifest.h`, `src/plugin/plugin_manifest.cpp`
- Test: `tests/unit/test_plugin_manifest.cpp` (`[plugin][manifest]`)

**Interfaces:**
- Produces:
  ```cpp
  enum class MenuKind { PrintFile, FilamentSlot, Spool, Tile };
  std::optional<MenuKind> parse_menu_kind(std::string_view);   // "print_file" ...
  const char* menu_kind_name(MenuKind);
  struct MenuItemDecl { std::string id, label, icon, handler, check; MenuKind menu; };
  // Manifest gains: std::vector<MenuItemDecl> menu_items;
  inline constexpr int kMaxMenuItemsPerMenu = 4, kMaxMenuItems = 16;
  ```
  `MenuKind` lives in `include/plugin_menu_kind.h` (tiny, no deps) so `plugin_menu.h` can include it without the manifest.

- [ ] **Step 1: failing tests** in the manifest file:
  - `menu_items parse`: two items (`print_file` with all fields, `spool` without icon/check) parse; fields equal input; `menu` maps to the enum.
  - `menu_items errors` (table-driven, each asserts the error string contains `menu_items[0]: '<field>'`): missing `id`, missing `label`, missing `handler`, `handler` "Bad Name", `check` with a space, unknown `menu` "toolbar" (`'menu' must be one of print_file, filament_slot, spool, tile`), duplicate `id` within the plugin, label over the display-field cap, `tile` item with no `widgets` (`'tile' items need a widget`), five items for one menu (`at most 4 items for print_file`), 17 items (`'menu_items' may list at most 16`), `menu_items` not an array.
- [ ] **Step 2:** `make t F='[plugin][manifest]'`. Expected: new cases FAIL (no `menu_items` member).
- [ ] **Step 3:** implement `parse_menu_items(const json&, Manifest&, errors)` in the anonymous namespace following `parse_widgets`'s `where`/`local` pattern; call it after `parse_widgets` (the tile rule reads `m.widgets`). Reuse `require_string`, `optional_string`, `check_display_field`.
- [ ] **Step 4:** `make t F='[plugin][manifest]'`. Expected: PASS.
- [ ] **Step 5:** commit `feat(plugins): parse menu_items in the manifest`.

### Task 2: Synchronous check calls and check mode

**Files:**
- Modify: `include/lua_runtime.h`, `src/plugin/lua_runtime.cpp`, `include/lua_bindings.h`, `src/plugin/lua_bind_core.cpp`, `lua_bind_io.cpp`, `lua_bind_ui.cpp`, `lua_bind_moonraker.cpp`, `lua_bind_printer.cpp`
- Test: `tests/unit/test_lua_runtime.cpp` (`[plugin][lua_runtime][check]`), `tests/unit/test_lua_bindings_ui.cpp` (`[plugin][bindings][ui][check]`)

**Interfaces:**
- Produces:
  ```cpp
  // LuaRuntime
  struct CheckLimits { std::chrono::milliseconds time_budget{10}, wall_ceiling{100}; };
  using ReadFn = std::function<void(lua_State* co, int nres)>;
  /// Runs fn_ref to completion in check mode. false = error, overrun, yield, or faulted
  /// runtime; each failure is passed to report_error. read runs only on success.
  bool call_sync(int fn_ref, const PushFn& args, const ReadFn& read, CheckLimits = {});
  bool in_check() const;
  // lua_bindings.h
  void forbid_in_check(lua_State* L, const char* call);  // luaL_error when in check mode
  bool call_ui_handler_sync(LuaRuntime&, const std::string& name, const LuaRuntime::PushFn&, const LuaRuntime::ReadFn&);
  bool dispatch_ui_handler(LuaRuntime&, const std::string& name, const LuaRuntime::PushFn& args); // table-arg sibling
  ```
- `call_sync` sets `in_check_` and its own deadlines for the duration, nests inside `enter` at depth 0 only (refuse at depth > 0, report_error). An overrun inside a check must NOT fault: `enter`'s `depth_==0 && killed_ -> fault` path is skipped while `in_check_`; it clears `killed_` and reports instead.

- [ ] **Step 1: failing tests** (`BoundRuntime`):
  - `call_sync returns a boolean`: handler `return true` → read sees `true`.
  - `call_sync returns a table`: `{visible=true, enabled=false, reason="no"}` readable.
  - `check overrun reports, does not fault`: `while true do end` → `call_sync` false, `faulted()` false, error count 1.
  - `check raising reports`: `error("x")` → false, not faulted.
  - `check yield fails`: `helix.sleep(1)` → false, no timer armed (`fake` saw no work), not faulted.
  - `side effects refused in check`: for each of `helix.ui.toast`, `helix.ui.dialog{...}`, `helix.storage.set`, `helix.settings.set`, `helix.subject.set`, `helix.timer.after`, `helix.moonraker.gcode` (with permission granted), `helix.http.request`: returns false and the fake backend / ui recorded nothing.
  - `reads allowed in check`: `helix.settings.get`, `helix.subject.get`, `helix.printer.*` read, local Lua state → true.
  - `three failing checks fault through report_error`: the existing ErrorWindow applies (third error faults); documents that checks share the quota.
  - `dispatch_ui_handler with a table`: handler receives `ctx.path == "a/b.gcode"`, `ctx.missing == nil`.
- [ ] **Step 2:** `make t F='[plugin][lua_runtime][check]'` and `F='[plugin][bindings][ui][check]'`. Expected: FAIL (no `call_sync`).
- [ ] **Step 3:** implement. Guard at the top of `await_async` (before `start`) covers every async binding; add `forbid_in_check` to: `storage_set`, `settings_set`, `subject_set`, `ui_toast`, `ui_confirm`, `open_surface`, `timer_after`, `timer_every`, navigation bindings, and the registration calls (`ui_on`, `subject_observe`, `settings_on_change`, `i18n_on_change`, `printer_watch`, `subscribe`, `on_agent_event`). Read results with `push_json`'s inverse: a `bool` or a table read via `lua_getfield`.
- [ ] **Step 4:** run both tags. Expected: PASS. Run `[plugin][lua_runtime]` and `[plugin][bindings]` once for regressions.
- [ ] **Step 5:** commit `feat(plugins): synchronous check calls in a side-effect-free mode`.

### Task 3: The app-facing menu API and host registry

**Files:**
- Create: `include/plugin_menu.h`, `include/plugin_menu_kind.h`, `src/plugin/plugin_menu.cpp`
- Modify: `include/plugin_host.h`, `src/plugin/plugin_host.cpp`
- Test: `tests/unit/test_plugin_menu.cpp` (`[plugin][menu]`)

**Interfaces:**
- Consumes: Task 1 `MenuItemDecl`, Task 2 `call_ui_handler_sync`, `dispatch_ui_handler(table)`.
- Produces (`plugin_menu.h`, namespace `helix::plugin_menu`):
  ```cpp
  struct Entry {
      std::string plugin_id, plugin_name, item_id, label, icon, reason;
      bool enabled = true;
      uint64_t gen = 0;   // host load generation; stale entries do nothing on activate
  };
  bool any_declared(MenuKind kind, std::string_view plugin_id = {});
  std::vector<Entry> resolve(MenuKind kind, const nlohmann-style json& context, std::string_view only_plugin = {});
  void activate(const Entry& e, const json& context);
  int row_cap(MenuKind kind);
  ```
  (`json` from `hv/json.hpp`.) With no live host every call returns empty/false/no-op.
- Host: `resolve` iterates `plugins()` in order, Loaded and `!rt->faulted()` only; per item: no `check` → enabled; `check` → `call_ui_handler_sync` with the context, mapping `true`/`false`/table; failures hide. Applies `row_cap`. `activate` looks up `loaded_[id]`, compares `gen`, then `dispatch_ui_handler(rt, handler, push_json(context))`. No separate registry container: the decls live on the loaded `Manifest`, so `unload()` removes them with nothing extra to drop.

- [ ] **Step 1: failing tests** (LVGLTestFixture + a temp plugin dir written by the test, as `test_plugin_host.cpp` does):
  - `declared items resolve with labels in load order`: plugins "b-plug" and "a-plug" each declare a `spool` item; `resolve(Spool, {})` → a-plug first, plugin_name from manifest.
  - `any_declared` true for declared kind, false for others, filtered by plugin id.
  - `check false hides; table disables with reason`.
  - `failing check hides only that item, plugin stays loaded`.
  - `disabled plugin contributes nothing`; `faulted runtime contributes nothing`.
  - `activate runs the handler with the context`: handler stores `ctx.spool_id` into storage; read back 42.
  - `activate after unload does nothing`: resolve, `disable(id)`, re-`enable(id)` (new gen), activate old entry → handler not run.
  - `cap applies across plugins`: three plugins × 3 `tile`... use `spool` with cap 6: 3 plugins × 3 items → 6 entries, the last plugin's third dropped.
- [ ] **Step 2:** `make t F='[plugin][menu]'`. Expected: FAIL (header missing).
- [ ] **Step 3:** implement; host exposes `std::vector<plugin_menu::Entry> resolve_menu(...)`, `bool menu_declared(...)`, `void activate_menu(...)` and `plugin_menu.cpp` forwards to `PluginHost::live()`.
- [ ] **Step 4:** `make t F='[plugin][menu]'` PASS; `[plugin][host]` once.
- [ ] **Step 5:** commit `feat(plugins): resolve and activate plugin menu items`.

### Task 4: Context builders

**Files:**
- Create: `include/plugin_menu_context.h`, `src/plugin/plugin_menu_context.cpp`
- Test: `tests/unit/test_plugin_menu_context.cpp` (`[plugin][menu][context]`)

**Interfaces:**
- Produces (pure, no LVGL):
  ```cpp
  struct PrintFileFacts {
      std::string name, dir, local_path;            // local_path non-empty = USB
      std::optional<FileMetadata> meta;
      std::vector<std::string> materials, colors;   // per tool, from the detail view
      std::vector<double> tool_grams;               // empty = unknown
      std::set<int> tools_used;
  };
  json print_file_context(const PrintFileFacts&);
  json slot_context(const SlotInfo&, const AmsUnit* unit, const std::string& label);
  json spool_context(const SpoolInfo&);
  json tile_context(const std::string& widget_id);
  ```
- Field rules (spec table): `source` "usb"/"printer"; `path` = `join_gcode_path(dir, name)` only for printer files; `estimated_time_s`, `layer_height`, `object_height` from `meta` when present and > 0, else absent; `filament` array of `{tool, material, color, grams}` with each unknown member absent; `tools_used` array. Slot: `unit` = unit_index, `slot` = slot_index within unit (0-based), `label`, `tool` absent when `mapped_tool < 0`, `spool_id` absent when 0, `color` as `#RRGGBB`, `material`/`brand` absent when empty. Spool: `remaining_g` absent when `remaining_weight_g <= 0`. Tile: `widget` = the declared short name (strip `<plugin>__`).

- [ ] **Step 1: failing tests**: printer file with full metadata (every field asserted); USB file (`source=="usb"`, no `path`, `local_path` set); metadata absent (`estimated_time_s` absent, not 0); slot with no spool/tool; slot with everything; spool with 0 g; tile id stripping.
- [ ] **Step 2:** `make t F='[plugin][menu][context]'` FAIL.
- [ ] **Step 3:** implement. Reuse `helix::gcode::join_gcode_path` and the existing colour-to-hex helper used by the AMS menu (grep `color_rgb` formatting; do not write a new one).
- [ ] **Step 4:** PASS.
- [ ] **Step 5:** commit `feat(plugins): menu context tables for files, slots, spools and tiles`.

### Task 5: Shared plugin rows and the plugin-items menu

**Files:**
- Create: `include/ui_plugin_menu_rows.h`, `src/ui/ui_plugin_menu_rows.cpp`, `include/ui_plugin_items_menu.h`, `src/ui/ui_plugin_items_menu.cpp`, `ui_xml/plugin_items_menu.xml`, `ui_xml/components/plugin_menu_row.xml` (or reuse `context_menu_action` row component if one exists: check `ui_xml/components/` first and use it)
- Test: `tests/unit/test_plugin_menu_rows.cpp` (`[ui][context_menu][plugin]`)

**Interfaces:**
- Consumes: Task 3 `Entry`, `activate`.
- Produces:
  ```cpp
  /// Appends a divider, then per plugin a heading row (plugin_name) and its item rows.
  /// Disabled entries are greyed with their reason as the row's second line. A tap
  /// closes `menu` and activates the entry with `context`. Returns rows added.
  int append_plugin_rows(ContextMenu& menu, lv_obj_t* container,
                         std::vector<plugin_menu::Entry> entries, json context);
  class PluginItemsMenu : public ContextMenu {   // print_file and tile menus
    public:
      bool show_for(lv_obj_t* parent, lv_obj_t* anchor, MenuKind kind, json context,
                    std::string_view only_plugin = {});
  };
  ```
  `PluginItemsMenu` resolves in `on_created`, shows an empty-state label "No plugin actions for this file" (`print_file`) / "No actions right now" (`tile`) when nothing resolves.

- [ ] **Step 1: failing tests** (XMLTestFixture, test-only `ContextMenu` subclass as in `test_context_menu_row_width.cpp`, plus a plugin in a temp dir): rows appear with heading + label; disabled row is `LV_STATE_DISABLED` with reason text; tapping a row (send `LV_EVENT_CLICKED`) runs the handler and hides the menu; tap after the plugin is disabled runs nothing; `PluginItemsMenu` with all checks false shows the empty-state label.
- [ ] **Step 2:** `make t F='[ui][context_menu][plugin]'` FAIL.
- [ ] **Step 3:** implement; rows are created with `lv_xml_create` (PrinterSwitchMenu pattern); each row's tap data (entry + context) is owned by the row (`lvgl_make_unique`, freed on `LV_EVENT_DELETE`). Register the new XML component where other context menu components are registered.
- [ ] **Step 4:** PASS; `[ui][context_menu]` once.
- [ ] **Step 5:** commit `feat(ui): plugin rows for context menus`.

### Task 6: Print details "⋯"

**Files:**
- Modify: `ui_xml/print_file_detail.xml`, `src/ui/ui_print_select_detail_view.cpp`, `include/ui_print_select_detail_view.h`, `src/ui/ui_panel_print_select.cpp` (callback registration)
- Test: `tests/unit/test_print_select_detail_subjects.cpp` (`[print_select][detail_view][plugin_menu]`)

**Interfaces:**
- Consumes: Task 3 `any_declared`, Task 4 `print_file_context`, Task 5 `PluginItemsMenu`.
- A new int subject `print_detail_has_plugin_items` (0/1) owned by the detail view, set in `show()` from `any_declared(PrintFile)` (and on plugin enable/disable through the host's existing change notification if one exists; otherwise on every `show()`, which is enough since the menu re-resolves on tap). XML binds: when 1, `print_button` hides its text (icon only, fixed square width via a style bound like `delete_btn_*`), a new `more_button` (`icon="dots_vertical"`, square) is shown between Delete and Print. When 0, layout identical to today.
- Tap: `on_print_select_more_button` builds `PrintFileFacts` from `current_filename_`, `current_path_`, `current_local_path_`, `cached_file_metadata_`, `current_filament_materials_`, `current_filament_colors_`, `get_tool_grams()`, `get_tools_used()` and opens `PluginItemsMenu` below the button.

- [ ] **Step 1: failing tests** (`OpenDetail` helper, register the new noop callback): no plugin → `more_button` hidden and print button label visible; a plugin declaring a `print_file` item → `more_button` visible, print label hidden; tapping more on a USB file opens the menu and the handler sees `source=="usb"` and no `path`; metadata not yet loaded → handler sees no `estimated_time_s`.
- [ ] **Step 2:** `make t F='[print_select][detail_view][plugin_menu]'` FAIL.
- [ ] **Step 3:** implement.
- [ ] **Step 4:** PASS; `[print_select][detail]` once. `ctl geom` the button row at 800x480, 480x320, 480x272 with a plugin enabled: three buttons fit, none clipped (record numbers in the commit body).
- [ ] **Step 5:** commit `feat(print-select): plugin actions on print details`.

### Task 7: Filament slot menu

**Files:**
- Modify: `ui_xml/ams_context_menu.xml` (a named `plugin_rows` container after the endless-spool row), `src/ui/ui_ams_context_menu.cpp`, `src/plugin/plugin_menu.cpp` (`row_cap(FilamentSlot)`)
- Test: `tests/unit/test_ams_context_menu.cpp` (`[ams][context_menu][plugin]`)

**Interfaces:**
- Consumes: Tasks 3-5. In `on_created`, when `!external_spool_mode_ && backend_`: `slot_context(backend_->get_slot_info(slot), system_info.unit_for_slot(slot), lane label shown in slot_header)`, then `append_plugin_rows` into `plugin_rows`.

- [ ] **Step 1: failing tests** (live-menu pattern from the file): plugin row under the dropdowns with the plugin heading; handler receives `unit`, `slot` (0-based in unit), `label`, `material`; external spool menu has no plugin rows; at 480x272 with every app row visible and `row_cap` plugin rows, the card's content height <= its height (no scroll) — measure first, then set the cap to the largest value that passes.
- [ ] **Step 2:** FAIL. **Step 3:** implement. **Step 4:** `make t F='[ams][context_menu]'` PASS.
- [ ] **Step 5:** commit `feat(ams): plugin actions in the filament slot menu`.

### Task 8: Spoolman spool menu

**Files:**
- Modify: `ui_xml/spoolman_context_menu.xml` (`plugin_rows` container at the end), `src/ui/ui_spoolman_context_menu.cpp`
- Test: Create `tests/unit/test_spoolman_context_menu.cpp` (`[spoolman][context_menu][plugin]`); register `spoolman_context_menu.xml` in the test if the fixture does not.

- [ ] **Step 1: failing tests**: plugin row appended with heading; handler sees `spool_id`, `material`, `color`, no `remaining_g` for 0 g; disabled plugin adds nothing.
- [ ] **Step 2:** FAIL. **Step 3:** implement in `on_created` from `pending_spool_`. **Step 4:** PASS.
- [ ] **Step 5:** commit `feat(spoolman): plugin actions in the spool menu`.

### Task 9: Tile gear

**Files:**
- Modify: `include/lua_panel_widget.h`, `src/plugin/lua_panel_widget.cpp`
- Test: `tests/unit/test_lua_bindings_widget.cpp` (`[plugin][lua_bindings_widget][menu]`)

- `has_edit_configure()` = `plugin_menu::any_declared(Tile, plugin_id_)` (and runtime alive). `on_edit_configure()` opens `PluginItemsMenu::show_for(lv_obj_get_screen(root()), root(), Tile, tile_context(widget_id_), plugin_id_)` and returns false.

- [ ] **Step 1: failing tests**: no tile items → `has_edit_configure()` false; with a tile item → true; `on_edit_configure()` opens a menu whose tap runs the handler with `ctx.widget == "<short name>"`; another plugin's tile items never appear on this tile.
- [ ] **Step 2:** FAIL. **Step 3:** implement. **Step 4:** PASS.
- [ ] **Step 5:** commit `feat(plugins): tile actions from the edit-mode gear`.

### Task 10: Consent "Adds to"

**Files:**
- Modify: `src/plugin/plugin_consent.cpp`, `include/plugin_consent.h`, `src/plugin/plugins_overlay.cpp`
- Test: `tests/unit/test_plugin_consent.cpp` (`[plugin][consent]`), `tests/unit/test_plugins_overlay.cpp` (`[plugin][plugins_overlay]`)

- Produces: `std::string menu_targets_line(const Manifest&)` → `"Adds to: Print details, Filament slot menu"` in MenuKind order, deduplicated, empty when none. Names: "Print details", "Filament slot menu", "Spool menu", "Its home tile". All through `lv_tr`.
- `consent_message` appends the line (after permissions, also for update prompts so the user sees it); the plugin row description in `populate_rows` appends it on a second line.

- [ ] **Step 1: failing tests**: line for two kinds; empty for none; consent message contains it; overlay row description contains it.
- [ ] **Step 2:** FAIL. **Step 3:** implement; `make translation-sync && make translations`, fill the new keys in all locales. **Step 4:** PASS.
- [ ] **Step 5:** commit `feat(plugins): list menu targets in plugin consent` (include translation YAMLs + `ui_xml/translations/*.xml`).

### Task 11: Example, guide limits, cleanup

**Files:**
- Modify: `examples/plugins/calibration-bridge/manifest.json`, its `main.lua`, `README.md`; `docs/devel/PLUGIN_DEVELOPMENT.md` (§10 limits rows: items per menu per plugin, per manifest, per-menu row caps, check budget; confirm the Menu items section matches what shipped); `docs/user/` plugins page if it lists what plugins add.
- Test: the example manifest parses (`tests/unit/test_plugin_examples*.cpp` or the existing example-manifest test; add the item there).

- calibration-bridge adds `{"id":"calibrate","menu":"print_file","label":"Calibrate with this file","icon":"tune","handler":"calibrate_file","check":"can_calibrate_file"}`; the check returns `ctx.source == "printer"`; the handler opens its existing dialog/overlay with the file path.
- [ ] Steps: example test first (FAIL on unknown handler not registered if the test checks that, else on missing item), implement, PASS, commit `docs(plugins): menu items example and limits`.
- [ ] Delete both plan files (`docs/devel/plans/2026-10-18-plugin-menu-items*.md`) in the final commit of the batch.
