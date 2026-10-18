# Plugin Dialogs Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Plugins open dialog-sized surfaces that never cover an app dialog, report exactly how they ended, and size responsively with their own breakpoint constants.

**Architecture:** A pure decision function plus a per-printer `PluginPresenter` (owned by `PluginHost`) queues every plugin surface request (dialog, overlay, confirm) until no app cover is up, then presents it through `PluginDialog` (a `Modal` subclass, so it is a real `ModalStack` entry), the existing `PluginOverlayHost`, or `modal_confirm`. `ModalStack` learns one generic thing, a string tag per entry, and one app-wide fix: overlays never land above visible modals. Plugin `<consts>` resolve through the theme's existing px ladder, split so it can take a plugin's tables.

**Tech Stack:** C++17, LVGL 9.5, helix-xml, Lua 5.4 plugin runtime, Catch2.

**Spec:** `docs/devel/plans/2026-10-17-plugin-dialogs-design.md`

Interfaces below were compiled with `scripts/syntax_check.py` against the real `ui_modal.h` (2026-10-17).

## Global Constraints

- MAJOR work: worktree via `scripts/setup-worktree.sh feature/plugin-dialogs`; never `git add -A`; commit with explicit pathspecs.
- Verification cadence (root CLAUDE.md): while building, `make t F='[tags the task touches]'` only, and each new test is proven red once by breaking the line it pins. No sweeps, gates, mutation or reviews per task. The finish (once): one review, `make mutate-diff MUTATE_ARGS="--tests '[plugin],[modal]'"`, `make land-gate`.
- Main thread only for everything here; plugin callbacks run through `LuaRuntime::invoke` (fault quota applies).
- Plugin-owned names are `<id>__<name>` (`is_owned_name`); every new plugin-facing name follows it.
- `how` values, verbatim: `"user"`, `"plugin"`, `"app"`, `"cancelled"`.
- No vendor names, no `std::regex`, spdlog only, SPDX headers, no comment archaeology.
- New `src/plugin/*` files go into `firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt` (plugins are not in the ESP32 cut); `src/ui/ui_modal.cpp` and `src/ui/ui_nav_manager.cpp` ARE compiled for ESP32, so changes there use no exceptions/RTTI.
- Headless only: `HELIX_HEADLESS=1 SDL_VIDEODRIVER=dummy` for any app run or screenshot.

## Deviations from the design, settled here

- A printer switch destroys `PluginHost` and every plugin runtime with it (`src/application/printer_session.cpp`), so a switch is an unload: no callbacks. `"app"` covers remote/`External` sweeps and hot reload.
- Hot reload (`Modal::rebuild_top`) closes the top modal with `ModalCloseReason::HotReload`; it does not keep showing. A plugin dialog closed that way reports `"app"` (dev builds only).
- The header close button in a plugin dialog is wired by the plugin to `plugin_event` like any button, so it reports `"plugin"`. `"user"` is backdrop tap and back/ESC.
- Overlays: user back and plugin `close()` share navigation's close path today. The overlay host records which one asked; nav-initiated closes are `"user"`.
- The printer-switch interstitial is not a blocker: the plugin host it would block is being destroyed.

## Review Focus

1. A plugin dialog presented while the app is asleep with auto-lock on: wake engages the lock, so the request must stay queued, not open behind the lock (test in Task 4).
2. A plugin calling `h.close()` inside its own `on_open`, or opening a second surface from `on_close`: callbacks must not re-enter the queue mid-iteration (test in Task 4).
3. A plugin unloaded while its dialog is mid exit-animation: no callback, no use of freed host state (test in Task 3).
4. Two plugins each with a queued dialog when the app modal closes: only the first presents; the second presents after the first closes (test in Task 4).
5. `modal_button_row` gaining `*_user_data` props must not change any existing app callback that reads `lv_event_get_user_data` (check in Task 6).

---

### Task 1: Overlays never cover visible modals

