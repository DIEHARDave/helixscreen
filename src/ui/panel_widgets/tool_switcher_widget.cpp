// SPDX-License-Identifier: GPL-3.0-or-later

#include "tool_switcher_widget.h"

#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_subject_registry.h"
#include "ui_temperature_utils.h"
#include "ui_utils.h"

#include "ams_error.h"
#include "ams_state.h"
#include "app_globals.h"
#include "filament_op_slot_resolver.h"
#include "helix/ui/text_metrics.h"
#include "helix/xml/indexed_subject_pool.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "static_subject_registry.h"
#include "theme_manager.h"
#include "tool_state.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace {

using helix::xml::IndexedSubjectPool;

// The published verdict and per-head data, read by panel_widget_tool_switcher.xml
// and the picker. Module-level, not members: they belong to the widget KIND,
// rows read the CURRENT values the moment a <repeat> creates them, and
// registration rides SubjectInitializer's panel-subject phase so they exist
// before any XML that binds them is parsed. Mode and label form are the
// ToolSwitcherMode and ToolLabelForm enum values.
lv_subject_t s_mode_subject{};
lv_subject_t s_cols_subject{};
lv_subject_t s_rows_subject{};
lv_subject_t s_cell_w_subject{};
lv_subject_t s_cell_h_subject{};
lv_subject_t s_rail_h_subject{};
lv_subject_t s_detail_subject{};
lv_subject_t s_detail_rows_subject{};
lv_subject_t s_label_form_subject{};
lv_subject_t s_park_supported_subject{};
lv_subject_t s_change_blocked_subject{};
lv_subject_t s_picker_code_form_subject{};
lv_subject_t s_active_label_subject{};
char s_active_label_buf[32] = {};
bool s_subjects_initialized = false;
SubjectManager s_subjects;

// Per head, indexed by tool: tool_switcher_seg_<i>_label and so on.
IndexedSubjectPool s_seg_label{"tool_switcher_seg", IndexedSubjectPool::Type::String, 48, "_label"};
IndexedSubjectPool s_seg_sub{"tool_switcher_seg", IndexedSubjectPool::Type::String, 48, "_sub"};
IndexedSubjectPool s_seg_color{"tool_switcher_seg", IndexedSubjectPool::Type::Color, 0, "_color"};
IndexedSubjectPool s_seg_has_color{"tool_switcher_seg", IndexedSubjectPool::Type::Int, 0,
                                   "_has_color"};
// Per detail row, in display order (tool_switcher_detail_order()).
IndexedSubjectPool s_detail_text{"tool_switcher_detail", IndexedSubjectPool::Type::String, 96,
                                 "_text"};
IndexedSubjectPool s_detail_color{"tool_switcher_detail", IndexedSubjectPool::Type::Color, 0,
                                  "_color"};
IndexedSubjectPool s_detail_has_color{"tool_switcher_detail", IndexedSubjectPool::Type::Int, 0,
                                      "_has_color"};
IndexedSubjectPool s_detail_hot{"tool_switcher_detail", IndexedSubjectPool::Type::Int, 0, "_hot"};
IndexedSubjectPool s_detail_main{"tool_switcher_detail", IndexedSubjectPool::Type::Int, 0, "_main"};

IndexedSubjectPool* const s_pools[] = {&s_seg_label,        &s_seg_sub,     &s_seg_color,
                                       &s_seg_has_color,    &s_detail_text, &s_detail_color,
                                       &s_detail_has_color, &s_detail_hot,  &s_detail_main};

void init_int(lv_subject_t& subject, const char* name) {
    lv_subject_init_int(&subject, 0);
    s_subjects.publish(name, &subject);
}

