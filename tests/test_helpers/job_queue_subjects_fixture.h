// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "job_queue_state.h"
#include "static_subject_registry.h"

#include <string>

namespace helix {

/// Drops every JobQueueState subject name out of helix-xml's global scope so
/// no later test resolves a pointer into this fixture's stack frame. Declare
/// AFTER the JobQueueState and BEFORE any widget/harness that observes the
/// subjects, so destruction runs in the reverse order: observers detach
/// first, then the names go away.
struct ScopedJobQueueSubjects {
    ~ScopedJobQueueSubjects() {
        StaticSubjectRegistry::instance().deinit_one("JobQueueState");
        lv_xml_unregister_subject(nullptr, "job_queue_count");
        lv_xml_unregister_subject(nullptr, "job_queue_run_state");
        lv_xml_unregister_subject(nullptr, "job_queue_up_next_text");
        lv_xml_unregister_subject(nullptr, "job_queue_start_next_text");
        lv_xml_unregister_subject(nullptr, "job_queue_next_estimate");
        lv_xml_unregister_subject(nullptr, "job_queue_next_thumbnail");
        lv_xml_unregister_subject(nullptr, "job_queue_automatic_transition");
    }
};

/// Installs a JobQueueState on the get_job_queue_state() global for the scope of a
/// test and always clears it on the way out: the pointer is a shared static in the
/// test binary, so a leaked pointer to a local would dangle for whatever runs next.
struct ScopedJobQueueState {
    explicit ScopedJobQueueState(JobQueueState* state) {
        set_job_queue_state(state);
    }
    ~ScopedJobQueueState() {
        set_job_queue_state(nullptr);
    }
    ScopedJobQueueState(const ScopedJobQueueState&) = delete;
    ScopedJobQueueState& operator=(const ScopedJobQueueState&) = delete;
};

/// A "ready" queue with n jobs: ids job-0.., filenames file-N.gcode.
inline JobQueueStatus status_with(int n) {
    JobQueueStatus s;
    s.queue_state = "ready";
    for (int i = 0; i < n; ++i) {
        s.queued_jobs.push_back({"job-" + std::to_string(i), "file-" + std::to_string(i) + ".gcode",
                                 1000.0 + i, 30.0 * (i + 1)});
    }
    return s;
}

} // namespace helix
