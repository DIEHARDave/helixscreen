# Edit Spool redesign — feat/edit-spool

Approved design (every point a ruling from the coordinator). Branch: `feat/edit-spool`,
worktree `.worktrees/edit-spool`, claim `worktree:edit-spool`. This file is deleted in
the change that ships the work.

## Why

Users open this page to CHANGE a spool (usually material/brand after a spool swap, often
colour; sometimes weight; almost never price/location/lot). Today it opens on a half-empty
overview, crams five jobs into one editor, silently discards edits on Back, and carries two
controls nobody uses there.

## Surfaces touched

| Surface | Change |
|---|---|
| `ui_xml/ams_edit_overlay.xml` | Rebuilt: overview view removed; editor (chip rows) is the entry view |
| `src/ui/ui_ams_edit_overlay.{h,cpp}` | Chip-row state machine; weight keypad; unsaved-changes prompt; new Spoolman save prompt; removals |
| (removed) | the chip component was retired with the pivot |
| `ui_xml/components/color_swatch_grid.xml` | Add `count` prop (default 30) for the narrow colour column |
| `ui_xml/ams_context_menu.xml` + `src/ui/ui_ams_context_menu.cpp` | "Spool Info" → "Edit Spool"; revive Assigned Tool row gated on `can_write_mapping_table()` |
| `include/filament_catalog.h` + `src/printer/filament_catalog.cpp` | Pure family/variant/brand/product queries |
| `include/filament_variants.h` + `src/printer/filament_variants.cpp` | `variant_label()` (chip label for a type in its family) |
| `include/spoolman_types.h` + `src/api/spoolman_types.cpp` (or sibling) | `recent_material_families()` / `recent_brands()` from the recency-sorted spool list |
| tests | `test_filament_catalog_edit_queries.cpp` (new, pure), `test_ams_edit_overlay_views.cpp` (updated), `test_ams_context_menu` (updated) |
| translations/*.yml (8) | Every new string |

## Design-point map

1. **Entry**: `MenuAction::EDIT` → `open_slot_editor()` → `set_view(VIEW_EDITOR)` (renumbered
   VIEW_OVERVIEW). No overview view exists anymore.
2. **Title**: `lane_label(active_lane_noun(), slot_index_)` ("Gate 3"); external slot keeps
   "External Filament".
3. **Identity picking = dropdown cascade form** (PIVOT after Preston saw the
   chip rows: chips rejected, Bambu/Creality-style labelled dropdowns shipped).
   Material (family) / Variant / Brand / Product dropdown rows, cascading
   exactly as the chips did; each list ends in a quiet "Browse catalog..."
   entry opening the FilamentCatalogPickerModal (search + free entry).
   MATERIAL = families recents-first; VARIANT = catalog variants for the family (Basic,
   +, CF, GF, HS, Silk, Matte, Wood, Marble, Recycled…; finishes are variants —
   `filament_variants.h` is the model); BRAND = Generic first, then brands making that
   material, recents first, then "All..."; PRODUCT = that brand's lines for the material,
   hidden while Generic is selected.
4. **Responsive**: `<if cond="ui_breakpoint eq 0 and ui_is_portrait eq 0">` = split
   (chip rows left, colour column right); everything else = stacked with full-width grid.
5. **Summary line** "eSUN Silk PETG, Blue" + from-tag badge + LIVE read-only temps
   "Nozzle 230–250 °C · Bed 80 °C" per tap; at 480x272 class the temps sit beside the
   weight pill.
6. **WEIGHT**: pill top-right, tap → `ui_keypad_show` (grams, edits remaining).
7. **Spoolman**: Scan QR + Choose from Spoolman = header icon buttons via the
   `header_bar-header_content` slot (left of Save), gated `printer_has_spoolman` (+ no
   camera on ESP32). "Add to Spoolman + label" quiet link at the end of the More row
   (untracked slots only).
8. **MORE**: collapsed disclosure (`ams_edit_more_open` subject): price, location, lot, notes.
9. **Tags**: `backend->tag_reading()/tag_provenance()` (generic accessors only);
   from-tag badges on rows; "Reset to tag" beside the custom colour cell (colour) and in
   the summary (identity) — restores via `helix::ams::reset_to_tag`.
10. **Saving**: header Save. Back with unsaved changes → `modal_confirm` Save/Discard with
    `on_dismiss` (stay open).
11. **Spoolman-linked save**: identity changed → "Update in Spoolman"
    (`SpoolmanSlotSaver::LinkIntent::UpdateLinked` — find-or-create filament + repoint;
    never edits the shared filament) or "Unlink, keep on this slot"
    (`clear_spoolman_link()` + local commit). "Save to Spoolman" switch REMOVED;
    untracked slots reach Spoolman only via "Add to Spoolman + label"
    (`LinkIntent::CreateAndRebind`).
12. **Removed**: "Change Filament" button (header picker button replaces), tool remap from
    the editor. Assigned Tool lives in the context menu's `tool_dropdown_row` (revived,
    gated `helix::printer::can_write_mapping_table(*backend)`).
13. **Catalog queries**: `FilamentCatalog::all_families()`, `variants_in_family()`,
    `brands_in_family()`, `products_in_family()` — pure, tested. UI never filters.
    **Recents = Spoolman history** (decision): one `get_spoolman_spools` on open when
    Spoolman is available (the API layer's `sort_spools_by_recency()` ordering is the
    source); pure `recent_material_families()/recent_brands()` derive first-appearance
    order; without Spoolman the rows are catalog order. No new persistence.
14. **Unknown values**: a brand/material/product the catalog lacks shows as an extra
    SELECTED chip; open+save changes nothing.
15. **Multi-colour**: summary dot paints the split (existing swatch painter); selected
    cell read-only; picking a colour replaces (clears `multi_color_hexes`); Reset to tag
    restores.
16. **Family change**: keep brand if `brands_in_family(new)` contains it (case-insens),
    else Generic; variant → Basic; product/catalog_id cleared when invalid.
17. **Terms**: "Color" (US); Endless Spool / Filament Mapping / Assigned Tool. All new
    strings in all 8 locales.
18. **Swatches**: `color_swatch` / `swatch_paint` / `apply_swatch_color` only;
    `ui_swatch.cpp` / `ui_spool_canvas.cpp` untouched (peer session owns them).

## Pure-function contracts (tests pin these)

```cpp
// filament_variants.h
std::string variant_label(std::string_view type, std::string_view family);
// "PLA"/"PLA" → "Basic"; "PLA-CF"/"PLA" → "CF"; "Silk PLA"/"PLA" → "Silk";
// "rPLA"/"PLA" → "Recycled"; "PLA+"/"PLA" → "+"; unknown remainder → itself.

// filament_catalog.h
struct FamilyVariant { std::string label; std::string sample_type; };
std::vector<std::string> all_families() const;
std::vector<FamilyVariant> variants_in_family(const std::string& family) const;
std::vector<std::string> brands_in_family(const std::string& family) const;
std::vector<const EffectiveFilament*> products_in_family(const std::string& brand,
                                                         const std::string& family) const;
static std::string compose_family_type(const std::string& family, const std::string& variant);
// spoolman_types.h
std::vector<std::string> recent_material_families(const std::vector<SpoolInfo>& spools);
std::vector<std::string> recent_brands(const std::vector<SpoolInfo>& spools);
```

Editor selection model (in the overlay, exposed to tests through the friend shim):
family + variant + brand + catalog_id; `apply_family_change()` pins rule 16;
`selection_from_slot()` pins rule 14 (unknown family/brand → raw label kept, selected).

## Test plan (tags)

- `[catalog]` (new file): families/variants/brands/products purity, rule-16 helper,
  variant_label, compose_family_type, recents derivation.
- `[ams][edit]` (existing views test): entry view is the editor; title is the lane label;
  chip rows publish expected labels; family change resets variant/product per rule 16;
  unknown material shows as selected extra chip; save prompts (linked identity change,
  unsaved back); weight keypad writes remaining; tag badge subjects.
- Hand-break each new test once (`make t F='...'` red, restore).

## Verification cadence

Tags while building; no sweeps/gates/mutation here — the coordinator runs the finish.
Final screenshots: 800x480, 480x320, 480x272, 320x480 headless → `/tmp/editspool/`.
