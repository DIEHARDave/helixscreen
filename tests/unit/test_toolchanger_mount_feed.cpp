// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_toolchanger_mount_feed.cpp
 * @brief Loading and unloading through the mounted tool (MedusaHC): the stage
 *        plan, the gcode each stage sends, the operator prompt, the carriage
 *        restore, the printer's own macros, and the print gate.
 */

#include "ui_update_queue.h"

#include "../test_helpers/toolchanger_test_access.h"
#include "../test_helpers/toolchanger_test_helper.h"
#include "ams_backend_toolchanger.h"
#include "filament_op_slot_resolver.h"
#include "macro_param_cache.h"
#include "mount_feed_sequence.h"

#include <algorithm>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
namespace mf = helix::mount_feed;
using helix::test::ToolChangerHelper;
using json = nlohmann::json;

namespace {

mf::Request load_request() {
    mf::Request r;
    r.load = true;
    r.head = 2;
    r.temp_c = 215;
    r.mount_gcode = "SELECT_TOOL T=2";
    r.restore_gcode = "SELECT_TOOL T=0";
    r.prompt_title = "Load filament";
    r.prompt_text = "Insert filament into Tool 3, then tap Continue";
    r.continue_label = "Continue";
    r.cancel_label = "Cancel";
    return r;
}

std::vector<std::string> gcodes(const std::vector<mf::Stage>& stages) {
    std::vector<std::string> out;
    for (const auto& s : stages) {
        out.push_back(s.gcode);
    }
    return out;
}

/// A MedusaHC-shaped changer whose acks the test hands out one at a time.
class AckingChanger : public ToolChangerHelper {
  public:
    explicit AckingChanger(int tools) : ToolChangerHelper(tools) {
        set_mounts_to_feed(true);
    }

    AmsError execute_gcode(const std::string& gcode, std::function<void()> on_complete,
                           std::function<void(const MoonrakerError&)> on_error,
                           bool /*silent*/) override {
        sent_.push_back(gcode);
        acks_.push_back(std::move(on_complete));
        errors_.push_back(std::move(on_error));
        return AmsErrorHelper::success();
    }

    /// Ack the newest stage and let the deferred handlers run.
    void ack() {
        REQUIRE_FALSE(acks_.empty());
        auto cb = acks_.back();
        cb();
        helix::ui::UpdateQueue::instance().drain();
        helix::ui::UpdateQueue::instance().drain();
    }

    void fail() {
        REQUIRE_FALSE(errors_.empty());
        MoonrakerError err;
        err.message = "simulated";
        errors_.back()(err);
        helix::ui::UpdateQueue::instance().drain();
        helix::ui::UpdateQueue::instance().drain();
    }

    std::vector<std::string> sent_;

  private:
    std::vector<std::function<void()>> acks_;
    std::vector<std::function<void(const MoonrakerError&)>> errors_;
};

} // namespace

// ============================================================================
// The plan
// ============================================================================

TEST_CASE("mount-to-feed: a load mounts, heats, prompts, purges and restores",
          "[toolchanger][mount_feed]") {
    const auto stages = mf::plan(load_request());
    REQUIRE(stages.size() == 5);
    CHECK(stages[0].step == mf::Step::Mount);
    CHECK(stages[0].gcode == "SELECT_TOOL T=2");
    CHECK(stages[1].gcode == "M109 S215");
    CHECK(stages[2].step == mf::Step::Insert);
    CHECK(stages[2].awaits_operator);
    CHECK(stages[2].gcode.find("action:prompt_begin Load filament") != std::string::npos);
    CHECK(stages[2].gcode.find("MSG=HELIX_MOUNT_FEED_CONTINUE|primary") != std::string::npos);
    CHECK(stages[2].gcode.find("MSG=HELIX_MOUNT_FEED_CANCEL|error") != std::string::npos);
    CHECK(stages[3].step == mf::Step::Purge);
    CHECK(stages[3].gcode ==
          mf::prompt_end_gcode() +
              "\nM83\nG1 E5 F60\nG1 E50 F300\nG1 E50 F300\nG1 E-10 F1800\nM104 S0");
    CHECK(stages[4].gcode == "SELECT_TOOL T=0");
}

TEST_CASE("mount-to-feed: the tool already on the carriage is neither mounted nor restored",
          "[toolchanger][mount_feed]") {
    mf::Request r = load_request();
    r.mount_gcode.clear();
    r.restore_gcode.clear();
    const auto stages = mf::plan(r);
    REQUIRE(stages.size() == 3);
    CHECK(stages.front().step == mf::Step::Heat);
    CHECK(stages.back().step == mf::Step::Purge);
}

