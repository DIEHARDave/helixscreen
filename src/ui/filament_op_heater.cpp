// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_op_heater.h"

#include "ui_error_reporting.h"
#include "ui_temperature_utils.h"

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
    const bool extrudable = in.current_c >= floor;

    if (extrudable && in.target_c > 0 && in.current_c >= in.target_c - PREHEAT_MARGIN_C) {
        return {};
    }
    const int required = filament_op_nozzle_temp(in.material_c.value_or(floor), in.held_c);
    if (in.current_c >= std::max(required - PREHEAT_MARGIN_C, floor)) {
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

FilamentOpHeater* get_filament_op_heater() {
    return PanelWidgetManager::instance().shared_resource<FilamentOpHeater>();
}

PreheatSkip preheat_skip_reason_unplanned() {
    return SafetySettingsManager::instance().get_allow_cold_extrude() ? PreheatSkip::UserOverride
                                                                      : PreheatSkip::None;
}

namespace {

/// The material @p slot (or, failing that, the external spool) names for an op.
std::optional<PreheatTarget> slot_material(int slot) {
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

bool is_terminal(int action) {
    return action == static_cast<int>(AmsAction::IDLE) ||
           action == static_cast<int>(AmsAction::ERROR);
}

} // namespace

FilamentOpHeater::FilamentOpHeater(PrinterState& state) : state_(state) {}

FilamentOpHeater::~FilamentOpHeater() {
    // Teardown sends nothing: the printer this heater talked to may already be
    // gone, and the controller with it.
    pending_.reset();
    temp_observer_.reset();
    target_observer_.reset();
    action_observer_.reset();
}

int FilamentOpHeater::current_c() const {
    auto& temps = state_.temperature_state();
    lv_subject_t* subj = temps.get_active_extruder_temp_subject();
    return subj ? temperature::deci_to_degrees(lv_subject_get_int(subj)) : 0;
}

int FilamentOpHeater::live_target_c() const {
    auto& temps = state_.temperature_state();
    lv_subject_t* subj = temps.get_active_extruder_target_subject();
    return subj ? temperature::deci_to_degrees(lv_subject_get_int(subj)) : 0;
}

FilamentOpHeater::Outcome FilamentOpHeater::run_after_preheat(PreheatRequest req) {
    if (pending_) {
        spdlog::info("{} {} requested while a preheat waits — cancelling the preheat", req.log_tag,
                     filament_heat_op_name(req.op));
        cancel();
        return Outcome::Cancelled;
    }
    // A new op supersedes the last one's completion watch.
    action_observer_.reset();

    if (req.skip != PreheatSkip::None) {
        spdlog::info("{} Skipping preheat for {} — {}", req.log_tag, filament_heat_op_name(req.op),
                     preheat_skip_name(req.skip));
        req.run();
        return Outcome::Ran;
    }

    auto& temps = state_.temperature_state();
    const std::string& extruder = temps.active_extruder_name();
    SafetyLimits limits;
    if (auto* api = get_moonraker_api()) {
        limits = api->get_safety_limits();
    }

    std::optional<PreheatTarget> material = slot_material(req.slot);
    if (!material) {
        material = req.fallback_material;
    }

    PreheatInputs in;
    in.current_c = current_c();
    in.target_c = live_target_c();
    in.material_c = material ? std::optional<int>(material->temp_c) : std::nullopt;
    in.held_c = static_cast<int>(std::lround(temps.get_active_extruder_last_nonzero_target()));
    in.floor_c = temperature::extrusion_floor_c(limits, extruder);
    in.ceiling_c = temperature::nozzle_max_temp_c(limits, extruder);
    const PreheatDecision decision = decide_preheat(in);

    if (!decision.heat) {
        spdlog::info("{} Nozzle hot enough for {} ({}°C, target {}°C) — no preheat", req.log_tag,
                     filament_heat_op_name(req.op), in.current_c, in.target_c);
        req.run();
        return Outcome::Ran;
    }

    Pending p;
    p.wait_c = decision.wait_c;
    p.prior_target_c = in.target_c;
    p.generation = ++generation_;
    const std::string material_name = material ? material->material_name : std::string();
    const bool announce = req.announce;
    spdlog::info("{} Preheating to {}°C ({}) for {} of slot {}: now {}°C, waiting for {}°C",
                 req.log_tag, decision.send_c, material_name.empty() ? "default" : material_name,
                 filament_heat_op_name(req.op), req.slot, in.current_c, decision.wait_c);
    p.req = std::move(req);
    pending_ = std::move(p);

    // A cooldown left from the previous op would zero this target mid-preheat.
    PostOpCooldownManager::instance().cancel();
    if (auto* c = get_temperature_controller()) {
        c->set_target(HeaterType::Nozzle, static_cast<double>(decision.send_c),
                      {.toast = false, .keep_previous_hot = true});
    }
    if (announce) {
        if (material_name.empty()) {
            NOTIFY_INFO(lv_tr("Heating to {}°C..."), decision.wait_c);
        } else {
            NOTIFY_INFO(lv_tr("Heating to {}°C for {}..."), decision.wait_c, material_name);
        }
    }

    temp_observer_ = observe<int>(
        temps.get_active_extruder_temp_subject(), this,
        [](FilamentOpHeater* self, int /*deci*/) { self->on_temperature(); },
        state_.get_subjects_lifetime());
    target_observer_ = observe<int>(
        temps.get_active_extruder_target_subject(), this,
        [](FilamentOpHeater* self, int deci) { self->on_target(deci); },
        state_.get_subjects_lifetime());

    report(PreheatPhase::Heating);
    return Outcome::Heating;
}

void FilamentOpHeater::report(PreheatPhase phase) {
    if (!pending_ || !pending_->req.on_progress) {
        return;
    }
    // Copied: the callback may start another op, which replaces pending_.
    auto cb = pending_->req.on_progress;
    cb(PreheatProgress{phase, current_c(), pending_->wait_c});
}

void FilamentOpHeater::finish_pending() {
    pending_.reset();
    temp_observer_.reset();
    target_observer_.reset();
}

void FilamentOpHeater::on_temperature() {
    if (!pending_) {
        return;
    }
    if (current_c() < pending_->wait_c - PREHEAT_MARGIN_C) {
        report(PreheatPhase::Heating);
        return;
    }
    spdlog::info("{} Preheat reached {}°C, running {}", pending_->req.log_tag, current_c(),
                 filament_heat_op_name(pending_->req.op));
    report(PreheatPhase::Reached);
    std::function<void()> run = std::move(pending_->req.run);
    finish_pending();
    if (run) {
        run();
    }
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
    const FilamentHeatOp op = pending_->req.op;
    spdlog::info("{} Nozzle target fell to {}°C during the preheat — not running {}",
                 pending_->req.log_tag, target, filament_heat_op_name(op));
    if (op == FilamentHeatOp::Load) {
        NOTIFY_WARNING(lv_tr("Load cancelled: nozzle target dropped"));
    } else {
        NOTIFY_WARNING(lv_tr("Preheat cancelled"));
    }
    report(PreheatPhase::Abandoned);
    finish_pending();
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
            c->set_target(HeaterType::Nozzle, 0.0, {.toast = false});
        }
    }
    NOTIFY_INFO(lv_tr("Preheat cancelled"));
    report(PreheatPhase::Cancelled);
    finish_pending();
    return true;
}