**Files:**
- Modify: `include/ui_modal.h` (ModalStack public API), `src/ui/ui_modal.cpp` (`raise_top_modal_to_foreground` becomes a call to the new method)
- Modify: `src/ui/ui_nav_manager.cpp#NavigationManager::push_overlay` (inside the deferred lambda, right after `helix::ui::bring_to_front(overlay_panel)`)
- Test: `tests/unit/test_modal_layering.cpp` (new), tag `[modal][layering]`

**Interfaces:**
- Produces: `void ModalStack::raise_visible_to_foreground();` brings every non-exiting backdrop to the front, bottom to top, preserving stack order.

- [ ] **Step 1: Write the failing test** `"push_overlay keeps a visible modal above the new overlay"` (`LVGLUITestFixture`, animations off, panels seeded as `test_plugin_overlay_host.cpp`'s `OverlayFx` does): show `modal_alert(...)`, register and `push_overlay` a fresh overlay, drain + `process_lvgl(500)`; `CHECK(lv_obj_get_index(backdrop) > lv_obj_get_index(overlay))` on `lv_screen_active()`. Second case: two stacked modals keep their relative order after the push.
- [ ] **Step 2: Run** `make t F='[modal][layering]'`. Expected: FAIL (overlay index above backdrop). If it passes, stop and report: the design's premise is wrong.
- [ ] **Step 3: Implement** `raise_visible_to_foreground()` and call it after `bring_to_front(overlay_panel)`; `raise_top_modal_to_foreground` delegates to it.
- [ ] **Step 4: Run** `make t F='[modal][layering],[modal],[navigation]'`. Expected: PASS.
- [ ] **Step 5: Commit** `fix(nav): an overlay pushed under a visible modal stays below it`.

### Task 2: ModalStack tags and the app-cover predicate

**Files:**
- Modify: `include/ui_modal.h`, `src/ui/ui_modal.cpp` (`ModalEntry`, `ModalStack`)
- Create: `include/ui_presentation_gate.h`, `src/ui/ui_presentation_gate.cpp`
- Test: `tests/unit/test_presentation_gate.cpp`, tag `[modal][presentation]`

**Interfaces:**
- Produces:
  - `ModalEntry::tag` (`std::string`, empty for app modals).
  - `void ModalStack::set_tag(lv_obj_t* backdrop, std::string tag);`
  - `bool ModalStack::has_visible_untagged() const;` (non-exiting entry with empty tag)
  - `std::string ModalStack::top_visible_tag() const;` (tag of the top non-exiting tagged entry, "" if none)
  - In `ui_presentation_gate.h`, namespace `helix::ui`:
    ```cpp
    struct AppCovers {
        bool app_modal = false;      // ModalStack::has_visible_untagged()
        bool external_cover = false; // ModalStack external cover count > 0 (PIN pad)
        bool lock_screen = false;    // LockScreenOverlay::instance().is_visible()
        bool aborting = false;       // AbortManager modal visible (state not IDLE/COMPLETE)
        bool busy = false;           // BusyOverlay::is_visible()
    };
    bool app_blocked(const AppCovers& c);   // pure: any field true
    AppCovers current_app_covers();          // reads the live sources above
    ```
  - `bool ModalStack::has_external_cover() const;`

- [ ] **Step 1: Write failing tests**: `"app_blocked: any cover blocks, none clears"` (each field alone true -> true; all false -> false); `"has_visible_untagged ignores tagged and exiting entries"` (`LVGLTestFixture`: show two `Modal::show("…")` dialogs, tag one, mark the untagged one exiting -> false; untagged visible -> true); `"top_visible_tag names the top tagged entry"`.
- [ ] **Step 2: Run** `make t F='[modal][presentation]'`. Expected: compile failure, then FAIL.
- [ ] **Step 3: Implement.** `current_app_covers()` takes each source's existing query (agent-verified: `LockScreenOverlay::is_visible`, `AbortManager::is_idle`/`get_state`, `BusyOverlay::is_visible`). The crash report modal is a ModalStack entry and needs nothing.
- [ ] **Step 4: Run** `make t F='[modal][presentation],[modal]'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(modal): tag stack entries and name every app cover in one predicate`.

### Task 3: PluginDialog, a Modal that reports how it closed

**Files:**
- Create: `include/plugin_dialog.h`, `src/plugin/plugin_dialog.cpp`
- Test: `tests/unit/test_plugin_dialog.cpp`, tag `[plugin][dialog]`

**Interfaces:**
- Consumes: `ModalStack::set_tag` (Task 2), `Modal::show_owned`.
- Produces:
  ```cpp
  namespace helix::plugin {
  enum class CloseHow { User, Plugin, App, Cancelled };
  const char* close_how_name(CloseHow how); // "user" | "plugin" | "app" | "cancelled"
  CloseHow close_how_for(ModalCloseReason reason); // pure; ModalCloseReason is global-namespace
  class PluginDialog : public Modal {
    public:
      PluginDialog(std::string plugin_id, std::string component,
                   std::vector<std::pair<std::string, std::string>> attrs,
                   std::function<void(CloseHow)> on_closed);
      const char* get_name() const override;       // "PluginDialog"
      const char* component_name() const override; // the plugin component
      static lv_obj_t* present(std::unique_ptr<PluginDialog> d); // show_owned + set_tag(plugin_id); nullptr on failure
      void close_by_plugin();                     // hide(Programmatic)
      void forget();                              // drop on_closed without running it (unload)
    protected:
      void on_hide() override;                    // maps close_reason_ and runs on_closed once
  };
  }
  ```
  `close_how_for`: `BackdropTap`, `EscKey`, `ButtonPress` -> `User`; `Programmatic` -> `Plugin`; `External`, `HotReload` -> `App`. `on_closed` runs from `on_hide()`, exactly once. `ModalStack::clear()` (teardown) runs nothing.

- [ ] **Step 1: Write failing tests** (`LVGLUITestFixture`, a test component registered from a string): `"close_how_for maps every ModalCloseReason"` (table over all six enumerators); `"backdrop tap reports user"`; `"close_by_plugin reports plugin"`; `"an External hide reports app"`; `"forget then hide runs nothing"`; `"present tags the entry with the plugin id"` (`top_visible_tag() == id`); `"on_closed runs exactly once"` (hide twice).
- [ ] **Step 2: Run** `make t F='[plugin][dialog]'`. Expected: compile failure, then FAIL.
- [ ] **Step 3: Implement**; model the owned-instance flow on `ConfirmationModal` in `src/ui/ui_modal.cpp`. Add `src/plugin/plugin_dialog.cpp` to `app_srcs_excluded.txt`.
- [ ] **Step 4: Run** `make t F='[plugin][dialog]'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(plugins): PluginDialog modal reports how it closed`.

### Task 4: PluginPresenter, the queue

**Files:**
- Create: `include/plugin_presenter.h`, `src/plugin/plugin_presenter.cpp`
- Modify: `include/plugin_host.h`, `src/plugin/plugin_host.cpp` (own a `PluginPresenter presenter_` beside `overlays_`; `unload(id)` calls `presenter_.drop_plugin(id)` before `overlays_.close_all(id)`)
- Test: `tests/unit/test_plugin_presenter.cpp`, tag `[plugin][presenter]`

**Interfaces:**
- Consumes: `app_blocked`, `current_app_covers` (Task 2), `ModalStack::top_visible_tag` (Task 2), `PluginDialog` (Task 3), `DisplayManager::wake_display()`.
- Produces:
  ```cpp
  namespace helix::plugin {
  enum class SurfaceKind { Dialog, Overlay, Confirm };
  enum class Decision { Present, Wait };
  // Pure. `showing_dialog_owner` is the plugin whose dialog is on screen ("" none).
  // `earlier_waiting` is true when an earlier request from another plugin is queued.
  Decision decide(bool app_blocked, const std::string& showing_dialog_owner,
                  const std::string& requester, bool earlier_waiting);
  class PluginPresenter {
    public:
      using PresentFn = std::function<bool()>;   // shows it; false = could not
      using CancelFn = std::function<void()>;    // runs on_close("cancelled")
      int request(const std::string& plugin_id, SurfaceKind kind, PresentFn present,
                  CancelFn cancelled);           // returns a ticket, > 0
      bool withdraw(int ticket);                 // true if it was still queued (runs cancelled)
      bool is_queued(int ticket) const;
      size_t queued_dialogs(const std::string& plugin_id) const;
      void drop_plugin(const std::string& plugin_id); // silent: no callbacks
      void pump();                               // present from the head while allowed
  };
  }
  ```
  `decide`: `app_blocked` -> Wait; `showing_dialog_owner == requester` -> Present; `showing_dialog_owner` non-empty -> Wait; `earlier_waiting` -> Wait; else Present.
  `pump()` presents from the head, stopping after it presents a Dialog; before presenting, it calls `wake_display()` and re-reads `current_app_covers()` (auto-lock may have engaged) and leaves the request queued if blocked. While anything is queued, an `lv_timer` (200 ms) calls `pump()`; it is deleted when the queue empties and cancelled in the destructor and `drop_plugin` path (threading rule 5). Polling, not hooks, because the covers live in five unrelated classes and only the queue cares; the timer exists only while something waits.
  Re-entrancy: `pump()` copies the head out before calling `present`, and a guard flag makes a nested `pump()` (from a callback) a no-op that re-arms the timer.

- [ ] **Step 1: Write failing tests.** Pure: `"decide: blocked waits; own dialog presents; other dialog waits; earlier waiter waits; clear presents"`. Live (`LVGLUITestFixture`): `"a dialog requested under an app modal presents after it closes"`; `"two plugins' dialogs present one at a time, FIFO"`; `"a plugin's own confirm presents over its own dialog"`; `"withdraw on a queued request runs cancelled, never present"`; `"drop_plugin runs nothing"`; `"wake that engages auto-lock keeps the request queued"` (fake lock: show `LockScreenOverlay` from the wake path via the existing test seam, or set `lock_screen` through a test hook on `current_app_covers`; pick the seam that exists); `"a callback that requests again during pump does not re-enter"`; `"unload while the dialog is exiting runs no callback"`.
- [ ] **Step 2: Run** `make t F='[plugin][presenter]'`. Expected: compile failure, then FAIL.
- [ ] **Step 3: Implement**; add the new file to `app_srcs_excluded.txt`.
- [ ] **Step 4: Run** `make t F='[plugin][presenter],[plugin][host]'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(plugins): queue plugin surfaces behind app covers`.

### Task 5: Lua API: helix.ui.dialog, overlay on_open/how, queued confirm

**Files:**
- Modify: `src/plugin/lua_bind_ui.cpp` (`ui_overlay`, `ui_confirm`, new `ui_dialog`, `UiState`), `include/plugin_overlay_host.h` / `src/plugin/plugin_overlay_host.cpp` (record who closed: `close(handle)` sets `Record::closed_by_plugin`; `on_closed` becomes `std::function<void(CloseHow)>`), `PluginUi` struct (add `dialog`, `withdraw` entries), `src/plugin/plugin_host.cpp#PluginHost::load` (wire them to `presenter_`)
- Test: `tests/unit/test_lua_bindings_ui.cpp` (extend), tag `[plugin][bindings][ui]`

**Interfaces:**
- Consumes: Tasks 3 and 4.
- Produces (Lua):
  - `helix.ui.dialog(component[, attrs])` -> handle `{ close = fn, is_open = fn }`; attrs: string props, `on_open`, `on_close(how)`. Errors: unloading; component not owned; non-string prop; non-function callback; a second dialog shown or queued for this plugin: `"helix.ui.dialog: at most 1 dialog per plugin"`.
  - `helix.ui.overlay(component[, attrs])`: same `on_open` / `on_close(how)`; re-request of a component already open or queued returns the same handle and changes nothing.
  - `helix.ui.confirm(...)`: unchanged signature, presented through the queue.
  - `h.close()`: queued -> `on_close("cancelled")`; shown -> `"plugin"`. `h.is_open()` true only while on screen. Both tolerate `h:close()`.

- [ ] **Step 1: Write failing tests**: `"dialog: queued under an app modal, on_open after it closes"`; `"dialog: close while queued -> cancelled, no on_open"`; `"dialog: close while shown -> plugin"`; `"dialog: backdrop tap -> user"`; `"dialog: second dialog is an error naming the limit"`; `"dialog: unowned component is an error"`; `"overlay: on_open runs; plugin close reports plugin, nav back reports user"`; `"overlay: re-request while queued returns the same handle"`; `"confirm: waits behind an app modal"`; `"unload: shown and queued surfaces close, no callbacks"`.
- [ ] **Step 2: Run** `make t F='[plugin][bindings][ui]'`. Expected: FAIL.
- [ ] **Step 3: Implement.** Refs taken only after every error path (as `ui_overlay` does today) and unref'd after the terminal callback.
- [ ] **Step 4: Run** `make t F='[plugin]'`. Expected: PASS (existing overlay and confirm tests stay green unchanged).
- [ ] **Step 5: Commit** `feat(plugins): helix.ui.dialog, and every plugin surface reports open and how it closed`.

### Task 6: Dialog chrome in plugin XML

**Files:**
- Modify: `ui_xml/modal_button_row.xml` (props `primary_user_data`, `secondary_user_data`, `tertiary_user_data`, `quaternary_user_data`, default `""`, passed as `user_data="$…_user_data"` on each `event_cb`), `ui_xml/modal_header.xml` (`close_user_data`)
- Modify: `src/plugin/plugin_xml_policy.cpp` (`is_allowlisted_app_component` += `ui_dialog`, `modal_header`, `modal_button_row`; object-name rule widened from `name` to `name` and `*_name` except on `prop`/`style`/`remove_style*`/`bind_style*`; on `modal_button_row`/`modal_header` every `*_name` the element exposes must be present and owned, so no default `btn_primary` survives in plugin XML; `*_user_data` treated as a target like `user_data`)
- Test: `tests/unit/test_plugin_xml_policy.cpp` (extend), `[plugin][xml_policy]`; `tests/unit/test_lua_bindings_ui.cpp` one wiring case

**Interfaces:**
- Produces: plugin XML may write `<modal_button_row primary_callback="plugin_event" primary_user_data="<id>__accept" primary_name="<id>__ok" secondary_name="<id>__no" tertiary_name="<id>__t" quaternary_name="<id>__q" .../>`.

- [ ] **Step 1: Check the Review Focus 5 hazard first.** Grep every `lv_event_get_user_data` in callbacks registered for `modal_button_row`/`modal_header` events (`lv_xml_register_event_cb` names used as `*_callback` values in `ui_xml/`). If any reads it, or helix-xml turns an empty `user_data=""` into a non-NULL pointer that any of them would mis-read, stop and report before changing the XML.
- [ ] **Step 2: Write failing tests**: policy `"modal_button_row with owned names and plugin_event passes"`, `"a missing or unowned *_name is rejected"`, `"a non-plugin_event callback is rejected"`, `"*_user_data must be owned"`; binding `"a plugin dialog's primary button reaches the plugin handler"` (dialog from a fixture component, click `<id>__ok`, handler ran with its arg).
- [ ] **Step 3: Run** `make t F='[plugin][xml_policy],[plugin][bindings][ui]'`. Expected: FAIL.
- [ ] **Step 4: Implement.**
- [ ] **Step 5: Run** `make t F='[plugin][xml_policy],[plugin][bindings][ui],[modal]'` and `make test-xml` (lint/schema for the changed app XML). Expected: PASS.
- [ ] **Step 6: Commit** `feat(plugins): dialog chrome components in plugin XML`.

### Task 7: Responsive plugin constants and breakpoint subjects

**Files:**
- Modify: `src/ui/theme_responsive.cpp` (split `theme_manager_resolve_px_tokens` so the tier pick runs over supplied tables), `include/theme_manager.h`
- Create: `include/plugin_consts.h`, `src/plugin/plugin_consts.cpp`
- Modify: `src/plugin/plugin_xml_policy.cpp` (allow `consts`, and `px`/`int`/`percentage`/`color`/`string` as children of `consts` only; their `name` must be owned), `src/plugin/plugin_host.cpp#PluginHost::load` (resolve and register after the policy pass; fail the load with the validation message), the application resize path (`application.cpp` resize callback and rotation probe: call `PluginHost::refresh_layout_constants()` after `theme_manager_refresh_layout_constants`)
- Modify: policy subject rule: `ui_breakpoint`, `ui_breakpoint_v`, `ui_is_portrait` allowed as `subject=` on `bind_flag_if_*` / `bind_style_if_*`, and as identifiers in `cond`; still rejected everywhere else
- Test: `tests/unit/test_plugin_consts.cpp` `[plugin][consts]`; `tests/unit/test_plugin_xml_policy.cpp`

**Interfaces:**
- Produces:
  ```cpp
  // theme_manager.h
  using PxTierTables = std::array<std::unordered_map<std::string, std::string>, 7>; // index = tier_for_suffix
  std::unordered_map<std::string, std::string>
  theme_manager_pick_px_tiers(const PxTierTables& tables, const char* narrow_suffix,
                              const char* vertical_suffix, bool app_tokens);
  // app_tokens=false: narrow axis only, no nav_width / VERTICAL_AXIS_TOKENS special cases
  // plugin_consts.h, namespace helix::plugin
  struct PluginConsts {
      PxTierTables px;                                    // suffixed px variants
      std::vector<std::pair<std::string, std::string>> plain; // un-suffixed consts of any type
  };
  PluginConsts collect_plugin_consts(const std::vector<std::string>& xml_files);
  std::vector<std::string> missing_tier_errors(const PxTierTables& px); // "<name>: missing _medium" ...
  void register_plugin_consts(const PluginConsts& c, lv_display_t* display); // lv_xml_set_const(globals, ...) with DPI scaling
  ```
  `theme_manager_resolve_px_tokens` keeps its signature and calls `theme_manager_pick_px_tiers(..., true)`; the ESP32 table fast path is untouched.

- [ ] **Step 1: Write failing tests**: `"pick_px_tiers: app tokens unchanged"` (resolve the real ui_xml tables both ways, maps equal); `"plugin px triplet resolves per tier with the app's fallbacks"` (micro->tiny->small, xxlarge->xlarge->large); `"plugin px values are DPI scaled like app tokens"`; `"a partial triplet is a load error naming the constant and the tier"`; `"un-suffixed plugin consts register as given"`; `"refresh re-resolves plugin constants on a breakpoint change"`; `"a plugin component reads its own constant from another of its files"`; policy: `"consts with owned names pass, unowned rejected, px outside consts rejected"`, `"ui_breakpoint allowed in bind_flag_if_eq, rejected in bind_value and bind_text"`.
- [ ] **Step 2: Run** `make t F='[plugin][consts],[plugin][xml_policy],[theme]'`. Expected: FAIL.
- [ ] **Step 3: Implement.** Add new files to `app_srcs_excluded.txt`.
- [ ] **Step 4: Run** the same tags. Expected: PASS, theme tests unchanged.
- [ ] **Step 5: Commit** `feat(plugins): responsive plugin constants through the theme's px ladder`.

### Task 8: calibration-bridge uses a dialog for the answer

**Files:**
- Modify: `examples/plugins/calibration-bridge/main.lua`, `ui/` (new `calibration-bridge__result.xml` dialog: `ui_dialog` + `modal_header` + the picker + `modal_button_row`, sized with a plugin px triplet), `README.md`, `i18n/de.xml` (new strings)
- Test: `tests/unit/test_example_plugin_calibration_bridge.cpp` `[plugin][example]`

**Interfaces:**
- Consumes: Lua API (Task 5), XML chrome (Task 6), consts (Task 7).
- Behaviour: the overlay keeps the flow (context, Start, progress). On `print_state == "complete"` for its own print it opens `helix.ui.dialog("calibration-bridge__result", {opts=..., on_close=...})`; Accept posts the result and closes (`"plugin"`); `on_close("user")` leaves state "asking" with the overlay's "Pick result" action still available. Starting a new calibration while the result dialog is queued withdraws it.

- [ ] **Step 1: Write failing tests**: `"completion opens the result dialog behind the stock completion modal"` (completion modal shown first; dialog appears after it closes); `"accept posts the choice and the dialog reports plugin"`; `"dismissing the dialog keeps the result askable"`; `"a new calibration withdraws a queued result dialog"`; existing 26 cases adjusted only where the picker moved.
- [ ] **Step 2: Run** `make t F='[plugin][example]'`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `make t F='[plugin][example]'`. Expected: PASS.
- [ ] **Step 5: Screenshot** the result dialog at 800x480 and 480x320 (headless, pinned socket), for the finish.
- [ ] **Step 6: Commit** `feat(examples): calibration-bridge asks for the result in a dialog`.

### Task 9: Docs

**Files:**
- Modify: `docs/devel/PLUGIN_DEVELOPMENT.md` (§7: allowlist additions, `*_name`/`*_user_data` rules, new "Responsive sizing" subsection: app tokens, `<consts>`, px suffix ladder, load error, breakpoint subjects in conditional binds; §8 `helix.ui`: `dialog`, `on_open`/`on_close(how)` for overlay and dialog, the four `how` values, queued confirm, the presentation rule paragraph, waking and lock screen; §12 worked example pointer)
- Modify: `docs/devel/PLUGIN_TUTORIAL.md` (only steps whose behaviour changed), `docs/devel/MODAL_SYSTEM.md` (overlay-vs-modal layering rule; `ModalStack` tags; one paragraph pointing at the plugin doc for queued plugin surfaces), `docs/devel/UI_CONTRIBUTOR_GUIDE.md` §2 (the px ladder also serves plugin constants), `examples/plugins/calibration-bridge/README.md` (Task 8 already), any `docs/user/` page describing plugin UI behaviour (grep `plugin` under `docs/user/`)
- Delete: `docs/devel/plans/2026-10-17-plugin-dialogs-design.md` and this plan, in the landing change.

- [ ] **Step 1:** Write the doc changes; code citations use `path#scope`, never line numbers.
- [ ] **Step 2:** `make check-doc-anchors` (advisory) and fix any new anchor it cannot resolve.
- [ ] **Step 3: Commit** `docs(plugins): dialogs, presentation order, responsive plugin XML`.

### Finish (once, orchestrator)

- [ ] One review of the whole branch (Opus), rubric `docs/devel/REVIEW_RUBRIC.md`.
- [ ] `make mutate-diff MUTATE_ARGS="--tests '[plugin],[modal],[theme]'"`.
- [ ] One screenshot per changed screen (Task 8's dialog).
- [ ] `make land-gate`; merge to main per Sharing This Tree; push; tear down the worktree.
- [ ] Tell compulsivejohnny the API shape (Preston relays).
