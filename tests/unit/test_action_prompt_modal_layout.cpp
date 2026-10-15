// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Layout tests for ActionPromptModal's regular-button rows.
//
// Each prompt_button_group gets its own row, and a run of ungrouped buttons
// shares one. A row of content-sized buttons wraps inside the fixed-width
// (320px) dialog, so a row of >= 4 (AFC's "Lane 1".."Lane 4", #1043) becomes an
// equal-width non-wrapping row instead. With <= 3 buttons a row stays
// content-sized.
//
// The count alone is not the rule, though. An equal-width row splits the
// container evenly, so it only holds while the labels fit their share: a
// preheat macro offering seven "PLA 220/60" presets gets a few dozen pixels per
// cell and clips every label. Those fall back to row_wrap and take the lines
// they need. The invariant these tests defend is that no label is ever clipped.
//
// Tagged [ui_integration] (NOT hidden) — it shows real widgets and forces a
// real LVGL layout pass, which is the only way the wrap actually manifests.

#include "ui_modal.h"
#include "ui_swatch.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/scoped_geometry.h"
#include "action_prompt_manager.h"
#include "action_prompt_modal.h"
#include "display_settings_manager.h"
#include "lvgl/src/misc/lv_area_private.h" // lv_area_is_in
#include "theme_manager.h"

#include <vector>

#include "../catch_amalgamated.hpp"

// White-box accessor (declared friend in action_prompt_modal.h) — avoids adding
// _for_testing() methods to the production class ([L065]/[L088]).
struct ActionPromptModalTestAccess {
    static const std::vector<lv_obj_t*>& buttons(const helix::ui::ActionPromptModal& m) {
        return m.created_buttons_;
    }
};

namespace {

helix::PromptData make_regular_prompt(int n_buttons) {
    helix::PromptData data;
    data.title = "Select Lane";
    data.text_lines.push_back("Pick a lane to calibrate");
    for (int i = 0; i < n_buttons; ++i) {
        helix::PromptButton btn;
        btn.label = "Lane " + std::to_string(i + 1);
        btn.gcode = "AFC_CALIBRATION LANE=lane" + std::to_string(i + 1);
        btn.color = "primary";
        btn.is_footer = false;
        data.buttons.push_back(std::move(btn));
    }
    return data;
}

// Z-Mod's color picker as its CHANGE_ZCOLOR macro sends it (#1806): a text
// line, three groups of eight label-less swatches and a footer Cancel.
helix::PromptData make_zmod_color_prompt() {
    using helix::ActionPromptManager;
    static const char* const kGroups[3][8] = {
        {"ffffff", "fff245", "def578", "21cc3d", "167a4b", "156682", "24e4a0", "7bd9f0"},
        {"4caaf8", "2e54dd", "48358c", "a341f7", "f435f6", "d584de", "fa6173", "f82d29"},
        {"805003", "f9903b", "fcebd7", "d5c5a1", "b17c38", "8c8c89", "bebebe", "1b1b1b"},
    };
    helix::PromptData data;
    data.title = "Select color";
    data.text_lines.push_back("Spool 1: PETG-CF/");
    for (int g = 0; g < 3; ++g) {
        for (const char* hex : kGroups[g]) {
            auto btn = ActionPromptManager::parse_button_spec(
                std::string("_ |CHANGE_ZCOLOR SLOT=1 HEX=") + hex + "|" + hex);
            btn.group_id = g + 1;
            data.buttons.push_back(std::move(btn));
        }
    }
    data.buttons.push_back(ActionPromptManager::parse_button_spec("Cancel|RESPOND|primary"));
    data.buttons.back().is_footer = true;
    return data;
}

class ActionPromptLayoutFixture : public LVGLUITestFixture {
  public:
    ActionPromptLayoutFixture() {
        prev_animations_ = helix::DisplaySettingsManager::instance().get_animations_enabled();
        helix::DisplaySettingsManager::instance().set_animations_enabled(false);
        modal_.set_gcode_callback([](const std::string&) { /* no-op */ });
    }
    ~ActionPromptLayoutFixture() override {
        helix::DisplaySettingsManager::instance().set_animations_enabled(prev_animations_);
    }

    helix::ui::ActionPromptModal modal_;
    bool prev_animations_ = true;
};

} // namespace

