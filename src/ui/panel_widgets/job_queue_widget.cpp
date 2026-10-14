// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "job_queue_widget.h"

#include "ui_event_safety.h"
#include "ui_filename_utils.h"
#include "ui_next_tick.h"
#include "ui_subject_registry.h"

#include "app_globals.h"
#include "helix_psram_attr.h"
#include "job_queue_start.h"
#include "job_queue_state.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observe_language.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "printer_state.h"
#include "queued_job_start.h"
#include "src/ui/panel_widgets/content_chrome.h"
#include "src/ui/panel_widgets/live_tile_owner.h"
#include "static_subject_registry.h"
#include "subject_managed_panel.h"
#include "text_measure.h"
#include "theme_manager.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>

using helix::ui::measure_text_px;

namespace {

// The verdict and texts the widget publishes, read by panel_widget_job_queue.xml.
// Module-level because the widget is single-instance and registration rides
// SubjectInitializer's panel-subject phase, so the subjects exist before any XML that
// binds them is parsed.
struct Subjects {
    lv_subject_t state{};
    lv_subject_t action{};
    lv_subject_t rung{};
    lv_subject_t show_thumb{};
    lv_subject_t show_detail{};
    lv_subject_t show_more{};
    lv_subject_t list_rows{};
    lv_subject_t hero{};
    lv_subject_t detail{};
    lv_subject_t more{};
    lv_subject_t action_text{};
    lv_subject_t after[helix::kJobQueueTileListRows]{};
    char hero_buf[320]{};
    char detail_buf[320]{};
    char more_buf[64]{};
    char action_text_buf[96]{};
    char after_buf[helix::kJobQueueTileListRows][320]{};
};
HELIX_PSRAM_BSS Subjects s_subj;
bool s_subjects_initialized = false;
SubjectManager s_subjects;
// The instance bound to the live tile. Only it publishes: a transient instance (a
// rebuild's outgoing widget) must not overwrite the tile's verdict.
helix::LiveTileOwner<helix::JobQueueWidget> s_tile;

void job_queue_init_subjects() {
    if (s_subjects_initialized)
        return;
    UI_MANAGED_SUBJECT_INT(s_subj.state, static_cast<int>(helix::JobQueueTileState::NotLoaded),
                           "jq_tile_state", s_subjects);
    UI_MANAGED_SUBJECT_INT(s_subj.action, static_cast<int>(helix::JobQueueTileAction::None),
                           "jq_tile_action", s_subjects);
    UI_MANAGED_SUBJECT_INT(s_subj.rung, static_cast<int>(helix::SizeRung::Cell), "jq_tile_rung",
                           s_subjects);
    UI_MANAGED_SUBJECT_INT(s_subj.show_thumb, 0, "jq_tile_show_thumb", s_subjects);
    UI_MANAGED_SUBJECT_INT(s_subj.show_detail, 0, "jq_tile_show_detail", s_subjects);
    UI_MANAGED_SUBJECT_INT(s_subj.show_more, 0, "jq_tile_show_more", s_subjects);
    UI_MANAGED_SUBJECT_INT(s_subj.list_rows, 0, "jq_tile_list_rows", s_subjects);
    UI_MANAGED_SUBJECT_STRING(s_subj.hero, s_subj.hero_buf, "", "jq_tile_hero", s_subjects);
    UI_MANAGED_SUBJECT_STRING(s_subj.detail, s_subj.detail_buf, "", "jq_tile_detail", s_subjects);
    UI_MANAGED_SUBJECT_STRING(s_subj.more, s_subj.more_buf, "", "jq_tile_more", s_subjects);
    UI_MANAGED_SUBJECT_STRING(s_subj.action_text, s_subj.action_text_buf, "", "jq_tile_action_text",
                              s_subjects);
    // Subject names are held by pointer, so they must outlive this function.
    static const char* const kAfterNames[helix::kJobQueueTileListRows] = {
        "jq_tile_after_0", "jq_tile_after_1", "jq_tile_after_2"};
    for (int i = 0; i < helix::kJobQueueTileListRows; ++i) {
        UI_MANAGED_SUBJECT_STRING(s_subj.after[i], s_subj.after_buf[i], "", kAfterNames[i],
                                  s_subjects);
    }
    s_subjects_initialized = true;

    StaticSubjectRegistry::instance().register_deinit("JobQueueWidgetSubjects", []() {
        if (s_subjects_initialized && lv_is_initialized()) {
            s_subjects.deinit_all();
            s_subjects_initialized = false;
        }
    });
}

int line_px(const lv_font_t* font) {
    return font ? static_cast<int>(lv_font_get_line_height(font)) : 0;
}

/// The minimum content box each rung draws in. Mirrors panel_widget_job_queue.xml: the
/// body's gap is space_xs and the root's is space_xxs. Names and the hero ellipsize, so
/// text widths are budgeted at the least they can show (the rung must hold while names
/// change) and the action button's label, which does not ellipsize, at its full width.
helix::RungNeeds measure_needs(const helix::ContentChrome& chrome) {
    using helix::rung_box;
    const lv_font_t* heading = theme_manager_get_font("font_heading");
    const lv_font_t* body = theme_manager_get_font("font_body");
    const lv_font_t* small = theme_manager_get_font("font_small");
    const int row_gap = theme_manager_get_spacing("space_xxs");
    const int col_gap = theme_manager_get_spacing("space_xs");
    const int pad_sm = theme_manager_get_spacing("space_sm");
    const int btn = theme_manager_get_spacing("button_height_sm");
    const int heading_h = line_px(heading);
    const int small_h = line_px(small);

    // A two-digit count is the least a hero can show; anything longer ellipsizes.
    const int hero_w = measure_text_px("88…", heading);
    const int action_short_w = measure_text_px(lv_tr("Start next"), body) + 2 * pad_sm;
    const int action_w = measure_text_px(lv_tr("Bed clear · Start next"), body) + 2 * pad_sm;
    // The thumbnail is a button-height square (jq_thumb).
    const int thumb_w = btn;
    // The text column is the hero over one line of small text.
    const int body_h = std::max(thumb_w, heading_h + small_h);
    const int text_w = thumb_w + col_gap + hero_w;

    helix::RungNeeds needs;
    needs.strip = rung_box(hero_w + col_gap + btn, std::max(heading_h, btn));
    // The root's gap is already in the content chrome, so the one between the body and
    // the button is not repeated here.
    needs.cell = rung_box(std::max(text_w, action_short_w), body_h + btn);
    needs.wide = rung_box(std::max(text_w, action_w), body_h + btn);

    const int list_row_h = line_px(body) + 2 * pad_sm;
    const int list_h =
        helix::kJobQueueTileListRows * list_row_h + (helix::kJobQueueTileListRows - 1) * row_gap;
    // Full is a size class as well as a content fit: a 2x1 tile can hold the content,
    // but the rung is for 2x2 and up.
    needs.full = rung_box(needs.wide.w, std::max(needs.wide.h + row_gap + list_h,
                                                 helix::widget_size::h_tall() - chrome.h));
    return needs;
}

std::string next_display_name(const JobQueueState* jqs, size_t index) {
    if (!jqs || index >= jqs->get_jobs().size())
        return {};
    return helix::gcode::get_display_filename(jqs->get_jobs()[index].filename);
}

} // namespace

