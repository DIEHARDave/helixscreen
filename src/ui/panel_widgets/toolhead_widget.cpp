// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "toolhead_widget.h"

#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_icon.h"
#include "ui_icon_codepoints.h"
#include "ui_panel_motion.h"
#include "ui_subject_registry.h"

#include "app_globals.h"
#include "display_numbering.h"
#include "helix_psram_attr.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_error.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "printer_capabilities_state.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/content_chrome.h"
#include "src/ui/panel_widgets/live_tile_owner.h"
#include "subject_managed_panel.h"
#include "text_measure.h"
#include "theme_manager.h"
#include "tool_state.h"
#include "toolhead_homing.h"
#include "unit_conversions.h"
#include "widget_subjects.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

using helix::ui::copy_string_if_changed;
using helix::ui::measure_text_px;
using helix::ui::set_int_if_changed;

namespace {

// The verdict and readings the widget publishes, read by panel_widget_toolhead.xml.
// Module-level because the widget is single-instance and registration rides
// SubjectInitializer's panel-subject phase, so the subjects exist before any XML that
// binds them is parsed.
HELIX_PSRAM_BSS lv_subject_t s_rung_subject{};
HELIX_PSRAM_BSS lv_subject_t s_show_letters_subject{};
HELIX_PSRAM_BSS lv_subject_t s_z_only_subject{};
HELIX_PSRAM_BSS lv_subject_t s_show_rows_subject{};
HELIX_PSRAM_BSS lv_subject_t s_show_plate_subject{};
HELIX_PSRAM_BSS lv_subject_t s_show_home_subject{};
HELIX_PSRAM_BSS lv_subject_t s_show_status_subject{};
HELIX_PSRAM_BSS lv_subject_t s_homed_subjects[3]{};
HELIX_PSRAM_BSS lv_subject_t s_x_text_subject{};
HELIX_PSRAM_BSS lv_subject_t s_y_text_subject{};
HELIX_PSRAM_BSS lv_subject_t s_z_text_subject{};
HELIX_PSRAM_BSS lv_subject_t s_tool_text_subject{};
HELIX_PSRAM_BSS lv_subject_t s_speed_text_subject{};
HELIX_PSRAM_BSS char s_x_text_buf[16];
HELIX_PSRAM_BSS char s_y_text_buf[16];
HELIX_PSRAM_BSS char s_z_text_buf[16];
HELIX_PSRAM_BSS char s_tool_text_buf[32];
HELIX_PSRAM_BSS char s_speed_text_buf[24];
helix::WidgetSubjects s_subjects{"ToolheadWidgetSubjects"};
// The instance bound to the live tile. Only it publishes and acts: a transient
// instance (a fits_at probe, a rebuild's outgoing widget) must not overwrite the
// tile's verdict.
helix::LiveTileOwner<helix::ToolheadWidget> s_tile;

constexpr const char* kHomedSubjectNames[3] = {"toolhead_homed_x", "toolhead_homed_y",
                                               "toolhead_homed_z"};

void toolhead_init_subjects() {
    s_subjects.ensure_init([] {
        UI_MANAGED_SUBJECT_INT(s_rung_subject, static_cast<int>(helix::SizeRung::Wide),
                               "toolhead_rung", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_letters_subject, 0, "toolhead_show_letters", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_z_only_subject, 0, "toolhead_z_only", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_rows_subject, 0, "toolhead_show_rows", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_plate_subject, 0, "toolhead_show_plate", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_home_subject, 0, "toolhead_show_home", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_status_subject, 0, "toolhead_show_status", s_subjects);
        for (int i = 0; i < 3; ++i) {
            UI_MANAGED_SUBJECT_INT(s_homed_subjects[i], 0, kHomedSubjectNames[i], s_subjects);
        }
        UI_MANAGED_SUBJECT_STRING(s_x_text_subject, s_x_text_buf, "--", "toolhead_x_text",
                                  s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_y_text_subject, s_y_text_buf, "--", "toolhead_y_text",
                                  s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_z_text_subject, s_z_text_buf, "--", "toolhead_z_text",
                                  s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_tool_text_subject, s_tool_text_buf, "", "toolhead_tool_text",
                                  s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_speed_text_subject, s_speed_text_buf, "", "toolhead_speed_text",
                                  s_subjects);
    });
}

struct Fonts {
    const lv_font_t* xs;
    const lv_font_t* small;
    const lv_font_t* body;
    const lv_font_t* icon;
};

Fonts load_fonts() {
    return {theme_manager_get_font("font_xs"), theme_manager_get_font("font_small"),
            theme_manager_get_font("font_body"), theme_manager_get_font("icon_font_xs")};
}

using helix::line_px;

/// The widest value any axis can show, so the rung does not change as the numbers do.
int widest_value_px(const lv_font_t* font, bool compact = false) {
    using helix::Axis;
    return std::max(
        {measure_text_px(helix::format_toolhead_axis(Axis::X, -88888, true, compact), font),
         measure_text_px(helix::format_toolhead_axis(Axis::Z, 88888, true, compact), font)});
}

/// The minimum content box each rung draws in. Mirrors panel_widget_toolhead.xml:
/// spacing is the XML's pad_row (space_xxs) and pad_column (space_xs). Idle and printing
/// share the boxes, and Full is measured for the tallest footer.
helix::RungNeeds measure_needs() {
    using helix::rung_box;
    const Fonts f = load_fonts();
    const int row_gap = theme_manager_get_spacing("space_xxs");
    const int col_gap = theme_manager_get_spacing("space_xs");
    // Measured and rendered widths disagree by a few pixels; a text rung is never
    // budgeted to the last one.
    const int comfort = theme_manager_get_spacing("space_xs");
    const int tight = std::max<int>(2, theme_manager_get_spacing("space_xxs") / 2);
    // A box measured exactly renders as an overlap by a pixel.
    constexpr int kSlackPx = 1;
    const int button_px = theme_manager_get_spacing("button_height_sm");
    const int dot_px = theme_manager_get_spacing("space_sm");

    const int icon_w = measure_text_px(helix::ui::icon::lookup_codepoint("axis_z_arrow"), f.icon);
    const int icon_h = line_px(f.icon);
    const int xs_h = line_px(f.xs);
    const int small_h = line_px(f.small);
    const int body_h = line_px(f.body);

    const int letters_w = measure_text_px("X", f.body) + measure_text_px("Y", f.body) +
                          measure_text_px("Z", f.body) + 2 * col_gap;
    const int z_xs_w =
        measure_text_px(helix::format_toolhead_axis(helix::Axis::Z, 88888, true), f.xs);

    helix::RungNeeds needs;
    // Glance holds either the letters or the Z height stacked under its glyph.
    needs.glance = rung_box(std::max({letters_w, icon_w, z_xs_w}) + tight,
                            std::max(body_h, icon_h + xs_h) + kSlackPx);
    needs.strip = rung_box(std::max(letters_w, icon_w + col_gap + z_xs_w) + comfort,
                           std::max({body_h, icon_h, xs_h}) + kSlackPx);

    const int letter_xs_w = measure_text_px("X", f.xs);
    needs.column = rung_box(letter_xs_w + row_gap + widest_value_px(f.xs, true) + tight,
                            3 * xs_h + 2 * row_gap + comfort + kSlackPx);

    const int letter_w = measure_text_px("X", f.body);
    needs.cell = rung_box(letter_w + row_gap + widest_value_px(f.body) + row_gap + dot_px + comfort,
                          3 * body_h + 2 * row_gap + comfort + kSlackPx);

    // The plate needs enough pixels for a crosshair to read against its Z bar.
    const int plate_min = 3 * theme_manager_get_spacing("space_xl");
    const int bar_w = theme_manager_get_spacing("space_xs");
    const int value_row_w = letter_w + row_gap + widest_value_px(f.body);
    needs.wide = rung_box(plate_min + bar_w + col_gap + value_row_w + comfort,
                          std::max(plate_min, 3 * body_h + 2 * row_gap) + comfort + kSlackPx);

    // Full lays the three axes in a row under the plate, in the small face.
    const int full_row_w = measure_text_px("X", f.small) + row_gap + widest_value_px(f.small);
    const int footer_h = small_h + row_gap + button_px;
    const int values_row_w = 3 * full_row_w + 2 * theme_manager_get_spacing("space_sm");
    needs.full = rung_box(std::max(values_row_w, plate_min + bar_w + col_gap) + comfort,
                          plate_min + row_gap + small_h + row_gap + footer_h + comfort + kSlackPx);
    return needs;
}

/// What the plate last drew, so an unchanged position costs no redraw.
using DrawKey = std::array<int, 10>;

DrawKey draw_key(const helix::ToolheadPlateGeometry& g) {
    return {g.show_crosshair ? 1 : 0,
            static_cast<int>(std::lround(g.cross_x)),
            static_cast<int>(std::lround(g.cross_y)),
            static_cast<int>(std::lround(g.z_fraction * g.z_bar.h)),
            g.fill_from_top ? 1 : 0,
            g.circular ? 1 : 0,
            static_cast<int>(std::lround(g.plate.w)),
            static_cast<int>(std::lround(g.plate.h)),
            g.has_room ? 1 : 0,
            static_cast<int>(std::lround(g.plate.x))};
}

/// The draw state owned by the plate object: freed with it, so the draw hook never
/// outlives what it reads.
struct PlateView {
    helix::ToolheadPlateInput input;
    std::optional<DrawKey> drawn;
};

lv_area_t px_area(const lv_area_t& origin, float x, float y, float w, float h) {
    lv_area_t a;
    a.x1 = origin.x1 + static_cast<int32_t>(std::lround(x));
    a.y1 = origin.y1 + static_cast<int32_t>(std::lround(y));
    a.x2 = a.x1 + std::max<int32_t>(0, static_cast<int32_t>(std::lround(w)) - 1);
    a.y2 = a.y1 + std::max<int32_t>(0, static_cast<int32_t>(std::lround(h)) - 1);
    return a;
}

/// A dashed ring: lv_draw_arc has no dash, so the dashes are short arcs.
void draw_dashed_circle(lv_layer_t* layer, const lv_area_t& box, lv_color_t color, int width_px) {
    constexpr int kDashes = 24;
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width_px;
    dsc.opa = LV_OPA_COVER;
    dsc.center.x = (box.x1 + box.x2 + 1) / 2;
    dsc.center.y = (box.y1 + box.y2 + 1) / 2;
    dsc.radius = static_cast<uint16_t>((box.x2 - box.x1 + 1) / 2);
    for (int i = 0; i < kDashes; ++i) {
        dsc.start_angle = static_cast<lv_value_precise_t>(i * 360 / kDashes);
        dsc.end_angle = static_cast<lv_value_precise_t>(i * 360 / kDashes + 360 / kDashes / 2);
        lv_draw_arc(layer, &dsc);
    }
}

void draw_line(lv_layer_t* layer, float x1, float y1, float x2, float y2, lv_color_t color,
               int width_px, int dash_px = 0) {
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width_px;
    dsc.opa = LV_OPA_COVER;
    dsc.dash_width = dash_px;
    dsc.dash_gap = dash_px;
    dsc.p1.x = x1;
    dsc.p1.y = y1;
    dsc.p2.x = x2;
    dsc.p2.y = y2;
    lv_draw_line(layer, &dsc);
}

void draw_plate(lv_event_t* e) {
    auto* view = static_cast<PlateView*>(lv_event_get_user_data(e));
    lv_obj_t* obj = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!view || !obj || !layer)
        return;

    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    view->input.box_w = lv_area_get_width(&area);
    view->input.box_h = lv_area_get_height(&area);
    const auto g = helix::plan_toolhead_plate(view->input);
    if (!g.has_room)
        return;

