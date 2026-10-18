# Plugin dialogs, presentation order, and responsive plugin XML

Status: design, awaiting approval. Deleted when the work ships (docs/CLAUDE.md).

## Why

The first outside plugin author (orcacal, a calibration plugin that talks to OrcaSlicer)
needs to ask the user a question when a calibration print it started finishes: "which
sample came out best?", sent back to Orca. Today the plugin can watch `print_state` and call
`helix.ui.overlay()`, but that opens at the same moment HelixScreen shows its own
print-complete modal, and from reading the code (not yet confirmed on screen) the overlay
lands on top of that modal: modals and overlays are both children of the active screen,
z-order is creation order, and `NavigationManager::push_overlay()` never raises the modal
stack back above a new overlay. The plugin author's AI noticed the collision risk and asked
for a way to wait.

The fix is a presentation rule the app enforces, not a flag each plugin has to remember,
plus a dialog-sized surface so a plugin question looks like every other HelixScreen
question. While we are in plugin XML, plugin components also get responsive sizing on equal
footing with app components.

## Goals

1. A plugin can open one of its own XML components as a **dialog** (centred card with
   backdrop, sized like app dialogs), not only full screen.
2. **A plugin surface never covers an app dialog.** Plugin dialogs, plugin overlays and
   plugin confirms wait while any app dialog is showing and appear when the screen is
   clear, in the order they were requested.
3. **App dialogs always win.** An app dialog (errors and warnings especially, but every app
   dialog) that arrives while a plugin surface is showing opens on top of it. The plugin
   surface is still there underneath when the app dialog closes.
4. **Unattended prompts wait.** Presenting a plugin dialog wakes the screen (screensaver
   dismissed, display woken); it stays until answered or closed by the plugin.
5. The plugin always knows where its request stands: it hears when the surface actually
   appears and exactly one terminal callback saying how it ended, including "never shown".
6. Plugin components size responsively: they can use app tokens (already true), declare
   their own constants, give those constants breakpoint variants that resolve exactly the
   way app tokens do, and read the current breakpoint in conditional bindings.
7. Docs and the example plugin are updated in the same change (below).

## Non-goals

- Plugin entries in app context menus. Separate design, next.
- Queueing app dialogs behind each other. App code is unchanged: app dialogs keep stacking
  as today. Only plugin surfaces wait.
- Persisting a queued plugin request across an app restart or plugin reload. The plugin
  re-requests if it still cares.
- A built-in "pick one of N" widget. The plugin brings its own component.
- Plugin control over z-order or priority. There is one rule and the app owns it.

## Anti-goals (fails even if it technically works)

- A plugin surface appearing over an error, warning, runout or print-complete dialog.
- A plugin able to block or delay an app dialog.
- A plugin dialog that looks or behaves unlike an app dialog (different card, padding,
  button row, dismissal).
- A plugin that has to poll or guess whether its dialog showed.
- A responsive plugin constant that silently resolves to nothing at some breakpoint (the
  `#1211` failure shape for app tokens).

## The presentation rule (decided)

- **App blockers** are every app surface that covers the screen for the user's attention:
  any visible, non-exiting `ModalStack` entry that is not a plugin surface, the external
  cover count (`ModalStack::add_external_cover`, the PIN pad), and the app's own
  `lv_layer_top()` covers: lock screen, AbortManager dialog, crash-report modal, busy
  overlay, printer-switch interstitial. The builder enumerates these with a test per
  surface; a new full-screen app cover added later must register as a blocker (one
  predicate, one place).
- **Request from plugin P, decided per request:**
  - an app blocker is up: queue (FIFO across all plugins);
  - another plugin's dialog is showing: queue (plugin dialogs from different plugins show
    one at a time);
  - P's own dialog is showing: present immediately, on top. A plugin talking to the user
    inside its own dialog (a Cancel button asking "are you sure?") must never wait on
    itself, or a plugin deadlocks;
  - otherwise: present immediately.
- Strict FIFO: a later request never jumps a queued earlier one from another plugin, so an
  overlay queued behind another plugin's waiting dialog waits too.
- When the last blocker closes, the queue presents from the head, stopping as soon as it
  presents a dialog (that dialog now blocks other plugins). Overlays and confirms present
  and the queue continues.
- Presenting wakes the display first, then re-checks blockers: if waking engaged
  auto-lock (`wake_should_auto_lock`), the request stays queued until unlock.
- App dialogs are never queued. One arriving over a plugin surface stacks on top as any
  modal does today; when it closes, `raise_top_modal_to_foreground` already restores the
  plugin dialog beneath.
- **Layering fix (app-wide):** pushing an overlay while modals are visible must keep the
  modals above the new overlay. This is a correctness fix for app overlays too, not just
  plugins. The builder confirms the bug with a failing test before fixing it.

## Lua API (decided shape; exact names are the builder's within this shape)