namespace helix {

JobQueueWidget::JobQueueWidget() = default;

JobQueueWidget::~JobQueueWidget() {
    detach();
}

JobQueueTileInputs JobQueueWidget::read_inputs() const {
    auto* jqs = get_job_queue_state();
    JobQueueTileInputs in;
    in.component_available = get_printer_state().is_job_queue_available();
    in.loaded = jqs && jqs->has_ever_loaded();
    in.job_count = jqs ? static_cast<int>(jqs->get_jobs().size()) : 0;
    in.queue_state = jqs ? jqs->run_state() : QueueRunState::Paused;
    in.printer_busy =
        lv_subject_get_int(get_printer_state().print_state().get_print_active_subject()) != 0;
    in.automatic_transition = jqs && jqs->automatic_transition();
    return in;
}

JobQueueTileVerdict JobQueueWidget::decide(int width_px, int height_px,
                                           const JobQueueTileInputs& in) const {
    // The tree's own padding where it exists; before it does (the load path asks
    // fits_at then) the root's padding on each side plus its row gap.
    ContentChrome chrome;
    if (root()) {
        chrome = measure_content_chrome(root(), 0);
    } else {
        const int pad = theme_manager_get_spacing("space_xxs");
        chrome = {2 * pad, 3 * pad};
    }
    JobQueueTileLayoutInput li;
    li.content_w = width_px - chrome.w;
    li.content_h = height_px - chrome.h;
    li.state = job_queue_tile_state(in);
    li.job_count = in.job_count;
    li.needs = measure_needs(chrome);
    const auto& n = li.needs;
    spdlog::trace("[JobQueueWidget] box {}x{} chrome {}x{} needs s{}x{} c{}x{} w{}x{} f{}x{} "
                  "state={}",
                  width_px, height_px, chrome.w, chrome.h, n.strip.w, n.strip.h, n.cell.w, n.cell.h,
                  n.wide.w, n.wide.h, n.full.w, n.full.h, static_cast<int>(li.state));
    return decide_job_queue_tile_layout(li);
}

void JobQueueWidget::attach(lv_obj_t* widget_obj, lv_obj_t* /*parent_screen*/) {
    s_tile.claim(this);
    install_delete_hook(widget_obj);

    const auto token = lifetime_.token();
    const auto on_change = [token](JobQueueWidget* self, int) {
        if (!token.expired())
            self->schedule_refresh();
    };
    auto* jqs = get_job_queue_state();
    const SubjectLifetime queue_lifetime = jqs ? jqs->get_subjects_lifetime() : SubjectLifetime{};
    const auto observe_queue_subject = [&](const char* name) {
        lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
        return subject ? ui::observe<int>(subject, this, on_change, queue_lifetime)
                       : ObserverGuard{};
    };
    count_observer_ = observe_queue_subject("job_queue_count");
    run_state_observer_ = observe_queue_subject("job_queue_run_state");
    automatic_observer_ = observe_queue_subject("job_queue_automatic_transition");
    estimate_observer_ = observe_queue_subject("job_queue_next_estimate");
    print_active_observer_ =
        ui::observe<int>(get_printer_state().print_state().get_print_active_subject(), this,
                         on_change, get_printer_state().get_subjects_lifetime());
    // The texts hold lv_tr() output from when they were composed, and the widths the
    // rung was chosen on were measured in that language.
    language_observer_ = ui::observe_language_change(this, [token](JobQueueWidget* self) {
        if (!token.expired())
            self->refresh();
    });

    // Instances are recycled across rebuilds and the size callback may not follow.
    refresh();
    spdlog::debug("[JobQueueWidget] Attached");
}

void JobQueueWidget::detach() {
    // Invalidating drops a queued refresh, so the coalescing flag goes with it, or a
    // recycled instance would never queue another.
    lifetime_.invalidate();
    refresh_pending_ = false;
    count_observer_.reset();
    run_state_observer_.reset();
    automatic_observer_.reset();
    estimate_observer_.reset();
    print_active_observer_.reset();
    language_observer_.reset();
    if (JobQueueWidget* displaced = s_tile.release(this))
        displaced->refresh();
    spdlog::debug("[JobQueueWidget] Detached");
}

void JobQueueWidget::on_hooked_root_deleted() {
    lifetime_.invalidate();
    refresh_pending_ = false;
}

void JobQueueWidget::on_activate() {
    if (auto* jqs = get_job_queue_state())
        jqs->fetch();
}

void JobQueueWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int width_px,
                                     int height_px) {
    granted_w_ = width_px;
    granted_h_ = height_px;
    refresh();
}

