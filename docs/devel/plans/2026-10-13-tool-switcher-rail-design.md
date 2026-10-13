# Tool switcher: segmented rail

Design for the home-panel tool switcher on multi-head printers (klipper-toolchanger,
Creator 5 Z-Mod changer, Snapmaker U1). Replaces the pills-plus-Park layout on
`feature/tool-switcher-labels-park`. Approved direction: "A, segmented rail" in the
mockup at https://claude.ai/artifact/AzajHrttkTNKbUdkzHQTeA (version 4). Issues:
prestonbrown/helixscreen#1769 (labels), #1784 (park).

## Goals

- **Fast switching.** Every head is one tap away whenever the tile can hold it safely.
- **Never too small to hit.** No tappable target below `#touch_target_min` (48 px) on
  either axis, at any tile size or breakpoint. A tile that cannot hold every head at
  that size falls back to fewer, larger targets plus a sheet.
- **Adaptive detail.** Larger tiles add the mounted head's material and temperature,
  then the other heads'.
- **Park** is visible whenever the tile has room for it, greyed when no head is
  mounted, and otherwise reachable from the sheet.

Typical printers have 4-5 heads; 2-3 is common, 6-8 rare.

## What the widget shows

### Modes

A tile picks one of four modes from its content box (outer tile minus `#space_md`
inset), the head count, and the 48 px minimum. Cells are separated by a 2 px gap.

| Mode | Face | Switching | Park |
|---|---|---|---|
| `RailWithPark` | a segment per head, then a Park segment | one tap | on the face |
| `Rail` | a segment per head | one tap | in the sheet (tap the mounted segment) |
| `Pair` | the mounted head as one large target, beside Park | tap the face, pick in the sheet | on the face |
| `Single` | the mounted head as one large target | tap the face, pick in the sheet | in the sheet |

Selection, first that fits wins:
1. `RailWithPark` if `count + 1` cells fit with every cell >= 48x48.
2. `Rail` if `count` cells fit with every cell >= 48x48.
3. `Pair` if two cells >= 48x48 fit along the box's longer axis.
4. `Single` otherwise.

"Fit" for N cells: try rows = 1..N; cols = ceil(N/rows); cell = ((w - 2(cols-1))/cols,
(h - 2(rows-1))/rows); the first row count whose cell height and width are both >= 48
fits, and stop as soon as cell height drops below 48.

Measured content boxes give, at 48 px (5 heads typical):

| Heads | 800×480 1x1 | 2x1 | 1x2 | 2x2 | 480×272 1x1 | 2x1 | 1x2 | 2x2 |
|---|---|---|---|---|---|---|---|---|
| 2 | Single | RailWithPark | RailWithPark | RailWithPark | Single | Rail | Rail | RailWithPark |
| 4 | Single | Rail | Rail | RailWithPark | Single | Pair | Pair | Rail |
| 5 | Single | Pair | Pair | RailWithPark | Single | Pair | Pair | Pair |
| 8 | Single | Pair | Pair | RailWithPark | Single | Pair | Pair | Pair |

These are the unit-test vectors (content boxes: 800×480 1x1 95×93, 2x1 214×93, 1x2
95×211, 2x2 214×211; 480×272 1x1 58×52, 2x1 130×52, 1x2 58×118, 2x2 130×118). Half-cell
tile sizes go through the same function on their measured box; none needs a special case.

### Segments

- One continuous control: segments share the rail's rounded outer corners, 2 px apart.
- The mounted segment is filled `#primary`; the others use the elevated tile colour.
- A strip of the head's filament colour runs along each segment's foot (about 9% of the
  segment height, at least 3 px). No colour known: the strip is omitted.
- Label: "Tool 1" over "T0" when the segment is wide enough for "Tool 1" at the
  breakpoint's body font; "T0" alone when it is not. A narrow but tall segment shows the
  head's temperature as its second line instead. The noun comes from
  `active_tool_noun()` ("Toolhead 1" on a U1). A configured tool name is shown as the
  physical label, unchanged from today.
- Tapping a non-mounted segment mounts that head. Tapping the mounted segment opens the
  sheet (in every rail mode, so the gesture is consistent; in `Rail` it is also the only
  way to Park).

### Park segment

An outlined segment with the eject/dock icon, styled apart from the heads. Disabled
(greyed) while no head is mounted; it keeps its cell so the rail never changes width.

### Pair and Single faces

One large target showing the mounted head: "Tool 2 (T1)" or "T1" by width, a colour dot
and temperature line when tall enough, and the colour strip at its foot. With no head
mounted it reads "Choose head" ("None" when narrow). Tapping it opens the sheet. In
`Pair` the Park cell takes about 30% of the longer axis (at least 48 px).

### Detail block (tall rail tiles)

When a rail mode leaves room under the rail (rail sized to its rows at up to
`body * 3.4` each, and at least 2.5 lines of `#font_small` left over), a list fills the
rest, **centred horizontally**:
- the mounted head first, at body size: colour dot, "Tool 2 (T1)" (or "T1" if narrow),
  material, temperature (hot colour at >= 150 °C);
- then the other heads at small size and muted: dot, "T0", material, temperature;
- with no head mounted, a first line "Carriage empty · tap a head";
- rows that do not fit are dropped, never squeezed.

### Sheet

The existing `ToolPicker` (`tool_switcher_picker.xml`), raised from the tile:
- a row per head (same label, colour strip and temperature as a segment) plus a Park row,
  every row >= `#touch_target_min` tall;