```lua
local h = helix.ui.dialog("orcacal__temperature_result", {
    best = "3",                       -- string props, as helix.ui.overlay attrs today
    on_open  = function() end,        -- the surface is actually on screen
    on_close = function(how) end,     -- exactly once: "user" | "plugin" | "app" | "cancelled"
})
h.close()      -- closes it if shown ("plugin"), withdraws it if queued ("cancelled")
h.is_open()    -- true only while on screen
```

- `helix.ui.dialog(component, attrs)` is new. `helix.ui.overlay(component, attrs)` keeps
  its signature and gains the same `on_open` and the `how` argument to `on_close`
  (backward compatible: existing handlers ignore the argument).
- `how`:
  - `"user"`: the user dismissed it without the plugin acting: backdrop tap, back/ESC, the
    header's close button.
  - `"plugin"`: the plugin called `close()` after it appeared. An answer button in the
    plugin's own component goes through `plugin_event` to Lua, and the handler records the
    answer and calls `h.close()`, so a real answer always reports `"plugin"`. The plugin
    knows what was picked; `how` only says who ended it.
  - `"app"`: the app closed it: remote `ctl` sweep / `ModalCloseReason::External`, soft
    restart, printer switch, or a queued request withdrawn by app teardown while the plugin
    runtime lives on. A hot-reload rebuild of the dialog is not a close (it keeps showing).
  - `"cancelled"`: withdrawn by the plugin's own `close()` before it ever appeared.
- Plugin unload closes and withdraws everything the plugin has open or queued and runs no
  callbacks, matching overlays today (the runtime is going away).
- `helix.ui.confirm` goes through the same queue. Its callbacks are unchanged.
- Limits:
  - a plugin has at most one dialog shown or queued at a time; a second is a Lua error
    naming the limit (same shape as the one-confirm limit today);
  - overlays keep today's behaviour: requesting a component the plugin already has open
    returns the existing handle and changes nothing (attrs and `on_close` stay the first
    request's). That now also holds while the first request is still queued. Existing
    plugins, calibration-bridge's close-then-reopen included, keep working.
- `on_open` and `on_close` run on the main thread in the plugin's runtime, through the same
  fault and quota handling as every other plugin callback.

## Dialog look and content (decided)

- A plugin dialog is a real `ModalStack` entry, built from the same pieces app dialogs use,
  so the card, backdrop, entrance/exit animation, dismissal and keyboard focus behave
  identically. The plugin component supplies the content.
- The plugin can build the standard dialog chrome itself: `ui_dialog`, `modal_header`,
  `modal_button_row` join the plugin XML allowlist. That needs changes to those app
  components and to the policy, not only an allowlist line:
  - `modal_button_row` and `modal_header` gain pass-through `*_user_data` props on each
    button's `event_cb`, so a `plugin_event` callback can name its handler
    (`parse_plugin_event` reads `user_data`). App usage is unchanged (props default empty).
  - Their widgets carry fixed default names (`btn_primary`, `btn_close`, `header_title`,
    ...). In a plugin file every `*_name` prop on these components must be set to an owned
    `<id>__<name>`, and the policy checks `*_name` attributes, not only `name=`, so no
    screen-wide lookup ever resolves a plugin object.
  - Callbacks on them must read `plugin_event`, as everywhere in plugin XML.
- How the host wraps the component (component root is a `ui_dialog`, or the host makes the
  card and the component is its content) is the builder's call; the requirement is that a
  plugin dialog is indistinguishable in chrome from an app dialog of the same size.
- Height follows the app's `#dialog_content_*` ladder (MODAL_SYSTEM.md "Height budget").

## Waking (decided)

When a plugin dialog or overlay is presented from the queue (or immediately), the display
wakes and the screensaver is dismissed through the existing wake path
(`DisplayManager::wake_display()` or whatever that delegates to). If the lock screen is up,
the request stays queued until unlock (the lock screen counts as blocked), so the surface
is never "opened" behind a lock the user cannot see past.

## Responsive plugin XML (decided)

- `<consts>` and its value children (`px`, `int`, `percentage`, `color`, `string`, as the
  XML engine supports) join the plugin allowlist. Every constant name must be
  `<id>__<name>`, like every other plugin-owned name, so a plugin cannot shadow an app
  token or another plugin's.
- Scope: a plugin constant is visible to all of that plugin's components (declare once,
  use in every file), not to the app or other plugins. Registration lives wherever that
  is cheapest in helix-xml; since helix-xml has no unregister, a reload re-sets the values
  and an unloaded plugin's names simply have no remaining users.
