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
 * 7. Tune walks the guided tuning loop: windowed rounds of both paths, advice
 *    and outcome lines from BeltTuneSession, then Confirm or Good enough.
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
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_detector.h"
#include "printer_state.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
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
    lv_obj_t* panel_low_ram_dialog() {
        return panel_->low_ram_dialog_;
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

TEST_CASE("Start runs A then B and lands on RESULTS", "[belt][panel]") {
    BeltPanelFixture fx;
    fx.mock().set_belt_peaks_hz(104.0f, 98.0f);
    REQUIRE(fx.wait_gate_open());

    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RESULTS)));

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
    CHECK(fx.text("bt_run_title") == "Re-measuring Path A");
    // A re-measure walks A toward B by at most 4 Hz: 110 -> 106, now 8 Hz from
    // B, inside the cap: the belt-hump pair outranks the 42 Hz one.
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RESULTS)));
    // "was" repeats the similarity the comparison showed before the re-measure.
    CHECK(fx.text("bt_note_a").find("was " + similarity_before) != std::string::npos);
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

/// Start, wait for RESULTS, then Tune.
void enter_tuning(BeltPanelFixture& fx) {
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(RESULTS));
    fx.panel().handle_tune_clicked();
    REQUIRE(fx.state_int("belt_tension_state") == TUNING);
}

/// One Re-measure: both windowed sweeps, then back out of SWEEPING.
void tune_round(BeltPanelFixture& fx) {
    fx.panel().handle_tune_measure_clicked();
    REQUIRE(fx.state_int("belt_tension_state") == TUNING);
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_SWEEPING);
    REQUIRE(fx.pump_until_int("bt_tune_phase", PHASE_SWEEPING, 0));
    for (uint32_t t = 0; t < 60000 && fx.state_int("bt_tune_phase") == PHASE_SWEEPING; t += 100) {
        fx.pump_ms(100);
    }
    REQUIRE(fx.state_int("bt_tune_phase") != PHASE_SWEEPING);
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

/// Parses "Zone 93-115 Hz".
std::pair<int, int> zone_range(const std::string& zone) {
    int lo = 0, hi = 0;
    if (std::sscanf(zone.c_str(), "Zone %d-%d Hz", &lo, &hi) != 2) {
        return {-1, -1};
    }
    return {lo, hi};
}

} // namespace

