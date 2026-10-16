// SPDX-License-Identifier: GPL-3.0-or-later
//
// A chamber heater that turns down the target it was sent: the latch on
// PrinterTemperatureState, the route around the gcode queue that
// TemperatureController takes when the backend has one, and the arbiter that
// decides when a print held by an unheated chamber is put to the user.
#include "../lvgl_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/scoped_shared_resource.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "chamber_heater_backend.h"
#include "chamber_stall_prompt.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "printer_temperature_state.h"
#include "settings_manager.h"
#include "temperature_controller.h"

#include <lvgl/src/others/translation/lv_translation.h>

#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ChamberStallAction;
using helix::ChamberStallArbiter;
using helix::ChamberStallInputs;
using helix::PrinterTemperatureState;
using helix::chamber::CommandRejection;
using helix::chamber::RefusalCause;

namespace {

void drain() {
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
}

struct RejectionStateFixture : public LVGLTestFixture {
    PrinterTemperatureState ts;

    RejectionStateFixture() {
        ts.init_subjects(false);
        ts.set_chamber_heater_name("heater_generic dragonbreath");
        ts.set_chamber_diagnostics_source("dragonbreath", "dragonbreath",
                                          "output_pin dragonbreath_filter");
    }

    void frame(const nlohmann::json& status) {
        ts.update_from_status(status);
    }
    int rejected() {
        return lv_subject_get_int(ts.get_chamber_heater_command_rejected_subject());
    }
    int seq() {
        return lv_subject_get_int(ts.get_chamber_heater_rejection_seq_subject());
    }
    std::string text() {
        return lv_subject_get_string(ts.get_chamber_heater_rejection_text_subject());
    }
};

/// The frame the U1 rig sent when the box answered M141 S60 with a revision
/// conflict: the module zeroed the heater target and held the reason.
nlohmann::json conflict_frame(int seq) {
    return {{"heater_generic dragonbreath", {{"temperature", 28.0}, {"target", 0.0}}},
            {"dragonbreath",
             {{"protocol_error", "revision_conflict"},
              {"protocol_error_code", "revision_conflict"},
              {"protocol_error_seq", seq},
              {"protocol_error_target", 60.0}}}};
}

} // namespace

TEST_CASE_METHOD(RejectionStateFixture,
                 "a rejected chamber target latches until the heater takes one",
                 "[chamber][rejection][latch]") {
    REQUIRE(rejected() == 0);
    frame(conflict_frame(1));
    CHECK(rejected() == 1);
    CHECK(seq() == 1);
    CHECK(text() == lv_tr("Another controller changed the heater at the same time."));
    CHECK(ts.chamber_command_rejection().kind == CommandRejection::Conflict);
    CHECK(ts.chamber_command_rejection().requested_target_c == Catch::Approx(60.0));

    SECTION("a frame that says nothing about it keeps the latch") {
        frame({{"dragonbreath", {{"ptc_temp", 31.0}}}});
        CHECK(rejected() == 1);
        CHECK(seq() == 1);
    }

    SECTION("each later rejection is a new event, the same reason or not") {
        frame({{"dragonbreath", {{"protocol_error_seq", 2}}}});
        CHECK(rejected() == 1);
        CHECK(seq() == 2);
        frame(
            {{"dragonbreath", {{"protocol_error_seq", 3}, {"protocol_error_code", "power_off"}}}});
        CHECK(seq() == 3);
        CHECK(ts.chamber_command_rejection().cause == RefusalCause::TurnedOff);
        CHECK(text() == lv_tr("The heater was turned off at the heater."));
    }

    SECTION("the heater taking the requested target clears it") {
        frame({{"heater_generic dragonbreath", {{"target", 59.0}}}});
        CHECK(rejected() == 1);
        frame({{"heater_generic dragonbreath", {{"target", 60.0}}}});
        CHECK(rejected() == 0);
        CHECK(text().empty());
        CHECK(ts.chamber_command_rejection().kind == CommandRejection::None);
    }

    SECTION("the device reporting nothing outstanding clears it") {
        frame({{"dragonbreath", {{"protocol_error", ""}, {"protocol_error_code", ""}}}});
        CHECK(rejected() == 0);
        CHECK(seq() == 1);
    }

    SECTION("clearing re-arms: the next rejection latches again") {
        frame({{"dragonbreath", {{"protocol_error_code", ""}}}});
        REQUIRE(rejected() == 0);
        frame(conflict_frame(2));
        CHECK(rejected() == 1);
        CHECK(seq() == 2);
    }
}

