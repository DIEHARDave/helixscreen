// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_belt_tension_panel_states.cpp
 * @brief The belt tension panel's four view states, its run queue and its gate.
 *
 * Drives the real singleton panel through MoonrakerClientMock's TEST_RESONANCES
 * transcript and asserts on the bt_* subjects the XML binds:
 *
 * 1. Start queues both paths, runs them in order and lands on RESULTS with
 *    the similarity headline, verdict, paired-peak facts and chart markers
 *    derived from compare_belt_paths() over the printer's sweep range.
 * 2. Re-test keeps the sibling path's run and ghosts the re-measured path's
 *    previous curve.
 * 3. Every mock failure mode and a silent sweep reach ERROR; a path whose
 *    sweep never rises above the peak floor is an error naming the path.
 * 4. Deactivating mid-run cancels listening and returns to START.
 * 5. Start is disabled while klippy is not READY, and re-arms.
 * 6. EMBEDDED tier never creates a chart.
 * 7. Tune walks the guided tuning loop: the already-matched offer, the
 *    calibration turn's full sweeps, the narrow baseline pair and each
 *    round's narrow sweeps, with advice and outcome lines from
 *    BeltTuneSession, then Confirm or Good Enough.
 *
 * The XML side (containers, buttons, subject bindings) is pinned in the
 * container/binding cases below.
 */

#include "ui_belt_gap_gauge.h"
#include "ui_belt_path_sketch.h"
#include "ui_context_menu.h"
#include "ui_fonts.h"
#include "ui_frequency_response_chart.h"
#include "ui_modal.h"
#include "ui_panel_belt_tension.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "../test_helpers/mock_kinematics_env.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/scoped_env.h"
#include "app_globals.h"
#include "belt_tension_types.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "mock_persona.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_detector.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "tool_state.h"
#include "translation_loader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

/// Registers the bt_* subjects the panel binds, seeds the global PrinterState
/// the gate observes, and wires the panel to a mock client. One instance per
/// TEST_CASE; the destructor unwires in the reverse order.
class BeltPanelFixture : public XMLTestFixture {
  public:
    explicit BeltPanelFixture(helix::PlatformTier tier = helix::PlatformTier::STANDARD)
        : client_(MoonrakerClientMock::PrinterType::VORON_24), api_(client_, api_state_) {
        // A previous test's mock run in THIS process may have left the
        // per-PID belt CSVs behind; the terminal-line path must start absent.
        MoonrakerClientMock::remove_belt_csvs();
        client_.set_belt_line_interval_ms(1);

        api_state_.init_subjects(false);
        api_state_.set_klippy_state_sync(KlippyState::READY);

        // The panel observes the GLOBAL PrinterState, not this fixture's own;
        // seed it the way a live connection would.
        PrinterStateTestAccess::reset(get_printer_state());
        get_printer_state().init_subjects(false);
        // Seed the INPUTS a live connection produces, not the derived subject:
        // nav_buttons_enabled is recomputed from connection state + klippy on
        // every klippy transition, so poking it directly would silently close
        // the gate again the moment klippy state changed.
        get_printer_state().set_printer_connection_state(
            static_cast<int>(ConnectionState::CONNECTED), nullptr);
        get_printer_state().set_klippy_state_sync(KlippyState::READY);
        lv_subject_set_int(get_printer_state().print_state().get_print_active_subject(), 0);
        lv_subject_copy_string(get_printer_state().motion_state().get_homed_axes_subject(), "xyz");
        // The settling laps plan from the toolhead's limits and max velocity.
        set_toolhead_limits(500.0);
        set_accel_subject(1);

        panel_ = &get_global_belt_tension_panel();
        ui_panel_belt_tension_register_callbacks();
        panel_->deinit_subjects();
        panel_->init_subjects();
        panel_->set_api(&client_, &api_);
        panel_->set_render_tier_for_test(tier, true);
        // The panel is a process-wide singleton, so a memory reading one case
        // injected would otherwise make the next case's Start ask about low RAM.
        helix::MemoryInfo roomy;
        roomy.total_kb = 4 * 1024 * 1024;
        roomy.available_kb = 3 * 1024 * 1024;
        panel_->set_memory_for_test(roomy);

        helix::ui::register_belt_path_sketch_widget();
        helix::ui::register_belt_gap_gauge_widget();
        REQUIRE(register_component("header_bar"));
        REQUIRE(register_component("panel_belt_tension"));
        view_ = panel_->create(test_screen());
        REQUIRE(view_ != nullptr);
        UpdateQueue::instance().drain();

        panel_->on_activate();
        UpdateQueue::instance().drain();
    }

    ~BeltPanelFixture() override {
        panel_->on_deactivate(DeactivateReason::NavigateAway);
        UpdateQueue::instance().drain();
        // Tears the widget tree down through the panel (nulls its cached root,
        // destroys the chart while the tree is still whole) BEFORE this
        // fixture's base deletes the screen underneath it.
        panel_->destroy_overlay_ui(view_);
        UpdateQueue::instance().drain();
        panel_->set_api(nullptr, nullptr);
        panel_->deinit_subjects();
        MoonrakerClientMock::remove_belt_csvs();
    }

    BeltTensionPanel& panel() {
        return *panel_;
    }
    MoonrakerClientMock& mock() {
        return client_;
    }
    lv_obj_t* view() {
        return view_;
    }

    /// Friendship pass-throughs: the chart and ghost series are not exposed
    /// by any subject, so the ghost case reads them directly.
    ui_frequency_response_chart_t* panel_chart() {
        return panel_->chart_;
    }
    int panel_ghost_id(int path_index) {
        return panel_->ghost_series_[path_index];
    }
    int panel_series_id(int path_index) {
        return panel_->series_[path_index];
    }
    lv_obj_t* panel_chart_host_tuning() {
        return panel_->chart_host_tuning_;
    }
    void panel_set_view_state(BeltTensionPanel::ViewState state) {
        panel_->set_view_state(state);
    }
    int panel_tune_ghost_id(int path_index, int k) {
        return panel_->tune_ghost_series_[path_index][k];
    }
    lv_obj_t* panel_low_ram_dialog() {
        return panel_->low_ram_dialog_;
    }
    /// What the latest tuning round booked as turned; nullopt for none.
    std::optional<float> panel_last_turns() {
        const auto& t = panel_->tune_;
        if (!t || t->rounds().empty()) {
            return std::nullopt;
        }
        return t->rounds().back().turns_made;
    }
    /// The suggested amount in stepper steps; 0 without one.
    int panel_advised_steps() {
        const auto& t = panel_->tune_;
        if (!t || !t->advice().turns) {
            return 0;
        }
        return static_cast<int>(
            std::lround(*t->advice().turns / helix::calibration::belt_tune::TURN_STEP));
    }
    const auto& panel_run(int path_index) {
        return panel_->runs_[path_index];
    }
    /// The tallest in-band bin of a path's current curve.
    float panel_run_peak(int path_index) {
        float tallest = 0.0f;
        for (const auto& [f, v] : panel_->runs_[path_index].curve) {
            if (f >= panel_->sweep_cfg_.min_freq && f <= panel_->sweep_cfg_.max_freq) {
                tallest = std::max(tallest, v);
            }
        }
        return tallest;
    }
    void panel_set_similarity(float percent) {
        panel_->similarity_percent_ = percent;
    }
    lv_timer_t* panel_note_age_timer() {
        return panel_->note_age_timer_.get();
    }
    bool panel_detection_pending() {
        return panel_->detection_pending_;
    }
    size_t facts_buf_size() const {
        return sizeof(panel_->facts_buf_);
    }
    size_t instruction_buf_size() const {
        return sizeof(panel_->tune_instruction_buf_);
    }
    size_t confirm_label_buf_size() const {
        return sizeof(panel_->tune_confirm_label_buf_);
    }
    size_t next_zone_label_buf_size() const {
        return sizeof(panel_->tune_next_zone_label_buf_);
    }
    size_t last_buf_size() const {
        return sizeof(panel_->tune_last_buf_);
    }
    size_t hint_buf_size() const {
        return sizeof(panel_->tune_hint_buf_);
    }
    size_t power_hint_buf_size() const {
        return sizeof(panel_->tune_power_hint_buf_);
    }
    size_t measure_label_buf_size() const {
        return sizeof(panel_->tune_measure_label_buf_);
    }
    /// The [resonance_tester] range as the panel holds it.
    helix::calibration::ResonanceTesterConfig& panel_sweep_cfg() {
        return panel_->sweep_cfg_;
    }
    /// The live tuning session; null outside TUNING.
    const helix::calibration::BeltTuneSession* panel_session() {
        return panel_->tune_ ? &*panel_->tune_ : nullptr;
    }

    int state_int(const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        INFO("subject not registered: " << name);
        REQUIRE(s != nullptr);
        return lv_subject_get_int(s);
    }
    std::string text(const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        INFO("subject not registered: " << name);
        REQUIRE(s != nullptr);
        return lv_subject_get_string(s);
    }

    /// Advance virtual time in 100ms steps, pumping LVGL timers and the
    /// UpdateQueue so mock sweep lines, guard timers and marshalled callbacks
    /// all get their turn.
    void pump_ms(uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 100) {
            lv_tick_inc(100);
            lv_timer_handler_safe();
            UpdateQueue::instance().drain();
        }
    }
    bool pump_until_state(int want, uint32_t budget_ms = 60000) {
        for (uint32_t t = 0; t < budget_ms; t += 100) {
            if (state_int("belt_tension_state") == want) {
                return true;
            }
            lv_tick_inc(100);
            lv_timer_handler_safe();
            UpdateQueue::instance().drain();
        }
        return state_int("belt_tension_state") == want;
    }
    bool pump_until_int(const char* name, int want, uint32_t budget_ms = 60000) {
        for (uint32_t t = 0; t < budget_ms; t += 100) {
            if (state_int(name) == want) {
                return true;
            }
            lv_tick_inc(100);
            lv_timer_handler_safe();
            UpdateQueue::instance().drain();
        }
        return state_int(name) == want;
    }
    bool pump_until_text(const char* name, const std::string& want, uint32_t budget_ms = 60000) {
        for (uint32_t t = 0; t < budget_ms; t += 100) {
            if (text(name) == want) {
                return true;
            }
            lv_tick_inc(100);
            lv_timer_handler_safe();
            UpdateQueue::instance().drain();
        }
        return text(name) == want;
    }
    /// Pumps until the gate opens (hardware detect + config probe answered).
    bool wait_gate_open(uint32_t budget_ms = 10000) {
        for (uint32_t t = 0; t < budget_ms; t += 100) {
            if (state_int("bt_can_start") == 1) {
                return true;
            }
            lv_tick_inc(100);
            lv_timer_handler_safe();
            UpdateQueue::instance().drain();
        }
        return state_int("bt_can_start") == 1;
    }

    /// The persona's travel and a max velocity, as a toolhead status frame.
    void set_toolhead_limits(double max_velocity) {
        const auto axis_max = helix::mock::descriptor(client_.get_printer_type()).axis_max;
        get_printer_state().update_from_status(
            {{"toolhead",
              {{"axis_minimum", {0.0, 0.0, 0.0, 0.0}},
               {"axis_maximum", {axis_max.x, axis_max.y, axis_max.z, 0.0}},
               {"max_velocity", max_velocity}}}});
        UpdateQueue::instance().drain();
    }

    void set_klippy_ready(bool ready) {
        get_printer_state().set_klippy_state_sync(ready ? KlippyState::READY
                                                        : KlippyState::SHUTDOWN);
        pump_ms(2000);
    }

  private:
    /// printer_has_accelerometer lives on PrinterCapabilitiesState, which unit
    /// tests never register; provide the name the panel looks up.
    static void set_accel_subject(int value) {
        static lv_subject_t s;
        lv_subject_init_int(&s, value);
        lv_xml_register_subject(nullptr, "printer_has_accelerometer", &s);
    }

    MoonrakerClientMock client_;
    PrinterState api_state_;
    MoonrakerAPIMock api_;
    BeltTensionPanel* panel_ = nullptr;
    lv_obj_t* view_ = nullptr;
};

// ============================================================================
// XML shape
// ============================================================================

TEST_CASE("belt tension panel has a container for every view state", "[belt][panel][xml]") {
    BeltPanelFixture fx;

    // One per ViewState value, in enum order.
    for (const char* name : {"state_start", "state_running", "state_results", "state_error"}) {
        INFO("missing state container: " << name);
        CHECK(lv_obj_find_by_name(fx.view(), name) != nullptr);
    }
    // Every named object the state machine and ctl driving depend on.
    for (const char* name :
         {"btn_start", "btn_stop", "btn_retest_a", "btn_retest_b", "btn_test_both", "btn_retry",
          "bt_similarity", "bt_facts_label", "bt_unpaired_label", "chart_host_running",
          "chart_host_results", "bt_sketch", "error_label"}) {
        INFO("missing named object: " << name);
        CHECK(lv_obj_find_by_name(fx.view(), name) != nullptr);
    }
    // The pluck-tuner states these replaced must be gone, not left alongside.
    CHECK(lv_obj_find_by_name(fx.view(), "state_position") == nullptr);
    CHECK(lv_obj_find_by_name(fx.view(), "state_listen") == nullptr);
    CHECK(lv_obj_find_by_name(fx.view(), "state_compare") == nullptr);
    // The direction rail is gone until real captures show a dependable
    // signed signal.
    CHECK(lv_obj_find_by_name(fx.view(), "bt_rail") == nullptr);
    // The per-path hero numbers are gone: the strongest pair's frequencies say
    // the least about the belts, so similarity is the only headline number.
    CHECK(lv_obj_find_by_name(fx.view(), "bt_peak_a") == nullptr);
    CHECK(lv_obj_find_by_name(fx.view(), "bt_peak_b") == nullptr);
}

TEST_CASE("belt tension panel binds only subjects that exist", "[belt][panel][xml]") {
    BeltPanelFixture fx;

    for (const char* name :
         {"belt_tension_state", "bt_can_start", "bt_gate_message", "bt_hw_kinematics",
          "bt_hw_accel", "bt_hw_sweep", "bt_run_title", "bt_run_detail", "bt_running_path",
          "bt_note_a", "bt_note_b", "bt_verdict", "bt_verdict_text", "bt_similarity", "bt_facts",
          "bt_unpaired", "bt_has_unpaired", "bt_chart_available", "bt_error_message"}) {
        INFO("subject not registered: " << name);
        CHECK(lv_xml_get_subject(nullptr, name) != nullptr);
    }
    // Retired with the pluck tuner. Leaving them registered would let a stale
    // binding survive review by continuing to resolve.
    for (const char* name : {"bt_hw_adxl", "bt_live_freq", "bt_median_freq", "bt_committed",
                             "bt_pluck_count", "bt_rail_value"}) {
        INFO("retired subject still registered: " << name);
        CHECK(lv_xml_get_subject(nullptr, name) == nullptr);
    }
}

// ============================================================================
// Run flow
// ============================================================================

TEST_CASE("Start settles, then runs A then B and lands on RESULTS", "[belt][panel]") {
    BeltPanelFixture fx;
    fx.mock().set_belt_peaks_hz(104.0f, 98.0f);
    REQUIRE(fx.wait_gate_open());
    fx.mock().clear_gcode_script_history();

    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RESULTS)));

    // The mock records a script line by line: the laps, then A, then B.
    const auto history = std::vector<std::string>(fx.mock().gcode_script_history());
    REQUIRE_FALSE(history.empty());
    CHECK(history.front() == "SAVE_GCODE_STATE NAME=helix_belt_settle");
    std::vector<std::string> order;
    for (const auto& g : history) {
        if (g == "SAVE_GCODE_STATE NAME=helix_belt_settle") {
            order.push_back("settle");
        } else if (g.rfind("TEST_RESONANCES AXIS=1,-1 ", 0) == 0) {
            order.push_back("A");
        } else if (g.rfind("TEST_RESONANCES AXIS=1,1 ", 0) == 0) {
            order.push_back("B");
        }
    }
    CHECK(order == std::vector<std::string>{"settle", "A", "B"});

    // With no previous run the note names the age and nothing else.
    CHECK(fx.text("bt_note_a") == "just now");
    CHECK(fx.text("bt_note_b") == "just now");
    CHECK(fx.state_int("bt_verdict") == static_cast<int>(helix::calibration::BeltVerdict::CLOSE));
    CHECK(fx.text("bt_verdict_text") == "Fair match");

    // The headline is the similarity; the facts line lists the pairs, A/B.
    const std::string similarity = fx.text("bt_similarity");
    CHECK(similarity.back() == '%');
    CHECK(fx.text("bt_facts").find("104/98") != std::string::npos);

    // The listed pairs are numbered on both curves, the strongest as 1.
    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    const auto marks_a = ui_frequency_response_chart_get_markers(chart, fx.panel_series_id(0));
    const auto marks_b = ui_frequency_response_chart_get_markers(chart, fx.panel_series_id(1));
    REQUIRE_FALSE(marks_a.empty());
    REQUIRE_FALSE(marks_b.empty());
    CHECK(marks_a.front().number == 1);
    CHECK(marks_a.front().freq_hz == Catch::Approx(104.0f).margin(1.0f));
    CHECK(marks_b.front().number == 1);
    CHECK(marks_b.front().freq_hz == Catch::Approx(98.0f).margin(1.0f));
}

TEST_CASE("a full sweep keeps both curves", "[belt][panel]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());

    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RESULTS)));

    for (int i = 0; i < 2; ++i) {
        const auto& run = fx.panel_run(i);
        REQUIRE_FALSE(run.curve.empty());
        REQUIRE(run.in_plane.size() == run.curve.size());
        for (size_t k = 0; k < run.curve.size(); ++k) {
            CHECK(run.in_plane[k].first == run.curve[k].first);
        }
    }
}