void tool_switcher_widget_init_subjects() {
    if (s_subjects_initialized)
        return;

    init_int(s_mode_subject, "tool_switcher_mode");
    lv_subject_set_int(&s_mode_subject, static_cast<int>(helix::ToolSwitcherMode::Single));
    init_int(s_cols_subject, "tool_switcher_cols");
    init_int(s_rows_subject, "tool_switcher_rows");
    init_int(s_cell_w_subject, "tool_switcher_cell_w");
    init_int(s_cell_h_subject, "tool_switcher_cell_h");
    init_int(s_rail_h_subject, "tool_switcher_rail_h");
    init_int(s_detail_subject, "tool_switcher_detail");
    init_int(s_detail_rows_subject, "tool_switcher_detail_rows");
    init_int(s_label_form_subject, "tool_switcher_label_form");
    init_int(s_park_supported_subject, "tool_switcher_park_supported");
    init_int(s_change_blocked_subject, "tool_switcher_change_blocked");
    init_int(s_picker_code_form_subject, "tool_switcher_picker_code_form");
    lv_subject_init_string(&s_active_label_subject, s_active_label_buf, nullptr,
                           sizeof(s_active_label_buf), "");
    s_subjects.publish("tool_switcher_active_label", &s_active_label_subject);
    // Every slot exists before a row can bind it, whatever the head count.
    for (IndexedSubjectPool* pool : s_pools) {
        pool->ensure_size(helix::ToolSwitcherWidget::kPoolSize);
    }
    s_subjects_initialized = true;

    StaticSubjectRegistry::instance().register_deinit("ToolSwitcherWidgetSubjects", []() {
        if (s_subjects_initialized && lv_is_initialized()) {
            s_subjects.deinit_all();
            for (IndexedSubjectPool* pool : s_pools) {
                pool->reclaim();
            }
            s_subjects_initialized = false;
        }
    });
}

void set_int_if_changed(lv_subject_t* subject, int value) {
    if (lv_subject_get_int(subject) != value) {
        lv_subject_set_int(subject, value);
    }
}

void set_color_if_changed(lv_subject_t* subject, uint32_t rgb) {
    if ((lv_color_to_u32(lv_subject_get_color(subject)) & 0xFFFFFF) != rgb) {
        lv_subject_set_color(subject, lv_color_hex(rgb));
    }
}

/// Resolve a responsive token to pixels, with a fallback.
int resolve_px_token(const char* name, int fallback) {
    const char* s = lv_xml_get_const(nullptr, name);
    return s ? std::atoi(s) : fallback;
}

/// Pixels between adjacent rail cells.
constexpr int kRailGap = 2;

/// A head count no box can lay out as a rail: the decision then picks the
/// face, whose sheet lists every head.
constexpr int kUnrailableHeads = 1 << 20;

/// What a head is loaded with, as far as the filament system knows.
struct HeadFilament {
    bool has_color = false;
    uint32_t color_rgb = 0;
    std::string material;
};

/// A head's filament from its own slot. An empty slot keeps reporting the
/// colour of whatever was in it, and the default grey is no colour at all, so
/// neither counts as known: the strip is omitted rather than drawn grey.
HeadFilament head_filament(const helix::ToolInfo& tool) {
    HeadFilament out;
    if (tool.backend_index < 0 || tool.backend_slot < 0)
        return out;
    const helix::AmsBackend* backend = helix::AmsState::instance().get_backend(tool.backend_index);
    if (!backend)
        return out;
    const helix::SlotInfo slot = backend->get_slot_info(tool.backend_slot);
    if (slot.status == helix::SlotStatus::EMPTY)
        return out;
    out.has_color = slot.color_rgb != helix::AMS_DEFAULT_SLOT_COLOR;
    out.color_rgb = slot.color_rgb;
    out.material = slot.material;
    return out;
}

/// Heads at or above this read hot (decidegrees).
constexpr int kHotDeci = 1500;

} // namespace

namespace helix {

// Static instance for event callback routing
ToolSwitcherWidget* ToolSwitcherWidget::s_active_instance = nullptr;

void register_tool_switcher_widget() {
    register_widget_factory("tool_switcher", [](const std::string&) {
        auto& ps = get_printer_state();
        return std::make_unique<ToolSwitcherWidget>(ps);
    });
    register_widget_subjects("tool_switcher", tool_switcher_widget_init_subjects);

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "tool_compact_cb", ToolSwitcherWidget::tool_compact_cb);
    lv_xml_register_event_cb(nullptr, "tool_switcher_park_cb", ToolSwitcherWidget::tool_park_cb);
}

ToolSwitcherWidget::ToolSwitcherWidget(PrinterState& printer_state)
    : TiledPanelWidget("tool_switcher", TileSizing::Content{"", "", "", true}),
      printer_state_(printer_state) {
    // A widget built outside the panel-subject phase still publishes into live
    // subjects. A no-op once they exist.
    tool_switcher_widget_init_subjects();
    refresh_label_budget();
}