bool JobQueueWidget::fits_at(int width_px, int height_px) const {
    // Refuse a size that cannot draw the Strip in either the empty-looking state or
    // the one with the widest strip content.
    JobQueueTileInputs not_loaded;
    not_loaded.component_available = true;
    JobQueueTileInputs waiting = not_loaded;
    waiting.loaded = true;
    waiting.job_count = 1;
    return decide(width_px, height_px, not_loaded).fits &&
           decide(width_px, height_px, waiting).fits;
}

void JobQueueWidget::schedule_refresh() {
    if (refresh_pending_)
        return;
    refresh_pending_ = true;
    ui::run_next_tick(lifetime_.token(), [this]() {
        refresh_pending_ = false;
        refresh();
    });
}

void JobQueueWidget::refresh() {
    if (granted_w_ <= 0 || granted_h_ <= 0)
        return;
    const JobQueueTileInputs in = read_inputs();
    verdict_ = decide(granted_w_, granted_h_, in);
    if (!s_tile.is_owner(this))
        return;
    publish(in);
    spdlog::debug("[JobQueueWidget] refresh {}x{} state={} rung={} fits={} rows={}", granted_w_,
                  granted_h_, static_cast<int>(job_queue_tile_state(in)),
                  static_cast<int>(verdict_.rung), verdict_.fits, verdict_.list_rows);
}