TEST_CASE_METHOD(RejectionStateFixture,
                 "a rejection without a requested target clears on a new one",
                 "[chamber][rejection][latch]") {
    frame({{"heater_generic dragonbreath", {{"temperature", 28.0}, {"target", 0.0}}},
           {"dragonbreath",
            {{"protocol_error_code", "heater_inhibited"}, {"protocol_error_seq", 1}}}});
    REQUIRE(rejected() == 1);
    CHECK(text() == lv_tr("The heater refused the command."));
    frame({{"heater_generic dragonbreath", {{"target", 45.0}}}});
    CHECK(rejected() == 0);
}

TEST_CASE_METHOD(RejectionStateFixture, "a refusal for a fault names the fault",
                 "[chamber][rejection][latch]") {
    frame({{"dragonbreath",
            {{"fault", true},
             {"fault_reason", "ptc_overtemp"},
             {"protocol_error_code", "heater_fault"},
             {"protocol_error_seq", 1}}}});
    REQUIRE(rejected() == 1);
    CHECK(text() == fmt::format(fmt::runtime(lv_tr("The heater reports a fault: {}.")),
                                lv_tr("Heater over-temperature")));
}

TEST_CASE_METHOD(RejectionStateFixture, "what a glue reports at connect decides the latch",
                 "[chamber][rejection][latch]") {
    SECTION("an old seq with nothing outstanding is not a rejection") {
        frame({{"dragonbreath", {{"protocol_error_code", ""}, {"protocol_error_seq", 7}}}});
        CHECK(rejected() == 0);
        CHECK(seq() == 0);
        frame({{"dragonbreath",
                {{"protocol_error_code", "auth_failed"}, {"protocol_error_seq", 8}}}});
        CHECK(rejected() == 1);
        CHECK(text() == lv_tr("The heater did not accept the printer's credentials."));
    }

    SECTION("an older glue's free-text complaint never latches") {
        frame({{"dragonbreath", {{"protocol_error", "revision_conflict"}}}});
        CHECK(rejected() == 0);
    }

    SECTION("a heater whose backend has no diagnostics never latches") {
        ts.set_chamber_diagnostics_source("", "", "");
        frame(conflict_frame(1));
        CHECK(rejected() == 0);
    }
}

// ============================================================================
// TemperatureController: the route around the gcode queue
// ============================================================================

namespace {

/// Answers the appliance's set-target method the way the test asks: taken,
/// refused, or absent (an older glue Moonraker has no endpoint for).
class RouteMockClient : public MoonrakerClientMock {
  public:
    using MoonrakerClientMock::MoonrakerClientMock;

    enum class Mode { Accept, Refuse, Missing };
    Mode mode = Mode::Accept;
    std::vector<double> route_targets;

    helix::RequestId send_jsonrpc(
        const std::string& method, const nlohmann::json& params,
        std::function<void(const nlohmann::json&)> success_cb,
        std::function<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms = 0,
        bool silent = false,
        std::optional<helix::rpc_error_policy::CallerIntent> intent = std::nullopt) override {
        if (method == "printer.dragonbreath.set_target") {
            route_targets.push_back(params.value("target", -1.0));
            if (mode == Mode::Missing) {
                MoonrakerError err;
                err.type = MoonrakerErrorType::JSON_RPC_ERROR;
                err.code = -32601;
                err.message = "Method not found";
                if (error_cb)
                    error_cb(err);
            } else if (success_cb) {
                success_cb({{"result",
                             {{"accepted", mode == Mode::Accept},
                              {"reason", mode == Mode::Accept ? "" : "power_off"},
                              {"seq", 1}}}});
            }
            return 0;
        }
        return MoonrakerClientMock::send_jsonrpc(method, params, std::move(success_cb),
                                                 std::move(error_cb), timeout_ms, silent, intent);
    }