TEST_CASE("Re-test A keeps B and ghosts the old A", "[belt][panel][chart]") {
    BeltPanelFixture fx; // mock defaults: A=110, B=98
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RESULTS)));
    // 110 and 98 sit 12 Hz apart, past the 10 Hz pairing cap, so the only pair
    // the defaults form is the ~42 Hz rig peak both curves share, and each
    // belt peak is listed as unpaired.
    const std::string similarity_before = fx.text("bt_similarity");
    REQUIRE(similarity_before.back() == '%');
    CHECK(fx.state_int("bt_has_unpaired") == 1);
    CHECK(fx.text("bt_unpaired").find("Only on A: 110") != std::string::npos);
    CHECK(fx.text("bt_unpaired").find("Only on B: 98") != std::string::npos);

    fx.panel().handle_retest_clicked(helix::calibration::BeltPath::PATH_A);
    REQUIRE(fx.state_int("belt_tension_state") ==
            static_cast<int>(BeltTensionPanel::ViewState::RUNNING));
    CHECK(fx.text("bt_run_title") == "Settling belts");
    CHECK(fx.pump_until_text("bt_run_title", "Re-measuring Path A"));
    // A re-measure walks A toward B by at most 4 Hz: 110 -> 106, now 8 Hz from
    // B, inside the cap: the belt-hump pair outranks the 42 Hz one.
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RESULTS)));
    // "was" under the similarity repeats the one shown before the re-measure;
    // the notes carry no history.
    CHECK(fx.text("bt_similarity_was") == "was " + similarity_before);
    CHECK(fx.state_int("bt_has_similarity_was") == 1);
    CHECK(fx.text("bt_note_a").find("was") == std::string::npos);
    CHECK(fx.text("bt_note_b").find("was") == std::string::npos);
    CHECK(fx.state_int("bt_has_unpaired") == 0);

    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    CHECK(ui_frequency_response_chart_is_series_visible(chart, fx.panel_ghost_id(0)));
    CHECK_FALSE(ui_frequency_response_chart_is_series_visible(chart, fx.panel_ghost_id(1)));
}

TEST_CASE("each mock failure reaches ERROR", "[belt][panel]") {
    BeltPanelFixture fx;
    for (const BeltMockFailure failure :
         {BeltMockFailure::ERROR, BeltMockFailure::NO_FILE, BeltMockFailure::MULTICHIP}) {
        fx.mock().set_belt_failure(failure);
        REQUIRE(fx.wait_gate_open());
        fx.panel().handle_start_clicked();
        REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::ERROR)));
        CHECK(fx.text("bt_error_message").empty() == false);

        fx.panel().handle_retry_clicked();
        CHECK(fx.state_int("belt_tension_state") ==
              static_cast<int>(BeltTensionPanel::ViewState::START));
    }
}

TEST_CASE("a stall trips the stall guard", "[belt][panel]") {
    BeltPanelFixture fx;
    fx.mock().set_belt_failure(BeltMockFailure::STALL);
    // 1ms lines: the sweep's quiet half arrives in the first moments of the
    // pump, so the whole budget is the silent STALL_TIMEOUT the guard waits.
    fx.mock().set_belt_line_interval_ms(1);
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RUNNING)));

    // The sweep dies at its midpoint and goes quiet. The guard re-arms on
    // every progress line, so its window starts at the LAST line, not at
    // Start: drain the live half first (one line per pump step, ~65 of them
    // over the mock's 5-135 Hz range), then wait out a full silent
    // STALL_TIMEOUT from that point.
    fx.pump_ms(15000);
    fx.pump_ms(BeltTensionPanel::STALL_TIMEOUT_MS + 2000);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::ERROR));
    CHECK(fx.text("bt_error_message").find("stopped reporting progress") != std::string::npos);
    CHECK(fx.text("bt_error_message").find("Power-cycle the printer") != std::string::npos);
    CHECK(fx.text("bt_error_message").find("SSH") != std::string::npos);
}

TEST_CASE("closing mid-run stops listening", "[belt][panel]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RUNNING)));

    fx.panel().on_deactivate(DeactivateReason::NavigateAway);
    UpdateQueue::instance().drain();
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::START));

    // The mock sweep timer keeps firing, but the cancelled run may not act on
    // it: the panel has to stay on START.
    fx.pump_ms(5000);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::START));
}

TEST_CASE("Start stays disabled while klippy is not ready", "[belt][panel][gating]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());

    fx.set_klippy_ready(false);
    CHECK(fx.state_int("bt_can_start") == 0);

    fx.set_klippy_ready(true);
    INFO("gate message after re-ready: " << fx.text("bt_gate_message"));
    CHECK(fx.state_int("bt_can_start") == 1);
}

TEST_CASE("a CoreXZ reports a closed gate naming its kinematics", "[belt][panel][gating]") {
    // The persona must be in place before the fixture's on_activate fires the
    // hardware detect, and the mock reads the env when it is constructed.
    helix_test::MockKinematicsEnv corexz("corexz");
    BeltPanelFixture fx;

    // Wait for detection to land: bt_hw_kinematics leaves "Detecting..." and
    // shows the raw name (an UNKNOWN kinematics has no friendly label).
    bool detected = false;
    for (uint32_t t = 0; t < 10000 && !detected; t += 100) {
        detected = fx.text("bt_hw_kinematics") == "corexz";
        if (!detected) {
            fx.pump_ms(100);
        }
    }
    REQUIRE(detected);

    CHECK(fx.state_int("bt_can_start") == 0);
    const std::string message = fx.text("bt_gate_message");
    INFO("gate message: " << message);
    CHECK(message.find("CoreXY") != std::string::npos);
    CHECK(message.find("corexz") != std::string::npos);
}

TEST_CASE("a sweep with too few frequency bins is an error", "[belt][panel]") {
    BeltPanelFixture fx;
    // A 5-6 Hz sweep writes only two bins; the comparison needs at least three
    // in-band bins per curve to say anything.
    fx.mock().set_resonance_sweep_range(5.0, 6.0);
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::ERROR)));
    CHECK(fx.text("bt_error_message").find("Not enough frequency data") != std::string::npos);
}

TEST_CASE("EMBEDDED tier creates no chart", "[belt][panel][chart]") {
    BeltPanelFixture fx(helix::PlatformTier::EMBEDDED);
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RESULTS)));
    CHECK(fx.state_int("bt_chart_available") == 0);
    CHECK(fx.panel_chart() == nullptr);
}

TEST_CASE("a rebuild drops the chart with the tree it lives in", "[belt][panel][chart]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RESULTS)));
    REQUIRE(fx.panel_chart() != nullptr);

    // What a hot-reload does: free the whole widget tree and create it again.
    // The chart's object is a child of a host inside that tree, so the panel
    // must not come out of rebuild still holding a pointer to it.
    REQUIRE(fx.panel().rebuild());
    UpdateQueue::instance().drain();
    CHECK(fx.panel_chart() == nullptr);
    CHECK(fx.state_int("bt_chart_available") == 0);

    // The rebuilt tree still carries both hosts, and a fresh run rebuilds the
    // chart into the re-cached one.
    CHECK(lv_obj_find_by_name(fx.panel().get_root(), "chart_host_results") != nullptr);
}

TEST_CASE("both curves share one percent scale set by the taller", "[belt][panel][chart]") {
    // Path A replays a capture whose peak is half the height of B's synthetic
    // one, so a shared scale puts A near 50% and B at 100%.
    const std::string csv_a = "/tmp/helix-belt-half-height-" + std::to_string(getpid()) + ".csv";
    {
        std::ofstream out(csv_a);
        out << "freq,psd_x,psd_y,psd_z,psd_xyz\n";
        for (double freq = 5.0; freq <= 135.0; freq += 3200.0 / 4096.0) {
            const double df = freq - 104.0;
            const double psd = 150.0 + 1.5e4 / (1.0 + df * df / 100.0);
            out << freq << ',' << psd * 0.45 << ',' << psd * 0.45 << ',' << psd * 0.1 << ',' << psd
                << '\n';
        }
    }
    helix::ScopedEnv replay("HELIX_MOCK_BELT_CSV_A", csv_a.c_str());

    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RESULTS)));
    std::remove(csv_a.c_str());

    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    const auto peak = [chart](int id) {
        const auto amps = ui_frequency_response_chart_get_series_amplitudes(chart, id);
        REQUIRE_FALSE(amps.empty());
        return *std::max_element(amps.begin(), amps.end());
    };
    CHECK(peak(fx.panel_series_id(1)) == Catch::Approx(100.0f).margin(0.5f));
    CHECK(peak(fx.panel_series_id(0)) == Catch::Approx(50.0f).margin(5.0f));
}

/// Renders @p obj and returns the RGB of every label it drew, the chart's
/// axis labels among them.
static std::vector<uint32_t> drawn_label_colors(lv_obj_t* obj) {
    std::vector<uint32_t> colors;
    const auto capture = [](lv_event_t* e) {
        auto* out = static_cast<std::vector<uint32_t>*>(lv_event_get_user_data(e));
        lv_draw_task_t* task = lv_event_get_draw_task(e);
        if (const auto* dsc = lv_draw_task_get_label_dsc(task)) {
            out->push_back(lv_color_to_u32(dsc->color) & 0xFFFFFF);
        }
    };
    lv_obj_add_flag(obj, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(obj, capture, LV_EVENT_DRAW_TASK_ADDED, &colors);
    // A snapshot renders the object whether or not the overlay is on screen.
    if (lv_draw_buf_t* snap = lv_snapshot_take(obj, LV_COLOR_FORMAT_ARGB8888)) {
        lv_draw_buf_destroy(snap);
    }
    lv_obj_remove_event_cb_with_user_data(obj, capture, &colors);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    return colors;
}

TEST_CASE("a theme toggle recolors the open chart's curves", "[belt][panel][chart]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RESULTS)));
    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    const auto rgb = [chart](int id) {
        return lv_color_to_u32(ui_frequency_response_chart_get_series_color(chart, id)) & 0xFFFFFF;
    };
    const uint32_t before = rgb(fx.panel_series_id(0));
    // The axis labels are drawn by the chart's own draw pass, not styled.
    lv_obj_t* plot = lv_obj_get_child(ui_frequency_response_chart_get_obj(chart), 0);
    REQUIRE(plot != nullptr);
    const uint32_t muted_before = lv_color_to_u32(theme_manager_get_color("text_muted")) & 0xFFFFFF;
    const auto labels_before = drawn_label_colors(plot);

    theme_manager_toggle_dark_mode();
    UpdateQueue::instance().drain();
    const uint32_t muted_after = lv_color_to_u32(theme_manager_get_color("text_muted")) & 0xFFFFFF;
    const auto labels_after = drawn_label_colors(plot);
    const uint32_t want_a = lv_color_to_u32(theme_manager_get_color("belt_path_a")) & 0xFFFFFF;
    const uint32_t want_b = lv_color_to_u32(theme_manager_get_color("belt_path_b")) & 0xFFFFFF;
    const uint32_t a = rgb(fx.panel_series_id(0));
    const uint32_t ghost_a = rgb(fx.panel_ghost_id(0));
    const uint32_t b = rgb(fx.panel_series_id(1));
    theme_manager_toggle_dark_mode();
    UpdateQueue::instance().drain();

    REQUIRE(want_a != before);
    CHECK(a == want_a);
    CHECK(ghost_a == want_a);
    CHECK(b == want_b);

    // Axis labels repaint in the new mode's text_muted.
    REQUIRE(muted_after != muted_before);
    const auto count = [](const std::vector<uint32_t>& v, uint32_t c) {
        return std::count(v.begin(), v.end(), c);
    };
    CHECK(count(labels_before, muted_before) > 0);
    CHECK(count(labels_after, muted_after) > 0);
    CHECK(count(labels_after, muted_before) == 0);
}

TEST_CASE("sweep fact line comes from the printer's resonance_tester config", "[belt][panel]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    // Mock defaults 5-135 Hz at 1 Hz/s: two sweeps take ceil(2*130/60) = 5 min.
    CHECK(fx.text("bt_hw_sweep") == "5-135 Hz · about 5 min");
}

TEST_CASE("low RAM asks before the sweep starts", "[belt][panel]") {
    BeltPanelFixture fx;
    helix::ui::modal_init_subjects();
    REQUIRE(fx.register_component("modal_dialog"));
    REQUIRE(fx.wait_gate_open());
    helix::MemoryInfo small;
    small.total_kb = 128 * 1024;
    small.available_kb = 80 * 1024;
    fx.panel().set_memory_for_test(small);

    fx.panel().handle_start_clicked();
    fx.pump_ms(500);
    // Klipper analyses the sweep on this host: on a small board the warning
    // comes first and nothing moves until it is answered.
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    CHECK(fx.panel_low_ram_dialog() == dialog);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::START));

    // Continuing starts the sweep and clears the handle.
    lv_obj_t* go = lv_obj_find_by_name(dialog, "btn_primary");
    REQUIRE(go != nullptr);
    lv_obj_send_event(go, LV_EVENT_CLICKED, nullptr);
    fx.pump_ms(500);
    CHECK(fx.panel_low_ram_dialog() == nullptr);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::RUNNING));
}

TEST_CASE("enough RAM starts the sweep without asking", "[belt][panel]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    helix::MemoryInfo big;
    big.total_kb = 1024 * 1024;
    big.available_kb = 600 * 1024;
    fx.panel().set_memory_for_test(big);

    fx.panel().handle_start_clicked();
    CHECK(fx.panel_low_ram_dialog() == nullptr);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::RUNNING));
}

TEST_CASE("too little headroom refuses the sweep", "[belt][panel]") {
    BeltPanelFixture fx;
    helix::ui::modal_init_subjects();
    REQUIRE(fx.register_component("modal_dialog"));
    REQUIRE(fx.wait_gate_open());
    // The CC1 as measured: Klipper's analysis is OOM-killed and klippy wedges.
    helix::MemoryInfo cc1;
    cc1.total_kb = 114 * 1024;
    cc1.available_kb = 25 * 1024;
    fx.panel().set_memory_for_test(cc1);

    fx.panel().handle_start_clicked();
    fx.pump_ms(500);
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    // Not the continue-anyway warning: there is no way to start from here.
    CHECK(fx.panel_low_ram_dialog() == nullptr);

    lv_obj_t* ok = lv_obj_find_by_name(dialog, "btn_primary");
    REQUIRE(ok != nullptr);
    lv_obj_send_event(ok, LV_EVENT_CLICKED, nullptr);
    fx.pump_ms(500);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::START));
}

// ============================================================================
// Tuning loop
// ============================================================================

namespace {

constexpr int TUNING = static_cast<int>(BeltTensionPanel::ViewState::TUNING);
constexpr int RESULTS = static_cast<int>(BeltTensionPanel::ViewState::RESULTS);
constexpr int PHASE_ADJUST = 0;
constexpr int PHASE_SWEEPING = 1;
constexpr int PHASE_MATCHED = 2;
constexpr int PHASE_CALIBRATE = 3;
constexpr int PHASE_ALREADY_MATCHED = 4;

/// Start, wait for RESULTS, then Tune.
void enter_tuning(BeltPanelFixture& fx) {
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(RESULTS));
    fx.panel().handle_tune_clicked();
    REQUIRE(fx.state_int("belt_tension_state") == TUNING);
}

/// One Measure or Re-measure: every sweep it starts, a baseline chained onto
/// a registered calibration included, then back out of SWEEPING.
void tune_round(BeltPanelFixture& fx) {
    fx.panel().handle_tune_measure_clicked();
    REQUIRE(fx.state_int("belt_tension_state") == TUNING);
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_SWEEPING);
    for (uint32_t t = 0; t < 120000 && fx.state_int("bt_tune_phase") == PHASE_SWEEPING; t += 100) {
        fx.pump_ms(100);
    }
    REQUIRE(fx.state_int("bt_tune_phase") != PHASE_SWEEPING);
}

/// From CALIBRATE, measure until a turn registers and the zone's baseline is in.
void calibrate(BeltPanelFixture& fx) {
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_CALIBRATE);
    for (int i = 0; i < 2 && fx.state_int("bt_tune_phase") == PHASE_CALIBRATE; ++i) {
        tune_round(fx);
    }
    INFO("instruction: " << fx.text("bt_tune_instruction") << " last: " << fx.text("bt_tune_last"));
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_ADJUST);
}

/// Start, Tune, then calibrate: the loop is advising on a zone.
void enter_adjust(BeltPanelFixture& fx) {
    enter_tuning(fx);
    calibrate(fx);
}

/// Step the stepper to the suggested amount, as a user who turned it would.
void step_as_advised(BeltPanelFixture& fx) {
    for (int i = fx.panel_advised_steps(); i > 0; --i) {
        fx.panel().handle_tune_amount_step(+1);
    }
}

std::vector<std::string> resonance_lines(BeltPanelFixture& fx) {
    std::vector<std::string> out;
    for (const auto& g : std::vector<std::string>(fx.mock().gcode_script_history())) {
        if (g.find("TEST_RESONANCES") != std::string::npos) {
            out.push_back(g);
        }
    }
    return out;
}

