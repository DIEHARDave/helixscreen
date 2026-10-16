// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_modal.h"
#include "ui_toast_manager.h"

#include "abort_manager.h"
#include "app_globals.h"
#include "chamber_stall_prompt.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "static_subject_registry.h"
#include "temperature_controller.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <memory>

namespace helix {

ChamberStallAction ChamberStallArbiter::decide(const ChamberStallInputs& in) {
    if (!in.job_active) {
        prompted_unheated_ = false;
        if (in.rejected && in.rejection_seq != toasted_seq_) {
            toasted_seq_ = in.rejection_seq;
            return ChamberStallAction::Toast;
        }
        return ChamberStallAction::None;
    }
    if (!in.chamber_wait) {
        return ChamberStallAction::None;
    }
    if (in.rejected) {
        if (in.rejection_seq == prompted_seq_) {
            return ChamberStallAction::None;
        }
        prompted_seq_ = in.rejection_seq;
        toasted_seq_ = in.rejection_seq;
        return ChamberStallAction::Prompt;
    }
    if (in.chamber_target_deci > 0 || prompted_unheated_) {
        return ChamberStallAction::None;
    }
    prompted_unheated_ = true;
    return ChamberStallAction::Prompt;
}

namespace ui {
namespace {

/// The chamber prompt. Its two subjects are global so the nested
/// modal_button_row resolves them, and static so they outlive any one show.
class ChamberStallModal : public Modal {
  public:
    const char* get_name() const override {
        return "Chamber Stall";
    }
    const char* component_name() const override {
        return "chamber_stall_modal";
    }

    static void init_subjects() {
        if (subjects_initialized_) {
            return;
        }
        subjects_initialized_ = true;
        lv_subject_init_string(&message_subject_, message_buf_, nullptr, sizeof(message_buf_), "");
        lv_subject_init_int(&no_retry_subject_, 0);
        lv_xml_register_subject(nullptr, "chamber_stall_message", &message_subject_);
        lv_xml_register_subject(nullptr, "chamber_stall_no_retry", &no_retry_subject_);
        StaticSubjectRegistry::instance().register_deinit("ChamberStallModal", []() {
            if (!subjects_initialized_) {
                return;
            }
            lv_xml_unregister_subject(nullptr, "chamber_stall_message");
            lv_xml_unregister_subject(nullptr, "chamber_stall_no_retry");
            lv_subject_deinit(&message_subject_);
            lv_subject_deinit(&no_retry_subject_);
            subjects_initialized_ = false;
        });
    }

    static void set_content(const std::string& message, bool can_retry) {
        init_subjects();
        lv_subject_copy_string(&message_subject_, message.c_str());
        lv_subject_set_int(&no_retry_subject_, can_retry ? 0 : 1);
    }

  protected:
    void on_show() override {
        wire_ok_button("btn_primary");
        wire_cancel_button("btn_secondary");
        wire_quaternary_button("btn_quaternary");
    }
    void on_ok() override {
        ChamberStallPrompt::retry();
        hide();
    }
    void on_quaternary() override {
        spdlog::info("[ChamberStall] Cancel print chosen while the chamber wait holds the queue");
        AbortManager::instance().start_abort();
        hide();
    }

