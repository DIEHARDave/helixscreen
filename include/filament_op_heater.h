// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file filament_op_heater.h
 * @brief The nozzle heat around a filament operation: heat before it, cool after it.
 *
 * Every Load / Unload / Purge / Extrude / Retract needs a hotend above Klipper's
 * min_extrude_temp, and the surface that asked for one is often gone before the
 * nozzle gets there: the home tile's dialog closes the moment Load is pressed.
 * So the wait belongs to an app-lifetime service, not to a panel.
 *
 * Three layers:
 *   - decide_preheat(): the pure rule. Hot enough, or heat to what and wait for what.
 *   - FilamentOpHeater: owns the one pending preheat, watches the nozzle, runs the
 *     op once it is hot, and schedules the post-op cooldown. Owned by
 *     SubjectInitializer, reached through get_filament_op_heater().
 *   - execute_filament_load/unload/purge() (filament_op_execute.h) go through it
 *     on every call, so a surface on the shared ladder cannot skip the preheat.
 *     Surfaces with a ladder of their own call run_after_preheat() directly.
 *
 * The heater decides; it draws nothing. A surface that shows its own heating UI
 * passes on_progress and announce = false; one with no UI of its own gets the
 * heater's toasts.
 */

#pragma once

#include "ui_observer_guard.h"

#include "filament_op_execute.h"
#include "filament_op_slot_resolver.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <lvgl.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace helix {
class PrinterState;
} // namespace helix

namespace helix::ui {

/// What a filament op does with the melt zone. Picks the slot whose material sets
/// the temperature, and the copy in log lines and toasts.
enum class FilamentHeatOp { Load, Unload, Purge, Extrude, Retract };

/// Short lowercase name for @p op, for log lines. Never null.
[[nodiscard]] const char* filament_heat_op_name(FilamentHeatOp op);

/// A preheat is reached, and a hot nozzle counts as hot, within this many degrees.
inline constexpr int PREHEAT_MARGIN_C = 5;

/// What the nozzle reads now and what the op needs, in whole degrees.
struct PreheatInputs {
    int current_c = 0;             ///< Live temperature
    int target_c = 0;              ///< Live target; 0 when the heater is off
    std::optional<int> material_c; ///< What the op's material wants, when anything names it
    int held_c = 0;                ///< Latched last non-zero target (the swap latch)
    int floor_c = 0;               ///< temperature::extrusion_floor_c()
    int ceiling_c = 0;             ///< temperature::nozzle_max_temp_c()
    /// A target someone set and the nozzle has reached counts as hot enough.
    /// True for ops that push what is already in the melt zone (purge, extrude,
    /// retract) and for any op while a job holds the machine: a paused print's
    /// reload runs at the print's own temperature.
    bool honor_live_target = false;
};

/// What to do about @p PreheatInputs.
struct PreheatDecision {
    bool heat = false; ///< false: the nozzle is hot enough, run the op now
    int send_c = 0;    ///< The target to command (the controller keeps a hotter one)
    int wait_c = 0;    ///< The temperature the op waits for, within PREHEAT_MARGIN_C
};

/**
 * @brief The preheat rule, with no printer attached.
 *
 * Nothing runs below the extrusion floor. Above it, the nozzle is hot enough when
 *   - it is within PREHEAT_MARGIN_C of the hotter of the material's temperature
 *     and the swap latch, so the filament already in the melt zone still flows
 *     (with no material named, the floor stands in for it), or
 *   - @p honor_live_target is set and it has reached a target someone set.
 *
 * Otherwise it heats to the material's temperature, or DEFAULT_LOAD_PREHEAT_TEMP
 * when nothing names one, held between the floor and the ceiling, and waits for
 * that raised to the latch: the controller's keep_previous_hot holds the nozzle
 * there, so a lower wait would fire before the nozzle settles.
 *
 * Skip reasons (preheat_skip_reason()) are the caller's to apply first.
 */
[[nodiscard]] PreheatDecision decide_preheat(const PreheatInputs& in);

/// Whether @p current_c has reached a preheat waiting for @p wait_c: within the
/// margin, and never below the extrusion floor.
[[nodiscard]] bool preheat_reached(int current_c, int wait_c, int floor_c);

/// Where a preheat stands, as reported to the surface that asked for it.
enum class PreheatPhase {
    Heating,   ///< Target sent, waiting. Repeats as the temperature moves.
    Reached,   ///< Hot; the op runs immediately after this report
    Cancelled, ///< Withdrawn: the user re-tapped, or another op replaced it
    Abandoned, ///< It cannot complete: target taken away, timed out, or a job started
};

struct PreheatProgress {
    PreheatPhase phase = PreheatPhase::Heating;
    int current_c = 0;
    int target_c = 0; ///< The temperature being waited for
};

/// Names one request, so its completion can say whether the heater heated for it.
/// 0 is no request.
using FilamentOpTicket = uint64_t;

/// One filament op that needs a hot nozzle.
struct PreheatRequest {
    FilamentHeatOp op = FilamentHeatOp::Load;