    std::vector<std::string> heater_gcode() {
        std::vector<std::string> out;
        for (const auto& g : gcode_script_history()) {
            if (g.find("SET_HEATER_TEMPERATURE") != std::string::npos ||
                g.find("M104") != std::string::npos) {
                out.push_back(g);
            }
        }
        return out;
    }
};

struct RouteFixture : public LVGLTestFixture {
    RouteMockClient client;
    helix::PrinterState& state;
    MoonrakerAPI api;
    std::shared_ptr<helix::TemperatureController> controller;
    helix_test::ScopedSharedResource<helix::TemperatureController> scope;

    RouteFixture()
        : client(MoonrakerClientMock::PrinterType::VORON_24), state(reset_global_state()),
          api(client, state),
          controller(std::make_shared<helix::TemperatureController>(state, &api)),
          scope(controller) {
        state.set_klippy_state_sync(helix::KlippyState::READY);
        helix::SettingsManager::instance().set_chamber_heater_assignment("auto");
        helix::PrinterDiscovery hardware;
        hardware.parse_objects(nlohmann::json{"heater_generic dragonbreath", "dragonbreath",
                                              "extruder", "heater_bed"});
        state.set_hardware(hardware);
        drain();
        client.clear_gcode_script_history();
    }

    ~RouteFixture() override {
        drain();
    }

    static helix::PrinterState& reset_global_state() {
        PrinterStateTestAccess::reset(get_printer_state());
        get_printer_state().init_subjects(false);
        return get_printer_state();
    }
};

} // namespace

TEST_CASE_METHOD(RouteFixture, "chamber targets take the backend's route around the gcode queue",
                 "[chamber][rejection][route]") {
    REQUIRE(controller->chamber_has_direct_route());

    SECTION("a chamber target goes by the route, not gcode") {
        controller->set_target(helix::HeaterType::Chamber, 60.0);
        drain();
        REQUIRE(client.route_targets == std::vector<double>{60.0});
        CHECK(client.heater_gcode().empty());
        CHECK(controller->last_chamber_target_c() == Catch::Approx(60.0));
    }

    SECTION("other heaters never do") {
        controller->set_target(helix::HeaterType::Nozzle, 210.0);
        drain();
        CHECK(client.route_targets.empty());
        CHECK(client.heater_gcode().size() == 1);
    }

    SECTION("a glue without the route falls back to gcode, once and for good") {
        client.mode = RouteMockClient::Mode::Missing;
        controller->set_target(helix::HeaterType::Chamber, 60.0);
        drain();
        CHECK(client.route_targets.size() == 1);
        REQUIRE(client.heater_gcode().size() == 1);
        CHECK(client.heater_gcode()[0].find("HEATER=dragonbreath TARGET=60") != std::string::npos);
        CHECK_FALSE(controller->chamber_has_direct_route());

        controller->set_target(helix::HeaterType::Chamber, 50.0);
        drain();
        CHECK(client.route_targets.size() == 1);
        CHECK(client.heater_gcode().size() == 2);
    }

    SECTION("a refusal reaches the caller as an error") {
        client.mode = RouteMockClient::Mode::Refuse;
        std::string reason;
        helix::SendOptions opts;
        opts.toast = false;
        opts.on_error = [&reason](const MoonrakerError& e) { reason = e.message; };
        bool succeeded = false;
        opts.on_success = [&succeeded]() { succeeded = true; };
        controller->set_target(helix::HeaterType::Chamber, 60.0, opts);
        drain();
        CHECK(reason == "power_off");
        CHECK_FALSE(succeeded);
        CHECK(client.heater_gcode().empty());
    }
}

TEST_CASE_METHOD(RouteFixture,
                 "a chamber retry never queues behind the wait it is meant to release",
                 "[chamber][rejection][route]") {
    bool failed = false;
    helix::SendOptions opts;
    opts.toast = false;
    opts.on_error = [&failed](const MoonrakerError&) { failed = true; };

    SECTION("with the route, the retry goes by it") {
        controller->retry_chamber_target(60.0, opts);
        drain();
        CHECK(client.route_targets == std::vector<double>{60.0});
        CHECK_FALSE(failed);
    }

    SECTION("a missing route fails the retry instead of sending gcode") {
        client.mode = RouteMockClient::Mode::Missing;
        controller->retry_chamber_target(60.0, opts);
        drain();
        CHECK(failed);
        CHECK(client.heater_gcode().empty());
        CHECK_FALSE(controller->chamber_has_direct_route());
    }

    SECTION("a backend with no route sends nothing at all") {
        controller->set_chamber_actions("", "", 0.0, "");
        controller->retry_chamber_target(60.0, opts);
        drain();
        CHECK(failed);
        CHECK(client.route_targets.empty());
        CHECK(client.heater_gcode().empty());
    }
}