    const lv_color_t edge = theme_manager_get_color("text_muted");
    const lv_color_t fill = theme_manager_get_color("card_bg");
    const lv_color_t accent = theme_manager_get_color("primary");
    const int line_w = std::max<int>(1, theme_manager_get_spacing("border_width"));
    const int dash = theme_manager_get_spacing("space_xs");
    const bool homed = g.show_crosshair;

    const lv_area_t plate = px_area(area, g.plate.x, g.plate.y, g.plate.w, g.plate.h);
    if (g.circular) {
        if (homed) {
            lv_draw_rect_dsc_t dsc;
            lv_draw_rect_dsc_init(&dsc);
            dsc.bg_color = fill;
            dsc.bg_opa = LV_OPA_COVER;
            dsc.border_color = edge;
            dsc.border_width = line_w;
            dsc.border_opa = LV_OPA_COVER;
            dsc.radius = LV_RADIUS_CIRCLE;
            lv_draw_rect(layer, &dsc, &plate);
        } else {
            draw_dashed_circle(layer, plate, edge, line_w);
        }
    } else if (homed) {
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = fill;
        dsc.bg_opa = LV_OPA_COVER;
        dsc.border_color = edge;
        dsc.border_width = line_w;
        dsc.border_opa = LV_OPA_COVER;
        lv_draw_rect(layer, &dsc, &plate);
    } else {
        const float l = static_cast<float>(plate.x1);
        const float t = static_cast<float>(plate.y1);
        const float r = static_cast<float>(plate.x2);
        const float b = static_cast<float>(plate.y2);
        draw_line(layer, l, t, r, t, edge, line_w, dash);
        draw_line(layer, r, t, r, b, edge, line_w, dash);
        draw_line(layer, r, b, l, b, edge, line_w, dash);
        draw_line(layer, l, b, l, t, edge, line_w, dash);
    }

