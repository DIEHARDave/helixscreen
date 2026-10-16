// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"

#include <string>
#include <string_view>

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
///   prompt once per rejection, so a Try Again that is refused again asks again.
/// - A print held by a chamber wait with the heater at target 0 and no
///   rejection: prompt once per job.
/// - A rejection while no job holds the machine: a toast, once per rejection.
/// - A rejection mid-print outside a chamber wait: nothing here; the
///   print-status banner carries it.
class ChamberStallArbiter {
  public:
    ChamberStallAction decide(const ChamberStallInputs& in);

    /// The prompt has nothing left to ask: the job ended, the wait cleared
    /// (the chamber reached its temperature from any source), or the heater
    /// took a target.
    static bool should_close(const ChamberStallInputs& in);

  private:
    int toasted_seq_ = 0;
    int prompted_seq_ = 0;
    bool prompted_unheated_ = false;
};

/// Which command holds the queue. They end differently: M191 returns on a
/// target of 0 or a print cancel, while TEMPERATURE_WAIT on a sensor's
/// MINIMUM ends only at that temperature or at a shutdown.
enum class ChamberWaitKind { Unknown, M191, TemperatureWait };

struct ChamberWaitLine {
    ChamberWaitKind kind = ChamberWaitKind::Unknown;
    double target_c = 0; ///< M191's S/R or TEMPERATURE_WAIT's MINIMUM; 0 = none
};

/// Read the command the print is executing from the file bytes that end at
/// virtual_sdcard.file_position (the last complete line). Anything else,
/// such as a macro whose body holds the wait, is Unknown.
ChamberWaitLine classify_chamber_wait_line(std::string_view chunk_ending_at_position);

/// How Cancel print ends a print held by a chamber wait.
/// - ClearTargetThenCancel (M191): a target of 0 by the route around the
///   queue ends the wait, then printer.print.cancel; no emergency stop.
/// - EmergencyStop (TEMPERATURE_WAIT): nothing short of M112 + FIRMWARE_RESTART
///   ends it, so the user confirms that first.
/// - ClearTargetThenAbort (unknown): clear the target in case it is M191,
///   then the usual abort, which escalates only per the user's own setting.
enum class ChamberCancelPath { ClearTargetThenCancel, EmergencyStop, ClearTargetThenAbort };
ChamberCancelPath chamber_cancel_path(ChamberWaitKind kind);

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
    /// else the last chamber target this app sent, else @p wait_target_c
    /// (the waiting command's own temperature); 0 when none is known.
    static double retry_target_c(double wait_target_c = 0);

    /// Re-send retry_target_c() by the route around the gcode queue. Returns
    /// false, sending nothing, when there is no target or no such route.
    static bool retry();

    /// End the print the way @p kind's wait allows (chamber_cancel_path). The
    /// emergency-stop path asks for confirmation and acts only on it.
    static void cancel_print(ChamberWaitKind kind);

  private:
    void evaluate();
    void show_prompt();
    void refresh_content();
    void probe_wait_line();
    void on_try_again();
    void on_cancel();
    friend class ChamberStallModalAccess;

    ObserverGuard rejected_observer_;
    ObserverGuard seq_observer_;
    ObserverGuard wait_observer_;
    ObserverGuard target_observer_;
    ObserverGuard text_observer_;
    ObserverGuard job_observer_;
    ChamberStallArbiter arbiter_;
    AsyncLifetimeGuard lifetime_;

    ChamberWaitLine wait_line_;
    bool wait_line_probed_ = false;
    bool retry_busy_ = false;
    std::string note_; ///< the last Try Again's outcome, "" when none
};

} // namespace ui
} // namespace helix