bool lv_hidden(lv_obj_t* obj) {
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* named(BeltPanelFixture& fx, const char* name) {
    lv_obj_t* obj = lv_obj_find_by_name(fx.view(), name);
    INFO("missing: " << name);
    REQUIRE(obj != nullptr);
    return obj;
}

/// Parses "Zone 93-115 Hz".
std::pair<int, int> zone_range(const std::string& zone) {
    int lo = 0, hi = 0;
    if (std::sscanf(zone.c_str(), "Zone %d-%d Hz", &lo, &hi) != 2) {
        return {-1, -1};
    }
    return {lo, hi};
}

/// The FREQ_START/FREQ_END a resonance line sent; {-1, -1} for a full sweep.
std::pair<int, int> freq_window(const std::string& line) {
    const auto start = line.find("FREQ_START=");
    const auto end = line.find("FREQ_END=");
    if (start == std::string::npos || end == std::string::npos) {
        return {-1, -1};
    }
    return {std::atoi(line.c_str() + start + 11), std::atoi(line.c_str() + end + 9)};
}

constexpr const char* SETTLE_START = "SAVE_GCODE_STATE NAME=helix_belt_settle";

/// What the run sent, in order: "settle" per lap script, "full" or "narrow"
/// per resonance test.
std::vector<std::string> run_steps(BeltPanelFixture& fx) {
    std::vector<std::string> out;
    for (const auto& g : std::vector<std::string>(fx.mock().gcode_script_history())) {
        if (g == SETTLE_START) {
            out.push_back("settle");
        } else if (g.find("TEST_RESONANCES") != std::string::npos) {
            out.push_back(g.find("FREQ_START=") == std::string::npos ? "full" : "narrow");
        }
    }
    return out;
}

/// A replay capture with equal-height Gaussian peaks (2 Hz sigma) at
/// `peaks_hz`, over the mock's 5-135 Hz band. No peaks writes the floor alone.
void write_two_peak_csv(const std::string& path, std::initializer_list<float> peaks_hz) {
    std::ofstream out(path);
    out << "freq,psd_x,psd_y,psd_z,psd_xyz\n";
    for (double freq = 5.0; freq <= 135.0; freq += 3200.0 / 4096.0) {
        double psd = 150.0;
        for (const float p : peaks_hz) {
            const double df = freq - p;
            psd += 1.5e4 * std::exp(-df * df / 8.0);
        }
        out << freq << ',' << psd * 0.45 << ',' << psd * 0.45 << ',' << psd * 0.1 << ',' << psd
            << '\n';
    }
}

/// Minutes `pairs` sweeps of both paths over `swept` take at the mock's 1 Hz/s,
/// the fixture's 500 mm/s laps included.
int sweep_minutes(helix::calibration::BeltSweepWindow swept, int pairs) {
    const auto axis_max =
        helix::mock::descriptor(MoonrakerClientMock::PrinterType::VORON_24).axis_max;
    helix::calibration::BeltSettleInputs in;
    in.x_max = static_cast<float>(axis_max.x);
    in.y_max = static_cast<float>(axis_max.y);
    in.has_xy_limits = true;
    in.probe_point = std::array<float, 3>{in.x_max / 2.0f, in.y_max / 2.0f, 20.0f};
    in.max_velocity_mm_s = 500.0f;
    const float laps = helix::calibration::plan_belt_settle(in).seconds;
    const float seconds =
        2.0f * static_cast<float>(pairs) * (swept.freq_end_hz - swept.freq_start_hz) + laps;
    return std::max(1, static_cast<int>(std::lround(seconds / 60.0f)));
}

/// A capture with one broad peak (10 Hz half width) at `centre_hz`, `height`
/// tall over a floor a two-hundredth of it.
void write_peak_csv_at(const std::string& path, double centre_hz, double height) {
    std::ofstream out(path);
    out << "freq,psd_x,psd_y,psd_z,psd_xyz\n";
    for (double freq = 5.0; freq <= 135.0; freq += 3200.0 / 4096.0) {
        const double df = freq - centre_hz;
        const double psd = height / 200.0 + height / (1.0 + df * df / 100.0);
        out << freq << ',' << psd * 0.45 << ',' << psd * 0.45 << ',' << psd * 0.1 << ',' << psd
            << '\n';
    }
}

/// Both paths replay narrow-peaked captures, rewritten between sweeps. Their
/// zone centroids sit on the peaks, so a script reads as written; the mock's
/// own broad curves pull every centroid toward the zone's middle.
class ReplayBelts {
  public:
    ReplayBelts(std::initializer_list<float> a, std::initializer_list<float> b)
        : a_("/tmp/helix-belt-replay-a-" + std::to_string(getpid()) + ".csv"),
          b_("/tmp/helix-belt-replay-b-" + std::to_string(getpid()) + ".csv") {
        set_a(a);
        set_b(b);
        env_a_.emplace("HELIX_MOCK_BELT_CSV_A", a_.c_str());
        env_b_.emplace("HELIX_MOCK_BELT_CSV_B", b_.c_str());
    }
    ~ReplayBelts() {
        env_a_.reset();
        env_b_.reset();
        std::remove(a_.c_str());
        std::remove(b_.c_str());
    }
    void set_a(std::initializer_list<float> peaks) {
        write_two_peak_csv(a_, peaks);
    }
    void set_b(std::initializer_list<float> peaks) {
        write_two_peak_csv(b_, peaks);
    }

  private:
    std::string a_;
    std::string b_;
    std::optional<helix::ScopedEnv> env_a_;
    std::optional<helix::ScopedEnv> env_b_;
};

/// With A at 110 and B at 98: B's calibration sweep reads 106 (registers),
/// and the baseline pair repeats it, a -4 Hz gap.
void replay_to_adjust(BeltPanelFixture& fx, ReplayBelts& belts) {
    enter_tuning(fx);
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_CALIBRATE);
    belts.set_b({106.0f});
    calibrate(fx);
    REQUIRE(fx.panel_session() != nullptr);
    INFO("gap " << fx.panel_session()->gap_hz());
    REQUIRE(fx.panel_session()->gap_hz() == Catch::Approx(-4.0f).margin(0.5f));
}

} // namespace

TEST_CASE("the Tune button names the job by verdict", "[belt][panel][tune]") {
    SECTION("Fair match: Tune Belts") {
        BeltPanelFixture fx;
        fx.mock().set_belt_peaks_hz(104.0f, 98.0f);
        REQUIRE(fx.wait_gate_open());
        fx.panel().handle_start_clicked();
        REQUIRE(fx.pump_until_state(RESULTS));
        REQUIRE(fx.state_int("bt_verdict") !=
                static_cast<int>(helix::calibration::BeltVerdict::MATCHED));
        CHECK(fx.text("bt_tune_cta") == "Tune Belts");
    }
    SECTION("Good match: Fine-tune") {
        BeltPanelFixture fx;
        fx.mock().set_belt_peaks_hz(104.0f, 104.0f);
        REQUIRE(fx.wait_gate_open());
        fx.panel().handle_start_clicked();
        REQUIRE(fx.pump_until_state(RESULTS));
        REQUIRE(fx.state_int("bt_verdict") ==
                static_cast<int>(helix::calibration::BeltVerdict::MATCHED));
        CHECK(fx.text("bt_tune_cta") == "Fine-tune");
    }
}

TEST_CASE("Tune opens on the calibration turn", "[belt][panel][tune]") {
    BeltPanelFixture fx; // mock defaults: A=110, B=98, each tallest peak unpaired
    get_printer_state().set_printer_type_sync("Voron 2.4");
    enter_tuning(fx);

    CHECK(fx.state_int("bt_tune_phase") == PHASE_CALIBRATE);
    CHECK(fx.text("bt_tune_instruction") == "Tighten B one full turn");
    CHECK(fx.state_int("bt_tune_path") == 1);
    CHECK(fx.state_int("bt_tune_dir") == 0);
    CHECK(lv_hidden(named(fx, "bt_tune_stepper")));
    CHECK(lv_hidden(named(fx, "bt_tune_row_stepper")));
    CHECK(fx.text("bt_tune_amount").empty());
    // No zone until the turn says which one moved.
    CHECK(fx.text("bt_tune_zone").empty());
    CHECK(fx.state_int("bt_tune_shape_warning") == 1);
    CHECK(fx.text("bt_tune_round") == "Round 1");
    CHECK(fx.text("bt_tune_gap_caption") == "B looser than A");
    CHECK(fx.text("bt_tune_swap_label") == "Adjust A instead");
    CHECK(fx.text("bt_tune_last").empty());
    // The calibration measures both paths in full, as the sweep fact line does.
    const std::string hw = fx.text("bt_hw_sweep");
    const auto about = hw.find("about ");
    REQUIRE(about != std::string::npos);
    CHECK(fx.text("bt_tune_measure_label") == "Measure (~" + hw.substr(about + 6) + ")");
    CHECK(fx.text("bt_tune_measure_short_label") == "Measure");
    CHECK_FALSE(lv_hidden(named(fx, "btn_tune_done")));
    CHECK(lv_hidden(named(fx, "btn_tune_calibrate_anyway")));

    // Until a zone is known the chart spans the band.
    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    const auto [f_lo, f_hi] = ui_frequency_response_chart_get_freq_range(chart);
    CHECK(f_lo < 10.0f);
    CHECK(f_hi > 130.0f);
}

TEST_CASE("matched belts open on Good Enough", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    fx.mock().set_belt_peaks_hz(100.0f, 100.0f);
    enter_tuning(fx);

    CHECK(fx.state_int("bt_tune_phase") == PHASE_ALREADY_MATCHED);
    CHECK(fx.text("bt_tune_instruction") == "Belts already match");
    const std::string last = fx.text("bt_tune_last");
    INFO("last: " << last);
    CHECK(last.rfind("Within 0.", 0) == 0);
    CHECK(last.find(" Hz of each other at 100 Hz") != std::string::npos);
    CHECK_FALSE(lv_hidden(named(fx, "btn_tune_calibrate_anyway")));
    CHECK_FALSE(lv_hidden(named(fx, "btn_tune_done")));
    for (const char* hidden :
         {"bt_tune_stepper", "bt_tune_row_stepper", "btn_tune_swap", "btn_tune_measure",
          "btn_tune_measure_short", "bt_tune_icon_cw", "bt_tune_icon_ccw"}) {
        INFO(hidden);
        CHECK(lv_hidden(named(fx, hidden)));
    }
    // The gauge shows how close they are.
    CHECK(fx.state_int("bt_tune_has_gap") == 1);
    REQUIRE(fx.panel_session() != nullptr);
    CHECK(helix::ui::ui_belt_gap_gauge_get_gap(named(fx, "bt_tune_gauge")) ==
          Catch::Approx(fx.panel_session()->gap_hz()));

    // Good Enough: back to RESULTS, nothing swept.
    fx.mock().clear_gcode_script_history();
    fx.panel().handle_tune_done_clicked();
    CHECK(fx.state_int("belt_tension_state") == RESULTS);
    fx.pump_ms(2000);
    CHECK(fx.mock().gcode_script_history().empty());

    // Calibrate Anyway: on to the calibration turn, nothing turned yet.
    fx.panel().handle_tune_clicked();
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_ALREADY_MATCHED);
    fx.panel().handle_tune_calibrate_anyway_clicked();
    CHECK(fx.state_int("bt_tune_phase") == PHASE_CALIBRATE);
    const std::string instruction = fx.text("bt_tune_instruction");
    INFO("instruction: " << instruction);
    CHECK((instruction == "Tighten A one full turn" || instruction == "Tighten B one full turn"));
    CHECK(fx.text("bt_tune_last").empty());
    CHECK(lv_hidden(named(fx, "btn_tune_calibrate_anyway")));
    CHECK_FALSE(lv_hidden(named(fx, "btn_tune_measure")));
    fx.pump_ms(2000);
    CHECK(fx.mock().gcode_script_history().empty());
}

TEST_CASE("Tune refuses a range the printer never reported", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(RESULTS));
    // Klipper's defaults (max 135) stand in until a query answers.
    fx.panel_sweep_cfg().from_printer = false;
    fx.mock().clear_gcode_script_history();

    fx.panel().handle_tune_clicked();
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::ERROR));
    CHECK(fx.text("bt_error_message") ==
          "The printer has not reported its [resonance_tester] range, so a narrow sweep cannot be "
          "kept inside it.");
    CHECK(fx.panel_session() == nullptr);
    fx.pump_ms(2000);
    CHECK(fx.mock().gcode_script_history().empty());
}

TEST_CASE("a calibration turn that doesn't register asks for another", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    fx.mock().set_belt_tune_script('B', {98.3f, 104.0f});
    enter_tuning(fx);
    const auto before_a = fx.panel_run(0).curve;
    const auto before_b = fx.panel_run(1).curve;
    fx.mock().clear_gcode_script_history();

    tune_round(fx);
    CHECK(fx.state_int("bt_tune_phase") == PHASE_CALIBRATE);
    CHECK(fx.text("bt_tune_last") ==
          "That turn didn't register yet. Tighten B one more full turn.");
    CHECK(fx.text("bt_tune_instruction") == "Tighten B one full turn");
    CHECK(run_steps(fx) == std::vector<std::string>{"settle", "full", "full"});
    // A calibration sweep feeds the session, never RESULTS' runs.
    CHECK(fx.panel_run(0).curve == before_a);
    CHECK(fx.panel_run(1).curve == before_b);
    CHECK(fx.state_int("belt_tension_state") == TUNING);
}

TEST_CASE("the second turn registers and chains a narrow baseline without settling",
          "[belt][panel][tune]") {
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    fx.mock().set_belt_tune_script('B', {98.3f, 104.0f});
    enter_tuning(fx);
    tune_round(fx);
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_CALIBRATE);
    fx.mock().clear_gcode_script_history();

    tune_round(fx);
    CHECK(fx.state_int("bt_tune_phase") == PHASE_ADJUST);
    CHECK(fx.text("bt_tune_last").empty());
    // The laps once, both full sweeps, then the baseline pair twice over with
    // no laps between: no belt changed since the calibration sweep.
    CHECK(run_steps(fx) == std::vector<std::string>{"settle", "full", "full", "narrow", "narrow",
                                                    "narrow", "narrow"});
    REQUIRE(fx.panel_session() != nullptr);
    const auto window = fx.panel_session()->sweep_window();
    REQUIRE(window.has_value());
    for (const auto& line : resonance_lines(fx)) {
        if (line.find("FREQ_START=") == std::string::npos) {
            continue;
        }
        INFO(line);
        const auto [lo, hi] = freq_window(line);
        CHECK(lo == static_cast<int>(window->freq_start_hz));
        CHECK(hi == static_cast<int>(window->freq_end_hz));
        CHECK(lo >= 5);
        CHECK(hi <= 135);
    }
    // The zone is known now; the chart spans it.
    const auto [z_lo, z_hi] = zone_range(fx.text("bt_tune_zone"));
    CHECK(z_lo > 0);
    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    const auto [f_lo, f_hi] = ui_frequency_response_chart_get_freq_range(chart);
    CHECK(f_lo >= static_cast<float>(z_lo) - 1.0f);
    CHECK(f_hi <= static_cast<float>(z_hi) + 1.0f);
}

TEST_CASE("a calibration that gives up says to turn back", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    fx.mock().set_belt_tune_script('B', {98.2f, 98.4f});
    enter_tuning(fx);
    tune_round(fx);
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_CALIBRATE);
    tune_round(fx);

    CHECK(fx.state_int("bt_tune_phase") == PHASE_MATCHED);
    CHECK(fx.text("bt_tune_instruction") == "Nothing to tune here");
    CHECK(fx.text("bt_tune_last") ==
          "Nothing followed 2 turns of B. Turn B back 2 turns, then check the mechanics.");
    CHECK(fx.state_int("bt_tune_shape_warning") == 1);
    CHECK(fx.state_int("bt_tune_has_zone") == 0);
    CHECK(lv_hidden(named(fx, "btn_tune_keep")));
}

TEST_CASE("three Re-measures walk CLOSER, CLOSER, MATCHED", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    // A wider start than the mock's default, so its broad curves leave room
    // for two closer rounds before the match.
    fx.mock().set_belt_peaks_hz(120.0f, 96.0f);
    fx.mock().set_belt_tune_script('B', {96.3f, 101.0f, 101.0f, 101.0f, 106.0f, 112.0f, 119.8f});
    enter_adjust(fx);

    const auto windowed_both = [&](const std::vector<std::string>& lines) {
        bool a = false, b = false;
        for (const auto& l : lines) {
            if (freq_window(l).first < 0) {
                return false;
            }
            a = a || l.find("AXIS=1,-1 ") != std::string::npos;
            b = b || l.find("AXIS=1,1 ") != std::string::npos;
        }
        return lines.size() == 2 && a && b;
    };

    for (const char* expect : {"Hz closer", "Hz closer"}) {
        fx.mock().clear_gcode_script_history();
        step_as_advised(fx);
        tune_round(fx);
        CHECK(windowed_both(resonance_lines(fx)));
        CHECK(run_steps(fx).front() == "settle");
        CHECK(fx.state_int("bt_tune_phase") == PHASE_ADJUST);
        INFO("last: " << fx.text("bt_tune_last"));
        CHECK(fx.text("bt_tune_last").rfind("last turn: ", 0) == 0);
        CHECK(fx.text("bt_tune_last").find(expect) != std::string::npos);
        CHECK(fx.text("bt_tune_instruction").rfind("Tighten B ", 0) == 0);
    }

    fx.mock().clear_gcode_script_history();
    step_as_advised(fx);
    tune_round(fx);
    CHECK(windowed_both(resonance_lines(fx)));
    CHECK(fx.state_int("bt_tune_phase") == PHASE_MATCHED);
    INFO("instruction: " << fx.text("bt_tune_instruction"));
    CHECK(fx.text("bt_tune_instruction").rfind("Matched at 1", 0) == 0);
    CHECK(fx.text("bt_tune_last").empty());
    // Swapping paths is an ADJUST action; the one-line hero is tappable in
    // every phase, so a tap once matched must do nothing.
    fx.panel().handle_tune_swap_clicked();
    CHECK(fx.state_int("bt_tune_path") == 1);

    // The chart marks each path's centroid with a hollow ring.
    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    const auto marks_b = ui_frequency_response_chart_get_markers(chart, fx.panel_series_id(1));
    REQUIRE(marks_b.size() == 1);
    CHECK(marks_b.front().number == 0);
    CHECK(marks_b.front().freq_hz == Catch::Approx(fx.panel_session()->adjusted_hz()));
}

