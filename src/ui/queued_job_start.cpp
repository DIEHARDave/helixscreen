// SPDX-License-Identifier: GPL-3.0-or-later

#include "queued_job_start.h"

#include "ui_error_reporting.h"
#include "ui_modal.h"
#include "ui_preflight_check_modal.h"
#include "ui_print_preparation_manager.h"
#include "ui_print_start_controller.h"

#include "app_globals.h"
#include "job_queue_start.h"
#include "job_queue_state.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>
#include <utility>

namespace helix::queue {

int saved_option_state(const QueuedJobOptions& saved, const std::string& option_id) {
    const auto it = saved.options.find(option_id);
    if (it == saved.options.end()) {
        return -1;
    }
    return it->second ? 1 : 0;
}

bool refuse_queued_start_while_busy(PrinterState& printer_state, const std::string& filename) {
    if (printer_state.print_state().can_start_new_print()) {
        return false;
    }
    NOTIFY_WARNING(lv_tr("Printer is busy - {} stays in the queue"), filename);
    return true;
}

void finish_started_queued_job(AsyncLifetimeGuard& lifetime, IMoonrakerAPI* api,
                               const std::string& job_id) {
    if (!api) {
        return;
    }
    spdlog::info("[QueuedJobStart] Print started for queued job {} - removing from queue", job_id);
    api->queue().remove_jobs(
        {job_id},
        lifetime.bg_cb("QueuedJobStart::queued_job_started",
                       [api, job_id]() {
                           // Options are deleted only once Moonraker confirms the removal:
                           // a refusal (or a stale id after a Moonraker restart) leaves the
                           // job queued, and Start next must still find its saved options.
                           delete_queued_job_options(api, job_id);
                           if (auto* jqs = get_job_queue_state()) {
                               jqs->fetch();
                           }
                       }),
        lifetime.bg_cb("QueuedJobStart::queued_job_removal_failed", [job_id](
                                                                        const MoonrakerError& err) {
            spdlog::warn("[QueuedJobStart] Removing queued job {} failed: {}", job_id, err.message);
            NOTIFY_WARNING(lv_tr("Could not remove the job from the queue"));
            if (auto* jqs = get_job_queue_state()) {
                jqs->fetch();
            }
        }));
}

// ============================================================================
// QueuedJobStarter
// ============================================================================

QueuedJobStarter::QueuedJobStarter(PrinterState& printer_state, IMoonrakerAPI* api)
    : printer_state_(printer_state), api_(api),
      prep_(std::make_unique<ui::PrintPreparationManager>()),
      controller_(std::make_unique<ui::PrintStartController>(printer_state, api)),
      facts_loader_(api) {
    prep_->set_dependencies(api, &printer_state);
    // Every option the job saved answers from the saved entry; the rest take
    // their defaults, as for a file opened fresh.
    prep_->set_option_state_provider(
        [this](const std::string& id) { return saved_option_state(saved_, id); });
    // Macro-analysis rows exist only once the analysis lands, so a start
    // waiting on it resumes here.
    prep_->set_macro_analysis_callback(
        [this](const PrintStartAnalysis&) { run_checks_when_gathered(); });

    // No detail view and no navigation: the user stays where the tap was.
    controller_->set_prep_manager(prep_.get());
    controller_->set_on_print_started([this]() {
        finish_started_queued_job(lifetime_, api_, job_id_);
        finish(Outcome::Started);
    });
    controller_->set_on_print_cancelled([this]() { finish(Outcome::NotStarted); });
}

QueuedJobStarter::~QueuedJobStarter() = default;

void QueuedJobStarter::set_api(IMoonrakerAPI* api) {
    if (in_flight_ || api == api_) {
        return;
    }
    api_ = api;
    prep_->set_dependencies(api, &printer_state_);
    controller_->set_api(api);
    facts_loader_.set_api(api);
}

bool QueuedJobStarter::begin(const JobQueueEntry& job, DoneCallback& done) {
    if (in_flight_ || !api_ || refuse_queued_start_while_busy(printer_state_, job.filename)) {
        spdlog::info("[QueuedJobStart] Not starting queued job {} ({})", job.job_id,
                     in_flight_ ? "a start is in flight" : (!api_ ? "no API" : "printer busy"));
        if (done) {
            done(Outcome::Refused);
        }
        return false;
    }
    in_flight_ = true;
    done_ = std::move(done);
    job_id_ = job.job_id;
    return true;
}

void QueuedJobStarter::start(const JobQueueEntry& job, DoneCallback done) {
    if (!begin(job, done)) {
        return;
    }
    spdlog::info("[QueuedJobStart] Starting queued job {} -> '{}'", job.job_id, job.filename);
    // A missing entry answers defaults: every option at its default.
    load_queued_job_options(lifetime_, api_, job.job_id,
                            [this, job](QueuedJobOptions saved) { gather(job, std::move(saved)); });
}

void QueuedJobStarter::start_with_options(const JobQueueEntry& job, QueuedJobOptions saved,
                                          DoneCallback done) {
    if (!begin(job, done)) {
        return;
    }
    gather(job, std::move(saved));
}

void QueuedJobStarter::gather(const JobQueueEntry& job, QueuedJobOptions saved) {
    saved_ = std::move(saved);

    auto pending = std::make_unique<Pending>();
    pending->job = job;
    const size_t slash = job.filename.rfind('/');
    pending->name = slash == std::string::npos ? job.filename : job.filename.substr(slash + 1);
    pending->dir = slash == std::string::npos ? std::string{} : job.filename.substr(0, slash);
    const std::string name = pending->name;
    const std::string dir = pending->dir;
    pending_ = std::move(pending);

    // The operations scan answers the printer-stopping command check. Its
    // answer arrives here on the main thread, success or failure; an answer
    // already held for this file (or one given while the call runs) needs none.
    prep_->set_on_scan_answered([this]() {
        if (pending_) {
            pending_->scanned = true;
            run_checks_when_gathered();
        }
    });
    if (!prep_->has_scan_result_for(name)) {
        prep_->scan_file_for_operations(name, dir);
    }
    if (pending_ &&
        (prep_->has_scan_result_for(name) || prep_->has_printer_stop_answer_for(name))) {
        pending_->scanned = true;
    }

    facts_loader_.load(job.filename, [this](FileStartFacts facts) {
        if (pending_) {
            pending_->facts = std::move(facts);
            run_checks_when_gathered();
        }
    });
}

void QueuedJobStarter::run_checks_when_gathered() {
    if (!pending_ || !pending_->facts || !pending_->scanned ||
        prep_->is_macro_analysis_in_progress()) {
        return;
    }
    auto pending = std::move(pending_);
    prep_->set_on_scan_answered(nullptr);
    FileStartFacts facts = std::move(*pending->facts);
    run_checks(*pending, std::move(facts));
}

void QueuedJobStarter::run_checks(const Pending& pending, FileStartFacts facts) {
    const JobQueueEntry& job = pending.job;
    auto start_gates = [this, name = pending.name, dir = pending.dir, facts]() {
        controller_->set_file_facts(facts);
        controller_->set_file(name, dir, facts.palette.colors, "");
        controller_->initiate();
    };

    // A used tool mapped to an empty slot: the same modal a Print tap shows.
    const PreflightResult preflight = preflight_for(facts);
    if (!preflight.has_block()) {
        start_gates();
        return;
    }

    spdlog::info("[QueuedJobStart] Queued job {} maps a used tool to an empty slot", job.job_id);
    auto tok = lifetime_.token();
    auto chosen = std::make_shared<bool>(false);
    auto modal = std::make_unique<ui::PreflightCheckModal>();
    modal->set_checks(preflight);
    modal->set_on_force([tok, chosen, start_gates]() {
        if (tok.expired()) {
            return;
        }
        *chosen = true;
        start_gates();
    });
    // Remap edits the file view's mapping card, so the job goes there.
    modal->set_on_remap([this, tok, chosen, job = pending.job]() {
        if (tok.expired()) {
            return;
        }
        *chosen = true;
        finish(Outcome::OpenedFileView);
        open_queued_job_in_file_view(job);
    });
    modal->set_on_closed([this, tok, chosen]() {
        if (tok.expired() || *chosen) {
            return;
        }
        finish(Outcome::NotStarted);
    });
    if (!Modal::show_owned(std::move(modal), lv_screen_active())) {
        finish(Outcome::NotStarted);
    }
}

void QueuedJobStarter::finish(Outcome outcome) {
    if (!in_flight_) {
        return;
    }
    in_flight_ = false;
    pending_.reset();
    prep_->set_on_scan_answered(nullptr);
    auto done = std::move(done_);
    done_ = nullptr;
    if (done) {
        done(outcome);
    }
}

QueuedJobStarter& queued_job_starter() {
    // Never destructed, for the reason src/ui/job_queue_start.cpp#queue_modal
    // gives: its controller would tear down after lv_deinit().
    static QueuedJobStarter* starter =
        new QueuedJobStarter(get_printer_state(), get_moonraker_api());
    starter->set_api(get_moonraker_api());
    return *starter;
}

} // namespace helix::queue
