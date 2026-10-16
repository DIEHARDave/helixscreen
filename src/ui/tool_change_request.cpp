// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tool_change_request.h"

#include "ui_error_reporting.h"
#include "ui_modal.h"

#include "ams_state.h"
#include "display_numbering.h"
#include "printer_state.h"
#include "safety_settings_manager.h"
#include "spool_latch_gate.h"
#include "tool_state.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <utility>

namespace helix::ui {

AmsError tool_change_print_refusal(PrintState lifecycle, bool filament_ops_self_home) {
    if (!print_blocks_filament_op(lifecycle, filament_ops_self_home)) {
        return AmsErrorHelper::success();
    }
    return AmsErrorHelper::print_active(lifecycle == PrintState::Paused,
                                        /*pause_allows_ops=*/!filament_ops_self_home);
}

AmsError spools_on_bed_refusal() {
    return AmsError(AmsResult::WRONG_STATE, "Tool change blocked: spools on the bed",
                    lv_tr(spool_latch_message()), "");
}

ToolChangeDecision decide_tool_change(const ToolChangeInputs& in) {
    if (in.already_active) {
        return {ToolChangeVerdict::AlreadyActive};
    }
    if (in.spools_on_bed) {
        return {ToolChangeVerdict::RefuseSpoolsOnBed, spools_on_bed_refusal()};
    }
    AmsError refusal = tool_change_print_refusal(in.lifecycle, in.filament_ops_self_home);
    if (!refusal.success()) {
        return {ToolChangeVerdict::RefusePrint, std::move(refusal)};
    }
    if (in.backend_busy) {
        return {ToolChangeVerdict::Busy};
    }
    // Reaching here while PAUSED means the backend permits filament ops on a
    // paused job: pause-then-swap is the runout and colour-change recovery
    // workflow. It is confirmed whatever the setting says, because the head
    // moves into a part still on the bed.
    if (in.lifecycle == PrintState::Paused) {
        return {ToolChangeVerdict::ConfirmPaused};
    }
    if (in.confirm_enabled) {
        return {ToolChangeVerdict::ConfirmSetting};
    }
    return {ToolChangeVerdict::Immediate};
}

ToolChangeInputs tool_change_inputs(const PrinterState& printer_state, int tool_index) {
    ToolChangeInputs in;
    in.lifecycle = printer_state.print_state().get_print_lifecycle();
    in.spools_on_bed = printer_state.print_state().spool_latch_active();
    if (AmsBackend* backend = AmsState::instance().get_backend()) {
        in.filament_ops_self_home = backend->filament_ops_self_home();
        in.backend_busy = backend->get_system_info().is_busy();
    }
    in.already_active = tool_index == ToolState::instance().active_tool_index();
    in.confirm_enabled = SafetySettingsManager::instance().get_tool_change_require_confirmation();
    return in;
}

namespace {

void dispatch(int tool_index, IMoonrakerAPI* api, std::function<void()> on_success,
              std::function<void()> on_failed) {
    spdlog::info("[ToolChange] Requesting tool change to T{}", tool_index);
    // A null api is not a reason to skip the call: an AMS backend changes tool
    // without one, and request_tool_change() reports "No API connection"
    // through on_error when there is no backend either.
    ToolState::instance().request_tool_change(
        tool_index, api, std::move(on_success),
        [on_failed = std::move(on_failed)](const std::string& error) {
            NOTIFY_ERROR(lv_tr("Tool change failed: {}"), error);
            if (on_failed) {
                on_failed();
            }
        });
}

std::string full_tool_label(int tool_index) {
    const auto& tools = ToolState::instance().tools();
    if (tool_index < 0 || tool_index >= static_cast<int>(tools.size())) {
        return fmt::format("T{}", tool_index);
    }
    const ToolInfo& tool = tools[static_cast<size_t>(tool_index)];
    return tool_label_in_form(active_tool_noun(), tool.name, tool.index, ToolLabelForm::Full);
}

} // namespace

ToolChangeVerdict offer_tool_change(const PrinterState& printer_state, int tool_index,
                                    IMoonrakerAPI* api, ToolChangeHooks hooks) {
    const ToolChangeDecision decision =
        decide_tool_change(tool_change_inputs(printer_state, tool_index));

    switch (decision.verdict) {
    case ToolChangeVerdict::AlreadyActive:
        spdlog::debug("[ToolChange] T{} already active, ignoring", tool_index);
        break;
    case ToolChangeVerdict::Busy:
        spdlog::debug("[ToolChange] T{} ignored: filament system busy", tool_index);
        break;
    case ToolChangeVerdict::RefuseSpoolsOnBed:
    case ToolChangeVerdict::RefusePrint:
        spdlog::info("[ToolChange] Tool change to T{} refused: {}", tool_index,
                     decision.refusal.technical_msg);
        notify_ams_warning(decision.refusal);
        break;
    case ToolChangeVerdict::Immediate:
        dispatch(tool_index, api, std::move(hooks.on_success), std::move(hooks.on_failed));
        return decision.verdict;
    case ToolChangeVerdict::ConfirmPaused:
    case ToolChangeVerdict::ConfirmSetting: {
        spdlog::info("[ToolChange] Confirming tool change to T{}", tool_index);
        const bool paused = decision.verdict == ToolChangeVerdict::ConfirmPaused;
        const std::string message =
            paused ? std::string(lv_tr(
                         "The print is paused. Changing tools now moves the toolhead and swaps "
                         "the filament at the nozzle. Resume the print once the change finishes."))
                   : fmt::format(fmt::runtime(lv_tr("Switch to {}?")), full_tool_label(tool_index));
        ConfirmOptions options;
        options.on_cancel = hooks.on_declined;
        options.on_dismiss = hooks.on_declined;
        options.owner_token = hooks.owner_token;
        // The confirm callback owns the hooks it needs; the dialog outlives this frame.
        auto on_success = std::make_shared<std::function<void()>>(std::move(hooks.on_success));
        auto on_failed = std::make_shared<std::function<void()>>(std::move(hooks.on_failed));
        modal_confirm(
            paused ? lv_tr("Change Tool While Paused") : lv_tr("Change Tool"), message.c_str(),
            paused ? ::ModalSeverity::Warning : ::ModalSeverity::Info, lv_tr("Change Tool"),
            [tool_index, api, on_success, on_failed] {
                dispatch(tool_index, api, *on_success, *on_failed);
            },
            options);
        return decision.verdict;
    }
    }

    if (hooks.on_declined) {
        hooks.on_declined();
    }
    return decision.verdict;
}

} // namespace helix::ui
