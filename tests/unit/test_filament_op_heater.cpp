// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_op_heater.cpp
 * @brief The nozzle heat around a filament op: FilamentOpHeater, and every
 *        execute_filament_*() surface going through it.
 *
 * Run with: ./build/bin/helix-tests "[filament][preheat][heater]"
 *
 * The home Filament tile's dialog dispatches through execute_filament_load/
 * unload/purge() and closes the moment the button is pressed, so these drive
 * that entry with a real TemperatureController and read the gcode Moonraker
 * receives: the heat command, then the filament macro, in that order.
 */

#include "ui_filament_runout_handler.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/filament_runout_handler_test_access.h"
#include "../test_helpers/filament_sensor_widget_test_access.h"
#include "../test_helpers/scoped_shared_resource.h"
#include "../test_helpers/update_queue_test_access.h"
#include "ams_state.h"
#include "app_globals.h"
#include "filament_op_heater.h"
#include "macro_param_cache.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "post_op_cooldown_manager.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "safety_settings_manager.h"
#include "standard_macros.h"
#include "temperature_controller.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::FilamentSensorWidget;
using helix::FilamentSensorWidgetTestAccess;
using helix::ui::decide_preheat;
using helix::ui::FilamentOpHeater;
using helix::ui::PreheatDecision;
using helix::ui::PreheatInputs;

// ============================================================================
// decide_preheat(): the rule, with no printer
// ============================================================================

namespace {

PreheatInputs nozzle(int current, int target, std::optional<int> material = std::nullopt,
                     int held = 0) {
    PreheatInputs in;
    in.current_c = current;
    in.target_c = target;
    in.material_c = material;
    in.held_c = held;
    in.floor_c = 170;
    in.ceiling_c = 300;
    return in;
}

} // namespace

TEST_CASE("decide_preheat: a cold nozzle heats to the material", "[filament][preheat][heater]") {
    const PreheatDecision d = decide_preheat(nozzle(25, 0, 240));
    CHECK(d.heat);
    CHECK(d.send_c == 240);
    CHECK(d.wait_c == 240);
}

TEST_CASE("decide_preheat: with no material named it heats to the default load temperature",
          "[filament][preheat][heater]") {
    const PreheatDecision d = decide_preheat(nozzle(25, 0));
    CHECK(d.heat);
    CHECK(d.send_c == 220);
    CHECK(d.wait_c == 220);
}

TEST_CASE("decide_preheat: a nozzle holding a target it has reached runs at once",
          "[filament][preheat][heater]") {
    // A paused print at its own temperature, a user's deliberate preheat: the
    // op runs there even when the incoming material names a hotter one.
    CHECK_FALSE(decide_preheat(nozzle(210, 210, 240)).heat);
    CHECK_FALSE(decide_preheat(nozzle(206, 210, 240)).heat); // within the margin
    // Still heating toward that target is not reached.
    CHECK(decide_preheat(nozzle(150, 210, 240)).heat);
}

TEST_CASE("decide_preheat: a held target below the extrusion floor is not hot enough",
          "[filament][preheat][heater]") {
    const PreheatDecision d = decide_preheat(nozzle(150, 150, 210));
    CHECK(d.heat);
    CHECK(d.send_c == 210);
}

TEST_CASE("decide_preheat: with the heater off, near the material temperature is hot enough",
          "[filament][preheat][heater]") {
    CHECK_FALSE(decide_preheat(nozzle(206, 0, 210)).heat);
    CHECK(decide_preheat(nozzle(204, 0, 210)).heat);
    // Unknown material: anything at or above the floor extrudes.
    CHECK_FALSE(decide_preheat(nozzle(170, 0)).heat);
    CHECK(decide_preheat(nozzle(169, 0)).heat);
}

TEST_CASE("decide_preheat: the swap latch raises both the bar and the wait",
          "[filament][preheat][heater]") {
    // The previous material sat at 250 and the nozzle cooled: it reheats to purge
    // that material out ahead of the new one.
    const PreheatDecision d = decide_preheat(nozzle(215, 0, 210, /*held=*/250));
    CHECK(d.heat);
    CHECK(d.send_c == 210); // the controller's keep_previous_hot holds it at 250
    CHECK(d.wait_c == 250);
}