std::string ToolSwitcherWidget::tool_text(const ToolInfo& tool, ui::ToolLabelForm form) {
    return ui::tool_label_in_form(ui::active_tool_noun(), tool.name, tool.index, form);
}

void ToolSwitcherWidget::refresh_label_budget() {
    // The compact readout is the form for a box with no room, so it always
    // reads in the shortest one.
    std::string widest;
    for (const auto& tool : ToolState::instance().tools()) {
        std::string text = tool_text(tool, ui::ToolLabelForm::Code);
        if (text.size() > widest.size()) {
            widest = std::move(text);
        }
    }
    sizing_.set_content({widest, widest, "", true});
}

ToolSwitcherWidget::~ToolSwitcherWidget() {
    if (s_active_instance == this) {
        s_active_instance = nullptr;
    }
}

void ToolSwitcherWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    install_delete_hook(widget_obj);
    s_active_instance = this;

    auto& tool_state = ToolState::instance();
    auto token = lifetime_.token();

    active_tool_observer_ = helix::ui::observe<int>(
        tool_state.get_active_tool_subject(), this,
        [token](ToolSwitcherWidget* self, int tool) {
            if (token.expired())
                return;
            self->on_active_tool_changed(tool);
        },
        tool_state.get_subjects_lifetime());

    // A new head count changes the mode, the label budget and every pool, so
    // it re-runs the whole size path at the size the tile holds.
    tool_count_observer_ = helix::ui::observe<int>(
        tool_state.get_tool_count_subject(), this,
        [token](ToolSwitcherWidget* self, int /*count*/) {
            if (token.expired())
                return;
            self->rebuild_head_temps();
            self->on_size_changed(0, 0, self->current_width_px_, self->current_height_px_);
        },
        tool_state.get_subjects_lifetime());

    // Re-publish the gate on every print-state transition. PanelWidget
    // instances are RECYCLED across home-panel rebuilds, so registering here
    // (rather than only reacting to on_size_changed) is what keeps a reused
    // instance from carrying the previous screen's gating. print_lifecycle
    // rather than print_state_enum: the gate refuses during Preparing, and the
    // raw enum does not move on the Idle -> Preparing edge.
    //
    // Takes the lifetime token. print_lifecycle is one of PrinterPrintState's
    // static subjects, torn down by deinit_subjects() between test cases, and an
    // ObserverGuard that outlives that cycle calls lv_observer_remove() on freed
    // memory (#705).
    print_state_observer_ = helix::ui::observe<int>(
        printer_state_.print_state().get_print_lifecycle_subject(), this,
        [token](ToolSwitcherWidget* self, int /*state*/) {
            if (token.expired())
                return;
            self->refresh_print_gating();
        },
        printer_state_.print_state().get_static_subjects_lifetime());

    // A slot's colour or material changing recolours its head.
    slots_observer_ = helix::ui::observe<int>(
        AmsState::instance().get_slots_version_subject(), this,
        [token](ToolSwitcherWidget* self, int /*version*/) {
            if (token.expired())
                return;
            self->publish_heads();
        },
        AmsState::instance().get_subjects_lifetime());

    // Extruders discovered after the tools hand the heads their temperatures.
    extruders_observer_ = helix::ui::observe<int>(
        printer_state_.temperature_state().get_extruder_version_subject(), this,
        [token](ToolSwitcherWidget* self, int /*version*/) {
            if (token.expired())
                return;
            self->rebuild_head_temps();
            self->publish_heads();
        },
        printer_state_.temperature_state().get_subjects_lifetime());

    rebuild_head_temps();
    // A recycled instance keeps the size it was last granted.
    publish();
}

void ToolSwitcherWidget::detach() {
    lifetime_.invalidate();
    picker_.hide();
    active_tool_observer_.reset();
    tool_count_observer_.reset();
    print_state_observer_.reset();
    slots_observer_.reset();
    extruders_observer_.reset();
    head_temps_.clear();
    uninstall_delete_hook();
    forget_tile_widgets();
    if (s_active_instance == this) {
        s_active_instance = nullptr;
    }
}

void ToolSwitcherWidget::on_hooked_root_deleted() {
    // Runs inside LVGL's delete event: expire the pending deferred observer
    // callbacks and drop the cached pointers only. The observers themselves
    // stay registered on their (still live) subjects until detach() or the
    // destructor resets them — every callback checks its token first.
    lifetime_.invalidate();
    forget_tile_widgets();
}