    // The Z bar: an outline, filled from the bottom as the gantry rises or from the top
    // as the bed lowers.
    const lv_area_t bar = px_area(area, g.z_bar.x, g.z_bar.y, g.z_bar.w, g.z_bar.h);
    lv_draw_rect_dsc_t bar_dsc;
    lv_draw_rect_dsc_init(&bar_dsc);
    bar_dsc.bg_opa = LV_OPA_TRANSP;
    bar_dsc.border_color = edge;
    bar_dsc.border_width = line_w;
    bar_dsc.border_opa = LV_OPA_COVER;
    lv_draw_rect(layer, &bar_dsc, &bar);
    if (homed && g.z_fraction > 0.0f) {
        const float fill_h = g.z_fraction * g.z_bar.h;
        const float fill_y = g.fill_from_top ? g.z_bar.y : g.z_bar.y + g.z_bar.h - fill_h;
        const lv_area_t fill_area = px_area(area, g.z_bar.x, fill_y, g.z_bar.w, fill_h);
        lv_draw_rect_dsc_t fill_dsc;
        lv_draw_rect_dsc_init(&fill_dsc);
        fill_dsc.bg_color = accent;
        fill_dsc.bg_opa = LV_OPA_COVER;
        lv_draw_rect(layer, &fill_dsc, &fill_area);
    }