void FilamentOpHeater::op_succeeded() {
    // A job that is starting is about to heat the nozzle itself, and a paused
    // one is waiting to resume at its own temperature.
    if (job_holds_machine(state_.print_state().get_print_lifecycle())) {
        spdlog::debug("[FilamentOpHeater] Op finished during a job — no cooldown");
        return;
    }
    spdlog::info("[FilamentOpHeater] Op finished — scheduling the post-op cooldown");
    PostOpCooldownManager::instance().schedule();
}

void FilamentOpHeater::watch_backend_completion() {
    action_seen_busy_ = false;
    action_observer_ = observe<int>(
        AmsState::instance().get_ams_action_subject(), this,
        [](FilamentOpHeater* self, int action) { self->on_ams_action(action); },
        AmsState::instance().get_subjects_lifetime());
}

void FilamentOpHeater::on_ams_action(int action) {
    if (!is_terminal(action)) {
        action_seen_busy_ = true;
        return;
    }
    // A backend reports IDLE until its firmware starts, so a terminal edge with
    // no busy one before it is the dispatch moment, not the end.
    if (!action_seen_busy_) {
        return;
    }
    action_observer_.reset();
    if (action == static_cast<int>(AmsAction::IDLE)) {
        op_succeeded();
    }
}

} // namespace helix::ui