TEST_CASE("the Tune button names the job by verdict", "[belt][panel][tune]") {
    SECTION("Fair match: Tune belts") {
        BeltPanelFixture fx;
        fx.mock().set_belt_peaks_hz(104.0f, 98.0f);
        REQUIRE(fx.wait_gate_open());
        fx.panel().handle_start_clicked();
        REQUIRE(fx.pump_until_state(RESULTS));
        REQUIRE(fx.state_int("bt_verdict") !=
                static_cast<int>(helix::calibration::BeltVerdict::MATCHED));
        CHECK(fx.text("bt_tune_cta") == "Tune belts");
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

TEST_CASE("Tune opens on the looser belt's instruction", "[belt][panel][tune]") {
    BeltPanelFixture fx; // mock defaults: A=110, B=98, each tallest peak unpaired
    enter_tuning(fx);

    CHECK(fx.state_int("bt_tune_phase") == PHASE_ADJUST);
    CHECK(fx.text("bt_tune_instruction") == "Tighten B");
    CHECK(fx.state_int("bt_tune_path") == 1);
    CHECK(fx.state_int("bt_tune_dir") == 0);
    const auto [lo, hi] = zone_range(fx.text("bt_tune_zone"));
    INFO("zone: " << fx.text("bt_tune_zone"));
    CHECK(lo > 80);
    CHECK(lo <= 98);
    CHECK(hi >= 110);
    CHECK(hi < 130);
    CHECK(fx.state_int("bt_tune_shape_warning") == 1);
    CHECK(fx.text("bt_tune_round") == "Round 1");
    CHECK(fx.text("bt_tune_gap_caption") == "B looser than A");
    CHECK(fx.text("bt_tune_swap_label") == "Adjust A instead");
    // No round measured yet: nothing to report about the last turn.
    CHECK(fx.text("bt_tune_last").empty());

    // The chart spans the zone window, not the full band.
    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    const auto [f_lo, f_hi] = ui_frequency_response_chart_get_freq_range(chart);
    CHECK(f_lo >= static_cast<float>(lo) - 1.0f);
    CHECK(f_hi <= static_cast<float>(hi) + 1.0f);
}

TEST_CASE("three Re-measures walk CLOSER, CLOSER, MATCHED", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    fx.mock().set_belt_tune_script('B', {102.1f, 107.6f, 109.8f});
    enter_tuning(fx);

    const char* name_a = helix::calibration::BeltTensionCalibrator::output_name(
        helix::calibration::BeltPath::PATH_A);
    const char* name_b = helix::calibration::BeltTensionCalibrator::output_name(
        helix::calibration::BeltPath::PATH_B);
    const auto windowed_both = [&](const std::vector<std::string>& lines) {
        bool a = false, b = false;
        for (const auto& l : lines) {
            if (l.find("FREQ_START=") == std::string::npos ||
                l.find("FREQ_END=") == std::string::npos) {
                return false;
            }
            a = a || l.find(std::string("NAME=") + name_a) != std::string::npos;
            b = b || l.find(std::string("NAME=") + name_b) != std::string::npos;
        }
        return lines.size() == 2 && a && b;
    };

    fx.mock().clear_gcode_script_history();
    tune_round(fx);
    CHECK(windowed_both(resonance_lines(fx)));
    CHECK(fx.state_int("bt_tune_phase") == PHASE_ADJUST);
    INFO("last: " << fx.text("bt_tune_last"));
    CHECK(fx.text("bt_tune_last").rfind("last turn: ", 0) == 0);
    CHECK(fx.text("bt_tune_last").find("Hz closer") != std::string::npos);
    CHECK(fx.text("bt_tune_round") == "Round 2");
    CHECK(fx.text("bt_tune_instruction") == "Tighten B");

    fx.mock().clear_gcode_script_history();
    tune_round(fx);
    CHECK(windowed_both(resonance_lines(fx)));
    CHECK(fx.state_int("bt_tune_phase") == PHASE_ADJUST);
    CHECK(fx.text("bt_tune_last").find("Hz closer") != std::string::npos);

    fx.mock().clear_gcode_script_history();
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

    // The chart marks each path's refined peak with a hollow ring.
    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    const auto marks_b = ui_frequency_response_chart_get_markers(chart, fx.panel_series_id(1));
    REQUIRE(marks_b.size() == 1);
    CHECK(marks_b.front().number == 0);
    CHECK(marks_b.front().freq_hz == Catch::Approx(109.8f).margin(0.5f));
}

TEST_CASE("each outcome has its own last-turn line", "[belt][panel][tune]") {
    struct Case {
        float b_hz;
        const char* expect;
        const char* instruction;
    };
    for (const Case c : {
             Case{96.0f,
                  "B's peak went down after tightening. Check you turned Path B's tensioner.",
                  "Tighten B"},
             Case{113.0f, "Went past A by 3", "Loosen B"},
             Case{98.1f, "Try a bigger turn.", "Tighten B"},
         }) {
        BeltPanelFixture fx;
        fx.mock().set_belt_tune_script('B', {c.b_hz});
        enter_tuning(fx);
        tune_round(fx);
        INFO("B at " << c.b_hz << ": " << fx.text("bt_tune_last"));
        CHECK(fx.text("bt_tune_last").find(c.expect) != std::string::npos);
        CHECK(fx.text("bt_tune_instruction") == c.instruction);
    }
}

TEST_CASE("Adjust A instead swaps the instruction", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    enter_tuning(fx);
    fx.panel().handle_tune_swap_clicked();
    CHECK(fx.text("bt_tune_instruction") == "Loosen A");
    CHECK(fx.state_int("bt_tune_path") == 0);
    CHECK(fx.state_int("bt_tune_dir") == 1);
    CHECK(fx.text("bt_tune_gap_caption") == "A tighter than B");
    CHECK(fx.text("bt_tune_swap_label") == "Adjust B instead");
}

TEST_CASE("the turn stepper shows only for screw tensioners", "[belt][panel][tune]") {
    SECTION("unknown kind: words only") {
        BeltPanelFixture fx;
        REQUIRE(get_printer_state().profile_state().printer_type().empty());
        enter_tuning(fx);
        CHECK(fx.state_int("bt_tune_kind") == 0);
        CHECK(fx.text("bt_tune_amount").empty());
        CHECK(fx.text("bt_tune_rate").empty());
        lv_obj_t* stepper = lv_obj_find_by_name(fx.view(), "bt_tune_stepper");
        REQUIRE(stepper != nullptr);
        CHECK(lv_hidden(stepper));
        // Words only: no screw arrow either way.
        for (const char* icon : {"bt_tune_icon_cw", "bt_tune_icon_ccw"}) {
            lv_obj_t* obj = lv_obj_find_by_name(fx.view(), icon);
            REQUIRE(obj != nullptr);
            INFO(icon);
            CHECK(lv_hidden(obj));
        }
    }
    SECTION("screw: an eighth-turn stepper, clamped") {
        BeltPanelFixture fx;
        get_printer_state().set_printer_type_sync("Voron 2.4");
        enter_tuning(fx);
        CHECK(fx.state_int("bt_tune_kind") == 1);
        lv_obj_t* stepper = lv_obj_find_by_name(fx.view(), "bt_tune_stepper");
        REQUIRE(stepper != nullptr);
        CHECK_FALSE(lv_hidden(stepper));
        // "Tighten B": the clockwise arrow, not the counter-clockwise one.
        CHECK_FALSE(lv_hidden(lv_obj_find_by_name(fx.view(), "bt_tune_icon_cw")));
        CHECK(lv_hidden(lv_obj_find_by_name(fx.view(), "bt_tune_icon_ccw")));
        CHECK(fx.text("bt_tune_amount") == "¼ turn");
        fx.panel().handle_tune_amount_step(+1);
        CHECK(fx.text("bt_tune_amount") == "⅜ turn");
        for (int i = 0; i < 10; ++i) {
            fx.panel().handle_tune_amount_step(+1);
        }
        CHECK(fx.text("bt_tune_amount") == "1 turn");
        for (int i = 0; i < 10; ++i) {
            fx.panel().handle_tune_amount_step(-1);
        }
        CHECK(fx.text("bt_tune_amount") == "⅛ turn");
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
    fx.mock().set_belt_tune_script('B', {102.1f, 107.6f, 109.8f});
    enter_tuning(fx);
    const std::string round0 = fx.text("bt_similarity");
    tune_round(fx);
    tune_round(fx);
    tune_round(fx);
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_MATCHED);

    fx.mock().clear_gcode_script_history();
    fx.panel().handle_tune_confirm_clicked();
    REQUIRE(fx.pump_until_state(RESULTS));
    const auto lines = resonance_lines(fx);
    REQUIRE(lines.size() == 2);
    for (const auto& l : lines) {
        CHECK(l.find("FREQ_START") == std::string::npos);
    }
    const std::string suffix = " · was " + round0;
    for (const char* note : {"bt_note_a", "bt_note_b"}) {
        const std::string n = fx.text(note);
        INFO(note << ": " << n);
        REQUIRE(n.size() >= suffix.size());
        CHECK(n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0);
    }
    INFO("facts: " << fx.text("bt_facts"));
    CHECK(fx.text("bt_facts").find("Tuned in 3 rounds") != std::string::npos);
    CHECK(fx.text("bt_facts").find("B tightened about") != std::string::npos);
    CHECK(fx.text("bt_facts").find("A tightened") == std::string::npos);
    CHECK(fx.text("bt_facts").find("A loosened") == std::string::npos);
}

TEST_CASE("Stop during a tuning sweep lands on START", "[belt][panel][tune]") {
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
    lv_obj_t* stop = lv_obj_find_by_name(dialog, "btn_primary");
    REQUIRE(stop != nullptr);
    lv_obj_send_event(stop, LV_EVENT_CLICKED, nullptr);
    fx.pump_ms(200);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::START));
    fx.pump_ms(5000);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::START));
}