- Park row disabled when no head is mounted;
- picking a head mounts it and closes; Park parks and closes; outside tap closes.

## Gating

Unchanged from the branch: while `job_holds_machine()` is true (Preparing, Printing,
Paused), switching and Park are disabled, and a tap that slips through is refused
with the existing toast. A paused-print tool change keeps the existing confirmation
modal.

## Data

All already published (no new plumbing): `ToolState` (tool list, names, `active_tool`,
`tool_count`), per-tool extruder temperature subjects (bound in C++, as
`nozzle_temps_widget` does), per-tool filament colour/material through
`AmsState::routed_tool_colors()` and the tool's backend slot. Park capability:
`AmsBackend::supports_tool_park()` / `park_active_tool()` from commit 12f267453 (U1
sends `PARK_EXTRUDER`, klipper-toolchanger `UNSELECT_TOOL`).

## Empty carriage on the U1

Read from the U1 (192.168.30.103) on 2026-10-13, idle after a print: every
`extruderN.state` is `"PARKED"` while `toolhead.extruder` still reads `"extruder2"`. The
toolhead object names the last head used, not a mounted one. So `active_tool` must be -1
whenever the head it names reports `PARKED`, or the rail shows T2 mounted and Park
enabled on an empty carriage. The U1 backend already parses `extruder_states_[i].state`;
the rule belongs there (vendor knowledge stays in the backend), surfaced through the
same active-tool path the toolchanger backends use, with a backend unit test feeding
this exact frame. After `PARK_EXTRUDER` the widget must read the carriage as empty.

## Code shape

Keep from the branch: the backend park capability and its tests (12f267453, backend
half), `ToolLabelForm` / `tool_label_in_form()` / `pick_tool_label_form()` (90fac34c3).
Replace the pill grid and the Park pill component.

New pure decision, in `include/tool_switcher_layout.h` (no LVGL):

```cpp
namespace helix::ui {
enum class ToolSwitcherMode { RailWithPark, Rail, Pair, Single };

struct ToolSwitcherLayout {
    ToolSwitcherMode mode;
    int cols;      ///< rail modes: grid columns; Pair/Single: 1 or 2 along the long axis
    int rows;
    int cell_w;    ///< px, >= min_target in every mode
    int cell_h;
    int rail_h;    ///< px of the content box the rail/face occupies
    bool detail;   ///< a detail block fits under the rail
};

ToolSwitcherLayout tool_switcher_layout(int tool_count, int content_w, int content_h,
                                        int min_target, int gap, int body_px, int small_px);
}
```

The widget follows the four-layer measured-layout pattern in
`docs/devel/PANEL_WIDGET_GUIDE.md` (nozzle-temps is the working instance):

1. **Decide (pure C++, no LVGL):** `tool_switcher_layout()` above. Responsive XML alone
   cannot make this call: breakpoints describe the screen, not the tile (the same screen
   holds a 1x1 and a 2x2), and the fit search over rows, head count and the 48 px floor
   needs loops the integer-only XML evaluator does not have.
2. **Measure (C++):** `on_size_changed()` and `attach()` (widgets are recycled) measure
   the content box and, for the label rung, the segment width against the strings the
   segments actually render.
3. **Publish (C++):** the verdict as int subjects `tool_switcher_mode` (enum value =
   subject int), `tool_switcher_cols`, `tool_switcher_rows`, `tool_switcher_detail`,
   `tool_switcher_label_form`, registered with `register_widget_subjects()` so they
   exist before any XML binds them. After that, `on_size_changed()` makes no widget
   calls.
4. **Draw (XML only):** the four modes as branches bound to `tool_switcher_mode`;
   segments from `<repeat count="tool_count">` over a `tool_switcher_segment` component
   (grid columns from `tool_switcher_cols`); mounted, disabled and greyed-Park states
   via `bind_state`; the detail list as a second `<repeat>` gated on
   `tool_switcher_detail`; events via `<event_cb>`. C++ creates no widgets in a loop.

`fits_at()` becomes true for every size: every box has a mode.

## Tests

- `tool_switcher_layout()`: every cell of the table above, plus 1 head, 16 heads, a
  0-height box (Single), and the 2x1 boundary at 800×480: four 48 px segments need
  198 px of the 214, five need 248, so 4 heads give `Rail` and 5 give `Pair`.
- Mode never yields a cell under `min_target` (property check over a grid of sizes and
  counts 1-16).
- Widget: rail builds `count` (+1) segments; tapping a non-mounted segment requests that
  tool; tapping the mounted segment opens the sheet; Park disabled with no head mounted;
  `Pair` face opens the sheet; detail block present only when `layout.detail`, mounted
  head first.
- Sheet: rows per head plus Park, row height >= `touch_target_min`, Park disabled with
  no head mounted.
- Gating: switching and Park refused while `job_holds_machine()`.
- Hand-mutate each branch of the mode selection and record it in the commit body.

## Verification

ctl on the toolchanger mock (`HELIX_AMS_GATES=2/4/5/8`) and the U1 persona at 800×480 and
`-s micro`: `ctl geom` every segment >= 48×48, the mode per size matches the table, and
screenshots of 1x1/2x1/1x2/2x2 for Preston to judge the look.

## Open (not blocking)

- The Filament panel's closed tool dropdown should read "T0" (decided earlier); separate
  small change on the same branch.