  private:
    static inline lv_subject_t message_subject_{};
    static inline char message_buf_[384] = {};
    static inline lv_subject_t no_retry_subject_{};
    static inline bool subjects_initialized_ = false;
};

bool job_holds_machine() {
    lv_subject_t* job = get_printer_state().print_state().get_job_holds_machine_subject();
    return job && lv_subject_get_int(job) != 0;
}

} // namespace

ChamberStallPrompt::ChamberStallPrompt() {
    ChamberStallModal::init_subjects();
    auto& state = get_printer_state();
    auto& temps = state.temperature_state();
    const auto& lifetime = state.get_subjects_lifetime();
    auto on_change = [](ChamberStallPrompt* self, int) { self->evaluate(); };

    SubjectLifetime rejected_lifetime;
    lv_subject_t* rejected = temps.get_chamber_heater_command_rejected_subject(rejected_lifetime);
    rejected_observer_ = observe<int>(rejected, this, on_change, rejected_lifetime);
    SubjectLifetime seq_lifetime;
    lv_subject_t* seq = temps.get_chamber_heater_rejection_seq_subject(seq_lifetime);
    seq_observer_ = observe<int>(seq, this, on_change, seq_lifetime);
    SubjectLifetime target_lifetime;
    lv_subject_t* target = temps.get_chamber_target_subject(target_lifetime);
    target_observer_ = observe<int>(target, this, on_change, target_lifetime);
    wait_observer_ = observe<int>(state.print_state().get_print_start_chamber_wait_subject(), this,
                                  on_change, lifetime);
    job_observer_ = observe<int>(state.print_state().get_job_holds_machine_subject(), this,
                                 on_change, lifetime);
}

ChamberStallPrompt::~ChamberStallPrompt() = default;

double ChamberStallPrompt::retry_target_c() {
    const auto rejection = get_printer_state().temperature_state().chamber_command_rejection();
    if (rejection.requested_target_c > 0) {
        return rejection.requested_target_c;
    }
    const auto* controller = get_temperature_controller();
    return controller ? controller->last_chamber_target_c() : 0.0;
}

bool ChamberStallPrompt::retry() {
    auto* controller = get_temperature_controller();
    const double target = retry_target_c();
    if (!controller || target <= 0 || !controller->chamber_has_direct_route()) {
        spdlog::info("[ChamberStall] Retry unavailable (target={}°C, route={})", target,
                     controller && controller->chamber_has_direct_route());
        return false;
    }
    SendOptions opts;
    opts.toast = true;
    controller->retry_chamber_target(target, std::move(opts));
    return true;
}

void ChamberStallPrompt::evaluate() {
    auto& state = get_printer_state();
    auto& temps = state.temperature_state();
    ChamberStallInputs in;
    in.job_active = job_holds_machine();
    in.chamber_wait =
        lv_subject_get_int(state.print_state().get_print_start_chamber_wait_subject()) != 0;
    in.rejected = lv_subject_get_int(temps.get_chamber_heater_command_rejected_subject()) != 0;
    in.rejection_seq = lv_subject_get_int(temps.get_chamber_heater_rejection_seq_subject());
    in.chamber_target_deci = lv_subject_get_int(temps.get_chamber_target_subject());
    switch (arbiter_.decide(in)) {
    case ChamberStallAction::Prompt:
        show_prompt();
        break;
    case ChamberStallAction::Toast: {
        const std::string text =
            fmt::format(fmt::runtime(lv_tr("Chamber heater didn't start. {}")),
                        lv_subject_get_string(temps.get_chamber_heater_rejection_text_subject()));
        ToastManager::instance().show(ToastSeverity::WARNING, text.c_str(), 6000);
        break;
    }
    case ChamberStallAction::None:
        break;
    }
}

void ChamberStallPrompt::show_prompt() {
    auto& temps = get_printer_state().temperature_state();
    const auto* controller = get_temperature_controller();
    const double target = retry_target_c();
    const bool can_retry = target > 0 && controller && controller->chamber_has_direct_route();

    std::string message =
        target > 0 ? fmt::format(fmt::runtime(lv_tr("The print is waiting for the chamber to "
                                                    "reach {}°C, but the heater is not heating.")),
                                 static_cast<int>(std::lround(target)))
                   : std::string(lv_tr("The print is waiting for the chamber, but the heater is "
                                       "not heating."));
    const char* reason = lv_subject_get_string(temps.get_chamber_heater_rejection_text_subject());
    if (reason && reason[0]) {
        message += "\n";
        message += reason;
    }
    if (!can_retry) {
        message += "\n";
        message += lv_tr("The heater can't be changed while the printer is waiting.");
    }
    spdlog::warn("[ChamberStall] Prompting: target={}°C retry={}", target, can_retry);
    ChamberStallModal::set_content(message, can_retry);
    // A prompt already up shows the new rejection through its bound text.
    if (ModalStack::instance().top_component_name() == "chamber_stall_modal") {
        return;
    }
    auto modal = std::make_unique<ChamberStallModal>();
    if (!modal->show(lv_screen_active())) {
        spdlog::error("[ChamberStall] chamber_stall_modal failed to create");
        return;
    }
    lv_obj_t* backdrop = modal->backdrop();
    ModalStack::instance().assume_ownership(backdrop, std::move(modal));
}

} // namespace ui
} // namespace helix
