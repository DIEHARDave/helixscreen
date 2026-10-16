// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_preparation_manager.h"

#include "../test_helpers/print_select_panel_fixture.h"
#include "../test_helpers/print_select_panel_test_access.h"
#include "../test_helpers/scoped_env.h"
#include "three_mf_reader.h"
#include "thumbnail_cache.h"
#include "thumbnail_load_context.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

std::string fixture_bytes(const char* name) {
    std::string dir = __FILE__;
    const auto pos = dir.rfind("/tests/unit/");
    dir = pos != std::string::npos ? dir.substr(0, pos) + "/tests/fixtures/three_mf/"
                                   : "tests/fixtures/three_mf/";
    std::ifstream in(dir + name, std::ios::binary);
    std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    REQUIRE(!bytes.empty());
    return bytes;
}

/// The panel over MoonrakerAPIMock: its transfers read the planted archives
/// byte range by byte range, and its client lists them with no metadata.
struct ArchivePanel : PrintSelectGlobalStateReset, PrintSelectPanelFixture {
    ArchivePanel()
        : PrintSelectPanelFixture(PrintSelectFilelistHandler::Unregistered,
                                  PrintSelectVisit::Immediate, PrintSelectApi::Mock) {}

    MoonrakerAPIMock& mock() {
        return static_cast<MoonrakerAPIMock&>(*api_);
    }

    /// List the planted files, then read @p name the way its card would.
    const PrintFileData& load(const std::string& name) {
        panel_->refresh_files(true);
        drain();
        REQUIRE(PrintSelectPanelTestAccess::find_file(*panel_, name) != nullptr);
        PrintSelectPanelTestAccess::fetch_metadata(*panel_, name);
        drain();
        const PrintFileData* row = PrintSelectPanelTestAccess::find_file(*panel_, name);
        REQUIRE(row != nullptr);
        return *row;
    }
};

} // namespace

TEST_CASE_METHOD(ArchivePanel, "a .3mf without printer metadata is described by its archive",
                 "[print_select][three_mf]") {
    PlantedGcode file("Door.gcode.3mf", "", fixture_bytes("single_plate.gcode.3mf"));
    const PrintFileData& row = load(file.name());

    REQUIRE(row.archive);
    CHECK(row.archive_extract);
    REQUIRE(row.archive->plates.size() == 1);
    CHECK(row.print_time_minutes == 5238 / 60);
    CHECK(row.filament_grams == Catch::Approx(23.73f));
    CHECK(row.filament_types == std::vector<std::string>{"ABS"});
    CHECK(row.filament_colors == std::vector<std::string>{"#000000"});
    // Its render is the card's thumbnail, prescaled like any other.
    CHECK(row.thumbnail_path.find(".bin") != std::string::npos);
    // The tail (central directory and slice_info) and the render: no metascan,
    // no whole-file read.
    CHECK(mock().transfers_mock().range_reads() == 2);
    CHECK(mock().transfers_mock().download_destinations().empty());
}

TEST_CASE_METHOD(ArchivePanel, "a .3mf the printer has metadata for keeps it",
                 "[print_select][three_mf]") {
    ScopedEnv native("HELIX_MOCK_3MF_METADATA", "1");
    PlantedGcode file("Door.gcode.3mf", "", fixture_bytes("single_plate.gcode.3mf"));
    const PrintFileData& row = load(file.name());

    // The mock reports no thumbnail for it, so only the render comes from the
    // archive; the plate is the printer's to choose.
    CHECK_FALSE(row.archive_extract);
    CHECK(row.thumbnail_path.find(".bin") != std::string::npos);

    PrintSelectPanelTestAccess::select_file(*panel_, file.name());
    CHECK(PrintSelectPanelTestAccess::plate_count(*panel_) == 0);
    REQUIRE(PrintSelectPanelTestAccess::prepare_archive_start(*panel_));
    auto* prep = PrintSelectPanelTestAccess::prep_manager(*panel_);
    REQUIRE(prep);
    CHECK(prep->archive_plate_for(file.name()) == 0);
}

