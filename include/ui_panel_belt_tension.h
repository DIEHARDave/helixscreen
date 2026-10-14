// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_belt_retest_menu.h"
#include "ui_observer_guard.h"
#include "ui_timer_guard.h"

#include "belt_gating.h"
#include "belt_tension_calibrator.h"
#include "belt_tension_types.h"
#include "belt_tune_session.h"
#include "memory_utils.h"
#include "operation_timeout_guard.h"
#include "overlay_base.h"
#include "platform_capabilities.h"
#include "resonance_console.h"
#include "static_panel_registry.h"
#include "subject_managed_panel.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace helix {
class IMoonrakerAPI;
}

// Forward declaration - see ui_frequency_response_chart.h
struct ui_frequency_response_chart_t;

namespace helix {
class IMoonrakerClient;
}

/// Test hook: tests/unit/test_belt_tension_panel_states.cpp reads the chart
/// and ghost series, which no subject exposes.
class BeltPanelFixture;

/**
 * @file ui_panel_belt_tension.h
 * @brief Belt tension panel: two resonance sweeps, compared
 *
 * Runs Klipper's TEST_RESONANCES down each CoreXY belt diagonal and compares
 * the resulting curves. Five view states drive one XML layout:
 * - START: gate result + hardware summary + start button
 * - RUNNING: one sweep at a time, live chart cursor + elapsed time
 * - RESULTS: strongest peak pair, verdict, similarity, ghost of the previous run
 * - ERROR: the failure, with retry
 * - TUNING: the guided loop (#1736). BeltTuneSession decides the zone, the
 *   advice and each round's outcome; the panel sweeps both paths over the
 *   zone's window per round and renders what the session says.
 *
 * START's action is gated on `bt_can_start`, which is driven solely by
 * evaluate_belt_gate() plus a klippy-READY leg. Hiding the Advanced-panel menu
 * row is not a gate: the panel is still reachable by `ctl navigate`, by a deep
 * link, and by a printer whose accelerometer drops out after entry. Binding
 * the button's disabled state to a subject holds in all three cases.
 *
 * ## Usage:
 * ```cpp
 * auto& panel = get_global_belt_tension_panel();
 * panel.init_subjects();
 * panel.create(screen);
 * panel.show();
 * ```
 */
class BeltTensionPanel : public OverlayBase {
  public:
    enum class ViewState {
        START = 0,   ///< gate result + hardware summary + start button
        RUNNING = 1, ///< sweeping one path
        RESULTS = 2, ///< both paths measured, compared
        ERROR = 3,
        TUNING = 4, ///< guided tuning loop, phased by TunePhase
    };

    /// bt_tune_phase: what the TUNING view is waiting on.
    enum class TunePhase {
        ADJUST = 0,   ///< the user turns a tensioner, then re-measures
        SWEEPING = 1, ///< a windowed round is running
        MATCHED = 2,  ///< the zone is matched (or none is left)
    };

    /// A sweep that goes this long without a progress line is treated as
    /// dead: Klipper prints a "Testing frequency" line every second, so two
    /// minutes of silence means the run is not coming back.
    static constexpr uint32_t STALL_TIMEOUT_MS = 120000;

    /// Pairs and unpaired peaks named in the results facts and marked on the
    /// chart, strongest first.
    static constexpr size_t MAX_LISTED_PEAKS = 3;

    BeltTensionPanel() = default;
    ~BeltTensionPanel() override;

    //
    // === OverlayBase Interface ===
    //

    void init_subjects() override;
    void deinit_subjects();
    lv_obj_t* create(lv_obj_t* parent) override;

    const char* xml_component() const override {
        return "panel_belt_tension";
    }

    const char* get_name() const override {
        return "Belt Tension";
    }

    void on_activate() override;
    void on_deactivating(DeactivateReason reason) override;
    void cleanup() override;
    void on_ui_destroyed() override;

    //
    // === Public API ===
    //

    using OverlayBase::show;
    void show();
    void set_api(helix::IMoonrakerClient* client, helix::IMoonrakerAPI* api);