    /// Prefixes log lines. Static storage duration, like FilamentOpSurface::log_tag.
    const char* log_tag = "[Filament]";

    /// preheat_skip_reason() for the plan about to run, or preheat_skip_reason_unplanned()
    /// for an op with no plan. Anything but None runs the op with no heat.
    PreheatSkip skip = PreheatSkip::None;

    /// The lane whose material sets the temperature, and whose mapped extruder
    /// heats: the lane a load or unload targets, the loaded lane for a purge /
    /// extrude / retract. Negative for none, which heats the active extruder.
    int slot = -1;

    /// The surface's own answer when neither the lane nor the external spool names
    /// a material (the Filament panel's material preset). Unset falls through to
    /// DEFAULT_LOAD_PREHEAT_TEMP.
    std::optional<PreheatTarget> fallback_material;

    /// Raise the heater's own "Heating to…" toast. A surface that renders its own
    /// heating state from on_progress turns it off; the toasts for a preheat that
    /// ends without running stay, because the op the user asked for did not happen.
    bool announce = true;

    /// The op. Runs on the main thread, now when no heat is needed, else once hot.
    /// It is handed the request's ticket, which its completion passes to
    /// op_succeeded() / op_failed(). Must outlive the surface: capture nothing a
    /// closing surface frees.
    std::function<void(FilamentOpTicket)> run;

    /// Phase reports, on the main thread. Wrap anything that touches the surface
    /// in its lifetime token: a report can land after the surface is gone.
    std::function<void(const PreheatProgress&)> on_progress;
};

/**
 * @brief The one pending preheat, and the cooldown after a filament op it heated for.
 *
 * Main thread only. One preheat at a time, because there is one nozzle:
 *   - a second request for the SAME op while one waits is the user changing their
 *     mind, so it cancels the pending one and runs nothing;
 *   - a request for a DIFFERENT op replaces it: the pending one is reported
 *     Cancelled to its surface and the new one proceeds.
 *
 * A preheat ends without running its op when the target it sent never lands,
 * when that target is taken away, when it runs past PREHEAT_TIMEOUT, or, for a
 * request made while no job held the machine, when a job takes it.
 */
class FilamentOpHeater {
  public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    /// How long a sent target may take to appear on the nozzle.
    static constexpr std::chrono::seconds TARGET_LAND_TIMEOUT{20};
    /// How long a preheat may take altogether.
    static constexpr std::chrono::minutes PREHEAT_TIMEOUT{10};
    /// How long a backend op may run before its completion stops being watched.
    static constexpr std::chrono::minutes BACKEND_WATCH_TIMEOUT{15};

    /// @param clock Steady time; unset reads std::chrono::steady_clock.
    explicit FilamentOpHeater(PrinterState& state, Clock clock = {});
    ~FilamentOpHeater();

    FilamentOpHeater(const FilamentOpHeater&) = delete;
    FilamentOpHeater& operator=(const FilamentOpHeater&) = delete;

    enum class Outcome {
        Ran,       ///< No heat needed (or skipped); run() has returned
        Heating,   ///< run() waits for the nozzle
        Cancelled, ///< The same op was pending; it is cancelled and run() never runs
    };