TEST_CASE_METHOD(RouteFixture, "Try again resends the target the heater turned down",
                 "[chamber][rejection][route][prompt]") {
    state.temperature_state().update_from_status(conflict_frame(1));
    REQUIRE(state.temperature_state().chamber_command_rejection().kind ==
            CommandRejection::Conflict);

    CHECK(helix::ui::ChamberStallPrompt::retry_target_c() == Catch::Approx(60.0));
    REQUIRE(helix::ui::ChamberStallPrompt::retry());
    drain();
    CHECK(client.route_targets == std::vector<double>{60.0});
    CHECK(client.heater_gcode().empty());

    SECTION("with no route it sends nothing") {
        controller->set_chamber_actions("", "", 0.0, "");
        CHECK_FALSE(helix::ui::ChamberStallPrompt::retry());
        drain();
        CHECK(client.route_targets.size() == 1);
        CHECK(client.heater_gcode().empty());
    }
}

// ============================================================================
// When the user hears about it
// ============================================================================

TEST_CASE("the chamber prompt arms once per rejection", "[chamber][rejection][prompt]") {
    ChamberStallArbiter arbiter;
    ChamberStallInputs waiting;
    waiting.job_active = true;
    waiting.chamber_wait = true;

    SECTION("a refused target during a chamber wait prompts, once per rejection") {
        auto in = waiting;
        in.rejected = true;
        in.rejection_seq = 1;
        CHECK(arbiter.decide(in) == ChamberStallAction::Prompt);
        CHECK(arbiter.decide(in) == ChamberStallAction::None);
        in.chamber_target_deci = 0;
        CHECK(arbiter.decide(in) == ChamberStallAction::None);
        in.rejection_seq = 2;
        CHECK(arbiter.decide(in) == ChamberStallAction::Prompt);
    }

    SECTION("a rejection outside a chamber wait mid-print stays on the banner") {
        auto in = waiting;
        in.chamber_wait = false;
        in.rejected = true;
        in.rejection_seq = 1;
        CHECK(arbiter.decide(in) == ChamberStallAction::None);
        // The wait that follows still prompts for it.
        in.chamber_wait = true;
        CHECK(arbiter.decide(in) == ChamberStallAction::Prompt);
    }

    SECTION("an unheated chamber with no rejection prompts once per job") {
        auto in = waiting;
        CHECK(arbiter.decide(in) == ChamberStallAction::Prompt);
        CHECK(arbiter.decide(in) == ChamberStallAction::None);
        in.chamber_wait = false;
        CHECK(arbiter.decide(in) == ChamberStallAction::None);
        in.chamber_wait = true;
        CHECK(arbiter.decide(in) == ChamberStallAction::None);
        in.job_active = false;
        CHECK(arbiter.decide(in) == ChamberStallAction::None);
        in.job_active = true;
        CHECK(arbiter.decide(in) == ChamberStallAction::Prompt);
    }

    SECTION("a heating chamber is just a wait") {
        auto in = waiting;
        in.chamber_target_deci = 600;
        CHECK(arbiter.decide(in) == ChamberStallAction::None);
    }

    SECTION("no job: a toast per rejection, never a prompt") {
        ChamberStallInputs in;
        in.rejected = true;
        in.rejection_seq = 1;
        CHECK(arbiter.decide(in) == ChamberStallAction::Toast);
        CHECK(arbiter.decide(in) == ChamberStallAction::None);
        in.rejection_seq = 2;
        CHECK(arbiter.decide(in) == ChamberStallAction::Toast);
        in.rejected = false;
        in.rejection_seq = 2;
        CHECK(arbiter.decide(in) == ChamberStallAction::None);
    }

    SECTION("a rejection already prompted does not toast after the job") {
        auto in = waiting;
        in.rejected = true;
        in.rejection_seq = 1;
        REQUIRE(arbiter.decide(in) == ChamberStallAction::Prompt);
        in.job_active = false;
        CHECK(arbiter.decide(in) == ChamberStallAction::None);
    }
}
