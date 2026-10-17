# Seed a remote printer's home layout from its own screen

## Intent

When a HelixScreen client (a K-Touch, a second Pi screen, a desktop build) adds a
printer whose Moonraker host already runs HelixScreen on its own display, the
client's home panel for that printer starts as a copy of the layout the
printer's own screen has. Someone who already arranged tiles, picked macros,
chose an LED and a camera on the printer should not have to do it again on
every other screen they add. Screen size differs between devices, so the copy
is a best effort ported to the client's grid, not a mirror.

## Goals

- The printer's own HelixScreen publishes its home layout to Moonraker.
- A client adding that printer uses the published layout automatically instead
  of the generated default, and says so with a toast.
- The home-layout settings (widget catalog overlay) offer "Copy layout from
  printer" to re-pull it later, behind a confirm.
- Per-widget config (macro picks, LED, fan, sensors, camera source, print
  status options) carries over, because that is the printer-specific work a
  user did; geometry is ported as well as the grids allow.

## Non-goals

- Live sync. After the seed the layout belongs to the client; edits on either
  side never propagate.
- Theme, temperature presets (`/preset_materials`), and anything else stored
  once per install rather than per printer.
- The navbar: it has no user configuration; its buttons follow capabilities.
- Publishing from remote clients. Only the instance on the Moonraker host is a
  source, so a printer has at most one published layout.

## Anti-goals

- A second porting implementation. Placement mapping between grids goes
  through the code that already does it (below); if that code needs a new
  parameter, extend it.
- Overwriting a layout the user has touched. Automatic seeding only ever
  replaces an unedited default.
- Screen-scoped tiles describing the wrong device (a copied network tile
  showing nothing useful, a shutdown tile whose target changed meaning).

## Decided

### Who publishes, and when

- An instance publishes only on a positive same-host verdict for the current
  connection. `get_printer_state().is_moonraker_remote()` alone is not that:
  the `moonraker_is_remote` subject starts at 0 (reads as local), is set on the
  CONNECTED edge through a deferred update
  (`src/application/moonraker_manager.cpp`), and after a printer switch holds
  the previous printer's value until then. The plan adds a "verdict known for
  this connection" state, cleared on disconnect and switch; no verdict, no
  publish. (`include/host_identity.h#helix::is_moonraker_on_same_host` is the
  underlying check.) *Builder's call, made in design review.*
- It publishes on every home-layout save, from the same point `save()` /
  the debounced `save_soon()` write `settings.json`
  (`src/system/panel_widget_config.cpp#PanelWidgetConfig::save`), and once
  after connecting so a layout saved before this feature shipped is published
  without an edit.
- Storage: Moonraker database, namespace `helix-screen` (already used by
  `src/printer/tool_state.cpp`), via `IMoonrakerAPI::database_post_item`
  (`include/i_moonraker_api.h`). No plugin dependency.

### What is published

A versioned object built from `PanelWidgetConfig::serialize_pages()` plus the
grid signature it was authored for:
`{schema, grid: "<cols>x<rows>", pages, main_page_index, saved_at, app_version}`.
`serialize_pages()` itself emits `default` (when present) and `next_page_id`;
the publisher strips both. `parked_grids`, `anchors` and legacy tags are not in
that output and are not published.

Version skew between source and client: any payload with a `schema` the
client knows is accepted. Unknown widget ids land in `retained` and unknown
per-widget config keys are ignored, the same tolerance `settings.json` already
needs across upgrades and downgrades; `app_version` is informational (logs,
debug bundles), never a gate. *Builder's call, made in design review.*

### Which widgets carry over

A new scope field on `PanelWidgetDef` (`include/panel_widget_registry.h`),
beside `category`, marks a widget printer-scoped (default) or screen-scoped.

- Screen-scoped: `network`, `clock`, `lock`, `tips`, `notifications`,
  `shutdown`.
- Everything else is printer-scoped, including `host_health` (Moonraker perf
  data, so it describes the printer host) and plugin widgets.

On import, screen-scoped entries from the source are dropped and the client
keeps its own defaults for those widgets (the default generator's choice for
this client), placed into free space or left enabled-unplaced if none.