void ToolSwitcherWidget::forget_tile_widgets() {
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void ToolSwitcherWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int width_px,
                                         int height_px) {
    current_width_px_ = width_px;
    current_height_px_ = height_px;

    refresh_label_budget();
    sizing_.measure_and_publish(width_px, height_px);
    publish();
}

void ToolSwitcherWidget::publish() {
    refresh_park_support();
    refresh_print_gating();
    publish_layout();
    publish_heads();
}

void ToolSwitcherWidget::publish_layout() {
    const auto& tools = ToolState::instance().tools();
    const int count = static_cast<int>(tools.size());
    const int inset = resolve_px_token("space_md", 10);
    const int min_target = resolve_px_token("touch_target_min", 48);
    const lv_font_t* body = theme_manager_get_font("font_body");
    const lv_font_t* small = theme_manager_get_font("font_small");
    const int body_px = body ? lv_font_get_line_height(body) : 0;
    const int small_px = small ? lv_font_get_line_height(small) : 0;
    const int content_w = current_width_px_ - 2 * inset;
    const int content_h = current_height_px_ - 2 * inset;

    // The rail draws a segment per head from pools of kPoolSize, so a printer
    // with more heads than that gets the face and reaches them in the sheet.
    const int decided_heads = count > kPoolSize ? kUnrailableHeads : count;
    layout_ = decide_tool_switcher_layout(decided_heads, content_w, content_h, min_target, kRailGap,
                                          body_px, small_px);
    const bool rail =
        layout_.mode == ToolSwitcherMode::RailWithPark || layout_.mode == ToolSwitcherMode::Rail;

    // A segment's label has its cell less space_xs on each side. Rail
    // segments read "Tool 1" over "T0" where every head's two lines fit, else
    // "T0"; the face reads "Tool 1 (T0)" where every head's fits, else "T0".
    const int label_room = layout_.cell_w - 2 * resolve_px_token("space_xs", 4);
    label_form_ = ui::pick_tool_label_form([&](ui::ToolLabelForm form) {
        for (const auto& tool : tools) {
            const std::string code = tool_text(tool, ui::ToolLabelForm::Code);
            int widest = 0;
            if (form == ui::ToolLabelForm::Code) {
                widest = ui::text_width(code.c_str(), body);
            } else if (rail) {
                widest = std::max(ui::text_width(tool.display_label.c_str(), body),
                                  ui::text_width(code.c_str(), body));
            } else {
                widest = ui::text_width(tool_text(tool, form).c_str(), body);
            }
            if (widest > label_room)
                return false;
        }
        return true;
    });

    set_int_if_changed(&s_mode_subject, static_cast<int>(layout_.mode));
    set_int_if_changed(&s_cols_subject, layout_.cols);
    set_int_if_changed(&s_rows_subject, layout_.rows);
    set_int_if_changed(&s_cell_w_subject, layout_.cell_w);
    set_int_if_changed(&s_cell_h_subject, layout_.cell_h);
    set_int_if_changed(&s_rail_h_subject, layout_.rail_h);
    set_int_if_changed(&s_detail_subject, layout_.detail ? 1 : 0);
    set_int_if_changed(&s_label_form_subject, static_cast<int>(label_form_));

    spdlog::debug("[ToolSwitcher] {}x{} box, {} heads: mode {} {}x{} cells {}x{} rail_h {} "
                  "detail {} form {}",
                  content_w, content_h, count, static_cast<int>(layout_.mode), layout_.cols,
                  layout_.rows, layout_.cell_w, layout_.cell_h, layout_.rail_h, layout_.detail,
                  static_cast<int>(label_form_));
}

void ToolSwitcherWidget::rebuild_head_temps() {
    head_temps_.clear();
    const auto& tools = ToolState::instance().tools();
    const size_t heads = std::min(tools.size(), static_cast<size_t>(kPoolSize));
    head_temps_.resize(heads);
    auto token = lifetime_.token();
    for (size_t i = 0; i < heads; ++i) {
        if (!tools[i].extruder_name)
            continue;
        HeadTemp& head = head_temps_[i];
        lv_subject_t* subject = printer_state_.temperature_state().get_extruder_temp_subject(
            *tools[i].extruder_name, head.lifetime);
        if (!subject)
            continue;
        head.deci = lv_subject_get_int(subject);
        head.known = true;
        head.observer = helix::ui::observe<int>(
            subject, this,
            [token, i](ToolSwitcherWidget* self, int deci) {
                if (token.expired() || i >= self->head_temps_.size())
                    return;
                self->head_temps_[i].deci = deci;
                self->publish_heads();
            },
            head.lifetime);
    }
}