    if (homed) {
        const float cx = static_cast<float>(area.x1) + g.cross_x;
        const float cy = static_cast<float>(area.y1) + g.cross_y;
        draw_line(layer, static_cast<float>(plate.x1), cy, static_cast<float>(plate.x2), cy, accent,
                  line_w);
        draw_line(layer, cx, static_cast<float>(plate.y1), cx, static_cast<float>(plate.y2), accent,
                  line_w);
        const int r = std::max<int>(2, theme_manager_get_spacing("space_xxs"));
        lv_area_t dot;
        dot.x1 = static_cast<int32_t>(std::lround(cx)) - r;
        dot.y1 = static_cast<int32_t>(std::lround(cy)) - r;
        dot.x2 = static_cast<int32_t>(std::lround(cx)) + r;
        dot.y2 = static_cast<int32_t>(std::lround(cy)) + r;
        lv_draw_rect_dsc_t dot_dsc;
        lv_draw_rect_dsc_init(&dot_dsc);
        dot_dsc.bg_color = accent;
        dot_dsc.bg_opa = LV_OPA_COVER;
        dot_dsc.radius = LV_RADIUS_CIRCLE;
        lv_draw_rect(layer, &dot_dsc, &dot);
    }
}

void on_plate_delete(lv_event_t* e) {
    delete static_cast<PlateView*>(lv_event_get_user_data(e));
}

} // namespace

