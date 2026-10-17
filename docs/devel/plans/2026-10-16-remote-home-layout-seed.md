# Remote Home Layout Seed Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A client adding a printer whose own HelixScreen published its home layout starts with a port of that layout instead of the generated default.

**Architecture:** A pure module (`home_layout_share`) builds and validates the published payload. `PanelWidgetConfig` gains one import entry point that loads a payload authored for one grid and ports it through the existing `switch_to_grid` path. A `HomeLayoutSync` service wires it to Moonraker: publishes on save when this instance hosts Moonraker, fetches on connect when it does not, and hands the result to the seed point in `PanelWidgetManager` or the catalog's manual action.

**Tech Stack:** C++17, LVGL 9.5, Catch2, libhv's nlohmann json (`hv/json.hpp`), Moonraker database API.

**Spec:** `docs/devel/plans/2026-10-16-remote-home-layout-seed-design.md` (read it first; this plan argues from it).

## Global Constraints

- Work in a worktree: `scripts/setup-worktree.sh feature/remote-home-layout-seed`. Critical path (PrinterState, threading): MAJOR.
- Moonraker database namespace `helix-screen`, key `home_layout`.
- Payload `schema` is `1`. The grid signature string is `"<cols>x<rows>"` in tracks, same as `PanelWidgetConfig::grid_signature()`.
- Screen-scoped widget ids, exactly: `network`, `clock`, `lock`, `tips`, `notifications`, `shutdown`. Everything else, including `host_health` and plugin widgets, is printer-scoped.
- Firmware-compiled code: no exceptions. `json_util::safe_*`, `json::parse(s, nullptr, false)`, no `.at()` / `.value()`. New sources go in `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt`; `scripts/check_esp32_app_srcs.py` must pass.
- Database callbacks run off the main thread: every LVGL or config touch goes through `ui_queue_update()` / `tok.defer()` with an `AsyncLifetimeGuard` (load the `helix-threading` skill before Tasks 4-5).
- User-facing strings wrapped in `lv_tr()`; run `make translation-sync` and `make translations` and commit the YAMLs plus `ui_xml/translations/*.xml`.
- No comment archaeology. `spdlog` only, `[HomeLayoutSync]` / `[HomeLayoutShare]` log prefixes.
- Inner loop is `make t F='[tag]'`. Mutation (`make mutate-diff`) and `make land-gate` run once, at the end (Task 7).

## Review Focus

1. **A remote client publishing before its verdict lands.** The first save after a printer switch must not post. Pinned in Task 4 (`publishes nothing while locality is unknown`).
2. **The fetch answering for the previous printer.** A switch between request and reply must discard the reply. Pinned in Task 4 (`a reply for a printer no longer active is dropped`).
3. **A published layout that reaches the client after the default was saved.** It replaces the layout only while that is still an unedited default. Pinned in Task 5.
4. **A hostile or truncated payload** (pages not an array, spans of 999, negative origins, 500 widgets). It must never leave the client without a layout or with off-grid cells. Pinned in Tasks 2 and 3.
5. **Every page empty after filtering** (a source whose layout was only screen-scoped tiles). The import is rejected and the client keeps its default. Pinned in Task 3 (`import of only screen-scoped widgets is rejected`).

---

### Task 1: Widget scope in the registry

**Files:**
- Modify: `include/panel_widget_registry.h` (`PanelWidgetDef`, beside `category`)
- Modify: `src/ui/panel_widget_registry.cpp` (the six screen-scoped defs)
- Test: `tests/unit/test_panel_widget_config.cpp`

**Interfaces:**
- Produces: `enum class WidgetScope { Printer, Screen };` and `WidgetScope scope = WidgetScope::Printer;` on `PanelWidgetDef`. `bool is_screen_scoped(const std::string& widget_id)`, declared in `panel_widget_registry.h` beside `find_widget_def`. It strips a `base:N` instance suffix before the lookup and returns false for unknown ids (plugins).