    /// Pin the platform tier the chart decision uses, so tests can force
    /// EMBEDDED (no chart at all). Call before create(); production reads
    /// PlatformCapabilities::detect() when the first measurement starts.
    void set_render_tier_for_test(helix::PlatformTier tier, bool supports_animations);

    /// Pin the memory the low-memory check reads. Production reads the host's
    /// own memory, which is the printer's: the check only runs co-located.
    void set_memory_for_test(const helix::MemoryInfo& mem);

    //
    // === Event Handlers (public for XML callbacks) ===
    //

    void handle_start_clicked();
    void handle_stop_clicked();
    void handle_retry_clicked();
    void handle_retest_clicked(helix::calibration::BeltPath path);
    /// The compact layouts' single Re-test button: a menu of the two paths.
    void handle_retest_menu_clicked(lv_obj_t* anchor);

    // Tuning loop
    void handle_tune_clicked();
    void handle_tune_measure_clicked();
    /// "Good enough": back to RESULTS as last shown, no sweep.
    void handle_tune_done_clicked();
    void handle_tune_swap_clicked();
    /// The turn stepper: +1 / -1 steps of belt_tune::TURN_STEP, clamped to
    /// [0, TURN_MAX]. Zero re-measures without a turn.
    void handle_tune_amount_step(int direction);
    void handle_tune_next_zone_clicked();
    /// A full A+B check against the round-0 similarity, landing on RESULTS.
    void handle_tune_confirm_clicked();
    void handle_tune_keep_clicked();

  private:
    /// One path's measurement history. `previous` holds the curve the
    /// re-test is measuring against, so RESULTS can ghost it.
    struct PathRun {
        helix::calibration::BeltCurve curve;
        helix::calibration::BeltCurve previous;
        bool has = false;
        bool has_previous = false;
        uint32_t measured_at_ms = 0;
    };

    void set_view_state(ViewState state);
    void on_hardware_detected(const helix::calibration::BeltTensionHardware& hw);
    void on_error(const std::string& message);

    //
    // === Run orchestration ===
    //

    /// Queue paths for measurement and switch to RUNNING. `clear_previous`
    /// drops both paths' ghost history (Start runs fresh); a single-path
    /// re-test keeps the sibling's. With a `window` the run is a tuning round:
    /// the view stays on TUNING (phase SWEEPING) and the curves feed the
    /// session instead of `runs_`.
    void begin_run(const std::vector<helix::calibration::BeltPath>& queue, bool clear_previous,
                   std::optional<helix::calibration::BeltSweepWindow> window = std::nullopt);
    void start_next_measurement();
    void on_sweep_progress(int percent, float freq_hz);
    void on_sweep_complete(helix::calibration::BeltPath path, helix::calibration::BeltCurve curve);
    void on_sweep_error(const std::string& message);
    /// The stall guard fired: no progress line for STALL_TIMEOUT_MS.
    void on_stall();
    /// Queue empty: compare the runs and populate RESULTS, or error when a
    /// sweep returned too little in-band frequency data to compare.
    void finish_run();
    void populate_results(const helix::calibration::BeltComparison& cmp);
    /// Cancel the sweep, the stall guard, the elapsed timer and the queue.
    void cancel_run();
    void back_to_start();
    /// RUNNING, or TUNING while a round sweeps.
    [[nodiscard]] bool measuring() const;

    //
    // === Tuning loop ===
    //

    /// Sweep both paths over the session's current window.
    void start_tune_round();
    /// Both windowed sweeps are in: score the round and leave SWEEPING.
    void finish_tune_round();
    void set_tune_phase(TunePhase phase);
    /// Every bt_tune_* subject, and the gauge, from the session.
    void refresh_tune();
    [[nodiscard]] std::string tune_last_message() const;
    /// The stepper's value restarts at the session's suggestion.
    void reset_tune_turns();
    /// The gauge's half-span for this zone: wide enough for its first gap.
    void reset_tune_gauge_range();
    /// Drop the session and its curves.
    void end_tuning();
    /// Minutes a full A+B check takes at the printer's sweep rate.
    [[nodiscard]] int full_check_minutes() const;
    /// Minutes a tuning round (both paths) takes over `window`.
    [[nodiscard]] int round_minutes(const helix::calibration::BeltSweepWindow& window) const;

