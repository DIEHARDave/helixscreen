// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_op_heater.h"

#include "ui_error_reporting.h"
#include "ui_temperature_utils.h"
#include "ui_timer_guard.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "app_constants.h"
#include "app_globals.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "panel_widget_manager.h"
#include "post_op_cooldown_manager.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "safety_settings_manager.h"
#include "temperature_controller.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace helix::ui {

const char* filament_heat_op_name(FilamentHeatOp op) {
    switch (op) {
    case FilamentHeatOp::Load:
        return "load";
    case FilamentHeatOp::Unload:
        return "unload";
    case FilamentHeatOp::Purge:
        return "purge";
    case FilamentHeatOp::Extrude:
        return "extrude";
    case FilamentHeatOp::Retract:
        return "retract";
    }
    return "filament op";
}

PreheatDecision decide_preheat(const PreheatInputs& in) {
    const int floor = in.floor_c;

    if (in.honor_live_target && in.target_c > 0 &&
        preheat_reached(in.current_c, in.target_c, floor)) {
        return {};
    }
    const int required = filament_op_nozzle_temp(in.material_c.value_or(floor), in.held_c);
    if (preheat_reached(in.current_c, required, floor)) {
        return {};
    }

    int send = in.material_c.value_or(app_constants::ams::DEFAULT_LOAD_PREHEAT_TEMP);
    send = std::max(send, floor);
    int wait = filament_op_nozzle_temp(send, in.held_c);
    if (in.ceiling_c > 0) {
        send = std::min(send, in.ceiling_c);
        wait = std::min(wait, in.ceiling_c);
    }
    return {true, send, wait};
}

bool preheat_reached(int current_c, int wait_c, int floor_c) {
    return current_c >= std::max(wait_c - PREHEAT_MARGIN_C, floor_c);
}

FilamentOpHeater* get_filament_op_heater() {
    return PanelWidgetManager::instance().shared_resource<FilamentOpHeater>();
}

PreheatSkip preheat_skip_reason_unplanned() {
    return SafetySettingsManager::instance().get_allow_cold_extrude() ? PreheatSkip::UserOverride
                                                                      : PreheatSkip::None;
}

std::optional<PreheatTarget> preheat_material_for_slot(int slot) {
    AmsBackend* backend = AmsState::instance().get_backend();
    SlotInfo info;
    const SlotInfo* info_ptr = nullptr;
    if (backend && slot >= 0) {
        info = backend->get_slot_info(slot);
        info_ptr = &info;
    }
    const auto ext = AmsState::instance().get_external_spool_info();
    return resolve_load_preheat_material(slot, info_ptr, ext ? &*ext : nullptr);
}

namespace {

bool is_terminal(int action) {
    return action == static_cast<int>(AmsAction::IDLE) ||
           action == static_cast<int>(AmsAction::ERROR);
}

/// Ops that push what is already in the melt zone rather than move a spool.
bool works_the_melt_zone(FilamentHeatOp op) {
    return op == FilamentHeatOp::Purge || op == FilamentHeatOp::Extrude ||
           op == FilamentHeatOp::Retract;
}

constexpr uint32_t DEADLINE_CHECK_MS = 1000;

} // namespace

FilamentOpHeater::FilamentOpHeater(PrinterState& state, Clock clock)
    : state_(state), clock_(std::move(clock)) {}

FilamentOpHeater::~FilamentOpHeater() {
    // Teardown sends nothing: the printer this heater talked to may already be
    // gone, and the controller with it.
    cancel_deadline_timer();
    pending_.reset();
    temp_observer_.reset();
    target_observer_.reset();
    lifecycle_observer_.reset();
    action_observer_.reset();
}

FilamentOpHeater::TimePoint FilamentOpHeater::now() const {
    return clock_ ? clock_() : std::chrono::steady_clock::now();
}

lv_subject_t* FilamentOpHeater::temp_subject(const std::string& extruder) const {
    auto& temps = state_.temperature_state();
    if (!extruder.empty()) {
        if (lv_subject_t* s = temps.get_extruder_temp_subject(extruder)) {
            return s;
        }
    }
    return temps.get_active_extruder_temp_subject();
}

lv_subject_t* FilamentOpHeater::target_subject(const std::string& extruder) const {
    auto& temps = state_.temperature_state();
    if (!extruder.empty()) {
        if (lv_subject_t* s = temps.get_extruder_target_subject(extruder)) {
            return s;
        }
    }
    return temps.get_active_extruder_target_subject();
}

int FilamentOpHeater::read_c(lv_subject_t* subject) const {
    return subject ? temperature::deci_to_degrees(lv_subject_get_int(subject)) : 0;
}