- [ ] **Step 1: Write the failing test** `TEST_CASE("PanelWidgetRegistry: screen-scoped widgets are exactly the six", "[panel_widget][widget_scope]")`. Iterate the full registry and collect the ids with `scope == WidgetScope::Screen`. REQUIRE the set equals `{network, clock, lock, tips, notifications, shutdown}`. REQUIRE `!is_screen_scoped("host_health")`, `!is_screen_scoped("no_such_widget")`, and `is_screen_scoped("clock")`.
- [ ] **Step 2: Run** `make t F='[widget_scope]'`. Expected: compile failure on `WidgetScope`.
- [ ] **Step 3: Implement.** Add the enum, the field and `is_screen_scoped`. Set `.scope = WidgetScope::Screen` on the six defs. Add one line to the `PanelWidgetDef` doc comment: the scope decides whether the widget describes the printer or the screen it runs on.
- [ ] **Step 4: Run** `make t F='[widget_scope]'`. Expected: PASS.
- [ ] **Step 5: Commit** `git commit -m "feat(home): tag home widgets printer- or screen-scoped" -- include/panel_widget_registry.h src/ui/panel_widget_registry.cpp tests/unit/test_panel_widget_config.cpp`.

### Task 2: Published-layout payload (pure)

**Files:**
- Create: `include/home_layout_share.h`, `src/system/home_layout_share.cpp`
- Modify: `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt`
- Test: `tests/unit/test_home_layout_share.cpp`

**Interfaces:**
- Consumes: `is_screen_scoped` (Task 1).
- Produces, in `namespace helix::home_layout`:
  - `inline constexpr const char* kNamespace = "helix-screen";`, `inline constexpr const char* kKey = "home_layout";`, `inline constexpr int kSchema = 1;`
  - `nlohmann::json build_payload(const nlohmann::json& serialized_pages, const std::string& grid_signature, const std::string& app_version, int64_t saved_at_unix);` It strips `default` and `next_page_id` from the pages object and returns `{schema, grid, pages, main_page_index, saved_at, app_version}`.
  - `struct ParsedLayout { int cols; int rows; nlohmann::json pages_payload; };` `pages_payload` has `restore_pages()`'s input shape: `{pages, main_page_index}`.
  - `std::optional<ParsedLayout> parse_payload(const nlohmann::json& value);` It returns nullopt when: the value is not an object; `schema` is missing or not `kSchema`; `grid` does not parse to positive cols and rows of at most 64 (`GridLayout` clamps to 4..64); or `pages` is not a non-empty array. It removes screen-scoped widget entries, then drops pages left with no entries and remaps `main_page_index` to the same page or to 0. If no page is left, it returns nullopt.

- [ ] **Step 1: Write failing tests**, tag `[home_layout_share]`:
  - `build_payload strips default and next_page_id`
  - `round trip: parse_payload(build_payload(x)) keeps every printer-scoped entry and its config`. Use a `favorite_macro` entry with `{macro: "CLEAN_NOZZLE"}` and a `preheat` entry with `{material_index: 2}`.
  - `screen-scoped entries are removed, host_health kept`
  - `a page left empty is dropped and main_page_index follows its page`. Three pages, main = 2, page 1 only `clock`: the result has 2 pages and main = 1.
  - `rejects`: non-object, `schema` 2, `schema` missing, `grid` "0x8", `grid` "12x", `grid` "200x8", `pages` "x", `pages` [], all pages screen-scoped only. Each REQUIREs `!parse_payload(v).has_value()`.
  - `unknown widget ids and unknown config keys survive parsing` (they reach `retained` later, in Task 3).
- [ ] **Step 2: Run** `make t F='[home_layout_share]'`. Expected: link failure.
- [ ] **Step 3: Implement** both functions with `json_util::safe_*`. Add `src/system/home_layout_share.cpp` to `app_srcs.txt`.
- [ ] **Step 4: Run** `make t F='[home_layout_share]'` and `python3 scripts/check_esp32_app_srcs.py`. Expected: PASS, and the check exits 0.
- [ ] **Step 5: Commit** `git add -N` the new files, then `git commit -m "feat(home): build and validate the published home layout payload" -- <the five paths>`.

### Task 3: `PanelWidgetConfig::import_layout`

**Files:**
- Modify: `include/panel_widget_config.h`, `src/system/panel_widget_config.cpp`
- Test: `tests/unit/test_panel_widget_config.cpp`

