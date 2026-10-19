// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "snapmaker_nozzle_offset.h"

#include "i_moonraker_client.h"
#include "moonraker_error.h"
#include "printer_discovery.h"
#include "snapmaker_exceptions.h"
#include "snapmaker_screws_tilt.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <atomic>

namespace helix::snapmaker::nozzle_offset {

namespace {

/// The macro each per-head step runs; the module refuses a head whose macro is missing.
constexpr std::array<const char*, 5> kPerHeadHooks = {"HEAT", "AUTO_CLEAN", "MANUAL_CLEAN",
                                                      "WAIT_COOL", "PROBE_CALIBRATE"};

/// Runners in this process that are mid-run.
std::atomic<int> s_live_runs{0};

bool phase_is_live(Runner::Phase phase) {
    return phase == Runner::Phase::Running || phase == Runner::Phase::AwaitingUser ||
           phase == Runner::Phase::Finishing;
}

/// The status map inside an objects.query response, or the response itself
/// when it already is one.
const nlohmann::json* status_map(const nlohmann::json& response) {
    const nlohmann::json* status = &response;
    if (const auto result = response.find("result");
        result != response.end() && result->is_object()) {
        status = &*result;
    }
    if (const auto inner = status->find("status"); inner != status->end() && inner->is_object()) {
        status = &*inner;
    }
    return status->is_object() ? status : nullptr;
}

/// The status object Klipper names each head's extruder by.
std::string extruder_object(int head) {
    return head == 0 ? std::string("extruder") : fmt::format("extruder{}", head);
}

} // namespace

bool supported(const helix::PrinterDiscovery& discovery) {
    if (!discovery.has_macro("_EXTRUDER_OFFSET_ACTION_PRESTART") ||
        !discovery.has_macro("_EXTRUDER_OFFSET_ACTION_EXIT")) {
        return false;
    }
    for (int head = 0; head < kHeads; ++head) {
        for (const char* hook : kPerHeadHooks) {
            if (!discovery.has_macro(fmt::format("_EXTRUDER_OFFSET_ACTION_{}_T{}", hook, head))) {
                return false;
            }
        }
    }
    return true;
}

helix::printer::DeviceSection device_section() {
    return {kSectionId, "Calibration", 60, "Toolhead offset calibration"};
}

helix::printer::DeviceAction device_action(bool print_holds_heads) {
    auto action = helix::printer::DeviceAction::button(
        kActionId, "Calibrate Toolhead Offsets", kSectionId, "",
        "Measure each toolhead's X, Y and Z offset (about 20 minutes)");
    action.guided_flow = kGuidedFlowId;
    if (print_holds_heads) {
        action.enabled = false;
        action.disable_reason = "Printer is busy with a print";
    }
    return action;
}

std::vector<Step> build_sequence() {
    std::vector<Step> steps;
    steps.push_back({StepKind::Prestart, -1, "EXTRUDER_OFFSET_ACTION_PRESTART", Prompt::None});
    for (int t = 0; t < kHeads; ++t) {
        steps.push_back({StepKind::Heat, t,
                         fmt::format("EXTRUDER_OFFSET_ACTION_HEAT TOOL_ID=T{}", t), Prompt::None});
        steps.push_back(
            {StepKind::AutoClean, t,
             fmt::format("EXTRUDER_OFFSET_ACTION_AUTO_CLEAN TOOL_ID=T{} SAFE_MOVE_Y_POS={}", t,
                         kSafeMoveYPos),
             Prompt::None});
        steps.push_back({StepKind::ManualClean, t,
                         fmt::format("EXTRUDER_OFFSET_ACTION_MANUAL_CLEAN TOOL_ID=T{}", t),
                         Prompt::None});
        // MANUAL_CLEAN parks the hot nozzle at the front; the user brushes it
        // before it cools to probing temperature.
        steps.push_back(
            {StepKind::WaitCool, t,
             fmt::format("EXTRUDER_OFFSET_ACTION_WAIT_COOL TOOL_ID=T{} TEMP={}", t, kProbeTempC),
             Prompt::CleanNozzle});
    }
    steps.push_back({StepKind::DetectPlate, -1, "EXTRUDER_OFFSET_ACTION_DETECT_PLATE PRESENCE=0",
                     Prompt::RemovePlate});
    for (int t = 0; t < kHeads; ++t) {
        steps.push_back({StepKind::Probe, t,
                         fmt::format("EXTRUDER_OFFSET_ACTION_PROBE_CALIBRATE TOOL_ID=T{}", t),
                         Prompt::None});
    }
    return steps;
}

std::string failure_text(const std::string& error_message) {
    if (const auto code = decode_exception_code(error_message)) {
        const auto ours = exception_message(*code);
        if (!ours.empty()) {
            return std::string(ours);
        }
    }
    const auto parsed = nlohmann::json::parse(error_message, nullptr, false);
    if (parsed.is_object()) {
        const auto msg = parsed.find("msg");
        if (msg != parsed.end() && msg->is_string() && !msg->get<std::string>().empty()) {
            return msg->get<std::string>();
        }
    }
    return error_message;
}

nlohmann::json offsets_query_params() {
    nlohmann::json objects = nlohmann::json::object();
    for (int head = 0; head < kHeads; ++head) {
        objects[extruder_object(head)] = nlohmann::json::array({"extruder_offset"});
    }
    return {{"objects", objects}};
}

std::optional<std::vector<std::array<double, 3>>>
offsets_from_query(const nlohmann::json& response) {
    const nlohmann::json* status = status_map(response);
    if (!status) {
        return std::nullopt;
    }

    std::vector<std::array<double, 3>> out;
    for (int head = 0; head < kHeads; ++head) {
        const auto obj = status->find(extruder_object(head));
        if (obj == status->end() || !obj->is_object()) {
            return std::nullopt;
        }
        const auto offset = obj->find("extruder_offset");
        if (offset == obj->end() || !offset->is_array() || offset->size() < 3) {
            return std::nullopt;
        }
        std::array<double, 3> xyz{};
        for (size_t axis = 0; axis < 3; ++axis) {
            if (!(*offset)[axis].is_number()) {
                return std::nullopt;
            }
            xyz[axis] = (*offset)[axis].get<double>();
        }
        out.push_back(xyz);
    }
    return out;
}

bool run_live() {
    return s_live_runs.load() > 0;
}

bool stale_calibration_state(const nlohmann::json& response, bool live) {
    const nlohmann::json* status = status_map(response);
    if (!status || live) {
        return false;
    }
    const auto held = screws_tilt::main_state_from_status(*status);
    return held && *held == kMainStateXyzOffsetCalibrate;
}

void reconcile_on_connect(helix::IMoonrakerClient& client) {
    nlohmann::json params = {
        {"objects", {{"machine_state_manager", nlohmann::json::array({"main_state"})}}}};
    client.send_jsonrpc(
        "printer.objects.query", params,
        [&client](const nlohmann::json& response) {
            // Decided at answer time: a run started since the query went out owns the state.
            if (!stale_calibration_state(response, run_live())) {
                return;
            }
            spdlog::info("[NozzleOffsetCal] Leaving a toolhead offset calibration state no run "
                         "here owns");
            client.send_jsonrpc(
                "printer.gcode.script", {{"script", kExitGcode}},
                [](const nlohmann::json&) {
                    spdlog::info("[NozzleOffsetCal] Calibration state cleared");
                },
                [](const MoonrakerError& err) {
                    spdlog::warn("[NozzleOffsetCal] {} failed: {}", kExitGcode, err.message);
                },
                kStepTimeoutMs, true);
        },
        [](const MoonrakerError& err) {
            spdlog::debug("[NozzleOffsetCal] Connect-time state read failed: {}", err.message);
        },
        0, true);
}

// ============================================================================
// Runner
// ============================================================================

Runner::Runner(Hooks hooks) : hooks_(std::move(hooks)), steps_(build_sequence()) {}

Runner::~Runner() {
    if (phase_is_live(phase_)) {
        s_live_runs.fetch_sub(1);
    }
}

const Step* Runner::current_step() const {
    if (phase_ == Phase::Idle || index_ >= steps_.size()) {
        return nullptr;
    }
    return &steps_[index_];
}

Prompt Runner::prompt() const {
    if (phase_ != Phase::AwaitingUser || index_ >= steps_.size()) {
        return Prompt::None;
    }
    return steps_[index_].prompt_before;
}

void Runner::start() {
    if (phase_ == Phase::Running || phase_ == Phase::AwaitingUser || phase_ == Phase::Finishing) {
        return;
    }
    ++run_;
    index_ = 0;
    error_.clear();
    spdlog::info("[NozzleOffsetCal] Starting a {}-step run", steps_.size());
    run_current();
}

void Runner::confirm() {
    if (phase_ != Phase::AwaitingUser) {
        return;
    }
    run_current();
}

void Runner::cancel() {
    if (phase_ != Phase::Running && phase_ != Phase::AwaitingUser) {
        return;
    }
    ++run_;
    // A probe pass holds Klipper's command queue until it ends, so EXIT would
    // wait behind it without the abort.
    const bool probing = phase_ == Phase::Running && index_ < steps_.size() &&
                         steps_[index_].kind == StepKind::Probe;
    if (probing && hooks_.abort_probe) {
        hooks_.abort_probe();
    }
    spdlog::info("[NozzleOffsetCal] Cancelled at step {}", index_);
    send_exit(Phase::Cancelled);
}

void Runner::reset() {
    if (phase_ != Phase::Done && phase_ != Phase::Failed && phase_ != Phase::Cancelled) {
        return;
    }
    index_ = 0;
    error_.clear();
    set_phase(Phase::Idle);
}

void Runner::run_current() {
    const Step& step = steps_[index_];
    if (step.prompt_before != Prompt::None && phase_ != Phase::AwaitingUser) {
        set_phase(Phase::AwaitingUser);
        return;
    }
    set_phase(Phase::Running);
    spdlog::debug("[NozzleOffsetCal] Step {}: {}", index_, step.gcode);
    const uint64_t run = run_;
    hooks_.send(step.gcode, kStepTimeoutMs,
                [this, run](bool ok, const std::string& error) { on_step_done(run, ok, error); });
}

void Runner::on_step_done(uint64_t run, bool ok, const std::string& error) {
    if (run != run_ || phase_ != Phase::Running) {
        return;
    }
    const Step& step = steps_[index_];
    if (!ok) {
        error_ = failure_text(error);
        spdlog::warn("[NozzleOffsetCal] {} failed: {}", step.gcode, error);
        if (step.kind == StepKind::DetectPlate && screws_tilt::plate_still_on_bed(error)) {
            set_phase(Phase::AwaitingUser);
            return;
        }
        ++run_;
        send_exit(Phase::Failed);
        return;
    }
    error_.clear();
    ++index_;
    if (index_ < steps_.size()) {
        run_current();
        return;
    }
    // The firmware saved every head's result inside the last probe step.
    spdlog::info("[NozzleOffsetCal] All heads calibrated; exiting the calibration state");
    send_exit(Phase::Finishing);
}

void Runner::send_exit(Phase then) {
    set_phase(then);
    const uint64_t run = run_;
    hooks_.send(kExitGcode, kStepTimeoutMs, [this, run](bool ok, const std::string& error) {
        if (!ok) {
            spdlog::warn("[NozzleOffsetCal] {} failed: {}", kExitGcode, error);
        }
        if (run == run_ && phase_ == Phase::Finishing) {
            set_phase(Phase::Done);
        }
    });
}

void Runner::set_phase(Phase phase) {
    const bool was_live = phase_is_live(phase_);
    const bool is_live = phase_is_live(phase);
    if (is_live != was_live) {
        s_live_runs.fetch_add(is_live ? 1 : -1);
    }
    phase_ = phase;
    if (hooks_.changed) {
        hooks_.changed();
    }
}

} // namespace helix::snapmaker::nozzle_offset
