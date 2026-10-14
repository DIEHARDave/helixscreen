// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_start_inputs.cpp
 * @brief The file half of the pre-start checks, computed with no file view.
 *
 * Run with: ./build/bin/helix-tests "[print_start_inputs]"
 *
 * A queued job started from the home tile gets its tool info, mapping, remap
 * and empty-slot pre-flight from fill_file_context() and preflight_for(). The
 * parity case pins that they answer what an open PrintSelectDetailView answers
 * for the same file with no mapping edit; the loader cases pin the metadata,
 * footer and whole-file scan steps the file view shares.
 */

#include "ui_callback_helpers.h"
#include "ui_print_select_detail_view.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/planted_gcode.h"
#include "../test_helpers/scoped_env.h"
#include "../test_helpers/scoped_mock_ams.h"
#include "app_globals.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "print_start_checks.h"
#include "print_start_inputs.h"
#include "printer_state.h"
#include "tools_used_cache.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::FileStartFacts;
using helix::FileStartFactsLoader;

namespace {

void noop_cb(lv_event_t* /*e*/) {}

/// A tools-used cache directory of this test's own.
struct CacheDir {
    helix::ScopedEnv restore{"HELIX_CACHE_DIR"};
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                ("print_start_inputs_" + std::to_string(::getpid()));
    CacheDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir);
        ::setenv("HELIX_CACHE_DIR", dir.c_str(), 1);
    }
    ~CacheDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

FileStartFacts two_tool_facts() {
    FileStartFacts facts;
    facts.filename = "sub/two_tool.gcode";
    facts.palette.colors = {"#FF0000", "#0000FF"};
    facts.palette.materials = {"PLA", "PETG"};
    facts.tools_used = {0, 1};
    return facts;
}

void check_same_mappings(const std::vector<helix::ToolMapping>& a,
                         const std::vector<helix::ToolMapping>& b) {
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].tool_index == b[i].tool_index);
        CHECK(a[i].mapped_slot == b[i].mapped_slot);
        CHECK(a[i].mapped_backend == b[i].mapped_backend);
        CHECK(a[i].is_auto == b[i].is_auto);
        CHECK(a[i].material_mismatch == b[i].material_mismatch);
    }
}

void check_same_tools(const std::vector<helix::GcodeToolInfo>& a,
                      const std::vector<helix::GcodeToolInfo>& b) {
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].tool_index == b[i].tool_index);
        CHECK(a[i].color_rgb == b[i].color_rgb);
        CHECK(a[i].material == b[i].material);
    }
}

void drain() {
    for (int i = 0; i < 8; ++i) {
        helix::ui::UpdateQueue::instance().drain();
    }
}

} // namespace