**Interfaces:**
- Consumes: `ParsedLayout` (Task 2).
- Produces: `bool import_layout(const helix::home_layout::ParsedLayout& layout, int cols, int rows);`
  - It returns false and changes nothing when `cols`/`rows` <= 0, or when restoring would yield no page holding a widget. Check that before `restore_pages()` runs, because `restore_pages()` builds defaults on an empty result.
  - On success it ends with:
    - the pages restored and authored for `layout.cols x layout.rows`, then ported by `switch_to_grid(cols, rows)`
    - `has_default_` false
    - `parked_grids_` empty, clearing both the client's old parks and the park `switch_to_grid` writes for the source grid
    - `grid_signature()` == `"<cols>x<rows>"`
    - the client's screen-scoped default widgets (the default-enabled screen-scoped ids in `build_defaults()`) appended to the main page enabled and unplaced, unless already present
    - `save()` called once
  - Share the page-loading code with `restore_pages()` rather than copying it.

- [ ] **Step 1: Write failing tests**, tag `[widget_config][import_layout]`:
  - `same grid copies placements one-to-one`: 12x8 to 12x8, every col/row/span identical, per-widget config identical.
  - `12x8 to 10x8 and to 16x10 port through layout_port`: compare to `port_legacy_layout` called directly on the same placements. Placements must equal its output, which proves no second porter.
  - `the import is the user's layout`: `!is_unedited_default()`, `!has_pending_anchors()`, and `reresolve_default_if_changed(cols, rows)` returns false afterwards.
  - `parked grids are cleared`: park a client layout on 16x10 first, import on 12x8, then `switch_to_grid(16,10)`. The result is a port of the import, not the old park.
  - `client screen-scoped defaults are appended unplaced`.
  - `import of only screen-scoped widgets is rejected`: hand-build a `ParsedLayout` whose pages hold only `clock`. Returns false; the layout, grid signature and parked grids are unchanged.
  - `spans larger than the grid and negative origins are clamped, never off-grid`: colspan 999, col -5. After import every placed entry lies inside 12x8.
  - `unknown widget id lands in retained, not in widgets`.
- [ ] **Step 2: Run** `make t F='[import_layout]'`. Expected: compile failure.
- [ ] **Step 3: Implement `import_layout`.**
- [ ] **Step 4: Run** `make t F='[import_layout]'` and `make t F='[widget_config]'`. Expected: PASS, with no regressions in the existing config cases.
- [ ] **Step 5: Commit** `git commit -m "feat(home): import a home layout authored for another grid" -- include/panel_widget_config.h src/system/panel_widget_config.cpp tests/unit/test_panel_widget_config.cpp`.

### Task 4: Locality verdict and `HomeLayoutSync` (publish + fetch)

**Files:**
- Modify: `include/printer_state.h`, `include/printer_network_state.h`, `src/printer/printer_network_state.cpp`, `src/printer/printer_state.cpp`, `src/application/moonraker_manager.cpp` (CONNECTED edge `#MoonrakerManager`, plus the disconnect and switch edges)
- Modify: `include/panel_widget_config.h`, `src/system/panel_widget_config.cpp` (save listener)
- Create: `include/home_layout_sync.h`, `src/application/home_layout_sync.cpp`; add the source to `app_srcs.txt`
- Test: `tests/unit/test_home_layout_sync.cpp`

**Interfaces:**
- Produces:
  - `enum class MoonrakerLocality { Unknown, Local, Remote };`
  - `MoonrakerLocality PrinterState::moonraker_locality() const;` (main thread)
  - `void PrinterState::clear_moonraker_locality();` It is called on every non-CONNECTED connection edge and on printer switch. `set_moonraker_is_remote(bool)` sets Local or Remote. `is_moonraker_remote()` keeps its meaning, so existing callers are untouched.
  - `static void PanelWidgetConfig::set_save_listener(std::function<void(const PanelWidgetConfig&)> fn);` It fires at the end of every successful `save()`, after the write.
  - `const std::string& PanelWidgetConfig::panel_id() const;` if absent; `nlohmann::json PanelWidgetConfig::published_pages() const;` (public wrapper over `serialize_pages()`).
  - `class HomeLayoutSync` (`::instance()`):
    - `void init(helix::IMoonrakerAPI& api);` installs the save listener.
    - `void on_connected();` If Local, publishes the current home layout. If Remote, fetches and holds the result tagged with the active printer id.
    - `void on_disconnected();` drops what it holds.
    - `std::optional<ParsedLayout> take_seed(const std::string& printer_id);` one-shot.
    - `void fetch_now(std::function<void(std::optional<ParsedLayout>)> done);` for the manual action. `done` runs on the main thread.
  - Publishing posts only when locality is `Local`, the panel id is `home`, and the payload differs from the last one this process posted (compare a hash of the payload without `saved_at`).