// ----------------------------------------------------------------------------
// 4 regular buttons must share a single row (the R2 bug case).
// ----------------------------------------------------------------------------
TEST_CASE_METHOD(ActionPromptLayoutFixture, "ActionPromptModal: 4 regular buttons fit on one row",
                 "[action_prompt][layout][ui_integration]") {
    auto data = make_regular_prompt(4);
    REQUIRE(modal_.show_prompt(test_screen(), data));

    // Force a synchronous layout pass on the whole tree so geometry settles.
    lv_obj_update_layout(test_screen());
    if (lv_obj_t* dialog = modal_.dialog()) {
        lv_obj_update_layout(dialog);
    }

    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 4);

    // All four buttons must be on the SAME visual row. If the 4th wraps, its y
    // jumps down by ~one button-height (>= ~48px) — far outside this tolerance.
    int32_t y0 = lv_obj_get_y(btns[0]);
    for (size_t i = 1; i < btns.size(); ++i) {
        int32_t yi = lv_obj_get_y(btns[i]);
        INFO("button[" << i << "] y=" << yi << " vs button[0] y=" << y0);
        REQUIRE(std::abs(yi - y0) <= 4);
    }

    modal_.hide();
}

// ----------------------------------------------------------------------------
// Regression: 3 regular buttons still share one row AND stay content-sized
// (NOT forced to equal width). Guards against regressing the <= 3 path.
// ----------------------------------------------------------------------------
TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: 3 regular buttons stay content-sized on one row",
                 "[action_prompt][layout][ui_integration]") {
    // Short labels of unequal length: they fit on one content-sized row (as the
    // legacy <= 3 path always has), and content-sizing yields unequal widths —
    // the >= 4 equal-width path would instead force them identical.
    helix::PromptData data;
    data.title = "Confirm";
    data.text_lines.push_back("Choose an action");
    data.buttons.push_back({"OK", "RESUME", "primary", "", false, -1});
    data.buttons.push_back({"Retry", "RETRY", "secondary", "", false, -1});
    data.buttons.push_back({"Cancel", "CANCEL", "error", "", false, -1});

    REQUIRE(modal_.show_prompt(test_screen(), data));
    lv_obj_update_layout(test_screen());
    if (lv_obj_t* dialog = modal_.dialog()) {
        lv_obj_update_layout(dialog);
    }

    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 3);

    // One row: all share (approximately) the same y.
    int32_t y0 = lv_obj_get_y(btns[0]);
    for (size_t i = 1; i < btns.size(); ++i) {
        REQUIRE(std::abs(lv_obj_get_y(btns[i]) - y0) <= 4);
    }

    // Content-sized: the short and long labels must NOT be equal width.
    int32_t w_ok = lv_obj_get_width(btns[0]);
    int32_t w_long = lv_obj_get_width(btns[1]);
    INFO("w(OK)=" << w_ok << " w(Retry the whole operation)=" << w_long);
    REQUIRE(w_long > w_ok);

    modal_.hide();
}

// ----------------------------------------------------------------------------
// The real AFC calibration prompt: four short lane buttons plus a longer
// "Calibrate All". Whichever layout the fit check picks, no label may be
// clipped — that is the invariant, not the row count.
// ----------------------------------------------------------------------------
TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: AFC calibration buttons are never clipped",
                 "[action_prompt][layout][ui_integration]") {
    // Mirrors src/printer/ams_backend_mock.cpp's simulated AFC_CALIBRATION prompt.
    helix::PromptData data;
    data.title = "AFC Calibration";
    data.text_lines.push_back("Select a lane to calibrate, or calibrate all lanes.");
    for (int i = 1; i <= 4; ++i) {
        data.buttons.push_back({"Lane " + std::to_string(i),
                                "AFC_CALIBRATION LANE=lane" + std::to_string(i), "primary", "",
                                false, -1});
    }
    data.buttons.push_back({"Calibrate All", "AFC_CALIBRATION ALL=1", "secondary", "", false, -1});
    data.buttons.push_back({"Cancel", "AFC_CALIBRATION CANCEL=1", "error", "", true, -1});

    REQUIRE(modal_.show_prompt(test_screen(), data));
    lv_obj_update_layout(test_screen());
    if (lv_obj_t* dialog = modal_.dialog()) {
        lv_obj_update_layout(dialog);
    }

    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 6); // 5 regular + 1 footer

    // Every regular button must be able to show its label in full.
    for (size_t i = 0; i < 5; ++i) {
        lv_obj_t* label = lv_obj_get_child(btns[i], 0);
        REQUIRE(label != nullptr);
        INFO("button[" << i << "] \"" << data.buttons[i].label << "\" width="
                       << lv_obj_get_width(btns[i]) << " label width=" << lv_obj_get_width(label));
        REQUIRE(lv_obj_get_width(btns[i]) >= lv_obj_get_width(label));
    }

    modal_.hide();
}