namespace helix {

struct ToolheadWidget::LiveState {
    bool printing = false;
    bool homed_all = false;
    bool homed[3] = {false, false, false};
    bool moving = false;
    int centimm[3] = {0, 0, 0};
    int velocity_mm_s = 0;
    std::string tool_label;
};

ToolheadWidget::ToolheadWidget(PrinterState& printer_state) : printer_state_(printer_state) {}

ToolheadWidget::~ToolheadWidget() {
    detach();
}

ToolheadWidget::LiveState ToolheadWidget::read_live() const {
    auto& motion = printer_state_.motion_state();
    LiveState s;
    s.printing = lv_subject_get_int(printer_state_.print_state().get_print_active_subject()) != 0;
    for (const Axis axis : kAllAxes)
        s.homed[axis_index(axis)] = axis_is_homed(printer_state_, axis);
    s.homed_all = s.homed[0] && s.homed[1] && s.homed[2];
    s.velocity_mm_s = lv_subject_get_int(motion.get_live_velocity_subject());
    s.moving = s.velocity_mm_s > 0;
    lv_subject_t* live[3] = {motion.get_live_position_x_subject(),
                             motion.get_live_position_y_subject(),
                             motion.get_live_position_z_subject()};
    lv_subject_t* resting[3] = {motion.get_position_x_subject(), motion.get_position_y_subject(),
                                motion.get_position_z_subject()};
    for (int i = 0; i < 3; ++i) {
        s.centimm[i] = toolhead_position_centimm(s.moving, lv_subject_get_int(live[i]),
                                                 lv_subject_get_int(resting[i]));
    }
    auto& tools = ToolState::instance();
    // Several tools name a tool changer's active tool; AMS slots feeding one hotend are
    // tools too and need no label.
    if (tools.is_multi_tool() && (tools.has_multiple_extruders() || !tools.ams_topology_active())) {
        const auto* active = tools.active_tool();
        s.tool_label = active ? active->display_label
                              : ui::lane_label(ui::active_tool_noun(),
                                               lv_subject_get_int(tools.get_active_tool_subject()));
    }
    return s;
}

ToolheadVerdict ToolheadWidget::decide(int width_px, int height_px, bool printing,
                                       bool homed) const {
    // The tree's own padding where it exists; before it does (the load path asks
    // fits_at then) the root's padding on each side.
    const ContentChrome chrome = content_chrome_or_estimate(root(), "space_xxs", "space_xxs");
    ToolheadInput in;
    in.content_w = width_px - chrome.w;
    in.content_h = height_px - chrome.h;
    in.printing = printing;
    in.homed = homed;
    in.needs = measure_needs();
    const auto& n = in.needs;
    spdlog::trace("[ToolheadWidget] box {}x{} chrome {}x{} needs g{}x{} s{}x{} c{}x{} cell{}x{} "
                  "w{}x{} f{}x{}",
                  width_px, height_px, chrome.w, chrome.h, n.glance.w, n.glance.h, n.strip.w,
                  n.strip.h, n.column.w, n.column.h, n.cell.w, n.cell.h, n.wide.w, n.wide.h,
                  n.full.w, n.full.h);
    return decide_toolhead_layout(in);
}

void ToolheadWidget::resolve_bed() {
    IMoonrakerAPI* api = get_moonraker_api();
    bed_ = bed_dimensions(api, &printer_state_);
    circular_ = api && circular_bed_kinematics(api->hardware().kinematics());
}

void ToolheadWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    s_tile.claim(this);
    install_delete_hook(widget_obj);

    plate_ = lv_obj_find_by_name(widget_obj, "toolhead_plate");
    if (plate_ && !lv_obj_get_user_data(plate_)) {
        auto* view = new PlateView();
        lv_obj_set_user_data(plate_, view);
        // DRAW_MAIN_END overlays the plate on the object's own pass. DRAW_TASK_ADDED
        // fires after the draw tasks are queued and cannot add any.
        lv_obj_add_event_cb(plate_, draw_plate, LV_EVENT_DRAW_MAIN_END, view);
        lv_obj_add_event_cb(plate_, on_plate_delete, LV_EVENT_DELETE, view);
    }