bool FilamentOpHeater::heated_for(FilamentOpTicket ticket) const {
    return ticket != 0 && std::any_of(heated_.begin(), heated_.end(),
                                      [ticket](const auto& h) { return h.first == ticket; });
}

void FilamentOpHeater::forget(FilamentOpTicket ticket) {
    heated_.erase(std::remove_if(heated_.begin(), heated_.end(),
                                 [ticket](const auto& h) { return h.first == ticket; }),
                  heated_.end());
    watches_.erase(std::remove_if(watches_.begin(), watches_.end(),
                                  [ticket](const Watch& w) { return w.ticket == ticket; }),
                   watches_.end());
    if (watches_.empty()) {
        action_observer_.reset();
    }
}

FilamentOpHeater::Outcome FilamentOpHeater::run_after_preheat(PreheatRequest req) {
    int prior_target_c = -1;
    if (pending_) {
        if (pending_->req.op == req.op) {
            spdlog::info("{} {} requested again while its preheat waits — cancelling", req.log_tag,
                         filament_heat_op_name(req.op));
            cancel();
            return Outcome::Cancelled;
        }
        spdlog::info("{} {} replaces the pending {} preheat", req.log_tag,
                     filament_heat_op_name(req.op), filament_heat_op_name(pending_->req.op));
        // The heater was off before the first request, and still counts as off
        // for whatever cancels the one that replaces it.
        prior_target_c = pending_->prior_target_c;
        end_pending(PreheatPhase::Cancelled);
    }

    const FilamentOpTicket ticket = ++next_ticket_;

    if (req.skip != PreheatSkip::None) {
        spdlog::info("{} Skipping preheat for {} — {}", req.log_tag, filament_heat_op_name(req.op),
                     preheat_skip_name(req.skip));
        req.run(ticket);
        return Outcome::Ran;
    }

    SafetyLimits limits;
    if (auto* api = get_moonraker_api()) {
        limits = api->get_safety_limits();
    }
    // The lane's mapped tool names the extruder that heats; with no mapping it is
    // the active one.
    const OpNozzle nozzle =
        resolve_op_nozzle(AmsState::instance().get_backend(), req.slot, state_, limits);
    auto& temps = state_.temperature_state();
    const std::string extruder =
        nozzle.extruder == temps.active_extruder_name() ? std::string() : nozzle.extruder;

    std::optional<PreheatTarget> material = preheat_material_for_slot(req.slot);
    if (!material) {
        material = req.fallback_material;
    }

    const bool job_holds = job_holds_machine(state_.print_state().get_print_lifecycle());
    float held = temps.get_active_extruder_last_nonzero_target();
    if (!extruder.empty()) {
        const auto it = temps.extruders().find(extruder);
        held = it != temps.extruders().end() ? it->second.last_nonzero_target : 0.0f;
    }

    PreheatInputs in;
    in.current_c = read_c(temp_subject(extruder));
    in.target_c = read_c(target_subject(extruder));
    in.material_c = material ? std::optional<int>(material->temp_c) : std::nullopt;
    in.held_c = static_cast<int>(std::lround(held));
    in.floor_c = nozzle.floor_c;
    in.ceiling_c = nozzle.ceiling_c;
    in.honor_live_target = job_holds || works_the_melt_zone(req.op);
    const PreheatDecision decision = decide_preheat(in);

    if (!decision.heat) {
        spdlog::info("{} Nozzle hot enough for {} ({}°C, target {}°C) — no preheat", req.log_tag,
                     filament_heat_op_name(req.op), in.current_c, in.target_c);
        req.run(ticket);
        return Outcome::Ran;
    }

    Pending p;
    p.ticket = ticket;
    p.extruder = extruder;
    p.wait_c = decision.wait_c;
    p.floor_c = in.floor_c;
    p.prior_target_c = prior_target_c >= 0 ? prior_target_c : in.target_c;
    p.job_free_at_request = !job_holds;
    p.started = now();
    p.req = std::move(req);
    start_heating(std::move(p), decision.send_c,
                  material ? material->material_name : std::string());
    return Outcome::Heating;
}