// ----------------------------------------------------------------------------
// Many buttons with labels too long to share one row must WRAP, not clip.
//
// The >= 4 rule above is a button *count* test, but the property that actually
// matters is whether the labels fit. A preheat macro that emits 7 material
// presets ("PLA 220/60" ...) gets ~40px per equal-width cell on a 480px card,
// so every label is clipped to a couple of glyphs. Wrapping is the correct
// answer once the row genuinely cannot hold them.
// ----------------------------------------------------------------------------
TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: long labels wrap onto multiple rows instead of clipping",
                 "[action_prompt][layout][ui_integration]") {
    helix::PromptData data;
    data.title = "Preheat for Load";
    data.text_lines.push_back("Preheat filament and choose a material");
    for (const char* label : {"PLA 220/60", "PETG 240/80", "ABS 250/100", "ASA 260/100",
                              "TPU 230/50", "PC 280/110", "Nylon 260/80"}) {
        data.buttons.push_back(
            {label, std::string("SET_MATERIAL M=") + label, "primary", "", false, -1});
    }

    REQUIRE(modal_.show_prompt(test_screen(), data));
    lv_obj_update_layout(test_screen());
    if (lv_obj_t* dialog = modal_.dialog()) {
        lv_obj_update_layout(dialog);
    }

    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 7);

    // The property under test: every button is wide enough to actually show its
    // label. Under the equal-width single-row layout each cell is far narrower
    // than its text, which is exactly the reported clipping.
    for (size_t i = 0; i < btns.size(); ++i) {
        lv_obj_t* label = lv_obj_get_child(btns[i], 0);
        REQUIRE(label != nullptr);
        int32_t w_btn = lv_obj_get_width(btns[i]);
        int32_t w_lbl = lv_obj_get_width(label);
        INFO("button[" << i << "] \"" << data.buttons[i].label << "\" width=" << w_btn
                       << " label width=" << w_lbl);
        REQUIRE(w_btn >= w_lbl);
    }

    // Not fitting on one row is the direct consequence: they must occupy at
    // least two distinct rows rather than being squeezed into one.
    int32_t y0 = lv_obj_get_y(btns[0]);
    bool any_wrapped = false;
    for (size_t i = 1; i < btns.size(); ++i) {
        if (std::abs(lv_obj_get_y(btns[i]) - y0) > 4) {
            any_wrapped = true;
        }
    }
    REQUIRE(any_wrapped);

    modal_.hide();
}

// ----------------------------------------------------------------------------
// prompt_button_group_start/end: each group is a row of its own, and the
// ungrouped buttons around it are not pulled into it.
// ----------------------------------------------------------------------------
namespace {
int32_t screen_y(lv_obj_t* obj) {
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return a.y1;
}
} // namespace

TEST_CASE_METHOD(ActionPromptLayoutFixture, "ActionPromptModal: each button group is its own row",
                 "[action_prompt][layout][ui_integration]") {
    // Two short ungrouped buttons, then a Yes/No group, then a lone grouped
    // button. Without rows, all five short labels would share one wrapping row.
    helix::PromptData data;
    data.title = "Groups";
    data.buttons = {
        {"A", "A", "", "", false, -1},  {"B", "B", "", "", false, -1},
        {"Yes", "Y", "", "", false, 0}, {"No", "N", "", "", false, 0},
        {"Z", "Z", "", "", false, 1},   {"Cancel", "C", "error", "", true, -1},
    };

    REQUIRE(modal_.show_prompt(test_screen(), data));
    lv_obj_update_layout(test_screen());

    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 6);
    const int32_t row_a = screen_y(btns[0]);
    const int32_t row_yes = screen_y(btns[2]);
    const int32_t row_z = screen_y(btns[4]);
    INFO("ungrouped y=" << row_a << " group0 y=" << row_yes << " group1 y=" << row_z);

    CHECK(screen_y(btns[1]) == row_a);   // ungrouped run shares a row
    CHECK(screen_y(btns[3]) == row_yes); // the group shares a row
    CHECK(row_yes > row_a);              // ...below the ungrouped one
    CHECK(row_z > row_yes);              // and the next group is below that

    modal_.hide();
}