TEST_CASE("a rebuild mid tuning sweep drops the late callbacks", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    enter_tuning(fx);
    fx.panel().handle_tune_measure_clicked();
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_SWEEPING);
    fx.pump_ms(300);

    // Frees the tree the chart lives in while the mock keeps sweeping.
    REQUIRE(fx.panel().rebuild());
    UpdateQueue::instance().drain();
    fx.pump_ms(10000);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::START));
    CHECK(fx.panel_chart() == nullptr);
}

TEST_CASE("a round with no peak says so and can be re-measured", "[belt][panel][tune]") {
    BeltPanelFixture fx;
    fx.mock().set_belt_tune_script('B', {std::nanf("")});
    enter_tuning(fx);
    const std::string gap = fx.text("bt_tune_gap");
    tune_round(fx);

    CHECK(fx.state_int("bt_tune_phase") == PHASE_ADJUST);
    CHECK(fx.text("bt_tune_last") == "No clear peak in this zone");
    CHECK(fx.text("bt_tune_gap") == gap);
    lv_obj_t* measure = lv_obj_find_by_name(fx.view(), "btn_tune_measure");
    REQUIRE(measure != nullptr);
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

TEST_CASE("a spring tensioner gets the re-seat instruction and no screw controls",
          "[belt][panel][tune]") {
    BeltPanelFixture fx;
    SpringPrinterDb db;
    REQUIRE(PrinterDetector::get_belt_tensioner("Test Spring Printer") ==
            helix::calibration::BeltTensionerKind::SPRING);
    get_printer_state().set_printer_type_sync("Test Spring Printer");
    enter_tuning(fx);

    CHECK(fx.state_int("bt_tune_kind") == 2);
    CHECK(fx.text("bt_tune_instruction") ==
          "Release B's tensioner, let the spring settle, lock it");
    CHECK(fx.text("bt_tune_amount").empty());
    for (const char* name : {"bt_tune_stepper", "bt_tune_icon_cw", "bt_tune_icon_ccw"}) {
        lv_obj_t* obj = lv_obj_find_by_name(fx.view(), name);
        REQUIRE(obj != nullptr);
        INFO(name);
        CHECK(lv_hidden(obj));
    }
}

namespace {

/// A replay capture with equal-height peaks at `peaks_hz`, over the mock's
/// 5-135 Hz band.
void write_two_peak_csv(const std::string& path, std::initializer_list<float> peaks_hz) {
    std::ofstream out(path);
    out << "freq,psd_x,psd_y,psd_z,psd_xyz\n";
    for (double freq = 5.0; freq <= 135.0; freq += 3200.0 / 4096.0) {
        double psd = 150.0;
        for (const float p : peaks_hz) {
            const double df = freq - p;
            psd += 1.5e4 / (1.0 + df * df / 9.0);
        }
        out << freq << ',' << psd * 0.45 << ',' << psd * 0.45 << ',' << psd * 0.1 << ',' << psd
            << '\n';
    }
}

} // namespace

TEST_CASE("Next zone measures the new zone before advising", "[belt][panel][tune]") {
    // Two pairs: 100/94 (the wider gap, tuned first) and 60/56.
    const std::string csv_a = "/tmp/helix-belt-zones-a-" + std::to_string(getpid()) + ".csv";
    const std::string csv_b = "/tmp/helix-belt-zones-b-" + std::to_string(getpid()) + ".csv";
    write_two_peak_csv(csv_a, {60.0f, 100.0f});
    write_two_peak_csv(csv_b, {56.0f, 94.0f});
    helix::ScopedEnv replay_a("HELIX_MOCK_BELT_CSV_A", csv_a.c_str());
    helix::ScopedEnv replay_b("HELIX_MOCK_BELT_CSV_B", csv_b.c_str());

    BeltPanelFixture fx;
    enter_tuning(fx);
    REQUIRE(fx.text("bt_tune_instruction") == "Tighten B");
    const auto [lo, hi] = zone_range(fx.text("bt_tune_zone"));
    REQUIRE(lo <= 94);
    REQUIRE(hi >= 100);

    // B's upper peak meets A's: this zone matches, the lower pair is next.
    write_two_peak_csv(csv_b, {56.0f, 100.0f});
    tune_round(fx);
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_MATCHED);
    REQUIRE(fx.state_int("bt_tune_has_next_zone") == 1);

    fx.panel().handle_tune_next_zone_clicked();
    // The baseline sweep starts at once, with no advice on screen.
    CHECK(fx.state_int("bt_tune_phase") == PHASE_SWEEPING);
    CHECK(fx.state_int("bt_tune_baseline") == 1);
    CHECK(fx.text("bt_tune_instruction") == "Measure this zone first");
    CHECK(fx.text("bt_tune_amount").empty());
    CHECK(fx.text("bt_tune_gap") == "--");
    const auto [lo2, hi2] = zone_range(fx.text("bt_tune_zone"));
    CHECK(lo2 <= 56);
    CHECK(hi2 >= 60);
    CHECK(hi2 < 90);

    for (uint32_t t = 0; t < 60000 && fx.state_int("bt_tune_phase") == PHASE_SWEEPING; t += 100) {
        fx.pump_ms(100);
    }
    REQUIRE(fx.state_int("bt_tune_phase") == PHASE_ADJUST);
    CHECK(fx.state_int("bt_tune_baseline") == 0);
    CHECK(fx.text("bt_tune_instruction") == "Tighten B");
    CHECK(fx.text("bt_tune_gap") == "4.0 Hz");
    // A baseline has no previous turn to report on.
    CHECK(fx.text("bt_tune_last").empty());

    std::remove(csv_a.c_str());
    std::remove(csv_b.c_str());
}