TEST_CASE("mount-to-feed: the printer's own macro replaces the built-in moves",
          "[toolchanger][mount_feed][macro]") {
    mf::Request load = load_request();
    load.macro = "LOAD_FILAMENT";
    CHECK(mf::plan(load)[3].gcode ==
          mf::prompt_end_gcode() + "\nLOAD_FILAMENT T=2 TEMP=215\nM104 S0");

    mf::Request unload = load_request();
    unload.load = false;
    // No feeder release: the retract alone takes the strand fully out.
    CHECK(gcodes(mf::plan(unload)) ==
          std::vector<std::string>{"SELECT_TOOL T=2", "M109 S215",
                                   "M83\nG1 E-25 F1800\nG1 E-150 F1200\nM104 S0",
                                   "SELECT_TOOL T=0"});
    unload.macro = "UNLOAD_FILAMENT";
    CHECK(mf::plan(unload)[2].gcode == "UNLOAD_FILAMENT T=2\nM104 S0");
}

TEST_CASE("mount-to-feed: a macro stands in only when it takes the tool as T=",
          "[toolchanger][mount_feed][macro]") {
    CachedMacroInfo with_t;
    with_t.knowledge = MacroParamKnowledge::KNOWN_PARAMS;
    with_t.params = {MacroParam{"T"}, MacroParam{"TEMP"}};
    CHECK(mf::macro_takes_tool(with_t));

    CachedMacroInfo without_t = with_t;
    without_t.params = {MacroParam{"TEMP"}};
    CHECK_FALSE(mf::macro_takes_tool(without_t));

    CachedMacroInfo unknown;
    CHECK_FALSE(mf::macro_takes_tool(unknown));
}

TEST_CASE("mount-to-feed: prompt text cannot break out of its RESPOND line",
          "[toolchanger][mount_feed]") {
    mf::Request r = load_request();
    r.prompt_text = "a \"quoted\" ; comment | pipe";
    const std::string g = mf::prompt_gcode(r);
    CHECK(g.find("a 'quoted' , comment / pipe") != std::string::npos);
}

TEST_CASE("mount-to-feed: only the button's own echo is a reply", "[toolchanger][mount_feed]") {
    CHECK(mf::parse_reply("// HELIX_MOUNT_FEED_CONTINUE") == mf::Reply::Continue);
    CHECK(mf::parse_reply("// HELIX_MOUNT_FEED_CANCEL ") == mf::Reply::Cancel);
    // The prompt's own button lines carry the marker text too.
    CHECK(mf::parse_reply("// action:prompt_footer_button Continue|RESPOND TYPE=command "
                          "MSG=HELIX_MOUNT_FEED_CONTINUE|primary") == mf::Reply::None);
    CHECK(mf::parse_reply("echo: something") == mf::Reply::None);
}

// ============================================================================
// The run
// ============================================================================

TEST_CASE("mount-to-feed: the run waits on the operator and cleans up on Cancel",
          "[toolchanger][mount_feed]") {
    mf::Run run(load_request());
    CHECK(run.current_gcode() == std::optional<std::string>("SELECT_TOOL T=2"));
    run.stage_done();
    CHECK(run.step_index() == 1);
    run.stage_done();
    REQUIRE(run.current_step() == mf::Step::Insert);
    run.stage_done(); // the prompt is up
    CHECK(run.awaiting_operator());
    CHECK_FALSE(run.current_gcode().has_value());
    run.stage_done(); // a stray ack moves nothing
    CHECK(run.awaiting_operator());

    run.operator_reply(mf::Reply::Cancel);
    CHECK(run.aborted());
    CHECK(run.current_gcode() ==
          std::optional<std::string>(mf::prompt_end_gcode() + "\nM104 S0\nSELECT_TOOL T=0"));
    run.stage_done();
    CHECK(run.finished());
}

TEST_CASE("mount-to-feed: a failure turns the rest into heater off and restore",
          "[toolchanger][mount_feed]") {
    mf::Run run(load_request());
    run.stage_done(); // mounted
    run.abort();      // the heat failed
    CHECK(run.current_gcode() == std::optional<std::string>("M104 S0\nSELECT_TOOL T=0"));
    run.abort(); // a failure inside the cleanup ends the run
    CHECK(run.finished());
}

// ============================================================================
// The backend
// ============================================================================

TEST_CASE("MedusaHC loads through the carriage and puts the old tool back",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.get_current_slot() == 0);
    CHECK(tc.head_filament_ops(2).load == FilamentOpSupport::Automatic);
    CHECK(tc.head_filament_ops(2).unload == FilamentOpSupport::Automatic);

    REQUIRE(tc.load_filament(2).success());
    REQUIRE(tc.sent_.size() == 1);
    CHECK(tc.sent_.back() == "SELECT_TOOL T=2");
    CHECK(tc.get_current_action() == AmsAction::SELECTING); // the Mount step
    CHECK(tc.get_system_info().operation_phase == 0);

    // A swap's own frames must not end the load or move its step.
    tc.feed_status("ready", 2);
    CHECK(tc.get_system_info().is_busy());
    CHECK(tc.get_system_info().operation_phase == 0);

    tc.ack();
    CHECK(tc.sent_.back() == "M109 S220"); // no material named: the default
    CHECK(tc.get_current_action() == AmsAction::HEATING);
    tc.ack();
    CHECK(tc.sent_.back().find("action:prompt_show") != std::string::npos);
    tc.ack();
    const size_t before_reply = tc.sent_.size();
    CHECK(tc.get_system_info().is_busy()); // waiting on the operator
    CHECK(tc.can_cancel_operation());

    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    REQUIRE(tc.sent_.size() == before_reply + 1);
    CHECK(tc.sent_.back().find("G1 E50 F300") != std::string::npos);
    CHECK(tc.get_current_action() == AmsAction::PURGING);
    tc.ack();
    CHECK(tc.sent_.back() == "SELECT_TOOL T=0");
    tc.ack();
    CHECK(tc.get_current_action() == AmsAction::IDLE);
    CHECK_FALSE(tc.can_cancel_operation());
}