void FilamentOpHeater::start_heating(Pending p, int send_c, const std::string& material_name) {
    spdlog::info("{} Preheating {} to {}°C ({}) for {} of slot {}: waiting for {}°C", p.req.log_tag,
                 p.extruder.empty() ? "the active extruder" : p.extruder, send_c,
                 material_name.empty() ? "default" : material_name, filament_heat_op_name(p.req.op),
                 p.req.slot, p.wait_c);
    const bool announce = p.req.announce;
    const std::string extruder = p.extruder;
    const int wait_c = p.wait_c;
    // A ticket whose op never reported back (an error a surface swallowed) is
    // dropped once it is older than any op could run.
    const TimePoint stale = now() - 2 * BACKEND_WATCH_TIMEOUT;
    heated_.erase(std::remove_if(heated_.begin(), heated_.end(),
                                 [stale](const auto& h) { return h.second < stale; }),
                  heated_.end());
    heated_.emplace_back(p.ticket, now());
    pending_ = std::move(p);

    // A cooldown left from the previous op would zero this target mid-preheat.
    PostOpCooldownManager::instance().cancel();
    if (auto* c = get_temperature_controller()) {
        if (extruder.empty()) {
            c->set_target(HeaterType::Nozzle, static_cast<double>(send_c),
                          {.toast = false, .keep_previous_hot = true});
        } else {
            // keep_previous_hot reads the active extruder only; this one's latch
            // is already in wait_c.
            c->set_target(extruder, static_cast<double>(wait_c), {.toast = false});
        }
    }
    if (announce) {
        if (material_name.empty()) {
            NOTIFY_INFO(lv_tr("Heating to {}°C..."), wait_c);
        } else {
            NOTIFY_INFO(lv_tr("Heating to {}°C for {}..."), wait_c, material_name);
        }
    }

    auto& temps = state_.temperature_state();
    SubjectLifetime temp_life;
    SubjectLifetime target_life;
    lv_subject_t* temp = extruder.empty() ? temps.get_active_extruder_temp_subject()
                                          : temps.get_extruder_temp_subject(extruder, temp_life);
    lv_subject_t* target = extruder.empty()
                               ? temps.get_active_extruder_target_subject()
                               : temps.get_extruder_target_subject(extruder, target_life);
    temp_observer_ = observe<int>(
        temp, this, [](FilamentOpHeater* self, int /*deci*/) { self->on_temperature(); },
        extruder.empty() ? state_.get_subjects_lifetime() : temp_life);
    target_observer_ = observe<int>(
        target, this, [](FilamentOpHeater* self, int deci) { self->on_target(deci); },
        extruder.empty() ? state_.get_subjects_lifetime() : target_life);
    lifecycle_observer_ = observe<int>(
        state_.print_state().get_print_lifecycle_subject(), this,
        [](FilamentOpHeater* self, int /*lifecycle*/) { self->on_lifecycle(); },
        state_.get_subjects_lifetime());
    cancel_deadline_timer();
    // TIMER_DTOR_OK: cancelled in the destructor through cancel_deadline_timer().
    deadline_timer_ = lv_timer_create(
        [](lv_timer_t* t) {
            static_cast<FilamentOpHeater*>(lv_timer_get_user_data(t))->check_deadlines();
        },
        DEADLINE_CHECK_MS, this);

    report_heating();
}

void FilamentOpHeater::report_heating() {
    if (!pending_ || !pending_->req.on_progress) {
        return;
    }
    // Copied: the callback may start another op, which replaces pending_.
    auto cb = pending_->req.on_progress;
    cb(PreheatProgress{PreheatPhase::Heating, read_c(temp_subject(pending_->extruder)),
                       pending_->wait_c});
}

void FilamentOpHeater::cancel_deadline_timer() {
    lv_timer_cancel_safe(deadline_timer_);
    deadline_timer_ = nullptr;
}

void FilamentOpHeater::end_pending(PreheatPhase phase) {
    if (!pending_) {
        return;
    }
    Pending ended = std::move(*pending_);
    pending_.reset();
    temp_observer_.reset();
    target_observer_.reset();
    lifecycle_observer_.reset();
    cancel_deadline_timer();
    if (phase != PreheatPhase::Reached) {
        forget(ended.ticket);
    }
    if (ended.req.on_progress) {
        ended.req.on_progress(
            PreheatProgress{phase, read_c(temp_subject(ended.extruder)), ended.wait_c});
    }
    if (phase == PreheatPhase::Reached && ended.req.run) {
        ended.req.run(ended.ticket);
    }
}

void FilamentOpHeater::abandon(const char* why, const char* toast) {
    if (!pending_) {
        return;
    }
    spdlog::info("{} {} — not running {}", pending_->req.log_tag, why,
                 filament_heat_op_name(pending_->req.op));
    NOTIFY_WARNING("{}", toast);
    end_pending(PreheatPhase::Abandoned);
}

void FilamentOpHeater::on_temperature() {
    if (!pending_) {
        return;
    }
    const int current = read_c(temp_subject(pending_->extruder));
    if (!preheat_reached(current, pending_->wait_c, pending_->floor_c)) {
        report_heating();
        return;
    }
    // A job that took the machine while we heated owns the nozzle now.
    if (pending_->job_free_at_request &&
        job_holds_machine(state_.print_state().get_print_lifecycle())) {
        on_lifecycle();
        return;
    }
    spdlog::info("{} Preheat reached {}°C, running {}", pending_->req.log_tag, current,
                 filament_heat_op_name(pending_->req.op));
    end_pending(PreheatPhase::Reached);
}