TEST_CASE("each outcome has its own last-turn line", "[belt][panel][tune]") {
    struct Case {
        float a_hz;
        float b_hz;
        const char* expect;
        const char* instruction;
    };
    // From a -4 Hz gap (B at 106 against A at 110), with no rate learned: the
    // sanity bound is 6 Hz.
    for (const Case c : {
             Case{110.0f, 104.0f,
                  "The gap grew after tightening B. Check you turned Path B's tensioner.",
                  "Tighten B"},
             Case{110.0f, 111.5f, "Went past A by 1.", "Loosen B"},
             Case{110.0f, 106.3f, "Try a bigger turn.", "Tighten B"},
             Case{110.0f, 108.0f, "Hz closer", "Tighten B"},
             // A 7 Hz jump: dropped, so the advice stands.
             Case{103.0f, 106.0f,
                  "That reading jumped too far to trust. Re-measure without turning.", "Tighten B"},
         }) {
        ReplayBelts belts({110.0f}, {98.0f});
        BeltPanelFixture fx;
        replay_to_adjust(fx, belts);
        belts.set_a({c.a_hz});
        belts.set_b({c.b_hz});
        tune_round(fx);
        INFO("A at " << c.a_hz << ", B at " << c.b_hz << ": " << fx.text("bt_tune_last"));
        CHECK(fx.text("bt_tune_last").find(c.expect) != std::string::npos);
        CHECK(fx.text("bt_tune_instruction") == c.instruction);
    }
}

TEST_CASE("Adjust A instead swaps the instruction", "[belt][panel][tune]") {
    SECTION("the calibration turn") {
        BeltPanelFixture fx;
        enter_tuning(fx);
        REQUIRE(fx.text("bt_tune_instruction") == "Tighten B noticeably");
        fx.panel().handle_tune_swap_clicked();
        CHECK(fx.state_int("bt_tune_phase") == PHASE_CALIBRATE);
        CHECK(fx.text("bt_tune_instruction") == "Tighten A noticeably");
        CHECK(fx.state_int("bt_tune_path") == 0);
        CHECK(fx.state_int("bt_tune_dir") == 0);
        CHECK(fx.text("bt_tune_gap_caption") == "A tighter than B");
        CHECK(fx.text("bt_tune_swap_label") == "Adjust B instead");
    }
    SECTION("a zone being tuned") {
        ReplayBelts belts({110.0f}, {98.0f});
        BeltPanelFixture fx;
        replay_to_adjust(fx, belts);
        fx.panel().handle_tune_swap_clicked();
        CHECK(fx.text("bt_tune_instruction") == "Loosen A");
        CHECK(fx.state_int("bt_tune_path") == 0);
        CHECK(fx.state_int("bt_tune_dir") == 1);
        CHECK(fx.text("bt_tune_gap_caption") == "A tighter than B");
        CHECK(fx.text("bt_tune_swap_label") == "Adjust B instead");
    }
}

TEST_CASE("the turn stepper shows only for screw tensioners", "[belt][panel][tune]") {
    SECTION("unknown kind: words only") {
        ReplayBelts belts({110.0f}, {98.0f});
        BeltPanelFixture fx;
        REQUIRE(get_printer_state().profile_state().printer_type().empty());
        replay_to_adjust(fx, belts);
        CHECK(fx.state_int("bt_tune_kind") == 0);
        CHECK(fx.text("bt_tune_amount").empty());
        CHECK(fx.text("bt_tune_rate").empty());
        CHECK(lv_hidden(named(fx, "bt_tune_stepper")));
        // Words only: no screw arrow either way.
        for (const char* icon : {"bt_tune_icon_cw", "bt_tune_icon_ccw"}) {
            INFO(icon);
            CHECK(lv_hidden(named(fx, icon)));
        }
    }
    SECTION("screw: a quarter-turn stepper that starts empty, clamped at two turns") {
        ReplayBelts belts({110.0f}, {98.0f});
        BeltPanelFixture fx;
        get_printer_state().set_printer_type_sync("Voron 2.4");
        replay_to_adjust(fx, belts);
        CHECK(fx.state_int("bt_tune_kind") == 1);
        CHECK_FALSE(lv_hidden(named(fx, "bt_tune_stepper")));
        // "Tighten B": the clockwise arrow, not the counter-clockwise one.
        CHECK_FALSE(lv_hidden(named(fx, "bt_tune_icon_cw")));
        CHECK(lv_hidden(named(fx, "bt_tune_icon_ccw")));
        // The calibration taught a rate, one decimal.
        CHECK(fx.text("bt_tune_rate").rfind("~", 0) == 0);
        CHECK(fx.text("bt_tune_rate").find(".") != std::string::npos);
        CHECK(fx.text("bt_tune_rate").find(" Hz / turn") != std::string::npos);
        CHECK(fx.text("bt_tune_amount") == "Not turned");
        // Minus from empty stays empty; plus goes to the smallest step.
        fx.panel().handle_tune_amount_step(-1);
        CHECK(fx.text("bt_tune_amount") == "Not turned");
        fx.panel().handle_tune_amount_step(+1);
        CHECK(fx.text("bt_tune_amount") == "¼ turn");
        for (int i = 0; i < 10; ++i) {
            fx.panel().handle_tune_amount_step(+1);
        }
        CHECK(fx.text("bt_tune_amount") == "2 turns");
        for (int i = 0; i < 7; ++i) {
            fx.panel().handle_tune_amount_step(-1);
        }
        CHECK(fx.text("bt_tune_amount") == "¼ turn");
        fx.panel().handle_tune_amount_step(-1);
        CHECK(fx.text("bt_tune_amount") == "Not turned");
        fx.panel().handle_tune_amount_step(-1);
        CHECK(fx.text("bt_tune_amount") == "Not turned");
    }
}

TEST_CASE("the instruction carries the advised amount", "[belt][panel][tune]") {
    SECTION("screw") {
        ReplayBelts belts({110.0f}, {98.0f});
        BeltPanelFixture fx;
        get_printer_state().set_printer_type_sync("Voron 2.4");
        replay_to_adjust(fx, belts);
        // An 8 Hz calibration move per turn against a 4 Hz gap: half a turn.
        CHECK(fx.text("bt_tune_instruction") == "Tighten B ½ turn");
    }
    SECTION("unknown kind: no amount") {
        ReplayBelts belts({110.0f}, {98.0f});
        BeltPanelFixture fx;
        replay_to_adjust(fx, belts);
        CHECK(fx.text("bt_tune_instruction") == "Tighten B");
    }
}

TEST_CASE("Good enough returns to RESULTS without sweeping", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    enter_tuning(fx);
    const std::string similarity = fx.text("bt_similarity");
    fx.mock().clear_gcode_script_history();

    fx.panel().handle_tune_done_clicked();
    CHECK(fx.state_int("belt_tension_state") == RESULTS);
    fx.pump_ms(2000);
    CHECK(resonance_lines(fx).empty());
    CHECK(fx.text("bt_similarity") == similarity);

    // RESULTS shows the full band again.
    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    const auto [f_lo, f_hi] = ui_frequency_response_chart_get_freq_range(chart);
    CHECK(f_lo < 10.0f);
    CHECK(f_hi > 130.0f);
}

TEST_CASE("Confirm runs one full pair and reports the tuning", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    fx.mock().set_belt_peaks_hz(120.0f, 96.0f);
    fx.mock().set_belt_tune_script('B', {96.3f, 101.0f, 101.0f, 101.0f, 106.0f, 112.0f, 119.8f});
    enter_tuning(fx);
    const std::string round0 = fx.text("bt_similarity");
    calibrate(fx);
    // Two calibration turns, then what each round's stepper recorded.
    float turned = 2.0f * helix::calibration::belt_tune::CALIBRATION_TURNS;
    for (int i = 0; i < 3; ++i) {
        turned +=
            static_cast<float>(fx.panel_advised_steps()) * helix::calibration::belt_tune::TURN_STEP;
        step_as_advised(fx);
        tune_round(fx);
    }
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_MATCHED);
    const int rounds = fx.panel_session()->total_rounds();

    fx.mock().clear_gcode_script_history();
    fx.panel().handle_tune_confirm_clicked();
    REQUIRE(fx.pump_until_state(RESULTS));
    const auto lines = resonance_lines(fx);
    REQUIRE(lines.size() == 2);
    for (const auto& l : lines) {
        CHECK(l.find("FREQ_START") == std::string::npos);
    }
    CHECK(fx.text("bt_similarity_was") == "was " + round0);
    for (const char* note : {"bt_note_a", "bt_note_b"}) {
        INFO(note << ": " << fx.text(note));
        CHECK(fx.text(note).find("was") == std::string::npos);
    }
    INFO("facts: " << fx.text("bt_facts"));
    const std::string expect =
        "Tuned in " + std::to_string(rounds) + " rounds · B tightened about " +
        helix::calibration::format_belt_turns(turned) + (turned > 1.0f ? " turns" : " turn");
    CHECK(fx.text("bt_facts").find(expect) != std::string::npos);
    CHECK(fx.text("bt_facts").find("A tightened") == std::string::npos);
    CHECK(fx.text("bt_facts").find("A loosened") == std::string::npos);
}

namespace {

/// Starts a sweep, confirms Stop's dialog, and checks the panel lands on
/// START and stays there.
void stop_lands_on_start(BeltPanelFixture& fx) {
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_SWEEPING);
    fx.panel().handle_stop_clicked();
    fx.pump_ms(200);
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_t* stop = lv_obj_find_by_name(dialog, "btn_primary");
    REQUIRE(stop != nullptr);
    lv_obj_send_event(stop, LV_EVENT_CLICKED, nullptr);
    fx.pump_ms(200);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::START));
    fx.pump_ms(5000);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::START));
    CHECK(fx.panel_session() == nullptr);
}

/// Rebuilds with a sweep in flight and checks its late callbacks change nothing.
void rebuild_drops_late_callbacks(BeltPanelFixture& fx) {
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_SWEEPING);
    // Past the laps: a sweep is running on the printer.
    REQUIRE(fx.pump_until_text("bt_run_title", "Measuring Path A"));
    fx.pump_ms(300);
    // Frees the tree the chart lives in while the mock keeps sweeping.
    REQUIRE(fx.panel().rebuild());
    UpdateQueue::instance().drain();
    // Long enough for a full sweep's every line to land.
    fx.pump_ms(30000);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::START));
    CHECK(fx.panel_chart() == nullptr);
    CHECK(fx.panel_session() == nullptr);
}

} // namespace

TEST_CASE("Stop during a tuning sweep lands on START", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    helix::ui::modal_init_subjects();
    REQUIRE(fx.register_component("modal_dialog"));
    enter_adjust(fx);
    fx.panel().handle_tune_measure_clicked();
    stop_lands_on_start(fx);
}

TEST_CASE("Stop during a calibration run lands on START", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    helix::ui::modal_init_subjects();
    REQUIRE(fx.register_component("modal_dialog"));
    enter_tuning(fx);
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_CALIBRATE);
    fx.panel().handle_tune_measure_clicked();
    stop_lands_on_start(fx);
}

TEST_CASE("a rebuild mid tuning sweep drops the late callbacks", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    enter_adjust(fx);
    fx.panel().handle_tune_measure_clicked();
    rebuild_drops_late_callbacks(fx);
}

TEST_CASE("a rebuild mid calibration run drops the late callbacks", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    enter_tuning(fx);
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_CALIBRATE);
    const auto before = fx.panel_run(1).curve;
    fx.panel().handle_tune_measure_clicked();
    rebuild_drops_late_callbacks(fx);
    // Nothing the abandoned calibration measured reached RESULTS' runs.
    CHECK(fx.panel_run(1).curve == before);
}

TEST_CASE("a round with no peak says so and can be re-measured", "[belt][panel][tune]") {
    ReplayBelts belts({110.0f}, {98.0f});
    BeltPanelFixture fx;
    replay_to_adjust(fx, belts);
    const std::string gap = fx.text("bt_tune_gap");
    belts.set_b({});
    tune_round(fx);

    CHECK(fx.state_int("bt_tune_phase") == PHASE_ADJUST);
    CHECK(fx.text("bt_tune_last") == "No clear peak in this zone");
    CHECK(fx.text("bt_tune_gap") == gap);
    lv_obj_t* measure = named(fx, "btn_tune_measure");
    CHECK_FALSE(lv_obj_has_state(measure, LV_STATE_DISABLED));
    CHECK_FALSE(lv_hidden(measure));
}

TEST_CASE("the text faces draw the turn fractions", "[belt][panel][tune][font]") {
    // The faces every build links: the turn amount and facts line use them.
    for (const lv_font_t* font :
         {&noto_sans_18, &noto_sans_26, &noto_sans_bold_28, &noto_sans_light_16}) {
        for (const uint32_t cp : {0xBCu, 0xBDu, 0xBEu, 0x215Bu, 0x215Cu, 0x215Du, 0x215Eu}) {
            lv_font_glyph_dsc_t dsc{};
            INFO("U+" << std::hex << cp << " in a " << std::dec << font->line_height << " px face");
            CHECK(lv_font_get_glyph_dsc(font, &dsc, cp, 0));
        }
        // A fraction outside the baked range is absent, so the lookup can fail.
        lv_font_glyph_dsc_t sixth{};
        CHECK_FALSE(lv_font_get_glyph_dsc(font, &sixth, 0x2159, 0));
    }
}

namespace {

/// Points PrinterDetector at a user extension naming one spring-tensioned
/// printer for the life of this object.
class SpringPrinterDb {
  public:
    SpringPrinterDb() : dir_("/tmp/helix-belt-spring-db-" + std::to_string(getpid())) {
        env_.emplace("HELIX_CONFIG_DIR", dir_.c_str());
        std::filesystem::create_directories(dir_ + "/printer_database.d");
        std::ofstream(dir_ + "/printer_database.d/spring.json")
            << R"({"printers": [{"id": "test_spring", "name": "Test Spring Printer",
                                  "belt_tensioner": "spring"}]})";
        PrinterDetector::reload();
    }
    ~SpringPrinterDb() {
        // The database reloads from the real config dir, not this one.
        env_.reset();
        PrinterDetector::reload();
        std::filesystem::remove_all(dir_);
    }

  private:
    std::string dir_;
    std::optional<helix::ScopedEnv> env_;
};

} // namespace

TEST_CASE("a spring printer skips calibration", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    SpringPrinterDb db;
    REQUIRE(PrinterDetector::get_belt_tensioner("Test Spring Printer") ==
            helix::calibration::BeltTensionerKind::SPRING);
    get_printer_state().set_printer_type_sync("Test Spring Printer");
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(RESULTS));
    fx.mock().clear_gcode_script_history();

    // The strongest zone at once: its baseline pair sweeps straight from Tune.
    fx.panel().handle_tune_clicked();
    REQUIRE(fx.state_int("belt_tension_state") == TUNING);
    CHECK(fx.state_int("bt_tune_phase") == PHASE_SWEEPING);
    CHECK(fx.text("bt_tune_instruction") == "Measure this zone first");
    for (uint32_t t = 0; t < 120000 && fx.state_int("bt_tune_phase") == PHASE_SWEEPING; t += 100) {
        fx.pump_ms(100);
    }
    CHECK(run_steps(fx) ==
          std::vector<std::string>{"settle", "narrow", "narrow", "narrow", "narrow"});

    CHECK(fx.state_int("bt_tune_phase") == PHASE_ADJUST);
    CHECK(fx.state_int("bt_tune_kind") == 2);
    CHECK(fx.text("bt_tune_instruction") ==
          "Release B's tensioner, let the spring settle, lock it");
    CHECK(fx.text("bt_tune_amount").empty());
    for (const char* name : {"bt_tune_stepper", "bt_tune_icon_cw", "bt_tune_icon_ccw"}) {
        INFO(name);
        CHECK(lv_hidden(named(fx, name)));
    }
}

TEST_CASE("Next Zone walks only registered zones", "[belt][panel][tune]") {
    // Two pairs on each path: 100/94 and 60/56.
    ReplayBelts belts({60.0f, 100.0f}, {56.0f, 94.0f});
    BeltPanelFixture fx;
    enter_tuning(fx);
    REQUIRE(fx.text("bt_tune_instruction") == "Tighten B noticeably");

    SECTION("both zones moved with the turn") {
        // The 94 peak moves 4 Hz, the 56 one 2 Hz: both register, the bigger first.
        belts.set_b({58.0f, 98.0f});
        calibrate(fx);
        const auto [lo, hi] = zone_range(fx.text("bt_tune_zone"));
        CHECK(lo <= 94);
        CHECK(hi >= 100);
        CHECK(fx.state_int("bt_tune_has_next_zone") == 1);

        // B's upper peak meets A's: this zone matches, the lower pair is next.
        belts.set_b({58.0f, 100.0f});
        tune_round(fx);
        REQUIRE(fx.state_int("bt_tune_phase") == PHASE_MATCHED);
        REQUIRE(fx.state_int("bt_tune_has_next_zone") == 1);

        fx.mock().clear_gcode_script_history();
        fx.panel().handle_tune_next_zone_clicked();
        // The baseline pair starts at once, with no advice on screen.
        CHECK(fx.state_int("bt_tune_phase") == PHASE_SWEEPING);
        CHECK(fx.state_int("bt_tune_baseline") == 1);
        CHECK(fx.text("bt_tune_instruction") == "Measure this zone first");
        CHECK(fx.text("bt_tune_amount").empty());
        CHECK(fx.text("bt_tune_gap") == "--");
        const auto [lo2, hi2] = zone_range(fx.text("bt_tune_zone"));
        CHECK(lo2 <= 56);
        CHECK(hi2 >= 60);
        CHECK(hi2 < 90);

        for (uint32_t t = 0; t < 120000 && fx.state_int("bt_tune_phase") == PHASE_SWEEPING;
             t += 100) {
            fx.pump_ms(100);
        }
        // Turns on the earlier zone moved this one: laps first, then the pair twice.
        CHECK(run_steps(fx) ==
              std::vector<std::string>{"settle", "narrow", "narrow", "narrow", "narrow"});
        REQUIRE(fx.state_int("bt_tune_phase") == PHASE_ADJUST);
        CHECK(fx.state_int("bt_tune_baseline") == 0);
        CHECK(fx.text("bt_tune_instruction") == "Tighten B");
        CHECK(std::stof(fx.text("bt_tune_gap")) == Catch::Approx(2.0f).margin(0.3f));
        // A baseline has no previous turn to report on.
        CHECK(fx.text("bt_tune_last").empty());
        CHECK(fx.state_int("bt_tune_has_next_zone") == 0);
    }
    SECTION("only the zone that moved") {
        belts.set_b({56.0f, 98.0f});
        calibrate(fx);
        CHECK(fx.state_int("bt_tune_has_next_zone") == 0);
        belts.set_b({56.0f, 100.0f});
        tune_round(fx);
        REQUIRE(fx.state_int("bt_tune_phase") == PHASE_MATCHED);
        CHECK(fx.state_int("bt_tune_has_next_zone") == 0);
        CHECK(fx.text("bt_tune_next_zone_label").empty());
    }
}