TEST_CASE("a closed gate stops the tuning sweeps", "[belt][panel][tune][gating]") {
    BeltPanelFixture fx;
    enter_tuning(fx);
    REQUIRE(fx.state_int("bt_can_start") == 1);

    // A print starts while the user is turning a screw.
    lv_subject_set_int(get_printer_state().print_state().get_print_active_subject(), 1);
    fx.pump_ms(200);
    CHECK(fx.state_int("bt_can_start") == 0);
    lv_obj_t* measure = lv_obj_find_by_name(fx.view(), "btn_tune_measure");
    REQUIRE(measure != nullptr);
    CHECK(lv_obj_has_state(measure, LV_STATE_DISABLED));

    fx.mock().clear_gcode_script_history();
    fx.panel().handle_tune_measure_clicked();
    fx.pump_ms(2000);
    CHECK(resonance_lines(fx).empty());
    CHECK(fx.state_int("bt_tune_phase") == PHASE_ADJUST);
}

TEST_CASE("a wrong-way line names the advice the user acted on", "[belt][panel][tune]") {
    // Told to tighten B, B's peak drops 98 -> 96.5 while A's falls 110 -> 95:
    // the gap flips sign, so the NEXT advice is to loosen B, but the turn just
    // made was a tightening.
    BeltPanelFixture fx;
    fx.mock().set_belt_tune_script('A', {95.0f});
    fx.mock().set_belt_tune_script('B', {96.5f});
    enter_tuning(fx);
    REQUIRE(fx.text("bt_tune_instruction") == "Tighten B");
    tune_round(fx);
    INFO("last: " << fx.text("bt_tune_last"));
    CHECK(fx.text("bt_tune_instruction") == "Loosen B");
    CHECK(fx.text("bt_tune_last") ==
          "B's peak went down after tightening. Check you turned Path B's tensioner.");
}

