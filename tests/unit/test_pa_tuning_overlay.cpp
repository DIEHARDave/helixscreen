// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_overlay_pa_tuning.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "app_globals.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "tool_state.h"
#include "tune_controller.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using helix::ui::PaTuningOverlay;
using nlohmann::json;

namespace {

struct TunerFixture : LVGLTestFixture {
    MoonrakerClientMock client;
    MoonrakerAPI api;
    PaTuningOverlay overlay;

    TunerFixture() : api(client, get_printer_state()) {
        auto& state = get_printer_state();
        state.init_subjects(false);
        state.set_klippy_state_sync(KlippyState::READY);
        state.temperature_state().init_extruders({"extruder"});
        client.connect("ws://mock/websocket", []() {}, []() {});
        overlay.init_subjects();
        overlay.set_api(&api);
        overlay.set_extruder_for_test("extruder");
        overlay.set_raise_on_start_for_test(false);
    }
    ~TunerFixture() override {
        overlay.cleanup();
        status({{"print_stats", {{"state", "standby"}, {"filename", ""}}}});
        client.stop_temperature_simulation();
        client.disconnect();
    }

    void status(const json& s) {
        get_printer_state().update_from_status(s);
        helix::ui::UpdateQueue::instance().drain();
    }

    void start_session() {
        overlay.session_for_test().start(pa::TowerGeometry{}, pa::Ramp{0.0f, 0.09f, 10}, 0.04f);
        overlay.begin_observing_for_test();
    }

    void printing(int layer) {
        status({{"print_stats",
                 {{"state", "printing"},
                  {"filename", PaTuningOverlay::TEST_FILENAME},
                  {"info", {{"current_layer", layer}, {"total_layer", 156}}}}}});
    }

    /// The printer echoes K the way Klipper reports it.
    void report_k(double k) {
        status({{"extruder", {{"pressure_advance", k}}}});
    }
};

} // namespace

TEST_CASE_METHOD(TunerFixture, "the tuner ramps K on its own print and takes a nudge",
                 "[pa_tune][pa_tuner]") {
    start_session();
    printing(1);
    REQUIRE(overlay.session_for_test().state() == pa::TuningSession::State::Printing);
    CHECK(client.last_send_script() == "SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.0000");
    report_k(0.0);

    printing(22);
    CHECK(client.last_send_script() == "SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.0100");
    report_k(0.01);

    overlay.nudge_for_test(1); // default step 0.005
    CHECK(client.last_send_script() == "SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.0150");
    CHECK(overlay.session_for_test().mode() == pa::TuningSession::Mode::Manual);
    report_k(0.015);

    status({{"print_stats", {{"state", "complete"}}}});
    REQUIRE(overlay.stage() == PaTuningOverlay::PICK);
    const auto bands = overlay.session_for_test().bands();
    REQUIRE(bands.size() == 2);
    CHECK(bands[1].k == Catch::Approx(0.015f));

    overlay.select_band_for_test(0);
    overlay.apply_pick_for_test();
    CHECK(overlay.stage() == PaTuningOverlay::RESULT);
    CHECK(client.last_send_script() == "SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.0000");
}

TEST_CASE_METHOD(TunerFixture, "a finished state from an earlier print does not end the run",
                 "[pa_tune][pa_tuner]") {
    status({{"print_stats", {{"state", "complete"}, {"filename", "benchy.gcode"}}}});
    start_session();
    CHECK(overlay.session_for_test().state() == pa::TuningSession::State::Starting);

    // Someone else's print starting is not ours either.
    status({{"print_stats", {{"state", "printing"}, {"filename", "benchy.gcode"}}}});
    CHECK(overlay.session_for_test().state() == pa::TuningSession::State::Starting);
}

TEST_CASE_METHOD(TunerFixture, "a pick after someone else's print started sends nothing",
                 "[pa_tune][pa_tuner]") {
    start_session();
    printing(1);
    report_k(0.0);
    printing(30);
    report_k(0.01);
    status({{"print_stats", {{"state", "complete"}}}});
    REQUIRE(overlay.stage() == PaTuningOverlay::PICK);

    // A different job takes the machine while the picker sits open.
    status({{"print_stats", {{"state", "printing"}, {"filename", "benchy.gcode"}}}});
    helix::tune::set_speed_percent(&api, 110);
    overlay.select_band_for_test(0);
    overlay.apply_pick_for_test();
    CHECK(client.last_send_script() == "M220 S110");
    CHECK(overlay.stage() == PaTuningOverlay::PICK);
}

TEST_CASE_METHOD(TunerFixture, "the Tune row's nudge during a run goes through the session",
                 "[pa_tune][pa_tuner]") {
    CHECK_FALSE(overlay.nudge_by(0.001f)); // no run yet: the caller sends itself
    start_session();
    printing(1);
    report_k(0.0);
    REQUIRE(overlay.nudge_by(0.001f));
    CHECK(overlay.session_for_test().mode() == pa::TuningSession::Mode::Manual);
    CHECK(client.last_send_script() == "SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.0010");
    // The ramp no longer overwrites it at the next band.
    printing(22);
    CHECK(client.last_send_script() == "SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.0010");
}

TEST_CASE("AMS lanes into one hotend are one head, and select no tool", "[pa_tune][pa_tuner]") {
    // Four lanes, one extruder: what a Happy Hare or AFC rig publishes.
    PhysicalHead lanes;
    lanes.extruder_name = "extruder";
    lanes.tool_indices = {0, 1, 2, 3};
    auto one = PaTuningOverlay::choose_head({lanes}, /*multi_extruder=*/false, 0, 2, "extruder");
    CHECK(one.tool == -1);
    CHECK(one.extruder == "extruder");

    // A changer with sparse numbering: the head's own tool is selected.
    PhysicalHead t0{std::string("extruder"), {0}};
    PhysicalHead t5{std::string("extruder5"), {5}};
    auto changer = PaTuningOverlay::choose_head({t0, t5}, true, 1, 0, "extruder");
    CHECK(changer.tool == 5);
    CHECK(changer.extruder == "extruder5");
    // Out-of-range picks clamp to a real head.
    CHECK(PaTuningOverlay::choose_head({t0, t5}, true, 9, 0, "extruder").tool == 5);
    CHECK(PaTuningOverlay::choose_head({}, true, 0, 0, "extruder1").extruder == "extruder1");
}