    auto& motion = printer_state_.motion_state();
    const auto lifetime = printer_state_.get_subjects_lifetime();
    const auto token = lifetime_.token();
    const auto on_position = [token](ToolheadWidget* self, int) {
        if (token.expired() || !self->uses_positions())
            return;
        self->schedule_refresh();
    };
    lv_subject_t* position_subjects[6] = {
        motion.get_position_x_subject(),      motion.get_position_y_subject(),
        motion.get_position_z_subject(),      motion.get_live_position_x_subject(),
        motion.get_live_position_y_subject(), motion.get_live_position_z_subject()};
    for (int i = 0; i < 6; ++i) {
        position_observers_[i] =
            ui::observe<int>(position_subjects[i], this, on_position, lifetime);
    }
    homed_observer_ = ui::observe<const char*>(
        motion.get_homed_axes_subject(), this,
        [token](ToolheadWidget* self, const char*) {
            if (token.expired())
                return;
            self->on_state_changed();
        },
        lifetime);
    print_active_observer_ = ui::observe<int>(
        printer_state_.print_state().get_print_active_subject(), this,
        [token](ToolheadWidget* self, int) {
            if (token.expired())
                return;
            self->on_state_changed();
        },
        lifetime);
    // Velocity ticks arrive several times a second. They matter only when the nozzle
    // starts or stops moving (the position source flips) or while the speed is on screen.
    velocity_observer_ = ui::observe<int>(
        motion.get_live_velocity_subject(), this,
        [token](ToolheadWidget* self, int velocity) {
            if (token.expired())
                return;
            const bool moving = velocity > 0;
            const bool flipped = moving != self->moving_;
            self->moving_ = moving;
            if ((flipped && self->uses_positions()) || self->verdict_.show_status_row)
                self->schedule_refresh();
        },
        lifetime);
    const auto on_slow_change = [token](ToolheadWidget* self, int) {
        if (token.expired())
            return;
        self->resolve_bed();
        self->schedule_refresh();
    };
    bed_moves_observer_ =
        ui::observe<int>(printer_state_.capabilities_state().subject(Capability::BedMoves), this,
                         on_slow_change, lifetime);
    if (IMoonrakerAPI* api = get_moonraker_api()) {
        // The version subject lives as long as the API, which outlives every tile.
        build_volume_observer_ = ui::observe<int>(api->get_build_volume_version_subject(), this,
                                                  on_slow_change, subject_never_freed());
    }
    auto& tools = ToolState::instance();
    const auto tools_lifetime = tools.get_subjects_lifetime();
    const auto on_tool = [token](ToolheadWidget* self, int) {
        if (token.expired() || !self->verdict_.show_status_row)
            return;
        self->schedule_refresh();
    };
    tools_version_observer_ =
        ui::observe<int>(tools.get_tools_version_subject(), this, on_tool, tools_lifetime);
    active_tool_observer_ =
        ui::observe<int>(tools.get_active_tool_subject(), this, on_tool, tools_lifetime);

    resolve_bed();
    // Instances are recycled across rebuilds: the verdict is republished here too.
    relayout_for_granted_size();
    publish_state(read_live());
}

void ToolheadWidget::detach() {
    lifetime_.invalidate();
    refresh_timer_.cancel();
    homed_observer_.reset();
    print_active_observer_.reset();
    for (auto& guard : position_observers_)
        guard.reset();
    velocity_observer_.reset();
    bed_moves_observer_.reset();
    build_volume_observer_.reset();
    tools_version_observer_.reset();
    active_tool_observer_.reset();
    if (ToolheadWidget* displaced = s_tile.release(this)) {
        displaced->relayout_for_granted_size();
        displaced->publish_state(displaced->read_live());
        if (PlateView* view = displaced->plate_
                                  ? static_cast<PlateView*>(lv_obj_get_user_data(displaced->plate_))
                                  : nullptr) {
            view->drawn.reset();
        }
    }
    plate_ = nullptr;
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void ToolheadWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int width_px,
                                     int height_px) {
    const LiveState live = read_live();
    verdict_ = decide(width_px, height_px, live.printing, live.homed_all);
    decided_printing_ = live.printing;
    decided_homed_ = live.homed_all;
    if (!s_tile.is_owner(this))
        return;
    publish_state(live);
    // The canvas draws from state captured at refresh time; a new rung or size needs it.
    if (verdict_.show_plate)
        schedule_refresh();

    spdlog::debug("[ToolheadWidget] on_size_changed {}x{} rung={} letters={} z_only={} rows={} "
                  "plate={} home={} status={} fits={} printing={} homed={}",
                  width_px, height_px, static_cast<int>(verdict_.rung), verdict_.show_letters,
                  verdict_.show_z_only, verdict_.show_rows, verdict_.show_plate,
                  verdict_.show_home_button, verdict_.show_status_row, verdict_.fits, live.printing,
                  live.homed_all);
}