TEST_CASE("a closed gate stops the tuning sweeps", "[belt][panel][tune][gating]") {
    BeltPanelFixture fx;
    const auto close_gate = [&] {
        REQUIRE(fx.state_int("bt_can_start") == 1);
        // A print starts while the user is turning a screw.
        lv_subject_set_int(get_printer_state().print_state().get_print_active_subject(), 1);
        fx.pump_ms(200);
        CHECK(fx.state_int("bt_can_start") == 0);
        CHECK(lv_obj_has_state(named(fx, "btn_tune_measure"), LV_STATE_DISABLED));
        fx.mock().clear_gcode_script_history();
        fx.panel().handle_tune_measure_clicked();
        fx.pump_ms(2000);
        CHECK(resonance_lines(fx).empty());
    };
    SECTION("a calibration") {
        enter_tuning(fx);
        close_gate();
        CHECK(fx.state_int("bt_tune_phase") == PHASE_CALIBRATE);
    }
    SECTION("a round") {
        enter_adjust(fx);
        close_gate();
        CHECK(fx.state_int("bt_tune_phase") == PHASE_ADJUST);
    }
}

TEST_CASE("a wrong-way line names the turn the user made", "[belt][panel][tune]") {
    // Told to tighten B, B holds while A rises 110 -> 113: the gap grew, which
    // is what tightening A instead of B does.
    ReplayBelts belts({110.0f}, {98.0f});
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    replay_to_adjust(fx, belts);
    REQUIRE(fx.text("bt_tune_instruction") == "Tighten B ½ turn");
    step_as_advised(fx);
    belts.set_a({113.0f});
    tune_round(fx);
    INFO("last: " << fx.text("bt_tune_last"));
    CHECK(fx.panel_last_turns() == std::optional<float>(0.5f));
    CHECK(fx.text("bt_tune_last") ==
          "The gap grew after tightening B. Check you turned Path B's tensioner.");
}

TEST_CASE("both peaks drifting together is no move", "[belt][panel][tune]") {
    // No turn: A and B both read 1.5 Hz higher. The gap held at -4.
    ReplayBelts belts({110.0f}, {98.0f});
    BeltPanelFixture fx;
    replay_to_adjust(fx, belts);
    belts.set_a({111.5f});
    belts.set_b({107.5f});
    tune_round(fx);
    INFO("last: " << fx.text("bt_tune_last"));
    CHECK(fx.text("bt_tune_last").find("The gap didn't close (moved 0.") == 0);
    CHECK(fx.text("bt_tune_instruction") == "Tighten B");
}

TEST_CASE("a reference peak that leaves the zone names the reference", "[belt][panel][tune]") {
    ReplayBelts belts({110.0f}, {98.0f});
    BeltPanelFixture fx;
    replay_to_adjust(fx, belts);
    SECTION("the reference's zone is flat") {
        // Told to tighten B; A, the reference, shows no peak.
        belts.set_a({});
        tune_round(fx);
        CHECK(fx.text("bt_tune_last") ==
              "Path A's peak left the zone. Check that only Path B's tensioner was turned.");
    }
    SECTION("the adjusted path losing its peak keeps the generic line") {
        belts.set_b({});
        tune_round(fx);
        CHECK(fx.text("bt_tune_last") == "No clear peak in this zone");
    }
}

TEST_CASE("readings that don't repeat give no advice", "[belt][panel][tune]") {
    ReplayBelts belts({110.0f}, {98.0f});
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    enter_tuning(fx);
    // B's calibration sweep registers at 106; the pair's second sweep reads
    // 2 Hz higher with nothing turned.
    belts.set_b({106.0f});
    fx.panel().handle_tune_measure_clicked();
    bool second_pair = false;
    for (uint32_t t = 0; t < 120000 && fx.state_int("bt_tune_phase") == PHASE_SWEEPING; t += 100) {
        if (!second_pair && fx.panel_session() &&
            fx.panel_session()->stage() == helix::calibration::BeltTuneStage::BASELINE &&
            !fx.panel_session()->rounds().empty()) {
            belts.set_b({108.0f});
            second_pair = true;
        }
        fx.pump_ms(100);
    }
    REQUIRE(second_pair);
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_ADJUST);

    CHECK(fx.state_int("bt_tune_unrepeatable") == 1);
    CHECK(fx.state_int("bt_tune_shape_warning") == 1);
    CHECK(fx.text("bt_tune_hint") == "These readings aren't repeatable, so tuning can't be "
                                     "steered. Check the mechanics first.");
    // No turn advice: neither a direction nor an amount.
    CHECK(fx.text("bt_tune_instruction") == "Readings don't repeat");
    CHECK(fx.text("bt_tune_amount").empty());
    for (const char* hidden :
         {"bt_tune_stepper", "bt_tune_row_stepper", "bt_tune_icon_cw", "bt_tune_icon_ccw"}) {
        INFO(hidden);
        CHECK(lv_hidden(named(fx, hidden)));
    }
    CHECK_FALSE(lv_hidden(named(fx, "btn_tune_done")));
    CHECK_FALSE(lv_hidden(named(fx, "btn_tune_measure")));
    // The wrapping line carries the whole sentence.
    CHECK_FALSE(lv_hidden(named(fx, "bt_tune_hint_wrap")));
    // Re-measure sweeps the pair twice: both counted.
    const auto window = fx.panel_session()->sweep_window();
    REQUIRE(window.has_value());
    REQUIRE(sweep_minutes(*window, 1) != sweep_minutes(*window, 2));
    CHECK(fx.text("bt_tune_measure_label") ==
          "Re-measure (~" + std::to_string(sweep_minutes(*window, 2)) + " min)");

    // Re-measure: a fresh pair, laps once; this one repeats.
    fx.mock().clear_gcode_script_history();
    tune_round(fx);
    CHECK(run_steps(fx) ==
          std::vector<std::string>{"settle", "narrow", "narrow", "narrow", "narrow"});
    CHECK(fx.state_int("bt_tune_unrepeatable") == 0);
    CHECK(fx.text("bt_tune_instruction").rfind("Tighten B ", 0) == 0);
    CHECK_FALSE(lv_hidden(named(fx, "bt_tune_stepper")));
}

TEST_CASE("the power hint names the stronger path", "[belt][panel][tune]") {
    // A spring printer goes straight to its zone. B's peak is four times A's.
    const std::string csv_a = "/tmp/helix-belt-power-a-" + std::to_string(getpid()) + ".csv";
    const std::string csv_b = "/tmp/helix-belt-power-b-" + std::to_string(getpid()) + ".csv";
    BeltPanelFixture fx;
    SpringPrinterDb db;
    get_printer_state().set_printer_type_sync("Test Spring Printer");

    const auto run = [&](double b_height) {
        write_peak_csv_at(csv_a, 108.0, 1.0e4);
        write_peak_csv_at(csv_b, 100.0, b_height);
        helix::ScopedEnv replay_a("HELIX_MOCK_BELT_CSV_A", csv_a.c_str());
        helix::ScopedEnv replay_b("HELIX_MOCK_BELT_CSV_B", csv_b.c_str());
        REQUIRE(fx.wait_gate_open());
        fx.panel().handle_start_clicked();
        REQUIRE(fx.pump_until_state(RESULTS));
        fx.panel().handle_tune_clicked();
        for (uint32_t t = 0; t < 120000 && fx.state_int("bt_tune_phase") == PHASE_SWEEPING;
             t += 100) {
            fx.pump_ms(100);
        }
        REQUIRE(fx.state_int("bt_tune_phase") == PHASE_ADJUST);
    };
    SECTION("B four times A") {
        run(4.0e4);
        CHECK(fx.state_int("bt_tune_has_power_hint") == 1);
        INFO("hint: " << fx.text("bt_tune_power_hint"));
        const auto hint = fx.panel_session()->power_hint();
        REQUIRE(hint.has_value());
        CHECK(hint->stronger == helix::calibration::BeltPath::PATH_B);
        CHECK(fx.text("bt_tune_power_hint") ==
              fmt::format("Path B resonates much more than A at {:.0f} Hz. That points at a "
                          "mechanical difference (pulley grub screws, idler bearings, belt "
                          "tracking), not tension.",
                          hint->freq_hz));
        CHECK_FALSE(lv_hidden(named(fx, "bt_tune_power_row")));
    }
    SECTION("equal heights") {
        run(1.0e4);
        CHECK(fx.state_int("bt_tune_has_power_hint") == 0);
        CHECK(lv_hidden(named(fx, "bt_tune_power_row")));
    }
    std::remove(csv_a.c_str());
    std::remove(csv_b.c_str());
}

TEST_CASE("waking the screen mid-tuning keeps the session", "[belt][panel][tune]") {
    ReplayBelts belts({110.0f}, {98.0f});
    BeltPanelFixture fx;
    replay_to_adjust(fx, belts);
    belts.set_b({108.0f});
    tune_round(fx);
    // Two baseline rounds, then this one.
    REQUIRE(fx.text("bt_tune_round") == "Round 4");
    const std::string zone = fx.text("bt_tune_zone");
    const std::string last = fx.text("bt_tune_last");
    REQUIRE_FALSE(fx.panel_detection_pending());

    SECTION("between rounds, at the tensioner") {
        fx.panel().on_deactivate(DeactivateReason::Suspended);
        fx.panel().on_activate();
        fx.pump_ms(500);
        CHECK(fx.state_int("belt_tension_state") == TUNING);
        CHECK(fx.state_int("bt_tune_phase") == PHASE_ADJUST);
        CHECK(fx.text("bt_tune_round") == "Round 4");
        CHECK(fx.text("bt_tune_zone") == zone);
        CHECK(fx.text("bt_tune_last") == last);
        // The calibrator is not asked to detect hardware again.
        CHECK_FALSE(fx.panel_detection_pending());
        // The loop carries on from where it was.
        tune_round(fx);
        CHECK(fx.text("bt_tune_round") == "Round 5");
    }
    SECTION("while a round sweeps") {
        fx.panel().handle_tune_measure_clicked();
        REQUIRE(fx.state_int("bt_tune_phase") == PHASE_SWEEPING);
        fx.pump_ms(300);
        fx.panel().on_deactivate(DeactivateReason::Suspended);
        fx.panel().on_activate();
        CHECK(fx.state_int("belt_tension_state") == TUNING);
        CHECK(fx.state_int("bt_tune_phase") == PHASE_SWEEPING);
        CHECK_FALSE(fx.panel_detection_pending());
        for (uint32_t t = 0; t < 60000 && fx.state_int("bt_tune_phase") == PHASE_SWEEPING;
             t += 100) {
            fx.pump_ms(100);
        }
        CHECK(fx.state_int("belt_tension_state") == TUNING);
        CHECK(fx.text("bt_tune_round") == "Round 5");
    }
    SECTION("leaving after a suspension makes the next entry a fresh one") {
        fx.panel().on_deactivate(DeactivateReason::Suspended);
        fx.panel().on_deactivate(DeactivateReason::NavigateAway);
        fx.panel().on_activate();
        fx.pump_ms(200);
        CHECK(fx.state_int("belt_tension_state") ==
              static_cast<int>(BeltTensionPanel::ViewState::START));
    }
}

TEST_CASE("a wake re-asks what the suspension cancelled", "[belt][panel]") {
    BeltPanelFixture fx;
    helix::ui::modal_init_subjects();
    REQUIRE(fx.register_component("modal_dialog"));

    SECTION("hardware detection still pending") {
        fx.panel().on_deactivate(DeactivateReason::NavigateAway);
        fx.panel().on_activate();
        REQUIRE(fx.panel_detection_pending());
        // The screen sleeps before detection answers; its callbacks expire.
        fx.panel().on_deactivate(DeactivateReason::Suspended);
        fx.pump_ms(2000);
        REQUIRE(fx.panel_detection_pending());
        fx.panel().on_activate();
        fx.pump_ms(2000);
        CHECK_FALSE(fx.panel_detection_pending());
        CHECK(fx.wait_gate_open());
    }
    SECTION("the low-RAM warning closes with the screen") {
        enter_tuning(fx);
        helix::MemoryInfo small;
        small.total_kb = 128 * 1024;
        small.available_kb = 80 * 1024;
        fx.panel().set_memory_for_test(small);
        fx.panel().handle_tune_measure_clicked();
        fx.pump_ms(200);
        REQUIRE(fx.panel_low_ram_dialog() != nullptr);
        fx.panel().on_deactivate(DeactivateReason::Suspended);
        fx.pump_ms(500);
        CHECK(fx.panel_low_ram_dialog() == nullptr);
        CHECK(ModalStack::instance().top_dialog() == nullptr);
        // After the wake, Measure asks again and Continue sweeps.
        fx.panel().on_activate();
        fx.panel().handle_tune_measure_clicked();
        fx.pump_ms(200);
        lv_obj_t* dialog = fx.panel_low_ram_dialog();
        REQUIRE(dialog != nullptr);
        lv_obj_send_event(lv_obj_find_by_name(dialog, "btn_primary"), LV_EVENT_CLICKED, nullptr);
        fx.pump_ms(200);
        CHECK(fx.state_int("bt_tune_phase") == PHASE_SWEEPING);
    }
}

TEST_CASE("Continue on the low-RAM warning re-checks the gate", "[belt][panel][tune][gating]") {
    BeltPanelFixture fx;
    helix::ui::modal_init_subjects();
    REQUIRE(fx.register_component("modal_dialog"));
    helix::MemoryInfo small;
    small.total_kb = 128 * 1024;
    small.available_kb = 80 * 1024;

    const auto continue_dialog = [&] {
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        REQUIRE(dialog != nullptr);
        REQUIRE(fx.panel_low_ram_dialog() == dialog);
        // A print starts from another client while the warning is up.
        lv_subject_set_int(get_printer_state().print_state().get_print_active_subject(), 1);
        fx.pump_ms(200);
        fx.mock().clear_gcode_script_history();
        lv_obj_send_event(lv_obj_find_by_name(dialog, "btn_primary"), LV_EVENT_CLICKED, nullptr);
        fx.pump_ms(2000);
        CHECK(resonance_lines(fx).empty());
    };

    SECTION("a calibration") {
        enter_tuning(fx);
        fx.panel().set_memory_for_test(small);
        fx.panel().handle_tune_measure_clicked();
        fx.pump_ms(200);
        continue_dialog();
        CHECK(fx.state_int("belt_tension_state") == TUNING);
        CHECK(fx.state_int("bt_tune_phase") == PHASE_CALIBRATE);
    }
    SECTION("a tuning round") {
        enter_adjust(fx);
        const std::string round = fx.text("bt_tune_round");
        fx.panel().set_memory_for_test(small);
        fx.panel().handle_tune_measure_clicked();
        fx.pump_ms(200);
        continue_dialog();
        CHECK(fx.state_int("belt_tension_state") == TUNING);
        CHECK(fx.state_int("bt_tune_phase") == PHASE_ADJUST);
        CHECK(fx.text("bt_tune_round") == round);
    }
    SECTION("a Re-test leaves both results in place") {
        REQUIRE(fx.wait_gate_open());
        fx.panel().handle_start_clicked();
        REQUIRE(fx.pump_until_state(RESULTS));
        const std::string note_a = fx.text("bt_note_a");
        fx.panel().set_memory_for_test(small);
        fx.panel().handle_retest_clicked(helix::calibration::BeltPath::PATH_A);
        fx.pump_ms(200);
        continue_dialog();
        CHECK(fx.state_int("belt_tension_state") == RESULTS);
        CHECK(fx.text("bt_note_a") == note_a);
        // Path A's result survived: Tune can still start from the comparison.
        lv_subject_set_int(get_printer_state().print_state().get_print_active_subject(), 0);
        fx.pump_ms(200);
        fx.panel().handle_tune_clicked();
        CHECK(fx.state_int("belt_tension_state") == TUNING);
    }
}

TEST_CASE("the low-RAM warning is asked once per tuning session", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    helix::ui::modal_init_subjects();
    REQUIRE(fx.register_component("modal_dialog"));
    enter_tuning(fx);
    helix::MemoryInfo small;
    small.total_kb = 128 * 1024;
    small.available_kb = 80 * 1024;
    fx.panel().set_memory_for_test(small);

    fx.panel().handle_tune_measure_clicked();
    fx.pump_ms(200);
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    REQUIRE(fx.panel_low_ram_dialog() == dialog);
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_CALIBRATE);
    lv_obj_send_event(lv_obj_find_by_name(dialog, "btn_primary"), LV_EVENT_CLICKED, nullptr);
    fx.pump_ms(200);
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_SWEEPING);
    // The calibration registers and its baseline follows without asking.
    for (uint32_t t = 0; t < 120000 && fx.state_int("bt_tune_phase") == PHASE_SWEEPING; t += 100) {
        fx.pump_ms(100);
    }
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_ADJUST);

    // The next round sweeps at once.
    fx.panel().handle_tune_measure_clicked();
    CHECK(fx.panel_low_ram_dialog() == nullptr);
    CHECK(fx.state_int("bt_tune_phase") == PHASE_SWEEPING);
    for (uint32_t t = 0; t < 60000 && fx.state_int("bt_tune_phase") == PHASE_SWEEPING; t += 100) {
        fx.pump_ms(100);
    }

    // A new tuning session asks again.
    fx.panel().handle_tune_done_clicked();
    fx.panel().handle_tune_clicked();
    REQUIRE(fx.state_int("belt_tension_state") == TUNING);
    fx.panel().handle_tune_measure_clicked();
    fx.pump_ms(200);
    CHECK(fx.panel_low_ram_dialog() != nullptr);
    CHECK(fx.state_int("bt_tune_phase") == PHASE_CALIBRATE);
}