TEST_CASE("file_palette_from_metadata splits materials and copies colors", "[print_start_inputs]") {
    FileMetadata metadata;
    metadata.filament_type = "PLA;PLA;PETG";
    metadata.filament_colors = {"#FF0000", "#00FF00", "#0000FF"};

    const auto palette = helix::file_palette_from_metadata(metadata);
    CHECK(palette.materials == std::vector<std::string>{"PLA", "PLA", "PETG"});
    CHECK(palette.colors == metadata.filament_colors);

    SECTION("an empty type gives no materials") {
        metadata.filament_type.clear();
        CHECK(helix::file_palette_from_metadata(metadata).materials.empty());
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "fill_file_context matches the file view",
                 "[print_start_inputs][detail_view]") {
    CacheDir cache;
    helix::test::ScopedMockAms ams({{0xFF0000, "PLA"}, {0x0000FF, "PETG"}});

    register_xml_callbacks({
        {"on_print_select_detail_backdrop", noop_cb},
        {"on_print_select_print_button", noop_cb},
        {"on_print_select_delete_button", noop_cb},
        {"on_print_detail_back_clicked", noop_cb},
        {"on_toggle_sliced_colors", noop_cb},
        {"on_print_select_detail_objects", noop_cb},
    });
    helix::ui::PrintSelectDetailView view;
    view.init_subjects();
    REQUIRE(view.create(test_screen()) != nullptr);
    struct CloseOnExit {
        helix::ui::PrintSelectDetailView& v;
        ~CloseOnExit() {
            v.hide();
            helix::ui::UpdateQueue::instance().drain();
            lv_timer_handler(); // the close callback runs on the next tick
        }
    } closer{view};

    const FileStartFacts facts = two_tool_facts();
    constexpr size_t kSize = 2468;
    constexpr time_t kMtime = 1357;
    // A warmed cache renders the authoritative state inside show().
    helix::ToolsUsedCache().store(facts.filename, kSize, kMtime, facts.tools_used);
    view.show("two_tool.gcode", "sub", "PLA", facts.palette.colors, facts.palette.materials, kSize,
              kMtime);
    view.recompute_preflight();
    REQUIRE(view.get_tools_used() == facts.tools_used);

    helix::PrintStartContext ctx;
    helix::fill_file_context(ctx, facts);

    CHECK(ctx.has_file_facts);
    // The card is showing, so both the card's copy and the effective one agree.
    REQUIRE_FALSE(view.get_filament_mappings().empty());
    check_same_mappings(ctx.mappings, view.get_filament_mappings());
    check_same_mappings(ctx.mappings, view.effective_mappings());
    check_same_tools(ctx.tool_info, view.get_filament_tool_info());
    check_same_tools(ctx.tool_info, view.get_used_tool_info());
    CHECK(ctx.effective_remap == view.get_effective_remap());
    CHECK(ctx.filament_materials == view.get_filament_materials());
    CHECK(ctx.available_slots.size() == view.get_available_slots().size());

    const auto ours = helix::preflight_for(facts);
    const auto theirs = view.preflight_result();
    REQUIRE(ours.checks.size() == theirs.checks.size());
    REQUIRE_FALSE(ours.checks.empty());
    for (size_t i = 0; i < ours.checks.size(); ++i) {
        CHECK(ours.checks[i].tool_index == theirs.checks[i].tool_index);
        CHECK(ours.checks[i].mapped_slot == theirs.checks[i].mapped_slot);
        CHECK(ours.checks[i].severity == theirs.checks[i].severity);
    }
    CHECK(ours.has_block() == theirs.has_block());
}

TEST_CASE_METHOD(LVGLUITestFixture, "fill_file_context feeds the material mismatch gate",
                 "[print_start_inputs]") {
    // Two lanes so the card would show: a mapping exists to be checked.
    helix::test::ScopedMockAms ams({{0xFF0000, "PLA"}, {0x00FF00, "PLA"}});

    FileStartFacts facts;
    facts.filename = "petg.gcode";
    facts.palette.colors = {"#FF0000"};
    facts.palette.materials = {"PETG"};
    facts.tools_used = {0};

    helix::PrintStartContext ctx;
    ctx.ams_available = true;
    ctx.has_active_backend = true;
    ctx.ams_manages_filament = true;
    ctx.filament_color_count = facts.palette.colors.size();
    helix::fill_file_context(ctx, facts);

    REQUIRE(ctx.has_file_facts);
    CHECK_FALSE(helix::material_mismatches_in(ctx).empty());

    SECTION("without file facts the gate has nothing to compare") {
        ctx.has_file_facts = false;
        CHECK(helix::material_mismatches_in(ctx).empty());
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "fill_file_context leaves no mapping on a printer with no AMS",
                 "[print_start_inputs]") {
    // A full-profile palette on a plain printer: the file view's card is
    // hidden, so the unresolved-tools gate must find no mapping to judge.
    FileStartFacts facts;
    facts.filename = "plain.gcode";
    facts.palette.colors = {"#FF0000", "#00FF00"};
    facts.palette.materials = {"PLA", "PLA"};
    facts.tools_used = {0};

    helix::PrintStartContext ctx;
    ctx.filament_color_count = facts.palette.colors.size();
    helix::fill_file_context(ctx, facts);
    CHECK(ctx.mappings.empty());
    CHECK(helix::unresolved_tools_in(ctx).empty());
}

// ============================================================================
// FileStartFactsLoader
// ============================================================================

namespace {

constexpr const char* kTwoToolFooter = "; filament_type = PLA;PETG\n"
                                       "; filament_colour = #FF0000;#0000FF\n"
                                       "G28\n"
                                       "T0\n"
                                       "; filament used [g] = 1.50, 2.25\n";

struct LoaderFixture : LVGLUITestFixture {
    LoaderFixture() : client(MoonrakerClientMock::PrinterType::VORON_24) {
        client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPIMock>(client, get_printer_state());
    }
    ~LoaderFixture() override {
        drain();
        api.reset();
        client.disconnect();
    }

    CacheDir cache;
    MoonrakerClientMock client;
    std::unique_ptr<MoonrakerAPIMock> api;
};

} // namespace

TEST_CASE_METHOD(LoaderFixture, "a footer that names its tools answers without a download",
                 "[print_start_inputs][loader]") {
    helix::PlantedGcode file("loader_footer.gcode", "", kTwoToolFooter);
    FileStartFactsLoader loader(api.get());

    int downloads = 0;
    std::optional<FileStartFacts> answer;
    loader.load(
        file.name(), [&answer](FileStartFacts facts) { answer = std::move(facts); },
        [&downloads](std::function<void(bool, const std::string&)> done) {
            ++downloads;
            done(false, {});
        });
    drain();

    REQUIRE(answer.has_value());
    CHECK(answer->filename == file.name());
    CHECK(answer->metadata.has_value());
    CHECK(answer->tools_used == std::set<int>{0, 1});
    CHECK(answer->palette.colors.size() == 2);
    CHECK(answer->palette.materials == std::vector<std::string>{"PLA", "PETG"});
    CHECK(answer->tool_grams == std::vector<double>{1.50, 2.25});
    CHECK(downloads == 0);
}

TEST_CASE_METHOD(LoaderFixture, "a failed metadata read still answers",
                 "[print_start_inputs][loader]") {
    helix::PlantedGcode file("loader_nometa.gcode", "", kTwoToolFooter);
    FileStartFactsLoader loader(api.get());
    client.fail_next("server.files.metadata");

    std::optional<FileStartFacts> answer;
    loader.load(
        file.name(), [&answer](FileStartFacts facts) { answer = std::move(facts); },
        [](std::function<void(bool, const std::string&)> done) { done(false, {}); });
    drain();

    REQUIRE(answer.has_value());
    CHECK_FALSE(answer->metadata.has_value());
    // The footer still names the tools and their colors.
    CHECK(answer->tools_used == std::set<int>{0, 1});
    CHECK(answer->palette.colors.size() == 2);
}

TEST_CASE_METHOD(LoaderFixture, "a later load supersedes an unanswered one",
                 "[print_start_inputs][loader]") {
    helix::PlantedGcode first("loader_first.gcode", "", kTwoToolFooter);
    helix::PlantedGcode second("loader_second.gcode", "", kTwoToolFooter);
    FileStartFactsLoader loader(api.get());

    std::vector<std::string> answered;
    auto record = [&answered](FileStartFacts facts) { answered.push_back(facts.filename); };
    client.defer_next("server.files.metadata");
    loader.load(first.name(), record);
    loader.load(second.name(), record);
    drain();
    client.fire_deferred("server.files.metadata");
    drain();

    CHECK(answered == std::vector<std::string>{second.name()});
}