// Klipper's secondary is the neutral button, not a success-green one.
TEST_CASE_METHOD(ActionPromptLayoutFixture, "ActionPromptModal: secondary uses the secondary token",
                 "[action_prompt][layout][ui_integration]") {
    helix::PromptData data;
    data.title = "Colors";
    data.buttons = {{"Later", "LATER", "secondary", "", false, -1}};

    REQUIRE(modal_.show_prompt(test_screen(), data));
    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 1);

    const lv_color_t bg = lv_obj_get_style_bg_color(btns[0], LV_PART_MAIN);
    CHECK(lv_color_eq(bg, theme_manager_get_color("secondary")));
    CHECK_FALSE(lv_color_eq(bg, theme_manager_get_color("success")));

    modal_.hide();
}

// A hex in the third field (or the legacy fourth) fills the button with that
// color, and the label picks whatever reads on it; a field that is not a color
// leaves the themed default (prestonbrown/helixscreen#1806).
TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: hex button fills with the color and keeps text readable",
                 "[action_prompt][layout][ui_integration][hex_color][1806]") {
    using helix::ActionPromptManager;
    helix::PromptData data;
    data.title = "Colors";
    data.buttons = {
        ActionPromptManager::parse_button_spec("Standard|GCODE_CMD_1|error"),
        ActionPromptManager::parse_button_spec("HEX Dark (White Text)|GCODE_CMD_2|ff5722"),
        ActionPromptManager::parse_button_spec("HEX Light (Black Text)|GCODE_CMD_3|#e0e0e0"),
        ActionPromptManager::parse_button_spec("Legacy|GCODE_CMD_4|primary|21cc3d"),
        ActionPromptManager::parse_button_spec("Garbage|GCODE_CMD_5|zzzzzz"),
    };

    REQUIRE(modal_.show_prompt(test_screen(), data));
    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 5);

    auto bg = [&](size_t i) { return lv_obj_get_style_bg_color(btns[i], LV_PART_MAIN); };
    CHECK(lv_color_eq(bg(0), theme_manager_get_color("danger")));
    CHECK(lv_color_eq(bg(1), lv_color_hex(0xff5722)));
    CHECK(lv_color_eq(bg(2), lv_color_hex(0xe0e0e0)));
    CHECK(lv_color_eq(bg(3), lv_color_hex(0x21cc3d)));
    CHECK(lv_color_eq(bg(4), theme_manager_get_color("primary")));

    for (size_t i = 0; i < btns.size(); ++i) {
        lv_obj_t* label = lv_obj_get_child(btns[i], 0);
        REQUIRE(label != nullptr);
        const lv_color_t text = lv_obj_get_style_text_color(label, LV_PART_MAIN);
        INFO("button[" << i << "] \"" << data.buttons[i].label << "\"");
        CHECK(helix::contrast_ratio(text, bg(i)) >= kThemeTextContrastThreshold);
    }

    modal_.hide();
}

// A Z-Mod swatch tile carries no text, so it needs an outline to stay visible
// when its color matches the card behind it.
TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: a label-less hex button is a swatch",
                 "[action_prompt][layout][ui_integration][hex_color][1806]") {
    using helix::ActionPromptManager;
    helix::PromptData data;
    data.title = "Select color";
    data.buttons = {
        ActionPromptManager::parse_button_spec("_ |CHANGE_ZCOLOR SLOT=1 HEX=ffffff|ffffff"),
        ActionPromptManager::parse_button_spec("Named|GCODE|e0e0e0"),
    };

    REQUIRE(modal_.show_prompt(test_screen(), data));
    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 2);

    CHECK(lv_obj_get_style_border_width(btns[0], LV_PART_MAIN) > 0);
    CHECK(lv_obj_get_style_border_width(btns[1], LV_PART_MAIN) == 0);

    // The tile is painted by the filament swatch painter: same fill and edge as
    // a swatch it paints directly.
    lv_obj_t* reference = lv_obj_create(test_screen());
    helix::ui::apply_swatch_color(reference, 0xffffff, "");
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(btns[0], LV_PART_MAIN), lv_color_hex(0xffffff)));
    CHECK(lv_obj_get_style_bg_opa(btns[0], LV_PART_MAIN) == LV_OPA_COVER);
    CHECK(lv_color_eq(lv_obj_get_style_border_color(btns[0], LV_PART_MAIN),
                      lv_obj_get_style_border_color(reference, LV_PART_MAIN)));
    CHECK(lv_obj_get_style_border_opa(btns[0], LV_PART_MAIN) ==
          lv_obj_get_style_border_opa(reference, LV_PART_MAIN));
    lv_obj_delete(reference);

    modal_.hide();
}