    //
    // === Subject refresh ===
    //

    /// bt_note_* from the runs. Notes read "sweeping" for the running path, an
    /// age ("just now", "3 min ago") plus " · was N%" when a previous curve
    /// exists.
    void refresh_notes();
    void refresh_run_detail();
    void start_elapsed_timer();

    //
    // === Gate ===
    //

    /// The single place the gate is computed. Nothing else may decide whether
    /// Start is live.
    void refresh_gate();
    /// Attach the gate's subject observers once. Idempotent: the accelerometer
    /// subject is owned by PrinterCapabilitiesState and may not exist yet the
    /// first time this runs, so activation retries.
    void ensure_gate_observers();
    /// Fetch the klippy UDS path from Moonraker and probe co-location.
    void probe_klippy_socket();
    /// Ask the printer its [resonance_tester] range for bt_hw_sweep.
    void query_hw_facts();

    //
    // === Chart ===
    //

    /// Series color for a path: primary for A, warning for B. One helper so a
    /// dedicated path palette lands in one place.
    [[nodiscard]] static lv_color_t path_color(helix::calibration::BeltPath path);
    /// Create the chart on first use (never on EMBEDDED). Returns null when
    /// the tier forbids a chart or the view carries no host.
    ui_frequency_response_chart_t* ensure_chart();
    void destroy_chart();
    /// Re-read belt_path_a/b into the chart's series after a theme change.
    void apply_path_colors();
    /// Park the chart obj in the RUNNING host; called when a new run starts.
    void chart_to_running_host();
    /// Fit the chart's axes to the curves it holds (or the sweep range before any).
    void push_chart_data();
    void push_chart_markers(const helix::calibration::BeltComparison& cmp);
    /// TUNING: both paths over the zone window, the adjusted path's earlier
    /// rounds muted, hollow rings at the session's refined peaks.
    void push_tune_chart_data();
    /// One curve into a series as a percentage of `tallest`; hidden when empty.
    void push_series_data(int series_id, const helix::calibration::BeltCurve& curve, float tallest);
    void run_after_ram_check(std::function<void()> go);
    /// Re-check the gate before any sweep starts; false (logged) when it is
    /// shut. `action` names the button in the log.
    [[nodiscard]] bool gate_allows_sweep(const char* action);

    // Subject manager for RAII cleanup
    SubjectManager subjects_;

    // View state subject lives in the .cpp (static) but registers here.
    lv_subject_t can_start_subject_{};
    lv_subject_t gate_message_subject_{};
    char gate_message_buf_[128] = {};

    lv_subject_t hw_kinematics_subject_{};
    char hw_kinematics_buf_[64] = {};
    lv_subject_t hw_accel_subject_{};
    char hw_accel_buf_[64] = {};
    lv_subject_t hw_sweep_subject_{};
    char hw_sweep_buf_[64] = {};

    lv_subject_t run_title_subject_{};
    char run_title_buf_[64] = {};
    lv_subject_t run_detail_subject_{};
    char run_detail_buf_[96] = {};
    lv_subject_t running_path_subject_{};

    lv_subject_t note_a_subject_{};
    char note_a_buf_[64] = {};
    lv_subject_t note_b_subject_{};
    char note_b_buf_[64] = {};

    lv_subject_t verdict_subject_{};
    lv_subject_t verdict_text_subject_{};
    char verdict_text_buf_[128] = {};
    lv_subject_t facts_subject_{};
    char facts_buf_[256] = {};
    lv_subject_t unpaired_subject_{};
    char unpaired_buf_[128] = {};
    lv_subject_t has_unpaired_subject_{};
    lv_subject_t similarity_subject_{};
    char similarity_buf_[16] = {};
    lv_subject_t chart_available_subject_{};
    lv_subject_t error_message_subject_{};
    char error_message_buf_[256] = {};