TEST_CASE("decide_preheat: targets stay between the floor and the hotend's maximum",
          "[filament][preheat][heater]") {
    const PreheatDecision hot = decide_preheat(nozzle(25, 0, 350));
    CHECK(hot.send_c == 300);
    CHECK(hot.wait_c == 300);
    const PreheatDecision cool = decide_preheat(nozzle(25, 0, 150));
    CHECK(cool.send_c == 170);
}

// ============================================================================
// The surfaces, end to end
// ============================================================================

namespace {

constexpr const char* HEAT_GCODE = "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=";

class HeaterFixture : public LVGLTestFixture {
  public:
    HeaterFixture() : client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        state.temperature_state().init_extruders({"extruder"});
        state.set_klippy_state_sync(helix::KlippyState::READY);
        client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPI>(client, state);
        previous_api_ = get_moonraker_api();
        set_moonraker_api(api.get());
        controller =
            std::make_unique<helix_test::ScopedSharedResource<helix::TemperatureController>>(
                std::make_shared<helix::TemperatureController>(state, api.get()));
        heater = std::make_unique<helix_test::ScopedSharedResource<FilamentOpHeater>>(
            std::make_shared<FilamentOpHeater>(state));

        helix::AmsState::instance().clear_backends();
        helix::MacroParamCache::instance().clear();
        configure_filament_macros();
        cold_extrude_was_ = helix::SafetySettingsManager::instance().get_allow_cold_extrude();
        helix::SafetySettingsManager::instance().set_allow_cold_extrude(false);
        PostOpCooldownManager::instance().init();
        client.clear_gcode_script_history();
    }

    ~HeaterFixture() override {
        heater.reset();
        controller.reset();
        helix::SafetySettingsManager::instance().set_allow_cold_extrude(cold_extrude_was_);
        PostOpCooldownManager::instance().cancel();
        set_moonraker_api(previous_api_);
        StandardMacros::instance().reset();
        helix::MacroParamCache::instance().clear();
        helix::AmsState::instance().clear_backends();
        helix::ui::UpdateQueue::instance().drain();
        client.disconnect();
        api.reset();
    }

    void configure_filament_macros() {
        helix::PrinterDiscovery hardware;
        nlohmann::json objects = {"extruder", "gcode_macro LOAD_FILAMENT",
                                  "gcode_macro UNLOAD_FILAMENT", "gcode_macro PURGE"};
        hardware.parse_objects(objects);
        StandardMacros::instance().reset();
        StandardMacros::instance().init(hardware);
        REQUIRE_FALSE(StandardMacros::instance().get(StandardMacroSlot::LoadFilament).is_empty());
        REQUIRE_FALSE(StandardMacros::instance().get(StandardMacroSlot::UnloadFilament).is_empty());
    }

    /// Klipper's report for the extruder, through the real status path.
    void feed_nozzle(double actual_c, double target_c) {
        state.update_from_status({{"extruder", {{"temperature", actual_c}, {"target", target_c}}}});
        helix::ui::UpdateQueue::instance().drain();
    }