void ToolSwitcherWidget::publish_heads() {
    if (!s_subjects_initialized)
        return;
    auto& tool_state = ToolState::instance();
    const auto& tools = tool_state.tools();
    const int count = static_cast<int>(tools.size());
    const int heads = std::min(count, kPoolSize);
    const int active = tool_state.active_tool_index();

    auto temp_text = [this](int i) -> std::string {
        if (i < 0 || i >= static_cast<int>(head_temps_.size()) || !head_temps_[i].known)
            return {};
        char buf[16];
        return ui::temperature::format_temperature(
            ui::temperature::deci_to_degrees(head_temps_[i].deci), buf, sizeof(buf));
    };
    auto is_hot = [this](int i) {
        return i >= 0 && i < static_cast<int>(head_temps_.size()) && head_temps_[i].known &&
               head_temps_[i].deci >= kHotDeci;
    };

    std::vector<HeadFilament> filament(static_cast<size_t>(heads));
    for (int i = 0; i < heads; ++i) {
        filament[i] = head_filament(tools[i]);

        // Full: the physical label over the G-code name, which a configured
        // name ("Left") does not have. Code: the G-code name over the
        // temperature, shown where the segment is tall enough.
        const std::string code = tool_text(tools[i], ui::ToolLabelForm::Code);
        std::string label = code;
        std::string sub = temp_text(i);
        if (label_form_ == ui::ToolLabelForm::Full) {
            label = tools[i].display_label;
            sub = code == label ? std::string() : code;
        }
        ui::copy_string_if_changed(s_seg_label.at(i), label.c_str());
        ui::copy_string_if_changed(s_seg_sub.at(i), sub.c_str());
        set_int_if_changed(s_seg_has_color.at(i), filament[i].has_color ? 1 : 0);
        if (filament[i].has_color) {
            set_color_if_changed(s_seg_color.at(i), filament[i].color_rgb);
        }
    }

    // Detail rows, mounted head first. The mounted head's line is in the body
    // font and reads "Tool 2 (T1) · PLA · 215°C" where the content box is wide
    // enough, else "T1 · ..."; the rest are small and always read "T0 · ...".
    const std::vector<int> order = tool_switcher_detail_order(heads, active);
    const int rows = std::min(static_cast<int>(order.size()), kPoolSize);
    const int content_w = current_width_px_ - 2 * resolve_px_token("space_md", 10);
    const lv_font_t* body = theme_manager_get_font("font_body");
    auto line = [&](int i, ui::ToolLabelForm form) {
        std::string text = tool_text(tools[i], form);
        for (const std::string& part : {filament[i].material, temp_text(i)}) {
            if (!part.empty())
                text += " · " + part;
        }
        return text;
    };
    for (int r = 0; r < rows; ++r) {
        const int i = order[r];
        const bool main = r == 0 && (i == active || i == kToolSwitcherEmptyCarriageRow);
        std::string text;
        if (i == kToolSwitcherEmptyCarriageRow) {
            text = lv_tr("Carriage empty · tap a head");
        } else if (main) {
            text = line(i, ui::ToolLabelForm::Full);
            if (ui::text_width(text.c_str(), body) > content_w)
                text = line(i, ui::ToolLabelForm::Code);
        } else {
            text = line(i, ui::ToolLabelForm::Code);
        }
        const bool has_color = i >= 0 && filament[i].has_color;
        ui::copy_string_if_changed(s_detail_text.at(r), text.c_str());
        set_int_if_changed(s_detail_main.at(r), main ? 1 : 0);
        set_int_if_changed(s_detail_hot.at(r), is_hot(i) ? 1 : 0);
        set_int_if_changed(s_detail_has_color.at(r), has_color ? 1 : 0);
        if (has_color) {
            set_color_if_changed(s_detail_color.at(r), filament[i].color_rgb);
        }
    }

    int fit = 0;
    if (layout_.detail) {
        const lv_font_t* small = theme_manager_get_font("font_small");
        const int content_h = current_height_px_ - 2 * resolve_px_token("space_md", 10);
        fit = tool_switcher_detail_rows_that_fit(rows, content_h - layout_.rail_h - kRailGap,
                                                 body ? lv_font_get_line_height(body) : 0,
                                                 small ? lv_font_get_line_height(small) : 0);
    }
    set_int_if_changed(&s_detail_rows_subject, fit);

    // An out-of-range active index has no position to name. "?" keeps the one
    // line the compact readout carries visibly occupied, where an empty label
    // reads as a widget that failed to draw.
    const std::string compact =
        (active >= 0 && active < count) ? tool_text(tools[active], ui::ToolLabelForm::Code) : "?";
    ui::copy_string_if_changed(&s_active_label_subject, compact.c_str());
}