void JobQueueWidget::publish(const JobQueueTileInputs& in) {
    auto* jqs = get_job_queue_state();
    const JobQueueTileState state = job_queue_tile_state(in);
    const JobQueueTileText text = compose_job_queue_tile_text(
        state, next_display_name(jqs, 0), in.job_count, jqs ? jqs->next_estimated_seconds() : 0);

    helix::ui::set_int_if_changed(&s_subj.state, static_cast<int>(state));
    helix::ui::set_int_if_changed(&s_subj.action, static_cast<int>(job_queue_tile_action(state)));
    helix::ui::set_int_if_changed(&s_subj.rung, static_cast<int>(verdict_.rung));
    // A part with no text would still take a line of the column.
    helix::ui::set_int_if_changed(&s_subj.show_thumb, verdict_.show_thumbnail ? 1 : 0);
    helix::ui::set_int_if_changed(&s_subj.show_detail,
                                  verdict_.show_detail && !text.detail.empty() ? 1 : 0);
    helix::ui::set_int_if_changed(&s_subj.show_more,
                                  verdict_.show_more && !text.more.empty() ? 1 : 0);
    helix::ui::set_int_if_changed(&s_subj.list_rows, verdict_.list_rows);
    helix::ui::copy_string_if_changed(&s_subj.hero, text.hero.c_str());
    helix::ui::copy_string_if_changed(&s_subj.detail, text.detail.c_str());
    helix::ui::copy_string_if_changed(&s_subj.more, text.more.c_str());
    helix::ui::copy_string_if_changed(
        &s_subj.action_text,
        std::string(verdict_.rung == SizeRung::Cell ? text.action_short : text.action).c_str());
    for (int i = 0; i < kJobQueueTileListRows; ++i)
        helix::ui::copy_string_if_changed(
            &s_subj.after[i],
            std::string(next_display_name(jqs, static_cast<size_t>(i) + 1)).c_str());
}

void JobQueueWidget::on_tile_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[JobQueueWidget] on_tile_clicked");
    if (auto* self = panel_widget_from_event<JobQueueWidget>(e))
        self->record_interaction();
    open_job_queue_modal();
    LVGL_SAFE_EVENT_CB_END();
}

void JobQueueWidget::on_action_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[JobQueueWidget] on_action_clicked");
    // A start tap is not a tap on the tile, which opens the queue modal. The button
    // does not bubble today; this guards against it ever gaining event_bubble.
    lv_event_stop_bubbling(e);
    if (lv_subject_get_int(&s_subj.action) != static_cast<int>(JobQueueTileAction::StartNext))
        return;
    auto* jqs = get_job_queue_state();
    if (!jqs || jqs->get_jobs().empty())
        return;
    const JobQueueEntry job = jqs->get_jobs().front();
    queue::queued_job_starter().start(job);
    LVGL_SAFE_EVENT_CB_END();
}

void register_job_queue_widget() {
    register_widget_factory("job_queue",
                            [](const std::string&) { return std::make_unique<JobQueueWidget>(); });
    register_widget_subjects("job_queue", job_queue_init_subjects);

    lv_xml_register_event_cb(nullptr, "on_job_queue_widget_clicked",
                             JobQueueWidget::on_tile_clicked);
    lv_xml_register_event_cb(nullptr, "on_job_queue_tile_action",
                             JobQueueWidget::on_action_clicked);
}

} // namespace helix
