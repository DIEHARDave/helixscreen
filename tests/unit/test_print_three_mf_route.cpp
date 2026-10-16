// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_filename_utils.h"
#include "ui_print_preparation_manager.h"
#include "ui_print_start_controller.h"

#include "../helix_test_fixture.h"
#include "../test_helpers/mock_printer.h"
#include "../test_helpers/planted_gcode.h"
#include "../test_helpers/print_preparation_manager_test_access.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/scoped_env.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "moonraker_api_mock.h"
#include "printer_state.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

namespace {

std::string fixture_dir() {
    std::string dir = __FILE__;
    const auto pos = dir.rfind("/tests/unit/");
    return pos != std::string::npos ? dir.substr(0, pos) + "/tests/fixtures/three_mf/"
                                    : "tests/fixtures/three_mf/";
}

std::string disk_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void drain_until_quiet() {
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance(), 16);
}

constexpr const char* kArchive = "Door.gcode.3mf";

struct ArchiveStart : HelixTestFixture {
    ArchiveStart() {
        lv_init_safe();
        // The success path names the print on the GLOBAL state.
        PrinterStateTestAccess::reset(get_printer_state());
        get_printer_state().init_subjects(false);
        manager.set_dependencies(&printer.api, &printer.state);
        printer.state.print_state().begin_preparing(PrintJobRef{kArchive, "", ""});
    }

    void plant(const char* fixture) {
        planted.emplace(kArchive, "", disk_bytes(fixture_dir() + fixture));
    }

    void start() {
        manager.start_print(
            kArchive, "", [this]() { ++navigated; },
            [this](bool ok, const std::string&) {
                completed = true;
                succeeded = ok;
            });
        drain_until_quiet();
    }

    MockPrinter printer;
    PrintPreparationManager manager;
    std::optional<PlantedGcode> planted;
    int navigated = 0;
    bool completed = false;
    bool succeeded = false;
};

} // namespace

TEST_CASE_METHOD(ArchiveStart, "a .3mf plate prints as staged G-code that maps back to the archive",
                 "[print_preparation][three_mf]") {
    plant("single_plate.gcode.3mf");
    manager.set_archive_plate(kArchive, 9);
    start();

    const auto& uploads = printer.api.transfers_mock().path_uploads();
    REQUIRE(uploads.size() == 1);
    CHECK(uploads[0].root == "gcodes");
    const std::string& staged = uploads[0].dest_path;
    INFO(staged);
    CHECK(staged.compare(staged.size() - 6, 6, ".gcode") == 0);
    CHECK(gcode::is_uploaded_rewrite_path(staged));
    CHECK(gcode::trusted_original_path(staged) == kArchive);
    CHECK(gcode::rewritten_archive_plate(staged) == 9);
    // What left the machine is the plate, not the archive.
    CHECK(uploads[0].content == disk_bytes(fixture_dir() + "src/plate.gcode"));

    // Klipper is asked for the staged copy, never through the plugin.
    REQUIRE(printer.api.job_mock().started_prints().size() == 1);
    CHECK(printer.api.job_mock().started_prints()[0] == staged);
    CHECK(printer.api.job_mock().modified_prints().empty());

    // Neither the archive nor the extracted plate outlives the flow.
    const auto& downloads = printer.api.transfers_mock().download_destinations();
    REQUIRE(downloads.size() == 1);
    CHECK_FALSE(std::filesystem::exists(downloads[0]));
    CHECK_FALSE(std::filesystem::exists(uploads[0].local_path));
}

TEST_CASE_METHOD(ArchiveStart, "a .3mf the printer reads natively starts as itself",
                 "[print_preparation][three_mf]") {
    plant("single_plate.gcode.3mf");
    SECTION("never marked") {}
    SECTION("marked, then forgotten") {
        manager.set_archive_plate(kArchive, 9);
        manager.set_archive_plate(kArchive, 0);
    }
    SECTION("another file marked") {
        manager.set_archive_plate("Other.gcode.3mf", 2);
    }
    start();

    CHECK(printer.api.transfers_mock().path_uploads().empty());
    CHECK(printer.api.transfers_mock().download_destinations().empty());
    REQUIRE(printer.api.job_mock().started_prints().size() == 1);
    CHECK(printer.api.job_mock().started_prints()[0] == kArchive);
}