// On a 480x272 panel the swatch grid takes the room the screen has: the card
// widens until every tile is a full touch target across, and the tiles grow
// into the free height under the card cap, with nothing pushed off the card.
TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: swatch grid uses the room a 480x272 screen has",
                 "[action_prompt][layout][ui_integration][hex_color][1806]") {
    helix::test::ScopedGeometry micro(480, 272);
    REQUIRE(modal_.show_prompt(test_screen(), make_zmod_color_prompt()));
    lv_obj_t* dialog = modal_.dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_update_layout(test_screen());

    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 25);
    lv_area_t card;
    lv_obj_get_coords(dialog, &card);
    lv_area_t screen;
    lv_obj_get_coords(test_screen(), &screen);
    CHECK(lv_area_is_in(&card, &screen, 0));

    const int32_t touch_min = theme_manager_get_spacing("touch_target_min");
    REQUIRE(touch_min > 0);
    for (size_t i = 0; i < 24; ++i) {
        lv_area_t a;
        lv_obj_get_coords(btns[i], &a);
        CAPTURE(i, lv_area_get_width(&a), lv_area_get_height(&a));
        CHECK(lv_area_get_width(&a) >= touch_min);
        CHECK(lv_area_get_height(&a) >= 38);
        CHECK(lv_area_is_in(&a, &card, 0));
    }
    lv_area_t footer;
    lv_obj_get_coords(btns[24], &footer);
    CHECK(lv_area_is_in(&footer, &card, 0));

    modal_.hide();
}

// A screen with room for full-size tiles keeps the card and tiles it had.
TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: swatch grid at 800x480 keeps the standard card",
                 "[action_prompt][layout][ui_integration][hex_color][1806]") {
    helix::test::ScopedGeometry medium(800, 480);
    REQUIRE(modal_.show_prompt(test_screen(), make_zmod_color_prompt()));
    lv_obj_t* dialog = modal_.dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_update_layout(test_screen());

    CHECK(lv_obj_get_width(dialog) == 480);
    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 25);
    for (size_t i = 0; i < 24; ++i) {
        CAPTURE(i);
        CHECK(lv_obj_get_height(btns[i]) == theme_manager_get_spacing("button_height"));
        CHECK(lv_obj_get_width(btns[i]) >= 51);
    }

    modal_.hide();
}

// Labelled buttons are not swatches: a 480x272 prompt of them keeps the
// standard card width and button height.
TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: labelled buttons at 480x272 keep the standard card",
                 "[action_prompt][layout][ui_integration][hex_color][1806]") {
    helix::test::ScopedGeometry micro(480, 272);
    REQUIRE(modal_.show_prompt(test_screen(), make_regular_prompt(4)));
    lv_obj_t* dialog = modal_.dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_update_layout(test_screen());

    CHECK(lv_obj_get_width(dialog) == 480 * 70 / 100);
    for (lv_obj_t* btn : ActionPromptModalTestAccess::buttons(modal_)) {
        CHECK(lv_obj_get_height(btn) == theme_manager_get_spacing("button_height"));
    }

    modal_.hide();
}

// More swatch rows than the card can hold at button_height: the tiles stop
// shrinking there and the grid scrolls, so the footer stays on the card.
TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: a swatch grid too tall for 480x272 scrolls",
                 "[action_prompt][layout][ui_integration][hex_color][1806]") {
    helix::test::ScopedGeometry micro(480, 272);
    helix::PromptData data = make_zmod_color_prompt();
    helix::PromptButton footer = data.buttons.back();
    data.buttons.pop_back();
    const size_t swatches = data.buttons.size();
    for (size_t i = 0; i < swatches; ++i) {
        helix::PromptButton extra = data.buttons[i];
        extra.group_id += 3;
        data.buttons.push_back(extra);
    }
    data.buttons.push_back(footer);

    REQUIRE(modal_.show_prompt(test_screen(), data));
    lv_obj_t* dialog = modal_.dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_update_layout(test_screen());

    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 2 * swatches + 1);
    for (size_t i = 0; i < 2 * swatches; ++i) {
        CAPTURE(i);
        CHECK(lv_obj_get_height(btns[i]) == theme_manager_get_spacing("button_height"));
    }
    lv_obj_t* container = lv_obj_find_by_name(dialog, "button_container");
    REQUIRE(container != nullptr);
    CHECK(lv_obj_has_flag(container, LV_OBJ_FLAG_SCROLLABLE));
    CHECK(lv_obj_get_scroll_bottom(container) > 0);

    lv_area_t card, footer_area;
    lv_obj_get_coords(dialog, &card);
    lv_obj_get_coords(btns.back(), &footer_area);
    CHECK(lv_area_is_in(&footer_area, &card, 0));

    modal_.hide();
}
