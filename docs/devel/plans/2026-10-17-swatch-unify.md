# Swatch unification plan (2026-10-17)

Every colour swatch in the app draws through one core. Decisions are Preston's
(2026-10-17): a pure colour is a circle, a labelled chip is a rounded rect,
preview areas and the AMS sidebar's loaded bar stay rounded rects; one
light-theme edge rule (the WCAG contrast check); one selection mark (the
color_swatch inside ring); multi-colour splits everywhere a spool colour shows;
finishes (silk, matte, marble, sparkle) derived from the spool's variant.

Branch: `feat/swatch-unify`, worktree `.worktrees/swatch-unify`.

## Core: `include/ui_swatch.h`, `src/ui/ui_swatch.cpp`

Pure decisions (no LVGL objects, unit tested in `tests/unit/test_ui_swatch.cpp`):

```cpp
enum class SwatchFinish : uint8_t { Plain, Matte, Silk, Marble, Sparkle };

bool swatch_needs_edge(uint32_t rgb, uint32_t surface);          // moved from ui_color_picker.h
bool swatch_needs_edge_here(uint32_t rgb);                        // against screen_bg + elevated_bg
bool swatch_colors_need_edge(const std::vector<lv_color_t>&);     // any colour blends -> edge
std::vector<lv_color_t> parse_multi_colors(const std::string& csv);
SwatchFinish swatch_finish_from_variant(std::string_view text);   // wraps helix::filament_finish_of
SwatchFinish swatch_effective_finish(SwatchFinish, int32_t w, int32_t h); // Plain below SWATCH_FINISH_MIN_PX
lv_color_t swatch_matte_color(lv_color_t);
std::vector<SwatchPolygon> swatch_split_polygons(const lv_area_t&, int32_t radius, size_t n);
std::vector<SwatchPoint> swatch_speckle_points(const lv_area_t&, int32_t radius, size_t count, uint32_t seed);
```

Drawing (canvas and draw-task code: path dots, temp-graph legend, bands,
illustrations):

```cpp
struct SwatchPaint {
    std::vector<lv_color_t> colors;          // 1..N; N >= 2 draws the diagonal split
    const lv_grad_dsc_t* gradient = nullptr; // fills the shape instead of colors
    int32_t radius = LV_RADIUS_CIRCLE;
    bool edge = true;                        // apply the contrast edge rule
    SwatchFinish finish = SwatchFinish::Plain;
    lv_opa_t opa = LV_OPA_COVER;
};
void draw_swatch(lv_layer_t* layer, const lv_area_t& area, const SwatchPaint& paint);
```

Object hook: `apply_swatch_color(obj, rgb, multi_csv, finish = Plain)` keeps its
signature and attaches a DRAW_MAIN paint that reads the object's radius, its
bg_color (when no colour list is set) and its bg_grad at draw time, so a theme
change or a subject-bound colour is drawn without re-applying. The edge is
decided at draw time from the live theme, so no edge subject pool is needed.

`helix::filament_finish_of(std::string_view)` in `include/filament_variants.h` is the
one place a name becomes a finish: Silk/Satin/Silky, Matte/Matt, Marble/Granite/
Stone, Sparkle/Glitter/Galaxy/Shimmer, matched as whole words.

## Widget

`<swatch_paint>` (C++ widget, `src/ui/ui_swatch.cpp`): an lv_obj with the paint
attached. Attributes: `color`, `bind_color` (color subject), `colors`,
`bind_colors` (string subject, CSV), `finish`, `bind_finish` (string subject),
`edge`. Empty values are ignored, so components pass optional props straight
through.

`color_swatch.xml` draws its disc with `<swatch_paint>` and gains props `color`,
`colors_subject`, `finish_subject`, `fill` (a style, for the LED rainbow
gradient), `clickable`, and the existing ring/reach props (reach 0 + no ring is
the small dot).

## Migrations

| Area | Sites |
|---|---|
| LED | led_control_overlay custom (rainbow) swatch, led_white_tone, ui_settings_led preset row |
| Spool wizard / create | spool_wizard summary, create_filament_modal, wizard_filament_row |
| Mapping rows | filament_slot_picker_row, filament_mapping_tool_row (chosen = circle, expected T0 = labelled chip), preflight_check_tool_row |
| Tool dots | ams_current_tool, panel_widget_tool_switcher dot, panel_widget_bypass dot, zone_tab light dot |
| AMS edit / Spoolman | spoolman_spool_item, ams_edit_overlay card/preview/scan |
| Other | theme_editor_overlay role swatches, ams_loaded_card bar (edge rule only), action_prompt_modal tiles, favorite_macro_config_modal grid, temp_graph_widget config swatch, ui_temp_graph legend |
| Bands | filament_swatch, tool_switcher_band: core edge/split/finish, own geometry |
| Illustrations | spool_canvas, AMS lane spools (3D + flat), Active Spool widget |
| Dead code | ui_panel_ams "color_swatch" child lookup |

## Lint

`scripts/check_hand_rolled_swatch.py` + `qc/` entry + meta-test: an object with
radius 9999/LV_RADIUS_CIRCLE and a filament/LED colour fill outside the core and
color_swatch fails. Baseline for anything legitimately left.

## Tests

`[swatch]` tag: edge decision, multi parse, split polygons stay inside the
radius and cover equal areas, finish-from-variant, finish fallback below the
minimum size, speckle points inside the shape. Each test hand-broken once.

## Done when

The plan is deleted and its lasting content lives in
`docs/devel/COLOR_SWATCHES.md`, registered in the docs index.