TEST_CASE_METHOD(ArchiveStart, "a plate whose checksum fails is not uploaded or started",
                 "[print_preparation][three_mf]") {
    plant("bad_md5.gcode.3mf");
    manager.set_archive_plate(kArchive, 9);
    start();

    CHECK(printer.api.transfers_mock().path_uploads().empty());
    CHECK(printer.api.job_mock().started_prints().empty());
    CHECK(completed);
    CHECK_FALSE(succeeded);
    CHECK_FALSE(printer.state.print_state().has_preparing_job());
    const auto& downloads = printer.api.transfers_mock().download_destinations();
    REQUIRE(downloads.size() == 1);
    CHECK_FALSE(std::filesystem::exists(downloads[0]));
}

TEST_CASE_METHOD(ArchiveStart, "a plate the archive does not hold fails the start",
                 "[print_preparation][three_mf]") {
    plant("multi_plate.gcode.3mf");
    manager.set_archive_plate(kArchive, 9); // plates 2 and 5 only
    start();
    CHECK(printer.api.transfers_mock().path_uploads().empty());
    CHECK(printer.api.job_mock().started_prints().empty());
    CHECK_FALSE(succeeded);
}

TEST_CASE_METHOD(ArchiveStart, "the chosen plate of several is the one that prints",
                 "[print_preparation][three_mf]") {
    plant("multi_plate.gcode.3mf");
    manager.set_archive_plate(kArchive, 5);
    start();
    const auto& uploads = printer.api.transfers_mock().path_uploads();
    REQUIRE(uploads.size() == 1);
    CHECK(gcode::rewritten_archive_plate(uploads[0].dest_path) == 5);
    CHECK(uploads[0].content.find("total layer number: 90") != std::string::npos);
}

TEST_CASE_METHOD(ArchiveStart, "a .3mf plate on a transport without local copies is refused",
                 "[print_preparation][three_mf]") {
    plant("single_plate.gcode.3mf");
    printer.api.transfers_mock().mock_no_local_copies();
    manager.set_dependencies(&printer.api, &printer.state);
    manager.set_archive_plate(kArchive, 9);
    start();

    CHECK(printer.api.transfers_mock().download_destinations().empty());
    CHECK(printer.api.job_mock().started_prints().empty());
    CHECK_FALSE(succeeded);
    CHECK_FALSE(printer.state.print_state().has_preparing_job());
}

TEST_CASE_METHOD(ArchiveStart, "a modified .3mf plate starts as itself even with the plugin",
                 "[print_preparation][three_mf]") {
    plant("single_plate.gcode.3mf");
    // The plugin would link the copy under the archive's name, which Klipper
    // does not print.
    printer.state.set_helix_plugin_installed(true);
    drain_until_quiet();
    REQUIRE(printer.state.plugin_status_state().service_has_helix_plugin());
    manager.set_archive_plate(kArchive, 9);
    manager.set_cached_scan_result(gcode::ScanResult{}, kArchive);
    PrintPreparationManagerTestAccess::modify_and_print(manager, kArchive);
    drain_until_quiet();

    const auto& uploads = printer.api.transfers_mock().path_uploads();
    REQUIRE(uploads.size() == 1);
    CHECK(gcode::rewritten_archive_plate(uploads[0].dest_path) == 9);
    CHECK(printer.api.job_mock().modified_prints().empty());
    REQUIRE(printer.api.job_mock().started_prints().size() == 1);
    CHECK(printer.api.job_mock().started_prints()[0] == uploads[0].dest_path);
}