TEST_CASE("a Stop confirmed after the round ended stops nothing", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    helix::ui::modal_init_subjects();
    REQUIRE(fx.register_component("modal_dialog"));
    enter_tuning(fx);
    fx.panel().handle_tune_measure_clicked();
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_SWEEPING);
    fx.panel().handle_stop_clicked();
    fx.pump_ms(200);
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);

    // The calibration and its baseline finish while the dialog is still up.
    for (uint32_t t = 0; t < 120000 && fx.state_int("bt_tune_phase") == PHASE_SWEEPING; t += 100) {
        fx.pump_ms(100);
    }
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_ADJUST);
    const std::string round = fx.text("bt_tune_round");

    fx.mock().clear_gcode_script_history();
    lv_obj_send_event(lv_obj_find_by_name(dialog, "btn_primary"), LV_EVENT_CLICKED, nullptr);
    fx.pump_ms(2000);
    CHECK(fx.state_int("belt_tension_state") == TUNING);
    CHECK(fx.text("bt_tune_round") == round);
    for (const auto& g : std::vector<std::string>(fx.mock().gcode_script_history())) {
        INFO(g);
        CHECK(g.find("FIRMWARE_RESTART") == std::string::npos);
    }
}

TEST_CASE("an untouched stepper is a repeat check", "[belt][panel][tune]") {
    ReplayBelts belts({110.0f}, {98.0f});
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    replay_to_adjust(fx, belts);
    REQUIRE(fx.text("bt_tune_instruction") == "Tighten B ½ turn");
    REQUIRE(fx.text("bt_tune_amount") == "Not turned");

    // B reads 0.6 Hz lower with nothing turned: against the advice, but no turn.
    belts.set_b({105.4f});
    tune_round(fx);
    INFO("last: " << fx.text("bt_tune_last"));
    // No verdict on a turn nobody made: the line reports the gap's move.
    CHECK(fx.text("bt_tune_last") == "No turn: the gap moved 0.6 Hz");
    CHECK(fx.text("bt_tune_last").find("grew") == std::string::npos);
    CHECK_FALSE(fx.panel_last_turns().has_value());
    // Nothing was turned, so the advice does not double.
    CHECK(fx.text("bt_tune_instruction").rfind("Tighten B ", 0) == 0);
    CHECK(fx.text("bt_tune_amount") == "Not turned");

    // A stepper set to a half books a half, then reads empty again.
    fx.panel().handle_tune_amount_step(+1);
    fx.panel().handle_tune_amount_step(+1);
    REQUIRE(fx.text("bt_tune_amount") == "½ turn");
    belts.set_b({108.0f});
    tune_round(fx);
    CHECK(fx.panel_last_turns() == std::optional<float>(0.5f));
    CHECK(fx.text("bt_tune_amount") == "Not turned");

    // A swap starts the stepper empty too.
    fx.panel().handle_tune_amount_step(+1);
    fx.panel().handle_tune_swap_clicked();
    fx.panel().handle_tune_swap_clicked();
    CHECK(fx.text("bt_tune_amount") == "Not turned");

    step_as_advised(fx);
    belts.set_b({110.0f});
    tune_round(fx);
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_MATCHED);
    fx.panel().handle_tune_confirm_clicked();
    REQUIRE(fx.pump_until_state(RESULTS));
    // Only the turned rounds booked anything.
    INFO("facts: " << fx.text("bt_facts"));
    CHECK(fx.text("bt_facts").find("B tightened about") != std::string::npos);
}

TEST_CASE("the gate re-reads the calibrator when a sweep ends", "[belt][panel][gating]") {
    BeltPanelFixture fx;
    lv_subject_t* print_active = get_printer_state().print_state().get_print_active_subject();
    // A gate subject notifying mid-sweep reads the calibrator as busy.
    const auto notify_mid_sweep = [&] {
        fx.pump_ms(300);
        lv_subject_notify(print_active);
        fx.pump_ms(100);
        REQUIRE(fx.state_int("bt_can_start") == 0);
    };

    SECTION("a full check") {
        REQUIRE(fx.wait_gate_open());
        fx.panel().handle_start_clicked();
        notify_mid_sweep();
        REQUIRE(fx.pump_until_state(RESULTS));
        CHECK(fx.state_int("bt_can_start") == 1);
    }
    SECTION("a tuning round") {
        enter_adjust(fx);
        fx.panel().handle_tune_measure_clicked();
        notify_mid_sweep();
        for (uint32_t t = 0; t < 60000 && fx.state_int("bt_tune_phase") == PHASE_SWEEPING;
             t += 100) {
            fx.pump_ms(100);
        }
        REQUIRE(fx.state_int("bt_tune_phase") == PHASE_ADJUST);
        CHECK(fx.state_int("bt_can_start") == 1);
    }
}

TEST_CASE("Next zone names how long its sweep takes", "[belt][panel][tune]") {
    ReplayBelts belts({60.0f, 100.0f}, {56.0f, 94.0f});
    BeltPanelFixture fx;
    enter_tuning(fx);
    belts.set_b({58.0f, 98.0f});
    calibrate(fx);
    belts.set_b({58.0f, 100.0f});
    tune_round(fx);
    REQUIRE(fx.state_int("bt_tune_has_next_zone") == 1);

    // The margins past the zone count too.
    const auto minutes = [](helix::calibration::BeltSweepWindow zone, int pairs) {
        return sweep_minutes(helix::calibration::zone_sweep_window(zone, 5.0f, 135.0f), pairs);
    };
    const auto next = fx.panel_session()->peek_next_zone();
    REQUIRE(next.has_value());
    // The 56/58 zone and its margins: about 50 Hz, swept four times.
    CHECK(fx.text("bt_tune_next_zone_label") ==
          "Next Zone (~" + std::to_string(minutes(next->window, 2)) + " min)");

    fx.panel().handle_tune_next_zone_clicked();
    for (uint32_t t = 0; t < 120000 && fx.state_int("bt_tune_phase") == PHASE_SWEEPING; t += 100) {
        fx.pump_ms(100);
    }
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_ADJUST);
    // After the baseline, a round sweeps the pair once.
    const auto zone = fx.panel_session()->zone();
    REQUIRE(zone.has_value());
    CHECK(fx.text("bt_tune_measure_label") ==
          "Re-measure (~" + std::to_string(minutes(zone->window, 1)) + " min)");
    // No third zone: the label empties with the button.
    CHECK(fx.state_int("bt_tune_has_next_zone") == 0);
    CHECK(fx.text("bt_tune_next_zone_label").empty());
}

TEST_CASE("every locale's tuning strings fit their buffers", "[belt][panel][tune][i18n]") {
    BeltPanelFixture fx;
    constexpr int MAX_LISTED = 3;
    // The longest each line can get: three-digit peaks, two-digit counts,
    // turn totals past one.
    for (const char* lang : {"en", "de", "es", "fr", "it", "ja", "pt", "ru", "zh"}) {
        INFO("locale " << lang);
        if (std::string(lang) != "en") {
            helix::ui::ensure_translation_loaded(lang);
            lv_translation_set_language(lang);
            REQUIRE(std::string(lv_tr("Next Zone")) != "Next Zone");
            REQUIRE(std::string(lv_tr("Belts already match")) != "Belts already match");
        }
        const auto f = [](const char* tag, auto... args) {
            return fmt::format(fmt::runtime(lv_tr(tag)), args...);
        };
        std::string pairs;
        for (int i = 0; i < MAX_LISTED; ++i) {
            pairs += std::string(i == 0 ? "" : " · ") + "133/132";
        }
        const std::string turns = f("{} turns", "10¾");
        std::string facts = f("Peaks {} Hz", pairs) + f(" · Tuned in {} rounds", 99);
        for (const char* per_belt : {" · {} tightened about {}", " · {} loosened about {}"}) {
            facts += f(per_belt, 'A', turns);
        }
        CHECK(facts.size() < fx.facts_buf_size());

        for (const char* instruction :
             {"Release {}'s tensioner, let the spring settle, lock it", "Tighten {}", "Loosen {}",
              "Tighten {0} one full turn", "Tighten {0} noticeably"}) {
            INFO(instruction);
            CHECK(f(instruction, 'A').size() < fx.instruction_buf_size());
        }
        for (const char* instruction : {"Tighten {0} {1}", "Loosen {0} {1}"}) {
            CHECK(f(instruction, 'A', turns).size() < fx.instruction_buf_size());
        }
        for (const char* instruction : {"Belts already match", "Nothing to tune here",
                                        "Readings don't repeat", "Measure this zone first"}) {
            INFO(instruction);
            CHECK(std::string(lv_tr(instruction)).size() < fx.instruction_buf_size());
        }
        CHECK(f("Matched at {:.1f} Hz", 133.3).size() < fx.instruction_buf_size());
        CHECK(f("Confirm (full, ~{} min)", 99).size() < fx.confirm_label_buf_size());
        CHECK(f("Next Zone (~{} min)", 99).size() < fx.next_zone_label_buf_size());
        CHECK(f("Measure (~{} min)", 99).size() < fx.measure_label_buf_size());
        CHECK(f("Re-measure (~{} min)", 99).size() < fx.measure_label_buf_size());
        for (const char* last :
             {"The gap grew after tightening {0}. Check you turned Path {0}'s tensioner.",
              "The gap grew after loosening {0}. Check you turned Path {0}'s tensioner.",
              "That turn didn't register yet. Tighten {0} one more full turn.",
              "That turn didn't register yet. Tighten {0} a little more.",
              "Nothing followed the turns on {0}. Undo them, then check the mechanics."}) {
            INFO(last);
            CHECK(f(last, 'A').size() < fx.last_buf_size());
        }
        CHECK(f("Nothing followed {1} turns of {0}. Turn {0} back {1} turns, then check the "
                "mechanics.",
                'A', 10)
                  .size() < fx.last_buf_size());
        CHECK(f("Within {0:.1f} Hz of each other at {1:.0f} Hz", 1.5, 133.0).size() <
              fx.last_buf_size());
        CHECK(std::string(lv_tr("That reading jumped too far to trust. Re-measure without "
                                "turning."))
                  .size() < fx.last_buf_size());
        CHECK(f("No turn: the gap moved {:.1f} Hz", 12.3).size() < fx.last_buf_size());
        CHECK(f("Path {0}'s peak left the zone. Check that only Path {1}'s tensioner was turned.",
                'A', 'B')
                  .size() < fx.last_buf_size());
        for (const char* hint :
             {"These readings aren't repeatable, so tuning can't be steered. Check the mechanics "
              "first.",
              "Curves differ in shape. Check the mechanics before chasing a perfect match.",
              "The gap is not closing. Something besides tension may be shaping it."}) {
            INFO(hint);
            CHECK(std::string(lv_tr(hint)).size() < fx.hint_buf_size());
        }
        CHECK(f("Path {0} resonates much more than {1} at {2:.0f} Hz. That points at a "
                "mechanical difference (pulley grub screws, idler bearings, belt tracking), not "
                "tension.",
                'A', 'B', 133.0)
                  .size() < fx.power_hint_buf_size());
    }
    lv_translation_set_language(helix::ui::kIdentityLocale);
}

// ============================================================================
// Compact layouts: below medium, or portrait
// ============================================================================

namespace {

/// Pins one int layout subject (ui_breakpoint, ui_is_portrait) for a case and
/// puts it back after.
class ScopedLayoutSubject {
  public:
    ScopedLayoutSubject(const char* name, int value) : subject_(lv_xml_get_subject(nullptr, name)) {
        INFO("layout subject not registered: " << name);
        REQUIRE(subject_ != nullptr);
        original_ = lv_subject_get_int(subject_);
        lv_subject_set_int(subject_, value);
    }
    ~ScopedLayoutSubject() {
        lv_subject_set_int(subject_, original_);
    }
    void set(int value) {
        lv_subject_set_int(subject_, value);
    }

  private:
    lv_subject_t* subject_;
    int original_ = 0;
};

constexpr int BP_MICRO = 0;
constexpr int BP_SMALL = 2;
constexpr int BP_MEDIUM = 3;

lv_obj_t* named_in(BeltPanelFixture& fx, const char* name) {
    lv_obj_t* obj = lv_obj_find_by_name(fx.view(), name);
    INFO("missing: " << name);
    REQUIRE(obj != nullptr);
    return obj;
}

std::vector<std::string> visible_of(BeltPanelFixture& fx,
                                    std::initializer_list<const char*> names) {
    std::vector<std::string> out;
    for (const char* name : names) {
        if (!lv_hidden(named_in(fx, name))) {
            out.emplace_back(name);
        }
    }
    return out;
}

std::vector<std::string> visible_results_buttons(BeltPanelFixture& fx) {
    return visible_of(
        fx, {"btn_retest_a", "btn_retest_b", "btn_retest_menu", "btn_test_both", "btn_tune"});
}

const std::vector<std::string> FOLDED{"btn_retest_menu", "btn_test_both", "btn_tune"};
const std::vector<std::string> UNFOLDED{"btn_retest_a", "btn_retest_b", "btn_test_both",
                                        "btn_tune"};

/// Opens the Re-test menu, returning the row named `row` inside it.
lv_obj_t* open_retest_menu(BeltPanelFixture& fx, const char* row) {
    lv_obj_send_event(named_in(fx, "btn_retest_menu"), LV_EVENT_CLICKED, nullptr);
    fx.pump_ms(100);
    REQUIRE(helix::ui::ContextMenu::active() != nullptr);
    lv_obj_t* item = lv_obj_find_by_name(lv_obj_get_screen(fx.view()), row);
    INFO("menu row: " << row);
    REQUIRE(item != nullptr);
    return item;
}

/// Opens the Re-test menu and taps one of its rows.
void pick_retest(BeltPanelFixture& fx, const char* row) {
    lv_obj_send_event(open_retest_menu(fx, row), LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    CHECK(helix::ui::ContextMenu::active() == nullptr);
}

void enter_results_with_menu(BeltPanelFixture& fx) {
    helix::ui::ContextMenu::register_shared_callbacks();
    REQUIRE(fx.register_component("belt_retest_menu"));
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(RESULTS));
}

} // namespace

TEST_CASE("RESULTS folds Re-test A/B into one menu below medium and in portrait",
          "[belt][panel][compact]") {
    BeltPanelFixture fx;
    ScopedLayoutSubject bp("ui_breakpoint", BP_MICRO);
    ScopedLayoutSubject portrait("ui_is_portrait", 0);
    enter_results_with_menu(fx);

    CHECK(visible_results_buttons(fx) == FOLDED);

    SECTION("every breakpoint below medium folds them") {
        for (int narrow : {1, BP_SMALL}) {
            bp.set(narrow);
            INFO("ui_breakpoint " << narrow);
            CHECK(visible_results_buttons(fx) == FOLDED);
        }
    }
    SECTION("landscape medium keeps the two Re-test buttons") {
        bp.set(BP_MEDIUM);
        CHECK(visible_results_buttons(fx) == UNFOLDED);
    }
    SECTION("portrait medium folds them") {
        bp.set(BP_MEDIUM);
        portrait.set(1);
        CHECK(visible_results_buttons(fx) == FOLDED);
    }
    SECTION("Re-test A from the menu re-measures A") {
        pick_retest(fx, "btn_retest_path_a");
        CHECK(fx.state_int("belt_tension_state") ==
              static_cast<int>(BeltTensionPanel::ViewState::RUNNING));
        CHECK(fx.pump_until_text("bt_run_title", "Re-measuring Path A"));
    }
    SECTION("Re-test B from the menu re-measures B") {
        pick_retest(fx, "btn_retest_path_b");
        CHECK(fx.state_int("belt_tension_state") ==
              static_cast<int>(BeltTensionPanel::ViewState::RUNNING));
        CHECK(fx.pump_until_text("bt_run_title", "Re-measuring Path B"));
    }
}

TEST_CASE("leaving the panel closes the Re-test menu so it cannot start a sweep",
          "[belt][panel][compact]") {
    BeltPanelFixture fx;
    ScopedLayoutSubject bp("ui_breakpoint", BP_MICRO);
    enter_results_with_menu(fx);

    lv_obj_t* row = open_retest_menu(fx, "btn_retest_path_a");
    fx.panel().on_deactivate(DeactivateReason::NavigateAway);
    UpdateQueue::instance().drain();
    fx.pump_ms(100);
    CHECK(helix::ui::ContextMenu::active() == nullptr);
    CHECK(lv_obj_find_by_name(lv_obj_get_screen(fx.view()), "btn_retest_path_a") == nullptr);

    // A tap that was already in flight on the row: nothing may start.
    fx.mock().clear_gcode_script_history();
    if (lv_obj_is_valid(row)) {
        lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
    }
    fx.pump_ms(2000);
    CHECK(resonance_lines(fx).empty());
    CHECK(fx.state_int("belt_tension_state") !=
          static_cast<int>(BeltTensionPanel::ViewState::RUNNING));
}