bool ToolheadWidget::fits_at(int width_px, int height_px) const {
    // Refuse a size that cannot draw the Glance content. The boxes do not depend on
    // the printer's state, so one probe decides.
    return decide(width_px, height_px, false, false).fits;
}

void ToolheadWidget::publish_state(const LiveState& live) {
    if (!s_tile.is_owner(this))
        return;
    set_int_if_changed(&s_rung_subject, static_cast<int>(verdict_.rung));
    set_int_if_changed(&s_show_letters_subject, verdict_.show_letters ? 1 : 0);
    set_int_if_changed(&s_z_only_subject, verdict_.show_z_only ? 1 : 0);
    set_int_if_changed(&s_show_rows_subject, verdict_.show_rows ? 1 : 0);
    set_int_if_changed(&s_show_plate_subject, verdict_.show_plate ? 1 : 0);
    set_int_if_changed(&s_show_home_subject, verdict_.show_home_button ? 1 : 0);
    set_int_if_changed(&s_show_status_subject, verdict_.show_status_row ? 1 : 0);
    for (int i = 0; i < 3; ++i)
        set_int_if_changed(&s_homed_subjects[i], live.homed[i] ? 1 : 0);
    publish_values(live);
}

void ToolheadWidget::publish_values(const LiveState& live) {
    if (verdict_.show_rows || verdict_.show_plate) {
        copy_string_if_changed(
            &s_x_text_subject,
            std::string(format_toolhead_axis(Axis::X, live.centimm[0], live.homed[0],
                                             verdict_.compact_values))
                .c_str());
        copy_string_if_changed(
            &s_y_text_subject,
            std::string(format_toolhead_axis(Axis::Y, live.centimm[1], live.homed[1],
                                             verdict_.compact_values))
                .c_str());
    }
    if (verdict_.show_rows || verdict_.show_plate || verdict_.show_z_only) {
        copy_string_if_changed(
            &s_z_text_subject,
            std::string(format_toolhead_axis(Axis::Z, live.centimm[2], live.homed[2],
                                             verdict_.compact_values))
                .c_str());
    }
    if (verdict_.show_status_row) {
        copy_string_if_changed(&s_tool_text_subject, live.tool_label.c_str());
        char speed[24];
        std::snprintf(speed, sizeof(speed), lv_tr("%d mm/s"), live.velocity_mm_s);
        copy_string_if_changed(&s_speed_text_subject, speed);
    }
}

bool ToolheadWidget::uses_positions() const {
    return verdict_.show_rows || verdict_.show_plate || verdict_.show_z_only;
}

bool ToolheadWidget::plate_visible() const {
    return plate_ && verdict_.show_plate && lv_obj_get_user_data(plate_) != nullptr;
}

void ToolheadWidget::on_state_changed() {
    const LiveState live = read_live();
    if (live.printing != decided_printing_ || live.homed_all != decided_homed_) {
        relayout_for_granted_size();
    }
    if (s_tile.is_owner(this)) {
        publish_state(live);
        schedule_refresh();
    }
}

void ToolheadWidget::schedule_refresh() {
    if (!s_tile.is_owner(this) || panel_hidden_)
        return;
    refresh_timer_.schedule_once([this]() { refresh(); });
}

void ToolheadWidget::refresh() {
    if (!s_tile.is_owner(this) || panel_hidden_)
        return;
    const LiveState live = read_live();
    publish_values(live);
    if (!plate_visible())
        return;
    auto* view = static_cast<PlateView*>(lv_obj_get_user_data(plate_));
    const int w = lv_obj_get_width(plate_);
    const int h = lv_obj_get_height(plate_);
    view->input = plate_input(live, w, h);
    const auto key = draw_key(plan_toolhead_plate(view->input));
    if (view->drawn != key) {
        view->drawn = key;
        lv_obj_invalidate(plate_);
    }
}