void FilamentOpHeater::on_target(int target_deci) {
    if (!pending_) {
        return;
    }
    // The target reaches ours some time after the send, so only a drop from a
    // target already seen means the preheat was undone (cleared by the user, a
    // heater fault, a Klipper shutdown). Such a preheat would never complete.
    const int target = temperature::deci_to_degrees(target_deci);
    if (target >= pending_->wait_c - PREHEAT_MARGIN_C) {
        pending_->target_seen = true;
        return;
    }
    if (!pending_->target_seen) {
        return;
    }
    if (pending_->req.op == FilamentHeatOp::Load) {
        abandon("Nozzle target dropped during the preheat",
                lv_tr("Load cancelled: nozzle target dropped"));
    } else {
        abandon("Nozzle target dropped during the preheat", lv_tr("Preheat cancelled"));
    }
}

void FilamentOpHeater::on_lifecycle() {
    if (!pending_ || !pending_->job_free_at_request) {
        return;
    }
    if (job_holds_machine(state_.print_state().get_print_lifecycle())) {
        abandon("A print took the machine during the preheat", lv_tr("Preheat cancelled"));
    }
}

void FilamentOpHeater::check_deadlines() {
    if (!pending_) {
        return;
    }
    const TimePoint t = now();
    if (!pending_->target_seen && t - pending_->started > TARGET_LAND_TIMEOUT) {
        abandon("The nozzle target never arrived", lv_tr("Filament operation timed out"));
        return;
    }
    if (t - pending_->started > PREHEAT_TIMEOUT) {
        abandon("The nozzle never reached temperature", lv_tr("Filament operation timed out"));
    }
}

bool FilamentOpHeater::cancel() {
    if (!pending_) {
        return false;
    }
    spdlog::info("{} Preheat for {} cancelled", pending_->req.log_tag,
                 filament_heat_op_name(pending_->req.op));
    PostOpCooldownManager::instance().cancel();
    // Back off only if it was off: a target someone else set stays theirs.
    if (pending_->prior_target_c == 0) {
        if (auto* c = get_temperature_controller()) {
            if (pending_->extruder.empty()) {
                c->set_target(HeaterType::Nozzle, 0.0, {.toast = false});
            } else {
                c->set_target(pending_->extruder, 0.0, {.toast = false});
            }
        }
    }
    NOTIFY_INFO(lv_tr("Preheat cancelled"));
    end_pending(PreheatPhase::Cancelled);
    return true;
}

void FilamentOpHeater::op_succeeded(FilamentOpTicket ticket) {
    if (!heated_for(ticket)) {
        return;
    }
    forget(ticket);
    // A job that is starting is about to heat the nozzle itself, and a paused
    // one is waiting to resume at its own temperature.
    if (job_holds_machine(state_.print_state().get_print_lifecycle())) {
        spdlog::debug("[FilamentOpHeater] Op finished during a job — no cooldown");
        return;
    }
    spdlog::info("[FilamentOpHeater] Op finished — scheduling the post-op cooldown");
    PostOpCooldownManager::instance().schedule();
}

void FilamentOpHeater::op_failed(FilamentOpTicket ticket) {
    forget(ticket);
}

void FilamentOpHeater::watch_backend_completion(FilamentOpTicket ticket) {
    if (!heated_for(ticket)) {
        return;
    }
    watches_.push_back(Watch{ticket, false, now() + BACKEND_WATCH_TIMEOUT});
    if (!action_observer_) {
        action_observer_ = observe<int>(
            AmsState::instance().get_ams_action_subject(), this,
            [](FilamentOpHeater* self, int action) { self->on_ams_action(action); },
            AmsState::instance().get_subjects_lifetime());
    }
}

void FilamentOpHeater::on_ams_action(int action) {
    const TimePoint t = now();
    std::vector<FilamentOpTicket> expired;
    std::vector<FilamentOpTicket> done;
    for (auto& w : watches_) {
        if (t > w.deadline) {
            expired.push_back(w.ticket);
        } else if (!is_terminal(action)) {
            w.seen_busy = true;
        } else if (w.seen_busy) {
            // A backend reports IDLE until its firmware starts, so only a
            // terminal edge after a busy one is the end of the op.
            done.push_back(w.ticket);
        }
    }
    for (FilamentOpTicket ticket : expired) {
        forget(ticket);
    }
    for (FilamentOpTicket ticket : done) {
        if (action == static_cast<int>(AmsAction::IDLE)) {
            op_succeeded(ticket);
        } else {
            op_failed(ticket);
        }
    }
}

} // namespace helix::ui