// ============================================================================
// Smallest screens
// ============================================================================

namespace {

/// Pins ui_breakpoint for one case and puts it back after.
class ScopedBreakpoint {
  public:
    explicit ScopedBreakpoint(int breakpoint)
        : subject_(lv_xml_get_subject(nullptr, "ui_breakpoint")) {
        REQUIRE(subject_ != nullptr);
        original_ = lv_subject_get_int(subject_);
        lv_subject_set_int(subject_, breakpoint);
    }
    ~ScopedBreakpoint() {
        lv_subject_set_int(subject_, original_);
    }
    void set(int breakpoint) {
        lv_subject_set_int(subject_, breakpoint);
    }

  private:
    lv_subject_t* subject_;
    int original_ = 0;
};

std::vector<std::string> visible_results_buttons(BeltPanelFixture& fx) {
    std::vector<std::string> out;
    for (const char* name :
         {"btn_retest_a", "btn_retest_b", "btn_retest_menu", "btn_test_both", "btn_tune"}) {
        lv_obj_t* btn = lv_obj_find_by_name(fx.view(), name);
        INFO("missing results button: " << name);
        REQUIRE(btn != nullptr);
        if (!lv_hidden(btn)) {
            out.emplace_back(name);
        }
    }
    return out;
}

/// Opens the Re-test menu and taps one of its rows.
void pick_retest(BeltPanelFixture& fx, const char* row) {
    lv_obj_t* opener = lv_obj_find_by_name(fx.view(), "btn_retest_menu");
    REQUIRE(opener != nullptr);
    lv_obj_send_event(opener, LV_EVENT_CLICKED, nullptr);
    fx.pump_ms(100);
    REQUIRE(helix::ui::ContextMenu::active() != nullptr);
    lv_obj_t* item = lv_obj_find_by_name(lv_obj_get_screen(fx.view()), row);
    INFO("menu row: " << row);
    REQUIRE(item != nullptr);
    lv_obj_send_event(item, LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    CHECK(helix::ui::ContextMenu::active() == nullptr);
}

} // namespace

TEST_CASE("RESULTS folds Re-test A/B into one menu on the smallest screens",
          "[belt][panel][micro]") {
    BeltPanelFixture fx;
    helix::ui::ContextMenu::register_shared_callbacks();
    REQUIRE(fx.register_component("belt_retest_menu"));
    ScopedBreakpoint bp(0);
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(RESULTS));