### When the client seeds

- Automatically, while the printer's layout is still an unedited pending
  default (`is_unedited_default()` / `has_pending_anchors()` in
  `include/panel_widget_config.h`); the first `populate_widgets` after klippy
  READY is where defaults are applied today.
- A published layout that arrives after the default was applied still
  replaces it if the layout is still an unedited default; otherwise it is
  ignored.
- Toast on success, e.g. "Home layout copied from the printer's screen".
- No published layout, unreadable payload, or a payload from a newer `schema`
  the client does not understand: today's default generator, silently.

### How placements are ported

The import loads the published pages as authored for their source grid and
lets `switch_to_grid` port them to the client's grid, exactly as the app does
when a screen first visits a new grid; that path uses
`src/ui/layout_port.cpp#port_legacy_layout`. `restore_pages()` cannot do the
loading as it stands: it takes no grid, `grid_signature_` is private with only
a getter, and a payload with no usable pages makes it build and stamp
defaults. So the plan adds one import entry point on `PanelWidgetConfig` that
takes the payload plus its source grid, validates the payload fully before
touching any state (a rejected payload changes nothing), sets the source grid,
and then ports. Extend `restore_pages()` for this rather than writing a
sibling, if the two can share the loading.

The import discards this printer's `parked_grids`: they are ports of the
layout being replaced, and keeping them would bring the old layout back on a
later UI-scale change. *Builder's call, made in design review.* Unseated widgets go
through the existing auto-place pass in `populate_widgets`; widgets larger than
the grid are disabled with the existing toast. Same-grid copies (the K-Touch
and a 7" 800x480 are both 12x8) come through one-to-one.

Pages that hold no seated or enabled widget after porting and filtering are
dropped; `main_page_index` is remapped accordingly.

### After the seed

A seeded layout is the user's own, exactly as if they had arranged it: no
default marker, so `reresolve_default_if_changed()` leaves it alone, a later
publish never replaces it automatically, and the catalog's Reset returns to the
generator default, not to the printer's layout. This is what the code already
does once `restore_pages()` / `switch_to_grid()` clear `has_default_`; no new
layout state is added. (Preston's call.)

### Manual re-import

"Copy layout from printer" in `src/ui/ui_widget_catalog_overlay.cpp`, beside
the existing reset action, using `modal_confirm`. It replaces the current
layout for this printer regardless of edits, then saves and
`notify_config_changed("home")` like reset does. Hidden when this instance is
itself the source (Moonraker on the same host).

### Platforms

The ESP32 K-Touch builds every file involved. New sources go in
`firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` (gated by
`scripts/check_esp32_app_srcs.py`), and the payload is parsed with the
no-exception JSON helpers (`json_util::safe_*`, `json::parse(s, nullptr, false)`).
The K-Touch never publishes; it is never the Moonraker host.

## Constraints

- The payload is network input from another device: validate it the way
  `settings.json` layouts are validated (unknown widget ids kept as
  `retained`, out-of-range coordinates handled by the existing clamp), and an
  unparseable payload must never leave the client with no layout.
- Every publish is a Moonraker database write; keep it to one per settled
  save, which the existing debounce already provides.
- Fetch and publish run on the Moonraker callback threads; any layout change
  they cause reaches LVGL through the UI queue (`.claude/rules/threading.md`).
- The mock Moonraker (`src/api/moonraker_client_mock_server.cpp`) already
  serves the database API, so both sides are testable under `--test`;
  `HELIX_MOCK_REMOTE_PRINTER` forces the remote verdict.

## Left to the builder

- The database key name and exact payload field names.
- Where the fetch is issued (connect edge vs first home-panel populate) and
  how its result is held until the seed point, within the timing rules above.
- How the client's own screen-scoped defaults are placed alongside the import.
- Toast and confirm wording (wrapped for translation).
- Test layout: at minimum same-grid identity, cross-grid port (12x8 to 10x8
  and 16x10), screen-scoped filtering, edited-layout-not-overwritten,
  malformed and newer-schema payloads, and the publish gate on remote vs local.
