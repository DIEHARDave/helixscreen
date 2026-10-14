// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_job_queue_next_media.cpp
 * @brief JobQueueState publishes the first queued job's estimated time and
 * thumbnail, fetched once per change of first job.
 */

#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/job_queue_state_test_access.h"
#include "../test_helpers/job_queue_subjects_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "job_queue_state.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_print_state.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

const char* const kFileA = "2022-big-ben-by-miniworld3d_ABS_36m21s.gcode";
const char* const kFileB = "ECC_0.4_stand_PLA0.2_2h42m.gcode";
constexpr const char* kMetadataMethod = "server.files.metadata";

struct NextMediaFixture : public LVGLUITestFixture {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::GENERIC_COREXY};
    MoonrakerAPI mock_api{client, state()};
    JobQueueState jqs{&mock_api, &client};
    ScopedJobQueueSubjects subject_guard;

    NextMediaFixture() {
        jqs.init_subjects();
    }

    void drain() {
        for (int i = 0; i < 8; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    }

    static JobQueueStatus queue_of(std::initializer_list<const char*> files) {
        JobQueueStatus s;
        s.queue_state = "ready";
        int i = 0;
        for (const char* f : files) {
            s.queued_jobs.push_back({"job-" + std::to_string(i++), f, 1000.0, 30.0});
        }
        return s;
    }

    void deliver(std::initializer_list<const char*> files) {
        JobQueueStateTestAccess::deliver_status(jqs, queue_of(files));
        drain();
    }

    /// What the mock serves for a file, read through the same call the state makes.
    int mock_estimate(const char* file) {
        int seconds = -1;
        mock_api.files().get_file_metadata(
            file, [&](const FileMetadata& md) { seconds = static_cast<int>(md.estimated_time); },
            [](const MoonrakerError&) {});
        return seconds;
    }

    int estimate_subject() {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "job_queue_next_estimate"));
    }
    std::string thumbnail_subject() {
        return lv_subject_get_string(lv_xml_get_subject(nullptr, "job_queue_next_thumbnail"));
    }
};

} // namespace

TEST_CASE_METHOD(NextMediaFixture, "first queued job's estimate comes from its file metadata",
                 "[job_queue][next_media]") {
    const int expected = mock_estimate(kFileA);
    REQUIRE(expected > 0);

    deliver({kFileA});

    CHECK(estimate_subject() == expected);
    CHECK(jqs.next_estimated_seconds() == expected);
}

TEST_CASE_METHOD(NextMediaFixture, "a metadata answer for a job no longer first is dropped",
                 "[job_queue][next_media]") {
    const int estimate_b = mock_estimate(kFileB);
    REQUIRE(estimate_b > 0);
    REQUIRE(mock_estimate(kFileA) != estimate_b);

    SECTION("through the real request path") {
        client.defer_next(kMetadataMethod);
        deliver({kFileA});
        deliver({kFileB});
        REQUIRE(estimate_subject() == estimate_b);

        client.fire_deferred(kMetadataMethod);
        drain();

        CHECK(estimate_subject() == estimate_b);
        CHECK(jqs.next_estimated_seconds() == estimate_b);
    }

    SECTION("through a direct delivery") {
        deliver({kFileB});
        REQUIRE(estimate_subject() == estimate_b);

        FileMetadata late;
        late.estimated_time = 999.0;
        JobQueueStateTestAccess::deliver_next_metadata(jqs, kFileA, late);

        CHECK(estimate_subject() == estimate_b);
    }
}

TEST_CASE_METHOD(NextMediaFixture, "an empty queue clears the next job's media",
                 "[job_queue][next_media]") {
    deliver({kFileA});
    REQUIRE(estimate_subject() > 0);

    deliver({});

    CHECK(estimate_subject() == 0);
    CHECK(thumbnail_subject() == PrinterPrintState::no_thumbnail_placeholder());
}

TEST_CASE_METHOD(NextMediaFixture, "the thumbnail subject is never empty",
                 "[job_queue][next_media]") {
    CHECK(thumbnail_subject() == PrinterPrintState::no_thumbnail_placeholder());

    FileMetadata no_thumbs;
    no_thumbs.estimated_time = 120.0;
    deliver({kFileA});
    JobQueueStateTestAccess::deliver_next_metadata(jqs, kFileA, no_thumbs);

    CHECK(estimate_subject() == 120);
    CHECK_FALSE(thumbnail_subject().empty());
}

TEST_CASE_METHOD(NextMediaFixture, "one fetch per change of first job", "[job_queue][next_media]") {
    const int before = client.call_count(kMetadataMethod);

    deliver({kFileA});
    CHECK(client.call_count(kMetadataMethod) == before + 1);

    deliver({kFileA});
    CHECK(client.call_count(kMetadataMethod) == before + 1);

    deliver({kFileB, kFileA});
    CHECK(client.call_count(kMetadataMethod) == before + 2);
}

TEST_CASE_METHOD(NextMediaFixture, "a failed metadata read leaves the job unknown",
                 "[job_queue][next_media]") {
    client.fail_next(kMetadataMethod);

    deliver({kFileA});

    CHECK(estimate_subject() == 0);
    CHECK(thumbnail_subject() == PrinterPrintState::no_thumbnail_placeholder());
}
