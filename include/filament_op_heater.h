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

#include "async_lifetime_guard.h"
#include "filament_op_execute.h"
#include "filament_op_slot_resolver.h"

#include <cstdint>
#include <functional>
#include <optional>

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
 * The nozzle is hot enough when it is at or above the extrusion floor and either
 *   - it has reached a target someone set (a paused print, the user's own preheat):
 *     that temperature was chosen deliberately and the op runs at it, or
 *   - it is within PREHEAT_MARGIN_C of the hotter of the material's temperature and
 *     the swap latch, so the filament already in the melt zone still flows.
 * With no material named, the floor stands in for it.
 *
 * Otherwise it heats to the material's temperature, or DEFAULT_LOAD_PREHEAT_TEMP
 * when nothing names one, held between the floor and the ceiling, and waits for
 * that raised to the latch: the controller's keep_previous_hot holds the nozzle
 * there, so a lower wait would fire before the nozzle settles.
 *
 * Skip reasons (preheat_skip_reason()) are the caller's to apply first.
 */
[[nodiscard]] PreheatDecision decide_preheat(const PreheatInputs& in);

/// Where a preheat stands, as reported to the surface that asked for it.
enum class PreheatPhase {
    Heating,   ///< Target sent, waiting. Repeats as the temperature moves.
    Reached,   ///< Hot; the op runs immediately after this report
    Cancelled, ///< Withdrawn by the user (a second request while one was pending)
    Abandoned, ///< The nozzle target was taken away; the op will not run
};

struct PreheatProgress {
    PreheatPhase phase = PreheatPhase::Heating;
    int current_c = 0;
    int target_c = 0; ///< The temperature being waited for
};

/// One filament op that needs a hot nozzle.
struct PreheatRequest {
    FilamentHeatOp op = FilamentHeatOp::Load;

    /// Prefixes log lines. Static storage duration, like FilamentOpSurface::log_tag.
    const char* log_tag = "[Filament]";

    /// preheat_skip_reason() for the plan about to run, or preheat_skip_reason_unplanned()
    /// for an op with no plan. Anything but None runs the op with no heat.
    PreheatSkip skip = PreheatSkip::None;

    /// The lane whose material sets the temperature: the lane a load or unload
    /// targets, the loaded lane for a purge / extrude / retract. Negative for none.
    int slot = -1;

    /// The surface's own answer when neither the lane nor the external spool names
    /// a material (the Filament panel's material preset). Unset falls through to
    /// DEFAULT_LOAD_PREHEAT_TEMP.
    std::optional<PreheatTarget> fallback_material;

    /// Raise the heater's own "Heating to…" / "Preheat cancelled" toasts. A surface
    /// that renders its own heating state from on_progress turns these off.
    bool announce = true;

    /// The op. Runs on the main thread, now when no heat is needed, else once hot.
    /// Must outlive the surface: capture nothing a closing surface frees.
    std::function<void()> run;

    /// Phase reports, on the main thread. Wrap anything that touches the surface
    /// in its lifetime token: a report can land after the surface is gone.
    std::function<void(const PreheatProgress&)> on_progress;
};

/**
 * @brief The one pending preheat, and the cooldown after a filament op.
 *
 * Main thread only. One preheat at a time: there is one nozzle, and a second
 * request while one waits is the user changing their mind, so it cancels the
 * pending one and runs nothing (the Filament panel's re-tap rule).
 */
class FilamentOpHeater {
  public:
    explicit FilamentOpHeater(PrinterState& state);
    ~FilamentOpHeater();

    FilamentOpHeater(const FilamentOpHeater&) = delete;
    FilamentOpHeater& operator=(const FilamentOpHeater&) = delete;

    enum class Outcome {
        Ran,       ///< No heat needed (or skipped); run() has returned
        Heating,   ///< run() waits for the nozzle
        Cancelled, ///< A preheat was pending; it is cancelled and run() never runs
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

    /// A filament op finished. Schedules the post-op cooldown unless a print job
    /// holds the machine. A failed op leaves the heater where the user can see
    /// what happened.
    void op_succeeded();

    /// A backend op was dispatched and completes on the AMS action feed. Calls
    /// op_succeeded() when the action returns to IDLE after having been busy;
    /// an ERROR ends the watch with the heater left alone.
    void watch_backend_completion();

  private:
    struct Pending {
        PreheatRequest req;
        int wait_c = 0;
        int prior_target_c = 0; ///< Live target when the request arrived
        bool target_seen = false;
        uint64_t generation = 0;
    };

    void on_temperature();
    void on_target(int target_deci);
    void on_ams_action(int action);
    void report(PreheatPhase phase);
    void finish_pending();
    [[nodiscard]] int current_c() const;
    [[nodiscard]] int live_target_c() const;

    PrinterState& state_;
    std::optional<Pending> pending_;
    uint64_t generation_ = 0;
    ObserverGuard temp_observer_;
    ObserverGuard target_observer_;
    ObserverGuard action_observer_;
    bool action_seen_busy_ = false;
    AsyncLifetimeGuard lifetime_;
};

/// The app's FilamentOpHeater (a shared resource SubjectInitializer registers),
/// or nullptr before it exists.
[[nodiscard]] FilamentOpHeater* get_filament_op_heater();

/// preheat_skip_reason() for an op with no tier plan (purge, extrude, retract):
/// only the user's own claim applies, because no backend or macro has been chosen.
[[nodiscard]] PreheatSkip preheat_skip_reason_unplanned();

} // namespace helix::ui