    CHECK(visible_results_buttons(fx) ==
          std::vector<std::string>{"btn_retest_menu", "btn_test_both", "btn_tune"});

    SECTION("every 480-wide screen folds them") {
        for (int narrow : {1, 2}) {
            bp.set(narrow);
            INFO("ui_breakpoint " << narrow);
            CHECK(visible_results_buttons(fx) ==
                  std::vector<std::string>{"btn_retest_menu", "btn_test_both", "btn_tune"});
        }
    }
    SECTION("a medium screen keeps the two Re-test buttons") {
        bp.set(3);
        CHECK(visible_results_buttons(fx) == std::vector<std::string>{"btn_retest_a",
                                                                      "btn_retest_b",
                                                                      "btn_test_both", "btn_tune"});
    }
    SECTION("Re-test A from the menu re-measures A") {
        pick_retest(fx, "btn_retest_path_a");
        CHECK(fx.state_int("belt_tension_state") ==
              static_cast<int>(BeltTensionPanel::ViewState::RUNNING));
        CHECK(fx.text("bt_run_title") == "Re-measuring Path A");
    }
    SECTION("Re-test B from the menu re-measures B") {
        pick_retest(fx, "btn_retest_path_b");
        CHECK(fx.state_int("belt_tension_state") ==
              static_cast<int>(BeltTensionPanel::ViewState::RUNNING));
        CHECK(fx.text("bt_run_title") == "Re-measuring Path B");
    }
}

TEST_CASE("TUNING on a 480-wide screen is one tappable line over the chart",
          "[belt][panel][micro][tune]") {
    BeltPanelFixture fx;
    get_printer_state().set_printer_type_sync("Voron 2.4");
    ScopedBreakpoint bp(0);
    enter_tuning(fx);
    REQUIRE(fx.state_int("bt_tune_kind") == 1);

    auto named = [&](const char* name) {
        lv_obj_t* obj = lv_obj_find_by_name(fx.view(), name);
        INFO("missing: " << name);
        REQUIRE(obj != nullptr);
        return obj;
    };
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

    REQUIRE(fx.text("bt_tune_instruction") == "Tighten B");
    lv_obj_send_event(named("bt_tune_line"), LV_EVENT_CLICKED, nullptr);
    CHECK(fx.state_int("bt_tune_path") == 0);
    CHECK(fx.text("bt_tune_swap_label") == "Adjust B instead");

    SECTION("a medium screen keeps the full hero") {
        bp.set(3);
        CHECK_FALSE(lv_hidden(named("bt_tune_hero")));
        CHECK(lv_hidden(named("bt_tune_line")));
        CHECK(lv_hidden(named("bt_tune_row_stepper")));
        CHECK_FALSE(lv_hidden(named("btn_tune_swap")));
    }
}