    // Tuning loop
    lv_subject_t tune_cta_subject_{};
    char tune_cta_buf_[32] = {};
    lv_subject_t tune_phase_subject_{};
    lv_subject_t tune_kind_subject_{};
    lv_subject_t tune_dir_subject_{};
    lv_subject_t tune_path_subject_{};
    lv_subject_t tune_severity_subject_{};
    lv_subject_t tune_baseline_subject_{};
    lv_subject_t tune_has_zone_subject_{};
    lv_subject_t tune_has_next_zone_subject_{};
    lv_subject_t tune_shape_warning_subject_{};
    lv_subject_t tune_instruction_subject_{};
    char tune_instruction_buf_[160] = {};
    lv_subject_t tune_amount_subject_{};
    char tune_amount_buf_[32] = {};
    lv_subject_t tune_gap_subject_{};
    char tune_gap_buf_[24] = {};
    lv_subject_t tune_gap_caption_subject_{};
    char tune_gap_caption_buf_[64] = {};
    lv_subject_t tune_round_subject_{};
    char tune_round_buf_[32] = {};
    lv_subject_t tune_zone_subject_{};
    char tune_zone_buf_[48] = {};
    lv_subject_t tune_rate_subject_{};
    char tune_rate_buf_[32] = {};
    lv_subject_t tune_last_subject_{};
    char tune_last_buf_[160] = {};
    lv_subject_t tune_has_last_subject_{};
    lv_subject_t tune_hint_subject_{};
    char tune_hint_buf_[128] = {};
    lv_subject_t tune_swap_label_subject_{};
    char tune_swap_label_buf_[48] = {};
    lv_subject_t tune_measure_label_subject_{};
    char tune_measure_label_buf_[48] = {};
    lv_subject_t tune_confirm_label_subject_{};
    char tune_confirm_label_buf_[64] = {};
    lv_subject_t tune_next_zone_label_subject_{};
    char tune_next_zone_label_buf_[64] = {};

    // Gate observers. Every one carries PrinterState's own SubjectLifetime -
    // the observe<V> factory requires it as its fourth parameter; a
    // default-constructed token leaves the guard tokenless (#705).
    ObserverGuard accel_observer_;
    ObserverGuard print_active_observer_;
    ObserverGuard connected_observer_;
    ObserverGuard klippy_observer_;
    ObserverGuard theme_observer_;
    bool gate_observers_wired_ = false;

    // Klippy's UDS path, from Moonraker's /server/config. Reachability is
    // probed once per activation, not per gate refresh: the gate recomputes on
    // every subject change and a connect() syscall each time would be waste.
    std::string klippy_socket_path_;
    bool klippy_socket_reachable_ = false;

    // Calibrator
    std::unique_ptr<helix::calibration::BeltTensionCalibrator> calibrator_;
    helix::IMoonrakerAPI* api_ = nullptr;
    helix::IMoonrakerClient* client_ = nullptr;

    // Chart. One chart, re-parented between the RUNNING and RESULTS hosts.
    ui_frequency_response_chart_t* chart_ = nullptr;
    int series_[2] = {-1, -1};
    int ghost_series_[2] = {-1, -1};
    /// Per path, the adjusted path's two previous tuning rounds, oldest first.
    int tune_ghost_series_[2][2] = {{-1, -1}, {-1, -1}};
    lv_obj_t* chart_host_running_ = nullptr;
    lv_obj_t* chart_host_results_ = nullptr;
    lv_obj_t* chart_host_tuning_ = nullptr;

    /// Render tier override for tests; nullopt reads PlatformCapabilities.
    std::optional<helix::PlatformTier> tier_override_;
    bool tier_animations_ = true;
    /// The printer's [resonance_tester] sweep; Klipper defaults until the query lands.
    helix::calibration::ResonanceTesterConfig sweep_cfg_;