TEST_CASE("MedusaHC unloads by retracting fully, sends no OPEN, and parks when nothing was "
          "mounted",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.set_feeder(toolchanger_addon::Feeder{true, "MedusaHC", "OPEN", "CLOSE"});
    tc.feed_status("ready", -1);

    REQUIRE(tc.unload_filament(1).success());
    CHECK(tc.sent_.back() == "SELECT_TOOL T=1");
    tc.ack();
    tc.ack();
    CHECK(tc.sent_.back() == "M83\nG1 E-25 F1800\nG1 E-150 F1200\nM104 S0");
    tc.ack();
    CHECK(tc.sent_.back() == "UNSELECT_TOOL");
    CHECK(std::find(tc.sent_.begin(), tc.sent_.end(), "OPEN") == tc.sent_.end());
    tc.ack();
    CHECK(tc.get_current_action() == AmsAction::IDLE);
}

TEST_CASE("MedusaHC runs the printer's LOAD_FILAMENT when it takes T=",
          "[toolchanger][mount_feed][medusahc][macro]") {
    auto& cache = MacroParamCache::instance();
    cache.populate_from_configfile(
        json{{"gcode_macro load_filament",
              {{"gcode", "{% set t = params.T|int %}{% set temp = params.TEMP|int %}M109"}}}},
        std::unordered_set<std::string>{"LOAD_FILAMENT"});
    struct ClearCache {
        ~ClearCache() {
            MacroParamCache::instance().clear();
        }
    } clear_cache;

    AckingChanger tc(4);
    tc.feed_status("ready", 2);
    REQUIRE(tc.load_filament(2).success());
    CHECK(tc.sent_.front() == "M109 S220"); // already mounted: no mount stage
    tc.ack();
    tc.ack();
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    CHECK(tc.sent_.back().find("LOAD_FILAMENT T=2 TEMP=220") != std::string::npos);
    CHECK(tc.sent_.back().find("G1 E50") == std::string::npos);
}

TEST_CASE("MedusaHC cancels a load waiting on the operator",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament(3).success());
    tc.ack();
    tc.ack();
    tc.ack(); // the prompt is up
    REQUIRE(tc.cancel().success());
    CHECK(tc.sent_.back() == mf::prompt_end_gcode() + "\nM104 S0\nSELECT_TOOL T=0");
    tc.ack();
    CHECK(tc.get_current_action() == AmsAction::IDLE);
}

TEST_CASE("MedusaHC refuses a second op while one runs, and cleans up a failed stage",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament(1).success());
    CHECK_FALSE(tc.load_filament(2).success());
    CHECK_FALSE(tc.unload_filament(1).success());

    tc.ack();  // mounted
    tc.fail(); // the heat failed
    CHECK(tc.sent_.back() == "M104 S0\nSELECT_TOOL T=0");
    tc.ack();
    CHECK(tc.get_current_action() == AmsAction::IDLE);
}

TEST_CASE("MedusaHC filament ops are refused while a print is running or paused",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    // The backend's print gate and every surface's button gating read this:
    // a paused print is no time to mount another tool.
    REQUIRE(tc.filament_ops_self_home());
    CHECK(helix::print_blocks_filament_op(PrintState::Printing, tc.filament_ops_self_home()));
    CHECK(helix::print_blocks_filament_op(PrintState::Paused, tc.filament_ops_self_home()));
    CHECK_FALSE(helix::print_blocks_filament_op(PrintState::Idle, tc.filament_ops_self_home()));
    // The backend heats the tool it mounts: the UI must not heat the active one.
    CHECK(tc.supports_auto_heat_on_load());
    CHECK(tc.supports_auto_heat_on_unload());
}

TEST_CASE("MedusaHC shows the mount-to-feed steps for a load and an unload",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    const auto load = tc.get_operation_step_model(StepOperationType::LOAD_FRESH);
    REQUIRE(load.steps.size() == 5);
    CHECK(load.steps[2].label == "Insert filament");
    const auto unload = tc.get_operation_step_model(StepOperationType::UNLOAD);
    REQUIRE(unload.steps.size() == 4);
    CHECK(unload.steps[2].label == "Retract filament");
    CHECK(unload.steps[3].label == "Restore tool");
}