TEST_CASE_METHOD(ArchiveStart, "a plate the printer already converted prints from its output",
                 "[print_preparation][three_mf]") {
    plant("single_plate.gcode.3mf");
    manager.set_archive_plate(kArchive, 9, "Door_plate9.gcode");
    CHECK(manager.archive_plate_for(kArchive) == 0);
    CHECK(manager.printable_path(kArchive) == "Door_plate9.gcode");
    start();

    // Nothing is fetched, extracted or uploaded: the output starts as it is.
    CHECK(printer.api.transfers_mock().download_destinations().empty());
    CHECK(printer.api.transfers_mock().path_uploads().empty());
    REQUIRE(printer.api.job_mock().started_prints().size() == 1);
    CHECK(printer.api.job_mock().started_prints()[0] == "Door_plate9.gcode");
}

TEST_CASE_METHOD(ArchiveStart, "staged copies go where the printer's Klipper can print them",
                 "[print_preparation][three_mf][staging]") {
    plant("single_plate.gcode.3mf");
    const bool forgex = GENERATE(true, false);
    CAPTURE(forgex);
    REQUIRE(printer.state.profile_state().set_printer_type(
        forgex ? "FlashForge Adventurer 5M (ForgeX)" : "FlashForge Adventurer 5M", true, false,
        false));
    const std::string root = forgex ? "helix_staging/modified_" : ".helix_temp/modified_";
    manager.set_archive_plate(kArchive, 9);
    start();
    const auto& uploads = printer.api.transfers_mock().path_uploads();
    REQUIRE(uploads.size() == 1);
    CHECK(uploads[0].dest_path.rfind(root, 0) == 0);
    REQUIRE(printer.api.job_mock().started_prints().size() == 1);
    CHECK(printer.api.job_mock().started_prints()[0] == uploads[0].dest_path);
}

TEST_CASE_METHOD(ArchiveStart,
                 "the plugin's own link directory decides the staging over the database",
                 "[print_preparation][three_mf][staging]") {
    plant("single_plate.gcode.3mf");
    // A stock entry in the database, but the plugin reports a visible link
    // directory: it found the dot-directory filter on this printer itself.
    REQUIRE(printer.state.profile_state().set_printer_type("FlashForge Adventurer 5M", true, false,
                                                           false));
    printer.state.set_helix_plugin_installed(true);
    printer.state.set_helix_plugin_link_dir("helix_print");
    drain_until_quiet();
    REQUIRE(printer.state.plugin_status_state().link_dir() == "helix_print");

    manager.set_archive_plate(kArchive, 9);
    start();
    const auto& uploads = printer.api.transfers_mock().path_uploads();
    REQUIRE(uploads.size() == 1);
    CHECK(uploads[0].dest_path.rfind("helix_staging/modified_", 0) == 0);
    // Its prints resolve back through that directory too.
    CHECK(gcode::resolve_gcode_filename("helix_print/full/Door.gcode.3mf") == "Door.gcode.3mf");
    printer.state.set_helix_plugin_link_dir("");
    drain_until_quiet();
}

TEST_CASE("the plugin's status carries its link directory", "[print_preparation][staging][api]") {
    MockPrinter printer;
    ScopedEnv installed("HELIX_MOCK_HELIX_PLUGIN", "1");
    std::optional<HelixPluginInfo> info;
    SECTION("a visible one") {
        ScopedEnv dir("HELIX_MOCK_HELIX_PLUGIN_LINK_DIR", "helix_print");
        printer.api.job().check_helix_plugin([&info](const HelixPluginInfo& i) { info = i; },
                                             [](const MoonrakerError&) {});
        REQUIRE(info);
        CHECK(info->available);
        CHECK(info->symlink_dir == "helix_print");
    }
    SECTION("the default") {
        printer.api.job().check_helix_plugin([&info](const HelixPluginInfo& i) { info = i; },
                                             [](const MoonrakerError&) {});
        REQUIRE(info);
        CHECK(info->symlink_dir == ".helix_print");
    }
}

