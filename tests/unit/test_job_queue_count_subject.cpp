// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_job_queue_count_subject.cpp
 * @brief `job_queue_count` is the refresh channel every queue surface observes
 * (the home tile, the print-status widget's queue row, the queue modal), so it
 * has to be registered and published after the texts those surfaces re-read.
 *
 * `deliver_status()` goes through `on_queue_fetched()` -> `update_subjects()`, the
 * same path a real Moonraker response takes, so a fix that registered the
 * subject but forgot to publish to it would still fail here.
 *
 * The guard is declared before anything that observes the subjects: the
 * observers must detach BEFORE `deinit_one()` deinits what they observe. It also
 * un-registers every queue-subject name from helix-xml's global scope, because
 * `JobQueueState` is process-lifetime in production and never does, so leaving a
 * local instance's subjects in that table would dangle for the rest of the binary.
 */

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/job_queue_state_test_access.h"
#include "../test_helpers/job_queue_subjects_fixture.h"
#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "job_queue_state.h"
#include "static_subject_registry.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE_METHOD(LVGLUITestFixture,
                 "job_queue_count is registered and published by a queue response",
                 "[1349][job_queue][subjects]") {
    // JobQueueState guards null api_/client_ in both fetch() (never reached
    // here) and subscribe_to_notifications(), so nullptr/nullptr is safe.
    JobQueueState jqs(nullptr, nullptr);
    ScopedJobQueueSubjects subject_guard;
    jqs.init_subjects();

    lv_subject_t* count = lv_xml_get_subject(nullptr, "job_queue_count");
    REQUIRE(count != nullptr);
    CHECK(lv_subject_get_int(count) == 0);

    lv_subject_t* up_next = lv_xml_get_subject(nullptr, "job_queue_up_next_text");
    REQUIRE(up_next != nullptr);
    CHECK(std::string(lv_subject_get_string(up_next)).empty());

    // The up-next line must be settled whenever the count moves: count is the
    // rebuild trigger, so an observer firing on it re-reads the line.
    std::string up_next_seen_on_count;
    lv_observer_t* count_observer = lv_subject_add_observer(
        count,
        [](lv_observer_t* observer, lv_subject_t*) {
            auto* dest = static_cast<std::string*>(lv_observer_get_user_data(observer));
            lv_subject_t* up_next_subj = lv_xml_get_subject(nullptr, "job_queue_up_next_text");
            if (up_next_subj != nullptr) {
                const char* text = lv_subject_get_string(up_next_subj);
                *dest = text ? text : "";
            }
        },
        &up_next_seen_on_count);

    JobQueueStateTestAccess::deliver_status(jqs, status_with(3));
    CHECK(lv_subject_get_int(count) == 3);
    // Up-next line follows the FIRST entry (display name, extension stripped)
    CHECK(std::string(lv_subject_get_string(up_next)) == "Up next: file-0 (+2)");
    CHECK(up_next_seen_on_count == "Up next: file-0 (+2)");

    JobQueueStateTestAccess::deliver_status(jqs, status_with(1));
    CHECK(lv_subject_get_int(count) == 1);
    CHECK(std::string(lv_subject_get_string(up_next)) == "Up next: file-0");
    CHECK(up_next_seen_on_count == "Up next: file-0");

    JobQueueStateTestAccess::deliver_status(jqs, status_with(0));
    CHECK(lv_subject_get_int(count) == 0);
    CHECK(std::string(lv_subject_get_string(up_next)).empty());
    CHECK(up_next_seen_on_count.empty());

    lv_observer_remove(count_observer);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "job_queue_run_state follows queue_state and settles before the count",
                 "[job_queue][subjects]") {
    JobQueueState jqs(nullptr, nullptr);
    ScopedJobQueueSubjects subject_guard;
    jqs.init_subjects();

    lv_subject_t* run_state = lv_xml_get_subject(nullptr, "job_queue_run_state");
    lv_subject_t* count = lv_xml_get_subject(nullptr, "job_queue_count");
    REQUIRE(run_state != nullptr);
    REQUIRE(count != nullptr);

    auto paused = status_with(2);
    paused.queue_state = "paused";
    JobQueueStateTestAccess::deliver_status(jqs, paused);
    CHECK(lv_subject_get_int(run_state) == static_cast<int>(QueueRunState::Paused));
    CHECK(lv_subject_get_int(run_state) == 3);

    JobQueueStateTestAccess::deliver_status(jqs, status_with(2));
    CHECK(lv_subject_get_int(run_state) == 0);
    CHECK(lv_subject_get_int(count) == 2);
}

TEST_CASE_METHOD(LVGLUITestFixture, "a count observer already reads the new run state",
                 "[job_queue][subjects]") {
    JobQueueState jqs(nullptr, nullptr);
    ScopedJobQueueSubjects subject_guard;
    jqs.init_subjects();

    lv_subject_t* count = lv_xml_get_subject(nullptr, "job_queue_count");
    REQUIRE(count != nullptr);

    int run_state_seen = -1;
    lv_observer_t* obs = lv_subject_add_observer(
        count,
        [](lv_observer_t* observer, lv_subject_t*) {
            auto* dest = static_cast<int*>(lv_observer_get_user_data(observer));
            *dest = lv_subject_get_int(lv_xml_get_subject(nullptr, "job_queue_run_state"));
        },
        &run_state_seen);

    JobQueueStateTestAccess::deliver_status(jqs, status_with(3)); // "ready"
    CHECK(run_state_seen == static_cast<int>(QueueRunState::Ready));

    lv_observer_remove(obs);
}

TEST_CASE_METHOD(LVGLUITestFixture, "invalidate keeps has_ever_loaded", "[job_queue][subjects]") {
    JobQueueState jqs(nullptr, nullptr);
    ScopedJobQueueSubjects subject_guard;
    jqs.init_subjects();

    CHECK_FALSE(jqs.has_ever_loaded());
    JobQueueStateTestAccess::deliver_status(jqs, status_with(1));
    CHECK(jqs.is_loaded());
    CHECK(jqs.has_ever_loaded());

    jqs.invalidate();
    CHECK_FALSE(jqs.is_loaded());
    CHECK(jqs.has_ever_loaded());
}