TEST_CASE_METHOD(ArchivePanel,
                 "the plate stepper walks the sliced plates and Print takes the shown one",
                 "[print_select][three_mf]") {
    PlantedGcode file("Set.gcode.3mf", "", fixture_bytes("multi_plate.gcode.3mf"));
    load(file.name());
    PrintSelectPanelTestAccess::select_file(*panel_, file.name());
    drain();

    CHECK(PrintSelectPanelTestAccess::plate_count(*panel_) == 2);
    CHECK(PrintSelectPanelTestAccess::plate_text(*panel_) == "Plate 1 of 2");
    const std::string first_time = PrintSelectPanelTestAccess::selected_print_time(*panel_);

    auto* prep = PrintSelectPanelTestAccess::prep_manager(*panel_);
    REQUIRE(prep);
    REQUIRE(PrintSelectPanelTestAccess::prepare_archive_start(*panel_));
    CHECK(prep->archive_plate_for(file.name()) == 2);

    PrintSelectPanelTestAccess::step_plate(*panel_, 1);
    drain();
    CHECK(PrintSelectPanelTestAccess::plate_text(*panel_) == "Plate 2 of 2");
    const PrintFileData* row = PrintSelectPanelTestAccess::find_file(*panel_, file.name());
    REQUIRE(row);
    CHECK(row->print_time_minutes == 30);
    REQUIRE(row->filament_types.size() == 2);
    CHECK(row->filament_types[1] == "PLA");
    CHECK(PrintSelectPanelTestAccess::selected_print_time(*panel_) != first_time);
    // The plate's own filaments reach the mapping card and the start.
    CHECK(PrintSelectPanelTestAccess::selected_colors(*panel_) ==
          std::vector<std::string>{"#000000", "#FF0000"});
    REQUIRE(PrintSelectPanelTestAccess::prepare_archive_start(*panel_));
    CHECK(prep->archive_plate_for(file.name()) == 5);

    // Both ways round, wrapping.
    PrintSelectPanelTestAccess::step_plate(*panel_, 1);
    CHECK(PrintSelectPanelTestAccess::plate_text(*panel_) == "Plate 1 of 2");
    PrintSelectPanelTestAccess::step_plate(*panel_, -1);
    CHECK(PrintSelectPanelTestAccess::plate_text(*panel_) == "Plate 2 of 2");
}

TEST_CASE_METHOD(ArchivePanel, "one sliced plate shows no stepper", "[print_select][three_mf]") {
    PlantedGcode file("Door.gcode.3mf", "", fixture_bytes("single_plate.gcode.3mf"));
    load(file.name());
    PrintSelectPanelTestAccess::select_file(*panel_, file.name());
    CHECK(PrintSelectPanelTestAccess::plate_count(*panel_) == 1);
    REQUIRE(PrintSelectPanelTestAccess::prepare_archive_start(*panel_));
    CHECK(PrintSelectPanelTestAccess::prep_manager(*panel_)->archive_plate_for(file.name()) == 9);
}

TEST_CASE_METHOD(ArchivePanel,
                 "a .3mf with nothing sliced, or on a board with no local copies, "
                 "does not start",
                 "[print_select][three_mf]") {
    SECTION("a project") {
        PlantedGcode file("Project.3mf", "", fixture_bytes("project.3mf"));
        load(file.name());
        PrintSelectPanelTestAccess::select_file(*panel_, file.name());
        CHECK_FALSE(PrintSelectPanelTestAccess::prepare_archive_start(*panel_));
    }
    SECTION("no local copies") {
        PlantedGcode file("Door.gcode.3mf", "", fixture_bytes("single_plate.gcode.3mf"));
        load(file.name());
        mock().transfers_mock().mock_no_local_copies();
        PrintSelectPanelTestAccess::select_file(*panel_, file.name());
        CHECK_FALSE(PrintSelectPanelTestAccess::prepare_archive_start(*panel_));
    }
}