TEST_CASE("an extracted plate's archive is gone before the rewrite runs",
          "[print_preparation][three_mf][disk]") {
    const std::string dir = std::filesystem::temp_directory_path().string();
    const std::string zip = dir + "/helix_rewrite_archive_" + std::to_string(::getpid()) + ".3mf";
    const std::string plate = disk_bytes(fixture_dir() + "src/plate.gcode");
    auto copy_archive = [&zip]() {
        std::ofstream out(zip, std::ios::binary);
        out << disk_bytes(fixture_dir() + "single_plate.gcode.3mf");
    };

    SECTION("a rewrite that changes the plate") {
        copy_archive();
        bool archive_during_rewrite = true;
        const std::string out = PrintPreparationManagerTestAccess::rewrite_archive_plate(
            "Door.gcode.3mf", zip, 9, [&](const std::string&) {
                archive_during_rewrite = std::filesystem::exists(zip);
                return true;
            });
        CHECK_FALSE(archive_during_rewrite);
        REQUIRE_FALSE(out.empty());
        CHECK(disk_bytes(out) == plate);
        CHECK_FALSE(std::filesystem::exists(out + ".keep"));
        std::remove(out.c_str());
    }
    SECTION("a rewrite that changes nothing still uploads the plate") {
        copy_archive();
        const std::string out = PrintPreparationManagerTestAccess::rewrite_archive_plate(
            "Door.gcode.3mf", zip, 9, [](const std::string&) { return false; });
        REQUIRE_FALSE(out.empty());
        CHECK(disk_bytes(out) == plate);
        CHECK_FALSE(std::filesystem::exists(zip));
        std::remove(out.c_str());
    }
}

TEST_CASE_METHOD(ArchiveStart, "a reprint of an archive plate extracts it again",
                 "[print_preparation][three_mf][reprint]") {
    plant("multi_plate.gcode.3mf");
    std::optional<bool> ended;
    manager.start_archive_reprint(kArchive, 5,
                                  [&ended](bool ok, const std::string&) { ended = ok; });
    drain_until_quiet();
    const auto& uploads = printer.api.transfers_mock().path_uploads();
    REQUIRE(uploads.size() == 1);
    CHECK(gcode::rewritten_archive_plate(uploads[0].dest_path) == 5);
    REQUIRE(printer.api.job_mock().started_prints().size() == 1);
    CHECK(printer.api.job_mock().started_prints()[0] == uploads[0].dest_path);
    CHECK(printer.api.job_mock().started_prints()[0] != kArchive);
}

TEST_CASE_METHOD(ArchiveStart,
                 "Reprint of an archive plate goes through the extraction, not a direct start",
                 "[print_preparation][three_mf][reprint]") {
    plant("single_plate.gcode.3mf");
    PrintStartController controller{printer.state, &printer.api};
    controller.set_prep_manager(&manager);
    bool started = false;
    bool errored = false;
    controller.initiate_reprint(
        kArchive, "", {}, [&started] { started = true; }, [&errored] { errored = true; }, 9);
    drain_until_quiet();

    const auto& uploads = printer.api.transfers_mock().path_uploads();
    REQUIRE(uploads.size() == 1);
    REQUIRE(printer.api.job_mock().started_prints().size() == 1);
    CHECK(printer.api.job_mock().started_prints()[0] == uploads[0].dest_path);
    CHECK_FALSE(errored);

    SECTION("without a plate it starts the file as it is") {
        PrintStartController plain{printer.state, &printer.api};
        plain.set_prep_manager(&manager);
        plain.initiate_reprint(kArchive, "", {}, [] {}, [] {});
        drain_until_quiet();
        REQUIRE(printer.api.job_mock().started_prints().size() == 2);
        CHECK(printer.api.job_mock().started_prints()[1] == kArchive);
    }
}