    // Hardware detection cache. Feeds BeltGateInputs::is_corexy.
    helix::calibration::BeltTensionHardware detected_hw_;
    bool detection_pending_ = false; ///< detect_hardware() has not answered yet

    // Run state
    PathRun runs_[2];
    // The similarity of the latest comparison, and the one a single-path
    // re-test is measuring against: the re-tested path's note reads
    // " · was N%" from it.
    float similarity_percent_ = 0.0f;
    float was_similarity_percent_ = 0.0f;
    std::optional<helix::MemoryInfo> mem_override_;
    lv_obj_t* low_ram_dialog_ = nullptr;
    /// The low-RAM warning was accepted in this tuning session; later rounds
    /// do not ask again.
    bool tune_low_ram_accepted_ = false;
    /// Deactivated for the screensaver: the next activation is a wake, which
    /// keeps the view, the run and the tuning session.
    bool suspended_ = false;
    /// RESULTS's Re-test menu on compact layouts.
    helix::ui::BeltRetestMenu retest_menu_;
    /// Close the Re-test menu and drop its callback, so nothing it dispatches
    /// later can start a sweep.
    void hide_retest_menu();
    std::vector<helix::calibration::BeltPath> queue_;
    /// Size of the queue this run started with, for "2 of 2" detail lines.
    size_t run_queue_total_ = 0;
    /// True from begin_run() to cancel_run(): sweep callbacks that land after
    /// a cancel belong to a run this panel has already abandoned.
    bool run_active_ = false;
    uint32_t run_started_ms_ = 0;
    helix::ui::LvglTimerGuard elapsed_timer_;
    OperationTimeoutGuard stall_guard_;
    /// The window of the tuning round in flight; nullopt for a full check.
    std::optional<helix::calibration::BeltSweepWindow> run_window_;

    // Tuning loop
    /// The comparison RESULTS shows; round 0 of a tuning session.
    helix::calibration::BeltComparison last_cmp_;
    std::optional<helix::calibration::BeltTuneSession> tune_;
    TunePhase tune_phase_ = TunePhase::ADJUST;
    /// The latest round's windowed curves; valid once tune_has_curves_.
    helix::calibration::BeltCurve tune_curves_[2];
    bool tune_has_curves_ = false;
    /// The adjusted path's curves from earlier rounds of this zone, oldest
    /// first, at most two.
    std::vector<helix::calibration::BeltCurve> tune_adj_history_;
    std::optional<helix::calibration::BeltTuneOutcome> tune_last_outcome_;
    /// The direction the last round's turn was advised in: what the user acted on.
    helix::calibration::BeltTurnDirection tune_last_direction_ =
        helix::calibration::BeltTurnDirection::TIGHTEN;
    /// How far the gap moved in the last round, Hz.
    float tune_last_move_hz_ = 0.0f;
    float tune_turns_ = helix::calibration::belt_tune::FIRST_TURN; ///< the stepper
    float tune_gauge_range_hz_ = 2.0f * helix::calibration::belt_tune::GAUGE_WARN_HZ;
    bool tune_shape_differs_ = false; ///< round 0's tallest peaks are unpaired
    float tune_round0_similarity_ = 0.0f;
    /// Appended to the facts line by the Confirm run's RESULTS.
    std::string tune_facts_suffix_;

    friend class BeltPanelFixture;
};

// Global instance accessor
inline BeltTensionPanel& get_global_belt_tension_panel() {
    return helix::lazy_global<BeltTensionPanel>("BeltTensionPanel");
}

/**
 * @brief Register XML event callbacks for belt tension panel
 *
 * Call once at startup before creating any panel_belt_tension XML.
 * Registers callbacks for all button events and initializes subjects.
 */
void ui_panel_belt_tension_register_callbacks();

/**
 * @brief Initialize row click callback for opening from Advanced panel
 *
 * Registers "on_belt_tension_row_clicked" callback.
 */
void init_belt_tension_row_handler();
