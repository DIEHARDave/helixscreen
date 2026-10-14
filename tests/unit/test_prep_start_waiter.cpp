// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_prep_start_waiter.cpp
 * @brief A start reads print-select's one prep manager only for its own file.
 *
 * Run with: ./build/bin/helix-tests "[start_waiter]"
 *
 * The manager caches one file's operations scan, and start_print() rewrites
 * the G-code by that scan's line numbers. when_ready_to_start() answers only
 * for the waiter's own path, re-scans once when the file view's scan of
 * another file pre-empts it and gives up after a second; start_print() refuses
 * a stored scan of any other path.
 */

#include "ui_print_preparation_manager.h"
#include "ui_update_queue.h"

#include "../test_helpers/delayed_file_transfers.h"
#include "../test_helpers/moonraker_client_mock_test_access.h"
#include "../test_helpers/planted_gcode.h"
#include "../test_helpers/print_select_panel_fixture.h"
#include "../test_helpers/print_start_grace.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/queued_job_starter_test_access.h"
#include "../test_helpers/queued_start_fixture.h"
#include "../test_helpers/scoped_macros_read.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "gcode_ops_detector.h"
#include "printer_state.h"
#include "queued_job_start.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::PlantedGcode;
using helix::PrinterStopCheck;
using helix::ui::PrePrintOptionState;
using helix::ui::PrintPreparationManager;
using Outcome = helix::queue::QueuedJobStarter::Outcome;

namespace {

constexpr const char* kPlainFile = "; filament_type = PLA\n"
                                   "G28\n"
                                   "; filament used [g] = 1.50\n";

class WaiterFixture : private helix::PrintSelectGlobalStateReset, public LVGLUITestFixture {
  public:
    WaiterFixture() : client(MoonrakerClientMock::PrinterType::VORON_24, 100.0) {
        client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<DelayedMoonrakerAPIMock>(client, get_printer_state(), transfers);
        manager = std::make_unique<PrintPreparationManager>();
        manager->set_dependencies(api.get(), &get_printer_state());
        get_printer_state().set_klippy_state_sync(helix::KlippyState::READY);
        settle();
    }

    ~WaiterFixture() override {
        starter.reset();
        auto& ps = get_printer_state();
        if (ps.print_state().has_preparing_job()) {
            ps.print_state().retire_preparing(helix::PreparingExit::Superseded);
        }
        helix::test::set_wire_state(ps, helix::PrintJobState::STANDBY);
        settle();
        manager.reset();
        api.reset();
        client.disconnect();
    }