TEST_CASE("TUNING below medium is one tappable line over the chart",
          "[belt][panel][compact][tune]") {
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    ScopedLayoutSubject bp("ui_breakpoint", BP_MICRO);
    ScopedLayoutSubject portrait("ui_is_portrait", 0);
    enter_tuning(fx);
    REQUIRE(fx.state_int("bt_tune_kind") == 1);

    auto named = [&](const char* name) { return named_in(fx, name); };
    // The calibration turn records nothing: no stepper, the short button measures.
    CHECK(lv_hidden(named("bt_tune_row_stepper")));
    CHECK_FALSE(lv_hidden(named("btn_tune_measure_short")));
    CHECK(fx.text("bt_tune_measure_short_label") == "Measure");
    calibrate(fx);
    CHECK(fx.text("bt_tune_measure_short_label") == "Re-measure");

    CHECK(lv_hidden(named("bt_tune_hero")));
    CHECK_FALSE(lv_hidden(named("bt_tune_line")));
    // The stepper and the short Re-measure share the button row; the swap
    // button gives its place to the tappable line.
    CHECK_FALSE(lv_hidden(named("bt_tune_row_stepper")));
    CHECK_FALSE(lv_hidden(named("btn_tune_measure_short")));
    CHECK(lv_hidden(named("btn_tune_measure")));
    CHECK(lv_hidden(named("btn_tune_swap")));

    const std::string amount = fx.text("bt_tune_amount");
    lv_obj_send_event(named("btn_tune_row_more"), LV_EVENT_CLICKED, nullptr);
    CHECK(fx.text("bt_tune_amount") != amount);
    lv_obj_send_event(named("btn_tune_row_less"), LV_EVENT_CLICKED, nullptr);
    CHECK(fx.text("bt_tune_amount") == amount);

    REQUIRE(fx.text("bt_tune_instruction").rfind("Tighten B ", 0) == 0);
    lv_obj_send_event(named("bt_tune_line"), LV_EVENT_CLICKED, nullptr);
    CHECK(fx.state_int("bt_tune_path") == 0);
    CHECK(fx.text("bt_tune_swap_label") == "Adjust B instead");

    SECTION("landscape medium keeps the full hero") {
        bp.set(BP_MEDIUM);
        CHECK_FALSE(lv_hidden(named("bt_tune_hero")));
        CHECK(lv_hidden(named("bt_tune_line")));
        CHECK(lv_hidden(named("bt_tune_row_stepper")));
        CHECK_FALSE(lv_hidden(named("btn_tune_swap")));
    }
    SECTION("micro portrait uses the one-line hero") {
        portrait.set(1);
        CHECK(lv_hidden(named("bt_tune_hero")));
        CHECK_FALSE(lv_hidden(named("bt_tune_line")));
        CHECK_FALSE(lv_hidden(named("bt_tune_row_stepper")));
        CHECK(lv_hidden(named("btn_tune_swap")));
    }
}

TEST_CASE("the one-line hero's gauge shows the round's gap", "[belt][panel][compact][tune]") {
    BeltPanelFixture fx;
    ScopedLayoutSubject bp("ui_breakpoint", BP_MICRO);
    enter_tuning(fx);
    tune_round(fx);
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_ADJUST);

    lv_obj_t* line_gauge = named_in(fx, "bt_tune_line_gauge");
    CHECK_FALSE(lv_hidden(line_gauge));
    const float hero_gap = helix::ui::ui_belt_gap_gauge_get_gap(named_in(fx, "bt_tune_gauge"));
    INFO("hero gauge gap " << hero_gap);
    REQUIRE(std::abs(hero_gap) > 1.0f);
    CHECK(helix::ui::ui_belt_gap_gauge_get_gap(line_gauge) == Catch::Approx(hero_gap));
}

TEST_CASE("Confirm drops its duration on compact layouts and beside Next zone",
          "[belt][panel][compact][tune]") {
    BeltPanelFixture fx;
    // The calibration sweep finds B at A's peak: its baseline matches.
    fx.mock().set_belt_tune_script('B', {110.0f});
    ScopedLayoutSubject bp("ui_breakpoint", BP_MEDIUM);
    ScopedLayoutSubject portrait("ui_is_portrait", 0);
    enter_tuning(fx);
    tune_round(fx);
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_MATCHED);
    lv_subject_t* next_zone = lv_xml_get_subject(nullptr, "bt_tune_has_next_zone");
    REQUIRE(next_zone != nullptr);
    const auto confirms = [&] {
        return visible_of(fx, {"btn_tune_confirm", "btn_tune_confirm_short"});
    };
    const std::vector<std::string> full{"btn_tune_confirm"};
    const std::vector<std::string> shortened{"btn_tune_confirm_short"};

    lv_subject_set_int(next_zone, 0);
    CHECK(confirms() == full);
    CHECK(lv_hidden(named_in(fx, "btn_tune_next_zone")));

    SECTION("Next zone beside it at medium") {
        lv_subject_set_int(next_zone, 1);
        CHECK_FALSE(lv_hidden(named_in(fx, "btn_tune_next_zone")));
        CHECK(confirms() == shortened);
    }
    SECTION("micro, no Next zone") {
        bp.set(BP_MICRO);
        CHECK(confirms() == shortened);
    }
    SECTION("portrait medium, no Next zone") {
        portrait.set(1);
        CHECK(confirms() == shortened);
    }
}

TEST_CASE("the tuning view hosts the chart", "[belt][panel][tune][chart]") {
    BeltPanelFixture fx;
    enter_tuning(fx);

    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    lv_obj_t* host = lv_obj_find_by_name(fx.view(), "chart_host_tuning");
    REQUIRE(host != nullptr);
    CHECK(fx.panel_chart_host_tuning() == host);
    CHECK(lv_obj_get_parent(ui_frequency_response_chart_get_obj(chart)) == host);
}

TEST_CASE("destroying the UI drops every chart host and the tuning ghosts",
          "[belt][panel][tune][chart]") {
    BeltPanelFixture fx;
    enter_tuning(fx);
    REQUIRE(fx.panel_chart_host_tuning() != nullptr);
    REQUIRE(fx.panel_tune_ghost_id(0, 0) >= 0);
    REQUIRE(fx.panel_tune_ghost_id(1, 1) >= 0);

    fx.panel().on_ui_destroyed();
    CHECK(fx.panel_chart() == nullptr);
    CHECK(fx.panel_chart_host_tuning() == nullptr);
    for (int path = 0; path < 2; ++path) {
        for (int k = 0; k < 2; ++k) {
            CHECK(fx.panel_tune_ghost_id(path, k) == -1);
        }
    }
}

TEST_CASE("leaving the panel ends the tuning session", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    fx.mock().set_belt_tune_script('B', {110.0f});
    enter_tuning(fx);
    tune_round(fx);
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_MATCHED);

    fx.panel().on_deactivate(DeactivateReason::NavigateAway);
    UpdateQueue::instance().drain();
    CHECK(fx.state_int("bt_tune_phase") == PHASE_ADJUST);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::START));
}

TEST_CASE("a fresh Start ends the tuning session", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    fx.mock().set_belt_tune_script('B', {110.0f});
    enter_tuning(fx);
    tune_round(fx);
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_MATCHED);

    fx.panel().handle_start_clicked();
    CHECK(fx.state_int("bt_tune_phase") == PHASE_ADJUST);
    REQUIRE(fx.pump_until_state(RESULTS));
    CHECK(fx.state_int("bt_tune_phase") == PHASE_ADJUST);
}

TEST_CASE("a tuning round keeps the peak markers on screen while it sweeps",
          "[belt][panel][tune][chart]") {
    BeltPanelFixture fx;
    enter_tuning(fx);
    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    for (int path = 0; path < 2; ++path) {
        REQUIRE(ui_frequency_response_chart_get_markers(chart, fx.panel_series_id(path)).size() ==
                1);
    }

    fx.panel().handle_tune_measure_clicked();
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_SWEEPING);
    for (int path = 0; path < 2; ++path) {
        CHECK(ui_frequency_response_chart_get_markers(chart, fx.panel_series_id(path)).size() == 1);
    }
}

TEST_CASE("a gate that closes during a tuning sweep ends it in ERROR",
          "[belt][panel][tune][gating]") {
    BeltPanelFixture fx;
    enter_tuning(fx);
    fx.panel().handle_tune_measure_clicked();
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_SWEEPING);

    lv_subject_set_int(get_printer_state().print_state().get_print_active_subject(), 1);
    fx.pump_ms(200);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::ERROR));
}

TEST_CASE("a theme toggle recolors the tuning ghosts", "[belt][panel][tune][chart]") {
    BeltPanelFixture fx;
    enter_tuning(fx);
    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    const auto rgb = [chart](int id) {
        return lv_color_to_u32(ui_frequency_response_chart_get_series_color(chart, id)) & 0xFFFFFF;
    };
    const uint32_t before = rgb(fx.panel_tune_ghost_id(0, 0));

    theme_manager_toggle_dark_mode();
    UpdateQueue::instance().drain();
    const uint32_t want_a = lv_color_to_u32(theme_manager_get_color("belt_path_a")) & 0xFFFFFF;
    const uint32_t want_b = lv_color_to_u32(theme_manager_get_color("belt_path_b")) & 0xFFFFFF;
    std::vector<uint32_t> after;
    for (int path = 0; path < 2; ++path) {
        for (int k = 0; k < 2; ++k) {
            after.push_back(rgb(fx.panel_tune_ghost_id(path, k)));
        }
    }
    theme_manager_toggle_dark_mode();
    UpdateQueue::instance().drain();

    REQUIRE(want_a != before);
    CHECK(after[0] == want_a);
    CHECK(after[1] == want_a);
    CHECK(after[2] == want_b);
    CHECK(after[3] == want_b);
}

TEST_CASE("a closed gate stops a Re-test", "[belt][panel][gating]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(RESULTS));

    lv_subject_set_int(get_printer_state().print_state().get_print_active_subject(), 1);
    fx.pump_ms(200);
    REQUIRE(fx.state_int("bt_can_start") == 0);

    fx.mock().clear_gcode_script_history();
    fx.panel().handle_retest_clicked(helix::calibration::BeltPath::PATH_A);
    fx.pump_ms(2000);
    CHECK(resonance_lines(fx).empty());
    CHECK(fx.state_int("belt_tension_state") == RESULTS);
}

TEST_CASE("cleanup closes the Re-test menu", "[belt][panel][compact]") {
    BeltPanelFixture fx;
    ScopedLayoutSubject bp("ui_breakpoint", BP_MICRO);
    enter_results_with_menu(fx);
    open_retest_menu(fx, "btn_retest_path_a");

    fx.panel().cleanup();
    UpdateQueue::instance().drain();
    CHECK(helix::ui::ContextMenu::active() == nullptr);
}

// ============================================================================
// Ultrawide: the chart grows on the left, the rest sits in a sidebar
// ============================================================================

TEST_CASE("TUNING on ultrawide puts the chart beside a sidebar with the full hero",
          "[belt][panel][ultrawide][tune]") {
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    // 1920x440 reads small, which alone would pick the one-line hero.
    ScopedLayoutSubject bp("ui_breakpoint", BP_SMALL);
    ScopedLayoutSubject portrait("ui_is_portrait", 0);
    ScopedLayoutSubject ultrawide("ui_is_ultrawide", 1);
    enter_tuning(fx);
    REQUIRE(fx.state_int("bt_tune_kind") == 1);

    auto named = [&](const char* name) { return named_in(fx, name); };
    CHECK_FALSE(lv_hidden(named("chart_host_wide")));
    CHECK(lv_hidden(named("chart_host_tuning")));
    CHECK_FALSE(lv_hidden(named("bt_tune_hero")));
    CHECK(lv_hidden(named("bt_tune_line")));
    CHECK(lv_hidden(named("bt_tune_line_gauge")));
    CHECK(lv_hidden(named("bt_tune_row_stepper")));
    CHECK_FALSE(lv_hidden(named("btn_tune_swap")));
    CHECK_FALSE(lv_hidden(named("btn_tune_measure")));
    CHECK(lv_hidden(named("btn_tune_measure_short")));

    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    lv_obj_t* chart_obj = ui_frequency_response_chart_get_obj(chart);
    CHECK(lv_obj_get_parent(chart_obj) == named("chart_host_wide"));

    // The sidebar keeps the token's width instead of stretching.
    lv_obj_update_layout(fx.view());
    const char* side_w = lv_xml_get_const(nullptr, "belt_sidebar_w");
    REQUIRE(side_w != nullptr);
    CHECK(lv_obj_get_width(named("bt_main")) == std::atoi(side_w));

    SECTION("leaving ultrawide hands the chart back to the state's own host") {
        ultrawide.set(0);
        UpdateQueue::instance().drain();
        fx.pump_ms(100);
        CHECK(lv_obj_get_parent(chart_obj) == named("chart_host_tuning"));
        CHECK(lv_hidden(named("chart_host_wide")));
        CHECK(lv_hidden(named("bt_tune_hero")));
        CHECK_FALSE(lv_hidden(named("bt_tune_line")));
    }
}

TEST_CASE("RESULTS on ultrawide keeps both Re-test buttons in the sidebar",
          "[belt][panel][ultrawide]") {
    BeltPanelFixture fx;
    ScopedLayoutSubject bp("ui_breakpoint", BP_SMALL);
    ScopedLayoutSubject portrait("ui_is_portrait", 0);
    ScopedLayoutSubject ultrawide("ui_is_ultrawide", 1);
    enter_results_with_menu(fx);

    CHECK(visible_results_buttons(fx) == UNFOLDED);
    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    CHECK(lv_obj_get_parent(ui_frequency_response_chart_get_obj(chart)) ==
          named_in(fx, "chart_host_wide"));
    CHECK(lv_hidden(named_in(fx, "chart_host_results")));

    SECTION("START has no chart, so the panel takes the full width") {
        fx.panel_set_view_state(BeltTensionPanel::ViewState::START);
        CHECK(lv_hidden(named_in(fx, "chart_host_wide")));
        lv_obj_update_layout(fx.view());
        CHECK(lv_obj_get_width(named_in(fx, "bt_main")) ==
              lv_obj_get_content_width(named_in(fx, "overlay_content")));
    }
}

// ============================================================================
// Portrait: width is scarce, height is spare
// ============================================================================

namespace {

int32_t chart_max_h_token() {
    const char* v = lv_xml_get_const(nullptr, "belt_chart_max_h");
    REQUIRE(v != nullptr);
    return std::atoi(v);
}

} // namespace

TEST_CASE("portrait gives the primary action its own line and caps the chart",
          "[belt][panel][portrait]") {
    BeltPanelFixture fx;
    ScopedLayoutSubject bp("ui_breakpoint", BP_MEDIUM);
    ScopedLayoutSubject portrait("ui_is_portrait", 1);
    ScopedLayoutSubject ultrawide("ui_is_ultrawide", 0);
    enter_results_with_menu(fx);

    lv_obj_update_layout(fx.view());
    lv_obj_t* tune = named_in(fx, "btn_tune");
    lv_obj_t* both = named_in(fx, "btn_test_both");
    CHECK(lv_obj_get_y(tune) > lv_obj_get_y(both));
    CHECK(lv_obj_get_width(tune) > lv_obj_get_width(both));
    CHECK(lv_obj_get_style_max_height(named_in(fx, "chart_host_results"), LV_PART_MAIN) ==
          chart_max_h_token());

    SECTION("landscape keeps one row and an uncapped chart") {
        portrait.set(0);
        lv_obj_update_layout(fx.view());
        CHECK(lv_obj_get_y(tune) == lv_obj_get_y(both));
        CHECK(lv_obj_get_style_max_height(named_in(fx, "chart_host_results"), LV_PART_MAIN) !=
              chart_max_h_token());
    }
}

TEST_CASE("portrait START stacks the sketch over the text", "[belt][panel][portrait]") {
    BeltPanelFixture fx;
    ScopedLayoutSubject bp("ui_breakpoint", BP_MEDIUM);
    ScopedLayoutSubject portrait("ui_is_portrait", 1);
    ScopedLayoutSubject ultrawide("ui_is_ultrawide", 0);
    REQUIRE(fx.state_int("belt_tension_state") ==
            static_cast<int>(BeltTensionPanel::ViewState::START));

    lv_obj_update_layout(fx.view());
    lv_obj_t* sketch = named_in(fx, "bt_sketch");
    lv_obj_t* text = named_in(fx, "bt_start_text");
    CHECK(lv_obj_get_y(text) > lv_obj_get_y(sketch));
    CHECK(lv_obj_get_x(text) == lv_obj_get_x(sketch));

    SECTION("landscape puts them side by side") {
        portrait.set(0);
        lv_obj_update_layout(fx.view());
        CHECK(lv_obj_get_x(text) > lv_obj_get_x(sketch));
    }
}

TEST_CASE("portrait shows the full tuning hero, stacked", "[belt][panel][portrait][tune]") {
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    ScopedLayoutSubject bp("ui_breakpoint", BP_MEDIUM);
    ScopedLayoutSubject portrait("ui_is_portrait", 1);
    ScopedLayoutSubject ultrawide("ui_is_ultrawide", 0);
    enter_adjust(fx);
    REQUIRE(fx.state_int("bt_tune_kind") == 1);

    auto named = [&](const char* name) { return named_in(fx, name); };
    CHECK_FALSE(lv_hidden(named("bt_tune_hero")));
    CHECK(lv_hidden(named("bt_tune_line")));
    CHECK(lv_hidden(named("bt_tune_line_gauge")));
    CHECK(lv_hidden(named("bt_tune_row_stepper")));
    CHECK_FALSE(lv_hidden(named("bt_tune_stepper")));
    // The tappable line is gone, so the swap is a button again.
    CHECK_FALSE(lv_hidden(named("btn_tune_swap")));
    CHECK_FALSE(lv_hidden(named("btn_tune_measure_short")));

    lv_obj_update_layout(fx.view());
    lv_obj_t* left = named("bt_tune_left");
    lv_obj_t* centre = named("bt_tune_centre");
    lv_obj_t* right = named("bt_tune_right");
    CHECK(lv_obj_get_y(centre) == lv_obj_get_y(left));
    CHECK(lv_obj_get_y(right) > lv_obj_get_y(left));
    CHECK(lv_obj_get_y(named("btn_tune_measure_short")) > lv_obj_get_y(named("btn_tune_done")));

    // The warning wraps instead of truncating.
    CHECK(lv_hidden(named("bt_tune_hint")));
    CHECK_FALSE(lv_hidden(named("bt_tune_hint_wrap")));

    SECTION("micro portrait keeps the one-line hero") {
        bp.set(BP_MICRO);
        CHECK(lv_hidden(named("bt_tune_hero")));
        CHECK_FALSE(lv_hidden(named("bt_tune_line")));
        CHECK(lv_hidden(named("btn_tune_swap")));
    }
    SECTION("landscape keeps the one-line warning") {
        portrait.set(0);
        CHECK_FALSE(lv_hidden(named("bt_tune_hint")));
        CHECK(lv_hidden(named("bt_tune_hint_wrap")));
    }
}

