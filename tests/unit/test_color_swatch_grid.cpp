// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_color_picker.h"
#include "ui_swatch.h"

#include "../lvgl_ui_test_fixture.h"

#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

extern "C" {
#include "helix-xml/src/xml/lv_xml.h"
}

namespace {

std::vector<uint32_t> g_tapped;

/// Reads the color the way every grid consumer does: off the event target.
void record_tap(lv_event_t* e) {
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    g_tapped.push_back(lv_color_to_u32(helix::ui::swatch_color_of(target)) & 0xFFFFFF);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "color_swatch_grid: a tap reports the tapped palette color",
                 "[color_swatch]") {
    lv_xml_register_event_cb(nullptr, "test_grid_tap_cb", record_tap);
    for (const auto palette :
         {helix::ui::ColorPicker::Palette::General, helix::ui::ColorPicker::Palette::Theme}) {
        const char* component = palette == helix::ui::ColorPicker::Palette::General
                                    ? "color_swatch_grid"
                                    : "theme_swatch_grid";
        INFO(component);
        const char* attrs[] = {"swatch_callback", "test_grid_tap_cb", nullptr};
        auto* grid = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), component, attrs));
        REQUIRE(grid != nullptr);
        const auto& expected = helix::ui::swatch_palette(palette);
        REQUIRE(lv_obj_get_child_count(grid) == expected.size());

        g_tapped.clear();
        for (uint32_t i = 0; i < expected.size(); ++i) {
            lv_obj_send_event(lv_obj_get_child(grid, static_cast<int32_t>(i)), LV_EVENT_CLICKED,
                              nullptr);
        }
        CHECK(g_tapped == expected);
        lv_obj_delete(grid);
    }
}

TEST_CASE("swatch_needs_edge: a swatch too close to its surface gets an edge, in either theme",
          "[color_swatch]") {
    constexpr uint32_t DARK_SCREEN = 0x2E3440;
    constexpr uint32_t DARK_DIALOG = 0x4C566A;
    constexpr uint32_t LIGHT_SCREEN = 0xECEFF4;
    constexpr uint32_t LIGHT_DIALOG = 0xEDEFF6;
    // Dark-on-dark.
    CHECK(helix::ui::swatch_needs_edge(0x000000, DARK_SCREEN));
    CHECK(helix::ui::swatch_needs_edge(0x1A1A1A, DARK_SCREEN));
    CHECK(helix::ui::swatch_needs_edge(0x4A4A4A, DARK_SCREEN));
    CHECK(helix::ui::swatch_needs_edge(0x4A4A4A, DARK_DIALOG));
    // Light-on-light.
    CHECK(helix::ui::swatch_needs_edge(0xFFFFFF, LIGHT_SCREEN));
    CHECK(helix::ui::swatch_needs_edge(0xE8E8E8, LIGHT_DIALOG));
    CHECK(helix::ui::swatch_needs_edge(0xE0D5C7, LIGHT_SCREEN));
    // Plenty of contrast: no edge.
    CHECK_FALSE(helix::ui::swatch_needs_edge(0xFFFFFF, DARK_SCREEN));
    CHECK_FALSE(helix::ui::swatch_needs_edge(0x808080, DARK_SCREEN));
    CHECK_FALSE(helix::ui::swatch_needs_edge(0x1A1A1A, LIGHT_SCREEN));
    CHECK_FALSE(helix::ui::swatch_needs_edge(0xE53935, LIGHT_DIALOG));
}