    static void settle() {
        for (int i = 0; i < 6; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    }

    /// when_ready_to_start(@p name, @p dir), recording its answer.
    void wait(const std::string& name, const std::string& dir = {}) {
        answers = 0;
        answer.reset();
        manager->when_ready_to_start(name, dir, [this](std::optional<PrinterStopCheck> check) {
            ++answers;
            answer = check;
        });
    }

    void release_scans() {
        transfers.release_held_partials();
        settle();
    }

    /// A starter driving this fixture's manager, its start answered at once.
    void make_starter() {
        helix::test::wait_out_print_start_grace();
        helix::MoonrakerClientMockTestAccess::set_method_handler(
            client, "printer.print.start",
            [](MoonrakerClientMock*, const json&, std::function<void(const json&)> success_cb,
               std::function<void(const MoonrakerError&)>) -> bool {
                success_cb(json{{"result", "ok"}});
                return true;
            });
        starter = std::make_unique<helix::queue::QueuedJobStarter>(get_printer_state(), api.get());
        QueuedJobStarterTestAccess::set_prep(*starter, manager.get());
    }

    helix::test::ScopedMacrosRead macros;
    MoonrakerClientMock client;
    DelayedFileTransfers transfers{client, ""};
    std::unique_ptr<DelayedMoonrakerAPIMock> api;
    std::unique_ptr<PrintPreparationManager> manager;
    std::unique_ptr<helix::queue::QueuedJobStarter> starter;
    int answers = 0;
    std::optional<PrinterStopCheck> answer;
};

} // namespace

TEST_CASE_METHOD(WaiterFixture, "a file whose scan is held answers at once",
                 "[start_waiter][queued_job_start]") {
    PlantedGcode a("waiter_a.gcode", "", kPlainFile);
    manager->scan_file_for_operations(a.name(), "");
    settle();
    REQUIRE(manager->has_printer_stop_answer_for(a.name()));
    const int reads = transfers.partial_read_count;

    wait(a.name());
    CHECK(answers == 1);
    REQUIRE(answer.has_value());
    CHECK(answer->state == PrinterStopCheck::State::Clean);
    CHECK(transfers.partial_read_count == reads); // no second read
}

TEST_CASE_METHOD(WaiterFixture, "a same-named file in another directory is not this file's scan",
                 "[start_waiter][queued_job_start]") {
    // The mock serves a partial read by basename, so one planted file answers
    // for both directories; the scan keys are what tell them apart.
    PlantedGcode part("part.gcode", "", kPlainFile);
    manager->scan_file_for_operations("part.gcode", "waiter_dir_one");
    settle();
    REQUIRE(manager->has_scan_result_for("part.gcode"));

    transfers.hold_partials = true;
    const int reads = transfers.partial_read_count;
    wait("part.gcode", "waiter_dir_two");
    CHECK(answers == 0);
    CHECK(transfers.partial_read_count == reads + 1); // dir two read for itself

    release_scans();
    CHECK(answers == 1);
    CHECK(answer.has_value());
}

TEST_CASE_METHOD(WaiterFixture, "another file's scan pre-empting the wait is re-scanned once",
                 "[start_waiter][queued_job_start]") {
    PlantedGcode a("waiter_a.gcode", "", kPlainFile);
    PlantedGcode b("waiter_b.gcode", "", kPlainFile);
    PlantedGcode c("waiter_c.gcode", "", kPlainFile);
    transfers.hold_partials = true;

    wait(a.name());
    manager->scan_file_for_operations(b.name(), ""); // the file view opens B
    release_scans();
    // B's answer is not A's: A is read again and the wait goes on.
    CHECK(answers == 0);
    CHECK(transfers.partial_read_count == 3);

    SECTION("A's own answer then ends it") {
        release_scans();
        CHECK(answers == 1);
        REQUIRE(answer.has_value());
        CHECK(manager->has_scan_result_for(a.name()));
    }
    SECTION("a second pre-emption gives up") {
        manager->scan_file_for_operations(c.name(), "");
        release_scans();
        CHECK(answers == 1);
        CHECK_FALSE(answer.has_value());
    }
}

TEST_CASE_METHOD(WaiterFixture, "the wait holds while the PRINT_START analysis runs",
                 "[start_waiter][queued_job_start]") {
    PlantedGcode a("waiter_a.gcode", "", kPlainFile);
    manager->scan_file_for_operations(a.name(), "");
    settle();

    // A reconnect starts the analysis; its config listing is held.
    auto* connection = get_printer_state().network_state().get_printer_connection_state_subject();
    lv_subject_set_int(connection, static_cast<int>(ConnectionState::DISCONNECTED));
    settle();
    client.defer_next("server.files.list");
    lv_subject_set_int(connection, static_cast<int>(ConnectionState::CONNECTED));
    settle();
    REQUIRE(manager->is_macro_analysis_in_progress());

    wait(a.name());
    CHECK(answers == 0);

    client.fire_deferred("server.files.list");
    // A failed read retries on a timer; let any retry run to its end.
    for (int i = 0; i < 4 && manager->is_macro_analysis_in_progress(); ++i) {
        settle();
        process_lvgl(2100);
    }
    settle();
    REQUIRE_FALSE(manager->is_macro_analysis_in_progress());
    CHECK(answers == 1);
}

TEST_CASE_METHOD(WaiterFixture, "a start's option override comes before the attached provider",
                 "[start_waiter][queued_job_start]") {
    manager->set_option_state_provider([](const std::string& id) { return id == "qgl" ? 1 : -1; });
    manager->set_start_option_override([](const std::string& id) { return id == "qgl" ? 0 : -1; });
    CHECK(manager->get_option_state("qgl") == PrePrintOptionState::DISABLED);

    manager->set_start_option_override([](const std::string&) { return -1; });
    CHECK(manager->get_option_state("qgl") == PrePrintOptionState::ENABLED);

    manager->set_start_option_override(nullptr);
    CHECK(manager->get_option_state("qgl") == PrePrintOptionState::ENABLED);
}

TEST_CASE_METHOD(WaiterFixture, "start_print refuses a stored scan of another file",
                 "[start_waiter][queued_job_start]") {
    gcode::ScanResult scan;
    gcode::DetectedOperation op;
    op.type = gcode::OperationType::BED_MESH;
    op.embedding = gcode::OperationEmbedding::DIRECT_COMMAND;
    op.line_number = 2;
    scan.operations.push_back(op);
    manager->set_cached_scan_result(scan, "other.gcode");

    std::optional<bool> ok;
    std::string error;
    manager->start_print(
        "part.gcode", "", []() {},
        [&ok, &error](bool success, const std::string& e) {
            ok = success;
            error = e;
        });
    settle();

    REQUIRE(ok.has_value());
    CHECK_FALSE(*ok);
    CHECK(error.find("another file") != std::string::npos);
    CHECK(client.call_count("printer.print.start") == 0);
    CHECK(client.call_count("printer.gcode.script") == 0);

    SECTION("its own file's scan is used") {
        manager->set_cached_scan_result(scan, "part.gcode");
        ok.reset();
        manager->start_print(
            "part.gcode", "", []() {}, [&ok](bool success, const std::string&) { ok = success; });
        settle();
        // Past the check: the start went on to the printer.
        CHECK(client.call_count("printer.print.start") + client.call_count("printer.gcode.script") >
              0);
    }
}

TEST_CASE_METHOD(WaiterFixture,
                 "a tile start waiting on its scan never prints with the view's file",
                 "[start_waiter][queued_job_start]") {
    PlantedGcode a("tile_a.gcode", "", kPlainFile);
    PlantedGcode b("tile_b.gcode", "", kPlainFile);
    PlantedGcode c("tile_c.gcode", "", kPlainFile);
    make_starter();
    transfers.hold_partials = true;

    std::optional<Outcome> outcome;
    helix::test::WarningLog warnings;
    starter->start_with_options(helix::JobQueueEntry{"0001", a.name(), 0.0, 0.0}, {},
                                [&outcome](Outcome o) { outcome = o; });
    settle();
    REQUIRE(starter->in_flight());
    REQUIRE_FALSE(outcome.has_value());

    manager->scan_file_for_operations(b.name(), ""); // the file view opens B
    release_scans();
    CHECK_FALSE(outcome.has_value()); // B's scan answered; A is read again
    CHECK(client.call_count("printer.print.start") == 0);

    SECTION("A's own scan then starts A") {
        release_scans();
        REQUIRE(outcome.has_value());
        CHECK(*outcome == Outcome::Started);
        CHECK(client.call_count("printer.print.start") == 1);
        CHECK(manager->has_scan_result_for(a.name()));
    }
    SECTION("a second pre-emption refuses with a message and starts nothing") {
        manager->scan_file_for_operations(c.name(), "");
        release_scans();
        REQUIRE(outcome.has_value());
        CHECK(*outcome == Outcome::NotStarted);
        CHECK(warnings.contains("Could not check"));
        CHECK(client.call_count("printer.print.start") == 0);
        CHECK_FALSE(starter->in_flight());
    }
}