TEST_CASE_METHOD(ArchivePanel, "a .3mf the printer converted hides behind its G-code",
                 "[print_select][three_mf][conversions]") {
    PlantedGcode archive("Door.gcode.3mf", "", fixture_bytes("single_plate.gcode.3mf"));
    PlantedGcode output("Door.gcode", "", "; converted\nG28\n");
    struct stat st {};
    REQUIRE(::stat(archive.path().c_str(), &st) == 0);

    const std::string json_path = std::filesystem::temp_directory_path().string() +
                                  "/helix_3mf_conversions_" + std::to_string(::getpid()) + ".json";
    auto write_conversions = [&](const char* state, uint64_t size) {
        std::ofstream out(json_path);
        out << R"({"conversions":[{"source":"Door.gcode.3mf","source_mtime":)"
            << std::to_string(static_cast<double>(st.st_mtime)) << R"(,"source_size":)" << size
            << R"(,"plate":9,"output":"Door.gcode","state":")" << state << R"("}]})";
    };
    ScopedEnv conversions("HELIX_MOCK_3MF_CONVERSIONS", json_path.c_str());

    auto listed = [&]() {
        panel_->refresh_files(true);
        drain();
        return PrintSelectPanelTestAccess::list_contains(*panel_, archive.name());
    };

    SECTION("done, unchanged") {
        write_conversions("done", static_cast<uint64_t>(st.st_size));
        CHECK_FALSE(listed());
        CHECK(PrintSelectPanelTestAccess::list_contains(*panel_, output.name()));
    }
    SECTION("pending") {
        write_conversions("pending", static_cast<uint64_t>(st.st_size));
        CHECK(listed());
    }
    SECTION("the archive changed since") {
        write_conversions("done", static_cast<uint64_t>(st.st_size) + 1);
        CHECK(listed());
    }
    SECTION("the output is gone") {
        write_conversions("done", static_cast<uint64_t>(st.st_size));
        output.remove_from_disk();
        CHECK(listed());
    }
    SECTION("no converter on the printer") {
        ScopedEnv none("HELIX_MOCK_3MF_CONVERSIONS", nullptr);
        CHECK(listed());
    }
    std::remove(json_path.c_str());
}

TEST_CASE_METHOD(ArchivePanel, "a second size of a .3mf render keeps the card's prescaled image",
                 "[print_select][three_mf][thumbnail]") {
    PlantedGcode file("Door.gcode.3mf", "", fixture_bytes("single_plate.gcode.3mf"));
    const PrintFileData& row = load(file.name());
    REQUIRE(row.archive);
    const std::string card = row.thumbnail_path;
    REQUIRE(card.rfind("A:", 0) == 0);
    REQUIRE(std::filesystem::exists(card.substr(2)));
    const int reads = mock().transfers_mock().range_reads();

    // Print status asks for the same render at its own, larger size.
    std::string detail;
    const auto* plate = three_mf::find_plate(*row.archive, 0);
    REQUIRE(plate);
    auto alive = std::make_shared<std::atomic<bool>>(true);
    std::atomic<uint32_t> gen{0};
    helix::fetch_thumbnail_from_archive(
        file.name(), *three_mf::plate_thumbnail(*plate), row.modified_timestamp, api_.get(),
        helix::ThumbnailProcessor::get_target_for_display(helix::ThumbnailSize::Detail),
        ThumbnailLoadContext::capture(alive, &gen),
        [&detail](const std::string& path, bool) { detail = path; }, nullptr);
    drain();

    CHECK_FALSE(detail.empty());
    CHECK(detail != card);
    CHECK(std::filesystem::exists(card.substr(2)));
    // Prescaled from the cached PNG, not read again.
    CHECK(mock().transfers_mock().range_reads() == reads);
}