    /// Run @p req.run now, or heat first and run it once hot.
    Outcome run_after_preheat(PreheatRequest req);

    /// Whether a preheat is waiting.
    [[nodiscard]] bool pending() const {
        return pending_.has_value();
    }

    /// Withdraw the pending preheat: the heater goes back off if it was off when
    /// the request arrived, and the op never runs. False when nothing was pending.
    bool cancel();

    /// The op @p ticket names finished. Schedules the post-op cooldown when this
    /// heater heated for it and no job holds the machine; anything else (a hand
    /// preheat, a backend that heats for itself) is not ours to cool.
    void op_succeeded(FilamentOpTicket ticket);

    /// The op @p ticket names failed: the heater stays where the user can see
    /// what happened, and the ticket is forgotten.
    void op_failed(FilamentOpTicket ticket);

    /// A backend op for @p ticket is about to be dispatched and completes on the
    /// AMS action feed: op_succeeded() when the action goes busy and returns to
    /// IDLE, op_failed() on ERROR, forgotten after BACKEND_WATCH_TIMEOUT. Arm it
    /// BEFORE the dispatch so a fast backend cannot finish unseen; call
    /// op_failed() if the dispatch is refused. Does nothing for a ticket the heater
    /// did not heat for.
    void watch_backend_completion(FilamentOpTicket ticket);

  private:
    using TimePoint = std::chrono::steady_clock::time_point;

    struct Pending {
        PreheatRequest req;
        FilamentOpTicket ticket = 0;
        std::string extruder; ///< Empty for the active extruder
        int wait_c = 0;
        int floor_c = 0;
        int prior_target_c = 0; ///< Live target when the first request arrived
        bool target_seen = false;
        bool job_free_at_request = true;
        TimePoint started{};
    };

    struct Watch {
        FilamentOpTicket ticket = 0;
        bool seen_busy = false;
        TimePoint deadline{};
    };

    void start_heating(Pending p, int send_c, const std::string& material_name);
    void on_temperature();
    void on_target(int target_deci);
    void on_lifecycle();
    void on_ams_action(int action);
    void check_deadlines();
    /// Take the pending preheat out, stop watching the nozzle, then report @p phase
    /// to its surface: the report runs with the heater already idle, so it may
    /// start another op.
    void end_pending(PreheatPhase phase);
    void abandon(const char* why, const char* toast);
    void report_heating();
    void cancel_deadline_timer();
    [[nodiscard]] TimePoint now() const;
    [[nodiscard]] lv_subject_t* temp_subject(const std::string& extruder) const;
    [[nodiscard]] lv_subject_t* target_subject(const std::string& extruder) const;
    [[nodiscard]] int read_c(lv_subject_t* subject) const;
    [[nodiscard]] bool heated_for(FilamentOpTicket ticket) const;
    void forget(FilamentOpTicket ticket);

    PrinterState& state_;
    Clock clock_;
    std::optional<Pending> pending_;
    FilamentOpTicket next_ticket_ = 0;
    std::vector<std::pair<FilamentOpTicket, TimePoint>> heated_;
    std::vector<Watch> watches_;
    ObserverGuard temp_observer_;
    ObserverGuard target_observer_;
    ObserverGuard lifecycle_observer_;
    ObserverGuard action_observer_;
    lv_timer_t* deadline_timer_ = nullptr;
};

/// The app's FilamentOpHeater (a shared resource SubjectInitializer registers),
/// or nullptr before it exists.
[[nodiscard]] FilamentOpHeater* get_filament_op_heater();

/// The material a filament op on @p slot heats for: that lane's, else the
/// external spool's (resolve_load_preheat_material()). nullopt when neither names
/// one, so a surface's own tail (a material preset, a default) applies.
[[nodiscard]] std::optional<PreheatTarget> preheat_material_for_slot(int slot);

/// preheat_skip_reason() for an op with no tier plan (purge, extrude, retract):
/// only the user's own claim applies, because no backend or macro has been chosen.
[[nodiscard]] PreheatSkip preheat_skip_reason_unplanned();

} // namespace helix::ui