- [ ] **Step 1: Write failing tests**, tag `[home_layout_sync]`. Use the mock Moonraker API's database (`src/api/moonraker_client_mock_server.cpp` serves get/post/delete).
  - `publishes nothing while locality is unknown`
  - `publishes on save when Local, payload parses back`
  - `remote client never publishes`
  - `identical consecutive saves post once`
  - `on_connected as Remote fetches and take_seed returns it once`
  - `a reply for a printer no longer active is dropped`: switch the active printer id between request and reply; `take_seed` for either id is empty.
  - `missing key or unparseable value yields no seed and no error toast`
  - `locality: Unknown after clear, Local/Remote after set, cleared on disconnect edge`
- [ ] **Step 2: Run** `make t F='[home_layout_sync]'`. Expected: compile failure.
- [ ] **Step 3: Implement.** Call `init` where the other API-owning services start (find the `tool_state` database wiring and sit beside it). Call `on_connected` from the CONNECTED edge after `set_moonraker_is_remote`, and `on_disconnected` from the other edges.
- [ ] **Step 4: Run** `make t F='[home_layout_sync]'`, `make t F='[home_layout_share]'` and `python3 scripts/check_esp32_app_srcs.py`. Expected: PASS.
- [ ] **Step 5: Commit** with an explicit pathspec over every file above (`git add -N` new files first): `"feat(home): publish the home layout from the printer's own screen, fetch it on remote clients"`.

### Task 5: Seed on add, and late arrival

**Files:**
- Modify: `src/ui/panel_widget_manager.cpp` (the `has_pending_anchors()` branch of populate, `#PanelWidgetManager::populate_widgets`)
- Modify: `src/application/home_layout_sync.cpp` (the late-arrival path)
- Test: `tests/unit/test_home_layout_sync.cpp`

**Interfaces:**
- Consumes: `take_seed`, `import_layout`, `ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Home layout copied from the printer's screen"), 3000)`.
- Behaviour:
  - In the `content_box_measured && printer_ready` arm of `has_pending_anchors()`, `take_seed(active_printer_id)` is checked first. If it yields a layout and `import_layout(*seed, cols, rows)` succeeds, `apply_pending_anchors` is skipped and the toast shows. Otherwise the existing path runs unchanged.
  - When a fetch reply lands after the default was applied: if `is_unedited_default()` and the stamped grid is known, import against `grid_signature()`'s dims, then `notify_config_changed("home")` and toast. If the user edited it, drop the reply.

- [ ] **Step 1: Write failing tests**, same file, tag `[home_layout_sync][seed]`:
  - `seed present at the READY populate replaces the default`
  - `no seed: default anchors applied as before`: the layout equals a run with sync disabled.
  - `late reply replaces an unedited default`
  - `late reply leaves an edited layout alone`: `set_enabled_by_id` on one widget first.
  - `invalid seed falls back to defaults`
- [ ] **Step 2: Run** `make t F='[seed]'`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `make t F='[home_layout_sync]'` and `make t F='[widget_config]'`. Expected: PASS.
- [ ] **Step 5: Mock check, headless.** Run `HELIX_HEADLESS=1 SDL_VIDEODRIVER=dummy` with a pinned socket and config dir (CLAUDE.md Quick Start). First run `--test` without `HELIX_MOCK_REMOTE_PRINTER`, rearrange home via `ctl`, and confirm `[HomeLayoutSync]` logs a publish. Then run with `HELIX_MOCK_REMOTE_PRINTER=1` and a fresh config dir and add the mock printer. Confirm the seed log line and the toast, and `ctl ls` shows the arrangement. Note: the mock database must persist across the two runs, or be pre-seeded; if it does not, pre-seed it through the mock's database post in the second run and say so in the report.
- [ ] **Step 6: Commit** `"feat(home): seed a new printer's home layout from its own screen"`.

