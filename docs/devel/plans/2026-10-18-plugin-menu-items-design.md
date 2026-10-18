# Plugin menu items

Status: design, awaiting approval. Deleted when the work ships (docs/CLAUDE.md).

## Why

Plugins can draw tiles, open their own screens, dialogs and overlays, but they cannot
attach an action to something the user is already looking at: a file they are about to
print, a filament slot, a Spoolman spool. The first outside plugin author needs exactly
that kind of entry point ("calibrate with this file", "look this spool up in my
inventory"). Menu items give a plugin a named action in the app's own menus, carrying the
thing the menu was opened on.

## Goals

1. A plugin declares menu items in its manifest; each item appears in one app menu and,
   when tapped, runs one of the plugin's `helix.ui.on` handlers with that menu's context.
2. Four places take plugin items:
   - **Print details** (the file detail screen in print select), for the file shown.
   - **Filament slot menu** (`AmsContextMenu` opened on a slot of a filament system, every
     backend), for the slot. `AmsContextMenu` also serves the external spool
     (`ui_external_spool_menu.h`, the bypass home widget); that use takes no plugin items.
   - **Spoolman spool menu** (`SpoolmanContextMenu`), for the spool.
   - **The plugin's own home tile**, from the tile's configure (gear) button in home edit
     mode.
3. An item may be shown or hidden, and enabled or disabled with a reason, per context,
   through an optional Lua check.
4. Settings > Plugins lists what a plugin adds to menus before it is enabled, next to its
   permissions.
5. The plugin guide documents menu items (`PLUGIN_DEVELOPMENT.md`, a "Menu items" section),
   written from this design.

## Non-goals

- The print file list's long-press menu. It stays file management (Delete) only.
- Menus a plugin defines for its own screens (a plugin already draws those itself).
- Nested submenus, separators chosen by the plugin, or ordering across plugins.
- Plugin items in any other app menu (printer switch, external spool, home tile pickers of
  app widgets). Each new place is a later, explicit addition.

## Anti-goals

- A plugin item that runs app code or reaches app objects. Tapping an item only calls the
  plugin's own handler, with plain data.
- A plugin able to hide, reorder or relabel the app's own items.
- A menu that opens slowly or breaks because a plugin's check is slow, errors or is
  missing.
- A plugin item on screen while its plugin is disabled, faulted or unloaded.
- The print-details screen losing its full-width Print label on printers where no plugin
  adds a file item.

## Manifest (decided shape; field names are the builder's within it)

```json
"menu_items": [
  {
    "id": "calibrate",
    "menu": "print_file",
    "label": "Calibrate with this file",
    "icon": "tune",
    "handler": "calibrate",
    "check": "can_calibrate"
  }
]
```

- `menu`: one of `print_file`, `filament_slot`, `spool`, `tile`.
- `label`: English source text, translated through the plugin's own `i18n/` packs like
  every plugin string.
- `icon`: an app icon name, optional.
- `handler`: a `helix.ui.on` handler name; required.
- `check`: a `helix.ui.on` handler name, optional (below).
- `tile` items are offered only when the plugin declares a widget; they appear on every
  tile of that plugin.
- A malformed item fails the manifest parse with the field named, as other manifest errors
  do. Limits (builder sets the exact numbers, documented): a small number of items per menu
  per plugin, and a cap on plugin items per menu overall so an app menu stays usable.

## Tap: the handler and its context (decided)

Tapping an item closes the menu and calls the plugin's handler with one Lua table:

| Menu | Context fields |
|---|---|
| `print_file` | `source` (`"printer"` or `"usb"`), `path` (relative to the gcodes root; `nil` for a USB file), `local_path` (USB files only), `name`, plus what the details screen holds when the item is tapped: `estimated_time_s`, `filament` (per tool: `tool`, `material`, `color`, `grams`), `layer_height`, `object_height`, `tools_used`. A field the metadata lacks, or that has not loaded yet, is `nil` |
| `filament_slot` | backend-neutral ids (`unit`, `slot`, both 0-based, and the slot's display label as the menu shows it), `tool` (mapped tool or `nil`), and what is loaded when known: `material`, `color` (hex), `brand`, `spool_id` (Spoolman) |
| `spool` | `spool_id`, `material`, `color`, `brand`, `remaining_g` |
| `tile` | `widget` (the declared widget id) |

The handler is an ordinary plugin entry: same time budget, fault rules and permissions as
any `helix.ui.on` handler. It may open a dialog or overlay, call Moonraker within its
permissions, or do nothing.

## Per-context check (decided)

- An item without `check` always shows, enabled.
- An item with `check`: when the menu opens, the check handler is called with the same
  context table and returns `true`/`false` (shown and enabled, or hidden), or a table
  `{visible = bool, enabled = bool, reason = string}`. A disabled item shows greyed with its
  reason. This needs a new runtime entry that runs a handler synchronously with a table
  argument and returns its value (`LuaRuntime::invoke` discards results and
  `dispatch_ui_handler` passes one string).
- A check runs in **check mode**: every binding with a side effect or an async wait
  (Moonraker calls, gcode, http, storage writes, settings writes, `helix.sleep`, opening a
  dialog, overlay, confirm or toast, navigation) raises at once instead of acting. A check
  can read subjects, printer fields, settings and its own Lua state, and decide.
- A check has its own short time budget, separate from the plugin's entry budget. Overrunning
  it, raising, or returning anything else hides that item for this opening and is logged; it
  counts toward the plugin's error quota (`report_error`), never as an immediate fault. The
  menu always opens.

## Placement (decided with Preston)

- **Print details**: the bottom row today is Delete (icon) plus a full-width Print with its
  label. When at least one enabled plugin declares a `print_file` item, the row becomes
  square icon buttons: Delete, a "⋯" menu button, and Print (keeping its primary colour).
  Measured: the row is 296 / 176 / 181 px wide at 800x480 / 480x320 / 480x272 and the
  square buttons are 70 / 40 / 34 px, so three fit at every size (210 / 120 / 102). For a
  USB file Delete is already hidden, so the row is "⋯" plus Print. With no plugin
  declaring a file item, the row is exactly as today.
- The "⋯" decides from declarations, not checks, because the file's time, height and
  layer metadata arrive after the screen shows. Checks run when "⋯" is tapped, against the
  context as it stands then; if every item is hidden for this file, the menu says there are
  no plugin actions for it.
- **Filament slot menu**: plugin rows go at the bottom of the card, below the Tool and
  Endless Spool dropdowns, after a divider; each plugin's group is headed with the plugin's
  name, plugins in load order, items in manifest order (Preston's choice). The card never
  scrolls, so the builder measures it at 480x272 with the tallest app content and sets the
  overall plugin-row cap from that measurement.
- **Spoolman spool menu**: the same rule, appended to its flat list: a divider, then each
  plugin's group headed with its name.
- **Tile**: a plugin widget whose plugin declares `tile` items answers
  `has_edit_configure() == true` (from the declaration; no check runs to decide it); the
  gear in home edit mode opens a context menu of those items below the tile, running checks
  then. A tile with no `tile` items has no gear (as today).

## Consent

Informational only (Preston's choice). Settings > Plugins shows "Adds to: Print details,
Filament slot menu" on the enable prompt and on the plugin's detail row, where permissions
are listed. Menu items need no permission of their own: a handler can only do what the
plugin's granted permissions already allow. An update that adds or changes menu items loads
without asking again; only permission growth asks, as today.

## Lifetime

- Items come from the loaded manifest; unload, disable and fault remove them at once. A menu
  already open when its plugin goes away drops that plugin's items or closes, and a tap that
  lands after the unload does nothing.
- Handlers run on the main thread through the plugin runtime (`LuaRuntime::invoke`), as
  `helix.ui.on` handlers do today.

## Left to the builder

- How menus learn about plugin items (one small registry the four owners query, keyed by
  menu kind; app menus must not name plugins or know Lua).
- The Lua call mechanics for a table argument and for the synchronous check.
- Exact limits, the divider style, and the "⋯" icon choice (an existing app icon).
- Whether the context tables share a builder with existing code that already computes the
  same facts (the details screen's metadata, the slot info the AMS menu shows): reuse it,
  never recompute it.

## Tests the change must carry

- Manifest: valid items parse; each malformed field fails with its name; unknown `menu`
  rejected; a `tile` item in a plugin without a widget rejected.
- Each of the four menus: an enabled plugin's item appears with its label and icon; a
  disabled plugin's does not; tapping calls the handler with the documented context fields
  (assert values, including a `nil` for missing metadata).
- Check: `false` hides, `{enabled=false, reason}` greys with the reason; a check that
  raises, overruns its budget, or calls a side-effecting binding (gcode, Moonraker, dialog)
  hides only that item, performs no side effect, does not fault the plugin, and the menu
  still opens.
- Print details: no plugin declaring file items, then the row is unchanged (full Print
  label); one declared, then three icon buttons and "⋯" opens the item; a USB file, then
  "⋯" plus Print and the context has `source = "usb"`, `path = nil`, `local_path`; checks
  hiding every item, then "⋯" opens the "no plugin actions" state.
- Slot menu: plugin rows render below the dropdowns with the plugin's name, and the card
  fits without scrolling at 480x272 at the cap; the external spool menu shows no plugin
  items.
- Unload with the menu open: no handler runs on a later tap.
- Settings > Plugins shows the "Adds to" line.

## Docs (part of the change)

- `docs/devel/PLUGIN_DEVELOPMENT.md`: a "Menu items" section (manifest fields, the four
  menus and their context tables, the check contract, limits, lifetime), and a line in §5
  manifest.json.
- An example: add one item to an existing example plugin (calibration-bridge's "Calibrate
  with this file" on `print_file` is the natural one), with its README.
- `docs/user/` page for plugins, if one describes what plugins can add.
- This file is deleted in the change that ships the work.
