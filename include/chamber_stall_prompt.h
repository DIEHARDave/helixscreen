// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"

#include <string>

namespace helix {

/// What the chamber prompt reads on each evaluation.
struct ChamberStallInputs {
    bool job_active = false;   ///< a job holds the machine (printing, paused or preparing)
    bool chamber_wait = false; ///< the job is held by a heater wait on the chamber
    bool rejected = false;     ///< the heater turned down our last target
    int rejection_seq = 0;     ///< rejection event count; a new value is a new rejection
    int chamber_target_deci = 0;
};

enum class ChamberStallAction { None, Prompt, Toast };

/// Decides when a chamber that is not heating is put to the user, so that the
/// common case (the appliance retries a conflict and heats) stays invisible
/// and the rare one never sits silent.
///
/// - A print held by a chamber wait while the heater refused our target:
///   prompt once per rejection, so a Try again that is refused again asks again.
/// - A print held by a chamber wait with the heater at target 0 and no
///   rejection: prompt once per job.
/// - A rejection while no job holds the machine: a toast, once per rejection.
/// - A rejection mid-print outside a chamber wait: nothing here; the
///   print-status banner carries it.
class ChamberStallArbiter {
  public:
    ChamberStallAction decide(const ChamberStallInputs& in);

  private:
    int toasted_seq_ = 0;
    int prompted_seq_ = 0;
    bool prompted_unheated_ = false;
};

namespace ui {

/// The modal and toast for a chamber heater that did not start. Owned by
/// SubjectInitializer; observes the rejection latch, the chamber-wait flag,
/// the chamber target and the job-holds-machine state, and routes every
/// change through one ChamberStallArbiter.
class ChamberStallPrompt {
  public:
    ChamberStallPrompt();
    ~ChamberStallPrompt();

    ChamberStallPrompt(const ChamberStallPrompt&) = delete;
    ChamberStallPrompt& operator=(const ChamberStallPrompt&) = delete;

    /// The target a retry sends: the one the heater reported turning down,
    /// else the last chamber target this app sent; 0 when neither is known.
    static double retry_target_c();

    /// Re-send retry_target_c() by the route around the gcode queue. Returns
    /// false, sending nothing, when there is no target or no such route.
    static bool retry();

  private:
    void evaluate();
    void show_prompt();

    ObserverGuard rejected_observer_;
    ObserverGuard seq_observer_;
    ObserverGuard wait_observer_;
    ObserverGuard target_observer_;
    ObserverGuard job_observer_;
    ChamberStallArbiter arbiter_;
    AsyncLifetimeGuard lifetime_;
};

} // namespace ui
} // namespace helix