// ============================================================================
// Settling laps
// ============================================================================

TEST_CASE("the run title reads Settling belts while the laps run", "[belt][panel][settle]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());

    SECTION("a full check") {
        fx.panel().handle_start_clicked();
        REQUIRE(fx.state_int("belt_tension_state") ==
                static_cast<int>(BeltTensionPanel::ViewState::RUNNING));
        CHECK(fx.text("bt_run_title") == "Settling belts");
        CHECK(fx.state_int("bt_running_path") == 2); // neither path is sweeping
        CHECK(fx.text("bt_note_a") != "sweeping");
        CHECK(fx.text("bt_run_detail").find("elapsed") != std::string::npos);
        CHECK(fx.pump_until_text("bt_run_title", "Measuring Path A"));
        CHECK(fx.state_int("bt_running_path") == 0);
    }
    SECTION("a tuning round, in the sweeping row") {
        fx.panel().handle_start_clicked();
        REQUIRE(fx.pump_until_state(RESULTS));
        fx.panel().handle_tune_clicked();
        REQUIRE(fx.state_int("belt_tension_state") == TUNING);
        fx.panel().handle_tune_measure_clicked();
        REQUIRE(fx.state_int("bt_tune_phase") == PHASE_SWEEPING);
        lv_obj_t* title = lv_obj_find_by_name(fx.view(), "tune_run_title");
        REQUIRE(title != nullptr);
        CHECK(std::string(lv_label_get_text(title)) == "Settling belts");
        REQUIRE(fx.pump_until_text("bt_run_title", "Measuring Path A"));
        CHECK(std::string(lv_label_get_text(title)) == "Measuring Path A");
    }
}

TEST_CASE("Re-test settles once before its single sweep", "[belt][panel][settle]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(RESULTS));

    fx.mock().clear_gcode_script_history();
    fx.panel().handle_retest_clicked(helix::calibration::BeltPath::PATH_B);
    REQUIRE(fx.pump_until_state(RESULTS));
    CHECK(run_steps(fx) == std::vector<std::string>{"settle", "full"});
}

TEST_CASE("Confirm settles once before its pair", "[belt][panel][settle][tune]") {
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    fx.mock().set_belt_peaks_hz(120.0f, 96.0f);
    fx.mock().set_belt_tune_script('B', {101.0f, 101.0f, 101.0f, 106.0f});
    enter_adjust(fx);

    SECTION("a tuning round settles before both sweeps") {
        fx.mock().clear_gcode_script_history();
        step_as_advised(fx);
        tune_round(fx);
        CHECK(run_steps(fx) == std::vector<std::string>{"settle", "narrow", "narrow"});
    }
    SECTION("Confirm") {
        fx.mock().clear_gcode_script_history();
        fx.panel().handle_tune_confirm_clicked();
        REQUIRE(fx.pump_until_state(RESULTS));
        CHECK(run_steps(fx) == std::vector<std::string>{"settle", "full", "full"});
    }
}

TEST_CASE("a refused settle lands on ERROR and sends no sweep", "[belt][panel][settle]") {
    BeltPanelFixture fx;
    fx.mock().set_resonance_probe_points(false);
    REQUIRE(fx.wait_gate_open());
    fx.mock().clear_gcode_script_history();

    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::ERROR)));
    CHECK(fx.text("bt_error_message") ==
          "[resonance_tester] has no probe_points, so the toolhead has no safe height for the "
          "settling laps.");
    CHECK(fx.mock().gcode_script_history().empty());
}

TEST_CASE("a settle the printer rejects lands on ERROR", "[belt][panel][settle]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.mock().force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR, "Move out of range",
                                     SETTLE_START);
    fx.mock().clear_gcode_script_history();

    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::ERROR)));
    CHECK(fx.text("bt_error_message").find("Move out of range") != std::string::npos);
    CHECK(run_steps(fx) == std::vector<std::string>{"settle"});
}

TEST_CASE("Stop during the laps lands on START", "[belt][panel][settle]") {
    BeltPanelFixture fx;
    helix::ui::modal_init_subjects();
    REQUIRE(fx.register_component("modal_dialog"));
    REQUIRE(fx.wait_gate_open());
    // The laps' ack never arrives, so the run stays in its laps.
    fx.mock().force_next_gcode_dropped_response(SETTLE_START);
    fx.mock().clear_gcode_script_history();

    fx.panel().handle_start_clicked();
    fx.pump_ms(500);
    REQUIRE(run_steps(fx) == std::vector<std::string>{"settle"});
    REQUIRE(fx.text("bt_run_title") == "Settling belts");

    const int stops_before = fx.mock().call_count("printer.emergency_stop");
    fx.panel().handle_stop_clicked();
    fx.pump_ms(200);
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_send_event(lv_obj_find_by_name(dialog, "btn_primary"), LV_EVENT_CLICKED, nullptr);
    fx.pump_ms(200);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::START));
    CHECK(fx.mock().call_count("printer.emergency_stop") == stops_before + 1);
    fx.pump_ms(5000);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::START));
    CHECK(run_steps(fx) == std::vector<std::string>{"settle"});
}

TEST_CASE("a rebuild during the laps drops the late ack", "[belt][panel][settle]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.mock().clear_gcode_script_history();

    fx.panel().handle_start_clicked();
    // One drain runs the probe-point query's answer, which sends the laps; their
    // ack is still queued.
    UpdateQueue::instance().drain();
    REQUIRE(run_steps(fx) == std::vector<std::string>{"settle"});

    REQUIRE(fx.panel().rebuild());
    UpdateQueue::instance().drain();
    fx.pump_ms(5000);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::START));
    CHECK(run_steps(fx) == std::vector<std::string>{"settle"});
}

TEST_CASE("the sweep fact line counts the laps, even when the toolhead reports late",
          "[belt][panel][settle]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    // At 500 mm/s the laps take seconds: two 130 s sweeps round to 5 min.
    REQUIRE(fx.text("bt_hw_sweep") == "5-135 Hz · about 5 min");

    // A toolhead frame after the [resonance_tester] answer: 10 mm/s max
    // velocity laps at 6 mm/s, minutes instead of seconds.
    fx.set_toolhead_limits(10.0);
    fx.pump_ms(200);

    const auto axis_max =
        helix::mock::descriptor(MoonrakerClientMock::PrinterType::VORON_24).axis_max;
    helix::calibration::BeltSettleInputs in;
    in.x_max = static_cast<float>(axis_max.x);
    in.y_max = static_cast<float>(axis_max.y);
    in.has_xy_limits = true;
    in.probe_point = std::array<float, 3>{in.x_max / 2.0f, in.y_max / 2.0f, 20.0f};
    in.max_velocity_mm_s = 10.0f;
    const auto plan = helix::calibration::plan_belt_settle(in);
    REQUIRE(plan.refusal == helix::calibration::BeltSettleRefusal::NONE);
    const int minutes = static_cast<int>(std::ceil((260.0f + plan.seconds) / 60.0f));
    REQUIRE(minutes != 5); // the laps change the answer
    CHECK(fx.text("bt_hw_sweep") == "5-135 Hz · about " + std::to_string(minutes) + " min");
}

TEST_CASE("a wake re-asks the probe point the suspension expired", "[belt][panel][settle]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.mock().clear_gcode_script_history();

    fx.panel().handle_start_clicked();
    // The screen sleeps before the query answers; its callback expires.
    fx.panel().on_deactivate(DeactivateReason::Suspended);
    fx.pump_ms(500);
    REQUIRE(run_steps(fx).empty());
    fx.panel().on_activate();
    REQUIRE(fx.pump_until_state(RESULTS));
    CHECK(run_steps(fx) == std::vector<std::string>{"settle", "full", "full"});
}

TEST_CASE("tearing the panel down during the laps drops the late ack", "[belt][panel][settle]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.mock().clear_gcode_script_history();

    fx.panel().handle_start_clicked();
    UpdateQueue::instance().drain(); // the query answers; the laps go out, their ack queued
    REQUIRE(run_steps(fx) == std::vector<std::string>{"settle"});

    // Destruction's order: cleanup(), then the members, the calibrator among them.
    fx.panel().cleanup();
    fx.panel().set_api(nullptr, nullptr);
    fx.pump_ms(5000);
    CHECK(run_steps(fx) == std::vector<std::string>{"settle"});
}

TEST_CASE("a tool changer's laps keep to the central half", "[belt][panel][settle]") {
    BeltPanelFixture fx;
    auto& tools = helix::ToolState::instance();
    const auto use_tools = [&tools](nlohmann::json objects) {
        helix::PrinterDiscovery hw;
        hw.parse_objects(objects);
        tools.deinit_subjects();
        tools.init_subjects(/*register_xml=*/false);
        tools.init_tools(hw);
        UpdateQueue::instance().drain();
    };
    // Later cases expect a single-tool printer, whatever happens here.
    struct RestoreOneTool {
        std::function<void(nlohmann::json)> use;
        ~RestoreOneTool() {
            use({"extruder", "heater_bed"});
        }
    } restore{use_tools};
    use_tools({"toolchanger", "tool T0", "tool T1", "tool T2", "tool T3", "extruder", "extruder1",
               "extruder2", "extruder3"});
    REQUIRE(tools.has_multiple_toolheads());
    REQUIRE(fx.wait_gate_open());
    fx.mock().clear_gcode_script_history();

    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(RESULTS));
    const auto axis_max =
        helix::mock::descriptor(MoonrakerClientMock::PrinterType::VORON_24).axis_max;
    // A quarter of the span in from each end.
    const std::string first_corner =
        fmt::format("G1 X{:.1f} Y{:.1f} F18000", axis_max.x / 4.0, axis_max.y / 4.0);
    const auto history = std::vector<std::string>(fx.mock().gcode_script_history());
    CHECK(std::find(history.begin(), history.end(), first_corner) != history.end());
}

// ============================================================================
// Notes, similarity history, ghost scale, labels
// ============================================================================

TEST_CASE("a path measured earlier in this run reads done", "[belt][panel]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_text("bt_run_title", "Measuring Path B"));
    CHECK(fx.text("bt_note_a") == "done");
    CHECK(fx.text("bt_note_b") == "sweeping");
}

TEST_CASE("ages refresh on RESULTS", "[belt][panel]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(RESULTS));
    REQUIRE(fx.text("bt_note_a") == "just now");
    lv_timer_t* timer = fx.panel_note_age_timer();
    REQUIRE(timer != nullptr);
    // The harness runs only timers with a finite repeat count: lend it one
    // firing, then make it periodic again.
    lv_tick_inc(90000);
    lv_timer_set_repeat_count(timer, 2);
    lv_timer_handler_safe();
    lv_timer_set_repeat_count(timer, -1);
    CHECK(fx.text("bt_note_a") == "1 min ago");
    CHECK(fx.text("bt_note_b") == "1 min ago");

    // Only RESULTS shows ages: leaving it stops the timer.
    fx.panel().handle_start_clicked();
    CHECK(fx.panel_note_age_timer() == nullptr);
}

TEST_CASE("was N% sits under the similarity only after a re-test", "[belt][panel]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(RESULTS));
    // A fresh check replaced nothing.
    CHECK(fx.text("bt_similarity_was").empty());
    CHECK(fx.state_int("bt_has_similarity_was") == 0);
    lv_obj_t* was = lv_obj_find_by_name(fx.view(), "bt_similarity_was");
    REQUIRE(was != nullptr);
    CHECK(lv_hidden(was));

    const std::string before = fx.text("bt_similarity");
    fx.panel().handle_retest_clicked(helix::calibration::BeltPath::PATH_B);
    REQUIRE(fx.pump_until_state(RESULTS));
    CHECK(fx.text("bt_similarity_was") == "was " + before);
    CHECK_FALSE(lv_hidden(was));

    // A fresh Start drops the history again.
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(RESULTS));
    CHECK(fx.text("bt_similarity_was").empty());
    CHECK(lv_hidden(was));
}

namespace {

/// A capture-shaped CSV with one peak at @p centre_hz, @p height tall over a
/// floor a two-hundredth of it.
void write_peak_csv(const std::string& path, double centre_hz, double height) {
    std::ofstream out(path);
    out << "freq,psd_x,psd_y,psd_z,psd_xyz\n";
    for (double freq = 5.0; freq <= 135.0; freq += 3200.0 / 4096.0) {
        const double df = freq - centre_hz;
        const double psd = height / 200.0 + height / (1.0 + df * df / 100.0);
        out << freq << ',' << psd * 0.45 << ',' << psd * 0.45 << ',' << psd * 0.1 << ',' << psd
            << '\n';
    }
}

float series_peak(ui_frequency_response_chart_t* chart, int id) {
    const auto amps = ui_frequency_response_chart_get_series_amplitudes(chart, id);
    REQUIRE_FALSE(amps.empty());
    return *std::max_element(amps.begin(), amps.end());
}

} // namespace

TEST_CASE("a ghost keeps its own scale", "[belt][panel][chart]") {
    // Path A replays a capture a tenth the height of the mock's own curves, in
    // both its sweeps, so its ghost and its current curve are both quiet.
    const std::string csv_a = "/tmp/helix-belt-quiet-" + std::to_string(getpid()) + ".csv";
    write_peak_csv(csv_a, 104.0, 3.0e3);
    helix::ScopedEnv replay("HELIX_MOCK_BELT_CSV_A", csv_a.c_str());
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(RESULTS));
    fx.panel().handle_retest_clicked(helix::calibration::BeltPath::PATH_A);
    REQUIRE(fx.pump_until_state(RESULTS));
    std::remove(csv_a.c_str());

    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    REQUIRE(ui_frequency_response_chart_is_series_visible(chart, fx.panel_ghost_id(0)));
    // The ghost on its own scale.
    CHECK(series_peak(chart, fx.panel_ghost_id(0)) == Catch::Approx(100.0f).margin(0.5f));
    // The current curves on one scale, set by the taller (B).
    const float shared = std::max(fx.panel_run_peak(0), fx.panel_run_peak(1));
    const float a_share = 100.0f * fx.panel_run_peak(0) / shared;
    REQUIRE(a_share < 50.0f);
    CHECK(series_peak(chart, fx.panel_series_id(0)) == Catch::Approx(a_share).margin(1.0f));
    CHECK(series_peak(chart, fx.panel_series_id(1)) == Catch::Approx(100.0f).margin(0.5f));
}

TEST_CASE("a tuning ghost keeps its own scale", "[belt][panel][chart][tune]") {
    // B's round-0 sweep replays a quiet capture (loud enough to name a zone);
    // the tuning round's sweep is the mock's own, about three times taller. With B adjusted, the
    // quiet round-0 curve becomes its ghost.
    const std::string csv_b = "/tmp/helix-belt-quiet-b-" + std::to_string(getpid()) + ".csv";
    write_peak_csv(csv_b, 98.0, 9.0e3);
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    {
        helix::ScopedEnv replay("HELIX_MOCK_BELT_CSV_B", csv_b.c_str());
        fx.panel().handle_start_clicked();
        REQUIRE(fx.pump_until_state(RESULTS));
    }
    std::remove(csv_b.c_str());
    fx.panel().handle_tune_clicked();
    REQUIRE(fx.state_int("belt_tension_state") == TUNING);
    INFO("phase " << fx.state_int("bt_tune_phase") << " instr " << fx.text("bt_tune_instruction")
                  << " zone " << fx.text("bt_tune_zone"));
    if (fx.state_int("bt_tune_path") != 1) {
        fx.panel().handle_tune_swap_clicked();
    }
    REQUIRE(fx.state_int("bt_tune_path") == 1);
    tune_round(fx);

    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    const int ghost = fx.panel_tune_ghost_id(1, 0);
    REQUIRE(ui_frequency_response_chart_is_series_visible(chart, ghost));
    CHECK(series_peak(chart, ghost) == Catch::Approx(100.0f).margin(0.5f));
}

namespace {

/// Every label's text under @p obj.
void collect_label_texts(lv_obj_t* obj, std::vector<std::string>& out) {
    if (lv_obj_check_type(obj, &lv_label_class)) {
        out.emplace_back(lv_label_get_text(obj));
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        collect_label_texts(lv_obj_get_child(obj, static_cast<int32_t>(i)), out);
    }
}

} // namespace

TEST_CASE("the tuning buttons read in Title Case", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    enter_tuning(fx);
    std::vector<std::string> labels;
    collect_label_texts(fx.view(), labels);
    for (const char* text : {"Good Enough", "Next Zone", "Keep Tuning"}) {
        INFO(text);
        CHECK(std::find(labels.begin(), labels.end(), text) != labels.end());
    }
    for (const char* text : {"Good enough", "Next zone", "Keep tuning", "Tune belts"}) {
        INFO(text);
        CHECK(std::find(labels.begin(), labels.end(), text) == labels.end());
    }
}

TEST_CASE("a genuine 0% earlier similarity still shows", "[belt][panel]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(RESULTS));
    fx.panel_set_similarity(0.0f);
    fx.panel().handle_retest_clicked(helix::calibration::BeltPath::PATH_B);
    REQUIRE(fx.pump_until_state(RESULTS));
    CHECK(fx.text("bt_similarity_was") == "was 0%");
    CHECK(fx.state_int("bt_has_similarity_was") == 1);
}
