// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/**
 * @file queued_job_start.h
 * @brief Start a queued job with its saved pre-print options and no screen.
 *
 * Print-select starts a queued job by opening it in the file view seeded with
 * the options saved for it. The home tile starts one in a single tap: the
 * same option rule, the same pre-start checks and dialogs, the same
 * PrintStartController and PrintPreparationManager, and the same after-start
 * bookkeeping, with the file view's inputs computed from FileStartFacts.
 */

#include "async_lifetime_guard.h"
#include "i_moonraker_sub_apis.h" // JobQueueEntry
#include "print_start_inputs.h"
#include "queued_job_options.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

class QueuedJobStarterTestAccess; // NAMESPACE_OK: the test seam is a global class

namespace helix {
class IMoonrakerAPI;
class PrinterState;
} // namespace helix

namespace helix::ui {
class PrintPreparationManager;
class PrintStartController;
} // namespace helix::ui

namespace helix::queue {

/// PrintPreparationManager's OptionStateProvider answer for a saved entry: 1 on,
/// 0 off, -1 when the job saved nothing for @p option_id (the option's own
/// default then applies, as for a file opened fresh).
[[nodiscard]] int saved_option_state(const QueuedJobOptions& saved, const std::string& option_id);

/// The busy refusal every queued start shares: when the printer cannot take a
/// new print, warn that @p filename stays queued and answer true.
[[nodiscard]] bool refuse_queued_start_while_busy(PrinterState& printer_state,
                                                  const std::string& filename);

/// After a queued job's print has started: remove it from Moonraker's queue;
/// once Moonraker confirms, delete its saved options; refetch JobQueueState
/// either way. A refused removal keeps the job and its options and warns.
void finish_started_queued_job(AsyncLifetimeGuard& lifetime, IMoonrakerAPI* api,
                               const std::string& job_id);

class QueuedJobStarter {
  public:
    enum class Outcome {
        Started,        ///< the print started; finish_started_queued_job() ran
        Refused,        ///< not attempted: printer busy, a start in flight, no API
        NotStarted,     ///< the controller ended without a print (a gate, a refusal, a
                        ///< failed start); the job and its options stay
        OpenedFileView, ///< a check needed the file view; the job opened there
    };
    using DoneCallback = std::function<void(Outcome outcome)>;

    QueuedJobStarter(PrinterState& printer_state, IMoonrakerAPI* api);
    ~QueuedJobStarter();
    QueuedJobStarter(const QueuedJobStarter&) = delete;
    QueuedJobStarter& operator=(const QueuedJobStarter&) = delete;

    /// Follows a printer switch or reconnect; ignored while a start is in flight.
    void set_api(IMoonrakerAPI* api);

    /// Read @p job's saved options from the store, then start_with_options().
    void start(const JobQueueEntry& job, DoneCallback done = nullptr);

    /// Gather the file's facts and scan it, check the empty-slot pre-flight, run
    /// the gates, and start with @p saved answering every option.
    void start_with_options(const JobQueueEntry& job, QueuedJobOptions saved,
                            DoneCallback done = nullptr);

    [[nodiscard]] bool in_flight() const {
        return in_flight_;
    }

  private:
    friend class ::QueuedJobStarterTestAccess;

    void finish(Outcome outcome);

    /// Hand the shared prep manager back: drop the waiter and the option
    /// override this start set on it.
    void release_prep();

    /// Refuses @p job (answering @p done) when no start may begin; else marks
    /// the start in flight and keeps @p done.
    bool begin(const JobQueueEntry& job, DoneCallback& done);

    /// Gather the file's facts and wait for print-select's prep manager to be
    /// ready (its operations scan of the file, the PRINT_START analysis), then
    /// run_checks().
    void gather(const JobQueueEntry& job, QueuedJobOptions saved);

    /// What the start in flight is still waiting on.
    struct Pending {
        JobQueueEntry job;
        std::string name; ///< the file's basename
        std::string dir;  ///< its directory under gcodes, or empty
        std::optional<FileStartFacts> facts;
        bool scanned = false;
    };
    std::unique_ptr<Pending> pending_;

    void run_checks_when_gathered();

    /// The empty-slot pre-flight, then the controller's gates and start.
    void run_checks(const Pending& pending, FileStartFacts facts);

    PrinterState& printer_state_;
    IMoonrakerAPI* api_ = nullptr;
    /// Where the app's one PrintPreparationManager is found: print-select's
    /// (PrintSelectPanel::print_preparation_manager()). Asked per start.
    std::function<ui::PrintPreparationManager*()> prep_source_;
    /// The manager the start in flight drives; null when none is in flight.
    ui::PrintPreparationManager* prep_ = nullptr;
    std::unique_ptr<ui::PrintStartController> controller_;
    FileStartFactsLoader facts_loader_;
    QueuedJobOptions saved_;
    std::string job_id_;
    DoneCallback done_;
    bool in_flight_ = false;
    AsyncLifetimeGuard lifetime_;
};

/// The process-wide starter the home tile uses, bound to the current API.
/// Never destructed, like the shared job queue modal.
QueuedJobStarter& queued_job_starter();

} // namespace helix::queue