    /// The heater's handlers run deferred and queue the cooldown's own work, so
    /// drain until quiet.
    static void drain_all() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    /// Index of the first script containing @p needle, or -1.
    [[nodiscard]] int sent_at(const std::string& needle) const {
        const auto& hist = client.gcode_script_history();
        for (size_t i = 0; i < hist.size(); ++i) {
            if (hist[i].find(needle) != std::string::npos) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
    [[nodiscard]] bool sent(const std::string& needle) const {
        return sent_at(needle) >= 0;
    }

    MoonrakerClientMock client;
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPI> api;
    std::unique_ptr<helix_test::ScopedSharedResource<helix::TemperatureController>> controller;
    std::unique_ptr<helix_test::ScopedSharedResource<FilamentOpHeater>> heater;
    FilamentSensorWidget tile;

  private:
    helix::IMoonrakerAPI* previous_api_ = nullptr;
    bool cold_extrude_was_ = false;
};

} // namespace

TEST_CASE_METHOD(HeaterFixture, "The home tile's Load heats a cold nozzle before the load macro",
                 "[filament][preheat][heater][tile]") {
    feed_nozzle(25, 0);

    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(heater->get().pending());
    CHECK(sent(std::string(HEAT_GCODE) + "220"));
    CHECK_FALSE(sent("LOAD_FILAMENT"));

    feed_nozzle(150, 220);
    CHECK_FALSE(sent("LOAD_FILAMENT")); // still heating

    feed_nozzle(218, 220);
    CHECK_FALSE(heater->get().pending());
    REQUIRE(sent("LOAD_FILAMENT"));
    CHECK(sent_at(HEAT_GCODE) < sent_at("LOAD_FILAMENT"));
}

TEST_CASE_METHOD(HeaterFixture, "The home tile's Unload heats a cold nozzle before the macro",
                 "[filament][preheat][heater][tile]") {
    feed_nozzle(25, 0);

    FilamentSensorWidgetTestAccess::dispatch_unload(tile);
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(heater->get().pending());
    CHECK(sent(HEAT_GCODE));
    CHECK_FALSE(sent("UNLOAD_FILAMENT"));

    feed_nozzle(220, 220);
    REQUIRE(sent("UNLOAD_FILAMENT"));
    CHECK(sent_at(HEAT_GCODE) < sent_at("UNLOAD_FILAMENT"));
}

TEST_CASE_METHOD(HeaterFixture, "The home tile's Purge heats a cold nozzle before the macro",
                 "[filament][preheat][heater][tile]") {
    feed_nozzle(25, 0);

    FilamentSensorWidgetTestAccess::dispatch_purge(tile);
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(heater->get().pending());
    CHECK_FALSE(sent("PURGE"));

    feed_nozzle(220, 220);
    CHECK(sent("PURGE"));
}

TEST_CASE_METHOD(HeaterFixture, "A hot nozzle loads at once with no heat command",
                 "[filament][preheat][heater][tile]") {
    feed_nozzle(215, 215);
    client.clear_gcode_script_history();

    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    helix::ui::UpdateQueue::instance().drain();

    CHECK_FALSE(heater->get().pending());
    CHECK(sent("LOAD_FILAMENT"));
    CHECK_FALSE(sent(HEAT_GCODE));
}

TEST_CASE_METHOD(HeaterFixture, "The allow-cold-load setting skips the preheat",
                 "[filament][preheat][heater][tile]") {
    helix::SafetySettingsManager::instance().set_allow_cold_extrude(true);
    feed_nozzle(25, 0);

    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    helix::ui::UpdateQueue::instance().drain();

    CHECK_FALSE(heater->get().pending());
    CHECK(sent("LOAD_FILAMENT"));
    CHECK_FALSE(sent(HEAT_GCODE));
}

TEST_CASE_METHOD(HeaterFixture, "A runout reload during a pause with a hot nozzle does not wait",
                 "[filament][preheat][heater][runout]") {
    // The paused print holds its own temperature; the reload runs at it.
    feed_nozzle(210, 210);
    client.clear_gcode_script_history();

    helix::ui::FilamentRunoutHandler handler(api.get());
    helix::ui::FilamentRunoutHandlerTestAccess::dispatch_load(handler);
    helix::ui::UpdateQueue::instance().drain();

    CHECK_FALSE(heater->get().pending());
    CHECK(sent("LOAD_FILAMENT"));
    CHECK_FALSE(sent(HEAT_GCODE));
}

TEST_CASE_METHOD(HeaterFixture, "The load runs after the surface that asked for it is gone",
                 "[filament][preheat][heater][tile]") {
    feed_nozzle(25, 0);
    {
        FilamentSensorWidget closing_tile;
        FilamentSensorWidgetTestAccess::dispatch_load(closing_tile);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(heater->get().pending());
    }
    feed_nozzle(220, 220);
    CHECK(sent("LOAD_FILAMENT"));
}

TEST_CASE_METHOD(HeaterFixture, "A second tap while heating cancels and turns the heater back off",
                 "[filament][preheat][heater][cancel]") {
    feed_nozzle(25, 0);
    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(heater->get().pending());
    client.clear_gcode_script_history();

    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    helix::ui::UpdateQueue::instance().drain();

    CHECK_FALSE(heater->get().pending());
    CHECK(sent(std::string(HEAT_GCODE) + "0")); // it was off when the user asked

    // Heat arriving later for any other reason runs nothing.
    feed_nozzle(220, 220);
    CHECK_FALSE(sent("LOAD_FILAMENT"));
}

TEST_CASE_METHOD(HeaterFixture, "Cancelling leaves alone a target someone else had set",
                 "[filament][preheat][heater][cancel]") {
    feed_nozzle(25, 200); // the user's own preheat, still climbing
    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(heater->get().pending());
    client.clear_gcode_script_history();

    REQUIRE(heater->get().cancel());
    helix::ui::UpdateQueue::instance().drain();

    CHECK_FALSE(sent(std::string(HEAT_GCODE) + "0"));
}

TEST_CASE_METHOD(HeaterFixture, "A preheat whose target is taken away runs nothing",
                 "[filament][preheat][heater][cancel]") {
    feed_nozzle(25, 0);
    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(heater->get().pending());

    feed_nozzle(100, 220); // the target lands
    REQUIRE(heater->get().pending());
    feed_nozzle(100, 0); // and is cleared: the user, a heater fault, a shutdown
    CHECK_FALSE(heater->get().pending());

    feed_nozzle(220, 220);
    CHECK_FALSE(sent("LOAD_FILAMENT"));
}

TEST_CASE_METHOD(HeaterFixture, "A preheat cancels a cooldown left from the previous op",
                 "[filament][preheat][heater][cooldown]") {
    auto& cd = PostOpCooldownManager::instance();
    cd.schedule();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(cd.has_pending_timer());

    feed_nozzle(25, 0);
    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(heater->get().pending());
    CHECK_FALSE(cd.has_pending_timer());
}

TEST_CASE_METHOD(HeaterFixture, "A finished op schedules the cooldown only when idle",
                 "[filament][preheat][heater][cooldown]") {
    auto& cd = PostOpCooldownManager::instance();

    state.update_from_status({{"print_stats", {{"state", "standby"}}}});
    heater->get().op_succeeded();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(cd.has_pending_timer());

    cd.cancel();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(cd.has_pending_timer());

    // A paused print resumes at its own temperature.
    state.update_from_status({{"print_stats", {{"state", "paused"}}}});
    heater->get().op_succeeded();
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(cd.has_pending_timer());
}

TEST_CASE_METHOD(HeaterFixture, "A backend op's end on the action feed schedules the cooldown",
                 "[filament][preheat][heater][cooldown]") {
    auto& cd = PostOpCooldownManager::instance();
    helix::AmsState::instance().init_subjects(true);
    state.update_from_status({{"print_stats", {{"state", "standby"}}}});
    lv_subject_t* action = helix::AmsState::instance().get_ams_action_subject();
    lv_subject_set_int(action, static_cast<int>(helix::AmsAction::IDLE));

    SECTION("busy, then IDLE") {
        heater->get().watch_backend_completion();
        drain_all();
        CHECK_FALSE(cd.has_pending_timer()); // IDLE at dispatch is not the end

        lv_subject_set_int(action, static_cast<int>(helix::AmsAction::LOADING));
        drain_all();
        lv_subject_set_int(action, static_cast<int>(helix::AmsAction::IDLE));
        drain_all();
        CHECK(cd.has_pending_timer());
    }
    SECTION("busy, then ERROR leaves the heater for the recovery") {
        heater->get().watch_backend_completion();
        lv_subject_set_int(action, static_cast<int>(helix::AmsAction::LOADING));
        drain_all();
        lv_subject_set_int(action, static_cast<int>(helix::AmsAction::ERROR));
        drain_all();
        CHECK_FALSE(cd.has_pending_timer());
    }
}