ToolheadPlateInput ToolheadWidget::plate_input(const LiveState& live, int box_w, int box_h) const {
    ToolheadPlateInput in;
    in.box_w = box_w;
    in.box_h = box_h;
    in.inset_px = theme_manager_get_spacing("space_xxs");
    in.bar_w_px = theme_manager_get_spacing("space_xs");
    in.gap_px = theme_manager_get_spacing("space_xs");
    in.bed = bed_;
    in.circular = circular_;
    in.bed_moves =
        lv_subject_get_int(printer_state_.capabilities_state().subject(Capability::BedMoves)) != 0;
    in.homed = live.homed_all;
    in.align_start = verdict_.rung == SizeRung::Wide;
    in.x_mm = static_cast<float>(units::from_centimm(live.centimm[0]));
    in.y_mm = static_cast<float>(units::from_centimm(live.centimm[1]));
    in.z_mm = static_cast<float>(units::from_centimm(live.centimm[2]));
    const AxisBounds bounds = printer_state_.motion_state().get_axis_bounds();
    in.has_z_range = bounds.has_z;
    in.z_min_mm = bounds.z_min;
    in.z_max_mm = bounds.z_max;
    return in;
}

std::optional<ToolheadPlateInput> ToolheadWidget::drawn_plate_input() const {
    if (!plate_)
        return std::nullopt;
    const auto* view = static_cast<const PlateView*>(lv_obj_get_user_data(plate_));
    if (!view)
        return std::nullopt;
    return view->input;
}

void ToolheadWidget::on_activate() {
    panel_hidden_ = false;
    schedule_refresh();
}

void ToolheadWidget::on_deactivate() {
    panel_hidden_ = true;
    if (plate_) {
        if (auto* view = static_cast<PlateView*>(lv_obj_get_user_data(plate_)))
            view->drawn.reset();
    }
    refresh_timer_.cancel();
}

std::optional<ToolheadPlateGeometry> ToolheadWidget::plate_geometry() const {
    if (!plate_visible())
        return std::nullopt;
    return plan_toolhead_plate(
        plate_input(read_live(), lv_obj_get_width(plate_), lv_obj_get_height(plate_)));
}

void ToolheadWidget::home_all() {
    if (!s_tile.is_owner(this) || !verdict_.show_home_button)
        return;
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        NOTIFY_WARNING(lv_tr("Printer connection unavailable"));
        return;
    }
    NOTIFY_INFO(lv_tr("Homing all axes..."));
    api->motion().home_axes(
        "",
        lifetime_.bg_cb("ToolheadWidget::home_ok",
                        []() { NOTIFY_SUCCESS(lv_tr("Homing complete")); }),
        lifetime_.bg_cb("ToolheadWidget::home_error", [](const MoonrakerError& err) {
            NOTIFY_ERROR(lv_tr("Homing failed: {}"), err.localized_message());
        }));
}

void ToolheadWidget::open_motion_panel() {
    get_global_motion_panel().show(parent_screen_);
}

void ToolheadWidget::clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ToolheadWidget] clicked_cb");
    if (auto* self = panel_widget_from_event<ToolheadWidget>(e)) {
        self->record_interaction();
        self->open_motion_panel();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void register_toolhead_widget() {
    register_widget_factory("toolhead", [](const std::string&) {
        return std::make_unique<ToolheadWidget>(get_printer_state());
    });
    register_widget_subjects("toolhead", toolhead_init_subjects);

    lv_xml_register_event_cb(nullptr, "toolhead_clicked_cb", ToolheadWidget::clicked_cb);
    lv_xml_register_event_cb(nullptr, "toolhead_home_all_cb", [](lv_event_t* e) {
        LVGL_SAFE_EVENT_CB_BEGIN("[ToolheadWidget] home_all_cb");
        // A button tap is not a tap on the tile, which opens the Motion panel.
        lv_event_stop_bubbling(e);
        if (auto* owner = s_tile.owner())
            owner->home_all();
        LVGL_SAFE_EVENT_CB_END();
    });
}

} // namespace helix