- **Breakpoint variants are for `px` only**, the one type the app itself ladders by
  breakpoint (`theme_manager_resolve_px_tokens`). Suffixes follow the app's ladder exactly
  (`_small`, `_medium`, `_large` required as a set; `_micro`, `_tiny`, `_xlarge`,
  `_xxlarge` optional with the app's fallbacks), resolve on the narrow (cramped) axis, and
  are DPI-scaled the same way. `#<id>__card_w` resolves to the current tier's value. **The
  existing px resolver serves both**: give it an entry point that takes a plugin's parsed
  constants instead of scanning `ui_xml/`; the ESP32 build-time token table is untouched
  (no plugins on ESP32). `int`, `percentage`, `color` and `string` constants are plain
  values with no ladder, and plugin colour constants get no `_light`/`_dark` variants this
  round: plugins theme with the app's colour tokens.
- A breakpoint change at runtime (desktop resize, `theme_manager_refresh_layout_constants`)
  re-resolves plugin constants the same way it re-resolves app tokens.
- A plugin that declares a suffixed constant without the required set fails its load with a
  message naming the constant and the missing tiers (Settings > Plugins shows it), rather
  than resolving to nothing at some breakpoint.
- `ui_breakpoint`, `ui_breakpoint_v` and `ui_is_portrait` become readable from plugin XML
  in conditional bindings only (`bind_flag_if_*`, `bind_style_if_*`, `cond` where the
  plugin id allows it). They are never writable, and two-way bindings (`bind_value` and the
  like) still require an owned subject. Portrait-specific sizing is done with these binds;
  plugin px constants do not get the app's vertical-axis token list.
- Constants unregister (or become unreachable) when the plugin unloads, so a reload with
  changed values picks up the new ones.

## Left to the builder

- Where the queue lives (a plugin-side presenter watching `ModalStack`, or a small
  general mechanism in the modal layer) and how `ModalStack` announces "a blocker closed"
  (callback, subject or generation counter). Prefer the smallest change that keeps
  `ModalStack` free of plugin knowledge.
- How "is this entry a plugin surface" is recorded.
- The handle object's representation in Lua.
- Whether `overlay` and `dialog` share one host class or two.

## Tests the change must carry (each proven red once by breaking the line it pins)

- Layering: an overlay pushed while a modal is visible leaves the modal on top
  (fails today if the code reading above is right).
- A plugin dialog requested while an app modal is showing does not appear; it appears,
  and `on_open` runs, after that modal closes.
- An app modal raised while a plugin dialog is showing appears above it; closing it leaves
  the plugin dialog showing.
- FIFO across two plugins; one plugin's second dialog request while one is shown or queued
  is the documented error.
- `close()` on a queued request runs `on_close("cancelled")` and never `on_open`; on a
  shown one, `on_close("plugin")`; a user dismissal, `on_close("user")`; an external
  sweep or soft restart, `on_close("app")`; each exactly once.
- Same-plugin rule: a plugin's confirm requested while its own dialog shows appears at
  once, on top; another plugin's dialog requested meanwhile waits.
- Overlay re-request while queued or showing returns the same handle and changes nothing.
- Each `lv_layer_top()` app cover blocks; presenting wakes, and an auto-lock engaged by
  that wake keeps the request queued.
- Plugin `modal_button_row` with owned `*_name` props and `*_user_data` reaches the
  plugin's handler; one with a default (unowned) name is rejected.
- Unload with a shown and a queued request: both gone, no callbacks.
- Lock screen up: request stays queued; unlock presents it. Screensaver/sleep: presenting
  wakes the display.
- `helix.ui.confirm` queues behind an app modal.
- Plugin XML policy: `<consts>` with owned names passes; an unowned constant name, and a
  partial suffix set, are rejected with the reason; `ui_breakpoint` passes in a
  `bind_flag_if_eq` and is rejected in `bind_value`; `ui_dialog`/`modal_button_row` with a
  non-`plugin_event` callback are rejected.
- Responsive resolution: a plugin `_small/_medium/_large` constant resolves to each tier's
  value under the shared resolver, and re-resolves on a breakpoint change; app tokens are
  unchanged (existing theme tests stay green).

## Docs and examples (part of the change, not a follow-up)

- `docs/devel/PLUGIN_DEVELOPMENT.md`: `helix.ui.dialog`; the presentation rule in one
  paragraph (plugin surfaces wait for app dialogs, app dialogs always win, waking, lock
  screen); `on_open` / `on_close(how)` for overlay and dialog; confirm queueing; the new
  allowlisted elements; §7 gains a "Responsive sizing" subsection (app tokens, own
  constants and their suffix ladder, `ui_breakpoint` in conditional bindings, the load
  error for a partial set).
- `docs/devel/PLUGIN_TUTORIAL.md`: if any step it walks through changes, update it.
- `docs/devel/MODAL_SYSTEM.md`: the overlay-vs-modal layering rule, and how plugin
  surfaces queue (short; points at the plugin doc).
- `docs/devel/UI_CONTRIBUTOR_GUIDE.md` §2: note that the suffix ladder also serves plugin
  constants (one resolver).
- `examples/plugins/calibration-bridge`: keep its overlay for the flow (context, Start,
  progress), and ask "which sample was best?" in a `helix.ui.dialog` opened when the
  calibration print completes, as the worked example of this exact use. Its Cancel confirm
  then exercises the same-plugin rule. README updated to match.
- User-facing plugin docs under `docs/user/`, if any describe plugin UI behaviour.
- This plan file is deleted in the change that ships the work.