namespace {

/// Integer <px> tokens of one ui_xml file, keyed by full suffixed name. Values
/// that are not bare integers (token references) are skipped: no ladder this
/// test reads uses them.
std::map<std::string, int> read_px_tokens(const char* xml_path) {
    std::ifstream f(xml_path);
    REQUIRE(f.good()); // a missing token file must fail loudly, not vacuously
    std::map<std::string, int> tokens;
    std::string line;
    while (std::getline(f, line)) {
        const auto name_at = line.find("<px name=\"");
        if (name_at == std::string::npos)
            continue;
        const auto name_start = name_at + 10;
        const auto name_end = line.find('"', name_start);
        if (name_end == std::string::npos)
            continue;
        const auto value_at = line.find("value=\"", name_end);
        if (value_at == std::string::npos)
            continue;
        const auto value_start = value_at + 7;
        const auto value_end = line.find('"', value_start);
        try {
            tokens[line.substr(name_start, name_end - name_start)] =
                std::stoi(line.substr(value_start, value_end - value_start));
        } catch (const std::exception&) {
            // Not a bare integer (a token reference): not one of ours.
        }
    }
    return tokens;
}

/// The tier fallback chain the runtime resolver applies
/// (src/ui/theme_responsive.cpp theme_manager_pick_px_tier): micro falls to
/// tiny then small, tiny to small, and xlarge/xxlarge walk back down to large.
/// Mirrored here so the test asserts the values a screen of each class
/// actually resolves, not just the tiers that define their own entries.
int tier_value(const std::map<std::string, int>& tokens, const std::string& base,
               const char* suffix) {
    std::vector<std::string> chain{base + suffix};
    if (strcmp(suffix, "_micro") == 0)
        chain.push_back(base + "_tiny");
    if (strcmp(suffix, "_micro") == 0 || strcmp(suffix, "_tiny") == 0)
        chain.push_back(base + "_small");
    if (strcmp(suffix, "_xxlarge") == 0)
        chain.push_back(base + "_xlarge");
    if (strcmp(suffix, "_xxlarge") == 0 || strcmp(suffix, "_xlarge") == 0)
        chain.push_back(base + "_large");
    for (const auto& key : chain) {
        const auto it = tokens.find(key);
        if (it != tokens.end())
            return it->second;
    }
    FAIL("no tier of " << base << " resolves for class " << suffix);
    return 0;
}

} // namespace

TEST_CASE("swatch token ladders keep every touch target at the floor, per size class",
          "[color_swatch][swatch]") {
    const auto globals = read_px_tokens("ui_xml/globals.xml");
    const auto led = read_px_tokens("ui_xml/led_tokens.xml");

    // The swatch floor the AMS edit overlay's grid landed: a swatch's whole
    // target (disc + hit reach) clears 44px. Tappable controls have their own
    // taller floor (#touch_target_min, 48) - which must never drop below the
    // swatch floor, or the grids would outrank the buttons.
    constexpr int SWATCH_FLOOR = 44;
    REQUIRE(globals.count("touch_target_min") == 1);
    REQUIRE(globals.at("touch_target_min") >= SWATCH_FLOOR);

    struct Ladder {
        const char* label;
        const std::map<std::string, int>* tokens;
        const char *size, *gap, *half;
    };
    const Ladder ladders[] = {
        {"swatch_grid", &globals, "swatch_grid_size", "swatch_grid_gap", "swatch_grid_gap_half"},
        {"picker_tab", &globals, "picker_tab_swatch_size", "picker_tab_swatch_gap",
         "picker_tab_swatch_gap_half"},
        {"led_swatch", &led, "led_swatch_size", "led_swatch_gap", "led_swatch_gap_half"},
    };
    const char* classes[] = {"_micro", "_tiny",   "_small",  "_medium",
                             "_large", "_xlarge", "_xxlarge"};

    for (const auto& ladder : ladders) {
        for (const char* cls : classes) {
            INFO(ladder.label << cls);
            const int size = tier_value(*ladder.tokens, ladder.size, cls);
            const int gap = tier_value(*ladder.tokens, ladder.gap, cls);
            const int half = tier_value(*ladder.tokens, ladder.half, cls);
            // The whole target - disc plus its hit reach - clears the floor...
            CHECK(size + 2 * half >= SWATCH_FLOOR);
            // ...and the gap swallows both neighbours' reach, so adjacent
            // targets abut without overlapping.
            CHECK(gap >= 2 * half);
        }
    }

    // The shared grid keeps its 6-column shape: 6 swatches and 5 gaps.
    for (const char* cls : classes) {
        INFO("swatch_grid_width" << cls);
        CHECK(tier_value(globals, "swatch_grid_width", cls) ==
              6 * tier_value(globals, "swatch_grid_size", cls) +
                  5 * tier_value(globals, "swatch_grid_gap", cls));
    }
}