### Task 6: "Copy layout from printer" in the catalog

**Files:**
- Modify: `src/ui/ui_widget_catalog_overlay.cpp` (beside `on_catalog_reset`), `ui_xml/widget_catalog_overlay.xml`
- Modify: translation YAMLs + `ui_xml/translations/*.xml` (generated)
- Test: `tests/unit/test_widget_catalog_placement.cpp` (or a new `test_widget_catalog_copy_layout.cpp` if that file has no fixture for it)

**Interfaces:**
- Consumes: `fetch_now`, `import_layout`, `modal_confirm`.
- Behaviour:
  - The action is visible only when `moonraker_locality() == Remote`, bound to a subject; no C++ visibility toggling (`.claude/rules/declarative-ui.md`).
  - Tapping it fetches. With nothing published, toast `lv_tr("This printer's screen has not shared a layout")`.
  - Otherwise `modal_confirm(lv_tr("Copy Layout From Printer?"), lv_tr("This replaces this screen's home layout for this printer with the one on the printer's own screen."), ModalSeverity::Warning, lv_tr("Copy"), ...)`. On confirm: `import_layout`, `close_catalog()`, `notify_config_changed("home")`, matching reset.
- [ ] **Step 1: Write failing tests**, tag `[widget_catalog][copy_layout]`:
  - `action hidden when Local or Unknown`
  - `confirm imports and notifies`
  - `nothing published: no modal, layout unchanged`
- [ ] **Step 2: Run** `make t F='[copy_layout]'`. Expected: FAIL.
- [ ] **Step 3: Implement**, then `make translation-sync && make translations`.
- [ ] **Step 4: Run** `make t F='[widget_catalog]'`. Then take a headless screenshot of the catalog with `HELIX_MOCK_REMOTE_PRINTER=1` at 800x480 and at the smallest breakpoint, and confirm the header still fits (`ctl geom` on the header row).
- [ ] **Step 5: Commit** with explicit pathspecs, including the translation artefacts: `"feat(home): copy a printer's home layout from the widget catalog"`.

### Task 7: Documentation, then the gates

**Files:**
- Modify: `docs/devel/LAYOUT_SYSTEM.md`. New section "Layouts shared by the printer's own screen": the payload, the namespace and key, publish and seed timing, the locality gate, `import_layout`'s guarantees, and the after-seed ownership rule (Reset gives the generator default).
- Modify: `docs/devel/PANEL_WIDGET_GUIDE.md`. Registry-def checklist: choose `scope`, and when a widget is `Screen`.
- Modify: `docs/devel/MULTI_PRINTER.md`. Adding a printer: the seed step.
- Modify: `docs/devel/MOONRAKER_ARCHITECTURE.md`, if it lists the app's database keys: add `helix-screen/home_layout`.
- Modify: `docs/user/guide/home-panel.md`. A paragraph on the copied layout and the "Copy layout from printer" action, with no source references (`docs/user/CLAUDE.md` style).
- Modify: `docs/devel/CHANGELOG_1_1_DRAFT.md`. One user-facing line.
- Delete: `docs/devel/plans/2026-10-16-remote-home-layout-seed-design.md` and this plan, in the final docs commit (`docs/CLAUDE.md` plan lifecycle).

- [ ] **Step 1: Write the doc changes.** Cite code as `path#Symbol`, never line numbers. Run `make check-doc-anchors` and confirm it resolves every new anchor.
- [ ] **Step 2: Commit** `"docs(home): layouts shared by the printer's own screen"`, deleting both plan files in the same commit.
- [ ] **Step 3: Mutation** on the branch's hunks: `make mutate-diff`. Every surviving mutant in `home_layout_share.cpp`, `import_layout` and the publish gate gets a test or a written reason.
- [ ] **Step 4: Gate.** Run `make land-gate` (batch with peers per CLAUDE.md). Expected: an all-PASS table.