void ToolSwitcherWidget::on_active_tool_changed(int tool_index) {
    publish_heads();
    spdlog::debug("[ToolSwitcher] Active tool changed to T{}", tool_index);
}

// ============================================================================
// Tool picker popup (for compact mode)
// ============================================================================

void ToolSwitcherWidget::show_tool_picker() {
    if (picker_.is_visible() || !parent_screen_ || !widget_obj_) {
        return;
    }

    // Compact mode's only affordance is this picker, so refuse before opening a
    // list in which every entry is a guaranteed-failure dead end.
    const AmsError refusal = tool_change_refusal();
    if (!refusal.success()) {
        spdlog::info("[ToolSwitcher] Picker refused: {}", refusal.technical_msg);
        helix::ui::notify_ams_warning(refusal);
        return;
    }

    if (ToolState::instance().tools().empty()) {
        return;
    }

    refresh_park_support();

    // The card hangs off the widget tile's left edge, so the tool names line up
    // with the compact readout they replace.
    picker_.show_below_widget(parent_screen_, widget_obj_,
                              helix::ui::ContextMenu::AnchorAlign::Left);
}

void ToolSwitcherWidget::ToolPicker::on_created(lv_obj_t* backdrop) {
    lv_obj_t* tool_list = lv_obj_find_by_name(backdrop, "tool_list");
    if (!tool_list) {
        spdlog::error("[ToolSwitcher] tool_list not found in picker XML");
        return;
    }

    // The card is as wide as the widget tile it hangs off, so the buttons inside
    // it line up with the compact readout. Set before the rows are built: they are
    // width="100%" and cannot resolve against a width="content" card.
    if (lv_obj_t* menu_card = card()) {
        lv_obj_set_width(menu_card, lv_obj_get_width(owner_.widget_obj_));
    }

    // Cap the list at a share of the screen so a 15-lane AFC scrolls the list
    // instead of growing the card past the panel.
    lv_obj_set_style_max_height(tool_list, screen_height_pct(60), 0);

    auto& tool_state = ToolState::instance();
    const auto& tools = tool_state.tools();
    int active = tool_state.active_tool_index();

    // Rows carry both label forms; the subject shows one once the rows are laid
    // out and their width is known. Full until then.
    lv_subject_set_int(&s_picker_code_form_subject, 0);

    lv_obj_t* active_btn_in_picker = nullptr;
    std::vector<lv_obj_t*> row_buttons;
    for (size_t i = 0; i < tools.size(); ++i) {
        bool is_active = (static_cast<int>(i) == active);

        // Create picker button from XML template
        const std::string full = tool_text(tools[i], ui::ToolLabelForm::Full);
        const std::string code = tool_text(tools[i], ui::ToolLabelForm::Code);
        const char* btn_attrs[] = {"tool_text", full.c_str(), "tool_code_text", code.c_str(),
                                   nullptr};
        lv_obj_t* picker_btn =
            static_cast<lv_obj_t*>(lv_xml_create(tool_list, "tool_picker_button", btn_attrs));
        if (!picker_btn) {
            spdlog::error("[ToolSwitcher] lv_xml_create('tool_picker_button') returned NULL");
            continue;
        }

        // Find the actual ui_button — context menu buttons are full width
        lv_obj_t* btn = helix::ui::find_required(picker_btn, "tool_btn", "ToolSwitcher");
        if (!btn) {
            continue;
        }
        lv_obj_set_width(picker_btn, LV_PCT(100));
        lv_obj_set_width(btn, LV_PCT(100));
        row_buttons.push_back(btn);

        // Active tool: use primary variant styling (let ui_button handle colors)
        if (is_active) {
            active_btn_in_picker = picker_btn;
            // ui_button "ghost" doesn't have a bg — set primary bg directly
            lv_obj_set_style_bg_color(btn, theme_manager_get_color("primary"), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
            for (const char* name : {"tool_btn_label", "tool_btn_code_label"}) {
                if (lv_obj_t* label = helix::ui::find_required(picker_btn, name, "ToolSwitcher")) {
                    lv_obj_set_style_text_color(label, theme_manager_get_color("screen_bg"), 0);
                }
            }
        }

        // Pass tool index via event callback user_data (NOT obj user_data — L069:
        // ui_button already owns obj user_data for its internal button_data_t)
        lv_obj_add_event_cb(
            btn,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[ToolSwitcher] picker_tool_click");
                auto* picker = helix::ui::ContextMenu::active_as<ToolPicker>();
                if (!picker)
                    return;
                int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
                ToolSwitcherWidget& owner = picker->owner_;
                picker->hide();
                owner.handle_tool_selected(idx);
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    }

    // The card is only as wide as the tile, so a row's room for its label is
    // known only once the rows are laid out in it. Past the button's content
    // box a label wraps or clips.
    lv_obj_update_layout(tool_list);
    const lv_font_t* face = nullptr;
    if (!row_buttons.empty()) {
        if (lv_obj_t* label = lv_obj_find_by_name(row_buttons.front(), "tool_btn_label")) {
            face = lv_obj_get_style_text_font(label, LV_PART_MAIN);
        }
    }
    const ui::ToolLabelForm form = ui::pick_tool_label_form([&](ui::ToolLabelForm f) {
        for (size_t i = 0; i < row_buttons.size() && i < tools.size(); ++i) {
            if (ui::text_width(tool_text(tools[i], f).c_str(), face) >
                lv_obj_get_content_width(row_buttons[i])) {
                return false;
            }
        }
        return true;
    });
    lv_subject_set_int(&s_picker_code_form_subject, form == ui::ToolLabelForm::Code ? 1 : 0);

    // Scroll the active tool into view inside the capped list.
    if (active_btn_in_picker) {
        lv_obj_update_layout(tool_list);
        lv_obj_scroll_to_view(active_btn_in_picker, LV_ANIM_OFF);
    }

    spdlog::debug("[ToolSwitcher] Picker built with {} tools", tools.size());
}

// ============================================================================
// Tool selection with safety gate
// ============================================================================

AmsError ToolSwitcherWidget::tool_change_refusal() const {
    const auto lifecycle = printer_state_.print_state().get_print_lifecycle();
    const bool paused = lifecycle == PrintState::Paused;

    // No backend means a plain Tn / macro path with no firmware macro that could
    // hide a home — the documented argument for passing false here.
    AmsBackend* backend = AmsState::instance().get_backend();
    const bool self_homes = backend && backend->filament_ops_self_home();

    if (!helix::ui::print_blocks_filament_op(lifecycle, self_homes)) {
        return AmsErrorHelper::success();
    }
    // Same copy the backend would have produced had the request reached it, so
    // the pre-guard and the backend refusal never say two different things.
    return AmsErrorHelper::print_active(paused, /*pause_allows_ops=*/!self_homes);
}

void ToolSwitcherWidget::refresh_print_gating() {
    set_int_if_changed(&s_change_blocked_subject, tool_change_refusal().success() ? 0 : 1);
}

void ToolSwitcherWidget::dispatch_tool_change(int tool_index) {
    spdlog::info("[ToolSwitcher] Requesting tool change to T{}", tool_index);

    // A null api is NOT a reason to skip the call: the AMS backend performs the
    // change without one, and request_tool_change() reports "No API connection"
    // through on_error when there is no backend either. The previous
    // `if (api)` guard turned that case into silence too.
    ToolState::instance().request_tool_change(
        tool_index, get_moonraker_api(),
        /*on_success=*/nullptr, [](const std::string& error) {
            NOTIFY_ERROR(lv_tr("Tool change failed: {}"), error);
            // The published heads follow ToolState's active-tool subject, so
            // a refused change never moved the highlight in the first place.
            // Resync anyway: a backend that got partway before failing leaves
            // the subject as the only truth, and this costs one publish on an
            // error path.
            helix::ui::queue_update("ToolSwitcherWidget::resync_after_error", []() {
                if (s_active_instance) {
                    s_active_instance->on_active_tool_changed(
                        ToolState::instance().active_tool_index());
                }
            });
        });
}

bool ToolSwitcherWidget::park_offered() {
    const AmsBackend* backend = AmsState::instance().get_backend();
    return backend && backend->supports_tool_park();
}

void ToolSwitcherWidget::refresh_park_support() {
    set_int_if_changed(&s_park_supported_subject, park_offered() ? 1 : 0);
}

void ToolSwitcherWidget::handle_park_requested() {
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend || !backend->supports_tool_park()) {
        return;
    }

    // Park is disabled while a job holds the machine, paused included: a
    // print resumed with its tool docked prints nothing. A tap can still land
    // between the job starting and the binding catching up.
    if (job_holds_machine(printer_state_.print_state().get_print_lifecycle())) {
        spdlog::info("[ToolSwitcher] Park refused: a print holds the machine");
        helix::ui::notify_ams_warning(AmsError(AmsResult::WRONG_STATE,
                                               "Tool park blocked: print in progress",
                                               lv_tr("Cannot park while a print is active"), ""));
        return;
    }

    const int active = ToolState::instance().active_tool_index();
    if (active < 0) {
        spdlog::debug("[ToolSwitcher] Park ignored: no tool mounted");
        return;
    }

    spdlog::info("[ToolSwitcher] Parking T{}", active);
    const AmsError err = backend->park_active_tool();
    if (!err.success()) {
        helix::ui::notify_ams_error(err);
    }
}

void ToolSwitcherWidget::handle_tool_selected(int tool_index) {
    auto& tool_state = ToolState::instance();

    // Already on this tool
    if (tool_index == tool_state.active_tool_index()) {
        spdlog::debug("[ToolSwitcher] Tool T{} already active, ignoring", tool_index);
        return;
    }

    // The buttons are greyed by refresh_print_gating(), but a tap can still land
    // in the window between a print starting and the observer firing — and the
    // backend refuses PRINTING unconditionally, so offering the change behind a
    // confirmation modal was offering a dead end. Refuse here with copy the user
    // can act on, exactly as AmsOperationSidebar::handle_unload() does.
    const AmsError refusal = tool_change_refusal();
    if (!refusal.success()) {
        spdlog::info("[ToolSwitcher] Tool change to T{} refused: {}", tool_index,
                     refusal.technical_msg);
        helix::ui::notify_ams_warning(refusal);
        return;
    }

    // Reaching here while PAUSED means the backend permits filament ops on a
    // paused job (everything except AD5X IFS) — pause-then-swap is the runout
    // and colour-change recovery workflow, so the change is offered, with a
    // confirmation because it moves the toolhead into a part still on the bed.
    const auto lifecycle = printer_state_.print_state().get_print_lifecycle();
    if (lifecycle == PrintState::Paused) {
        spdlog::info("[ToolSwitcher] Print paused, showing confirmation for T{}", tool_index);

        helix::ui::modal_confirm(
            lv_tr("Change Tool While Paused"),
            lv_tr("The print is paused. Changing tools now moves the toolhead and swaps the "
                  "filament at the nozzle. Resume the print once the change finishes."),
            ::ModalSeverity::Warning, lv_tr("Change Tool"),
            // dispatch_tool_change() is static, so the capture is the tool index
            // by value - nothing here touches the widget instance.
            [tool_index] { dispatch_tool_change(tool_index); });
        return;
    }

    dispatch_tool_change(tool_index);
}

// ============================================================================
// Static XML event callbacks (registered at startup, used in XML if needed)
// ============================================================================

void ToolSwitcherWidget::tool_park_cb(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[ToolSwitcher] tool_park_cb");
    // The picker's Park row closes the picker like any other row.
    if (auto* picker = helix::ui::ContextMenu::active_as<ToolPicker>()) {
        picker->hide();
    }
    if (s_active_instance) {
        s_active_instance->handle_park_requested();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void ToolSwitcherWidget::tool_compact_cb(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[ToolSwitcher] tool_compact_cb");
    if (s_active_instance) {
        s_active_instance->show_tool_picker();
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
