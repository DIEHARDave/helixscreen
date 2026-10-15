// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_context_menu_fit_width.cpp
 * @brief A `fit_content` context menu is as wide as its rows, within the screen.
 *
 * The printer switch menu is the live case: its rows are `width="100%"`, which a
 * `width="content"` card ignores when sizing itself (`w_ignore_size`), so the
 * card has to be told how wide its names are.
 */

#include "ui_context_menu.h"
#include "ui_printer_switch_menu.h"

#include "../lvgl_test_fixture.h"
#include "../test_fixtures.h"
#include "../test_helpers/config_test_access.h"
#include "config.h"
#include "theme_manager.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ui::ContextMenu;
using CardWidth = ContextMenu::CardWidth;

// ============================================================================
// compute_card_width
// ============================================================================

TEST_CASE("compute_card_width: fitted card takes its content width", "[context_menu][fit]") {
    const CardWidth fit{50, 160, 0, true};
    CHECK(ContextMenu::compute_card_width(fit, 800, 250, 10) == 250);
}

TEST_CASE("compute_card_width: fitted card never drops below min", "[context_menu][fit]") {
    const CardWidth fit{50, 160, 0, true};
    CHECK(ContextMenu::compute_card_width(fit, 800, 90, 10) == 160);
}

TEST_CASE("compute_card_width: fitted card stops at its share of the screen",
          "[context_menu][fit]") {
    const CardWidth fit{50, 160, 0, true};
    CHECK(ContextMenu::compute_card_width(fit, 800, 600, 10) == 400);
    CHECK(ContextMenu::compute_card_width(fit, 480, 600, 10) == 240);
}

TEST_CASE("compute_card_width: fitted card honours max when it is tighter", "[context_menu][fit]") {
    const CardWidth fit{50, 160, 300, true};
    CHECK(ContextMenu::compute_card_width(fit, 800, 600, 10) == 300);
    CHECK(ContextMenu::compute_card_width(fit, 800, 250, 10) == 250);
}

TEST_CASE("compute_card_width: the screen margin beats min and the share", "[context_menu][fit]") {
    // A 100% share on a 200px screen would touch both edges; min would overflow it.
    const CardWidth fit{100, 260, 0, true};
    CHECK(ContextMenu::compute_card_width(fit, 200, 600, 10) == 180);
}

TEST_CASE("compute_card_width: a share policy without fit clamps the share to [min,max]",
          "[context_menu][fit]") {
    const CardWidth share{40, 200, 320, false};
    CHECK(ContextMenu::compute_card_width(share, 800, 0, 10) == 320);
    CHECK(ContextMenu::compute_card_width(share, 400, 0, 10) == 200);
    CHECK(ContextMenu::compute_card_width(share, 600, 999, 10) == 240);
}

// ============================================================================
// PrinterSwitchMenu
// ============================================================================

namespace {

class SwitchMenuFixture : public XMLTestFixture {
  public:
    SwitchMenuFixture() {
        REQUIRE(register_component("components/printer_switch_row"));
        REQUIRE(register_component("printer_switch_menu"));

        cfg_ = helix::Config::get_instance();
        saved_data_ = helix::ConfigTestAccess::data(*cfg_);
        saved_active_ = helix::ConfigTestAccess::active_printer_id(*cfg_);

        // A badge-sized anchor at the bottom of the left navbar.
        badge_ = lv_obj_create(test_screen());
        lv_obj_set_size(badge_, 60, 60);
    }

    ~SwitchMenuFixture() override {
        menu_.hide();
        process_lvgl(20);
        helix::ConfigTestAccess::data(*cfg_) = saved_data_;
        helix::ConfigTestAccess::active_printer_id(*cfg_) = saved_active_;
    }

    void set_printers(const std::vector<std::string>& names) {
        nlohmann::json data;
        data["config_version"] = 3;
        data["active_printer_id"] = "p0";
        for (size_t i = 0; i < names.size(); i++) {
            data["printers"]["p" + std::to_string(i)]["printer_name"] = names[i];
        }
        helix::ConfigTestAccess::data(*cfg_) = data;
        helix::ConfigTestAccess::active_printer_id(*cfg_) = "p0";
    }

    lv_obj_t* show() {
        lv_obj_update_layout(test_screen());
        menu_.show(test_screen(), badge_);
        lv_obj_t* card = lv_obj_find_by_name(test_screen(), "context_menu");
        REQUIRE(card != nullptr);
        lv_obj_update_layout(card);
        return card;
    }

    /// Text the printer_name label of row @p id draws: dotted text carries the dots.
    std::string drawn_name(const char* id) {
        lv_obj_t* list = lv_obj_find_by_name(test_screen(), "printer_list");
        REQUIRE(list != nullptr);
        lv_obj_t* row = lv_obj_find_by_name(list, id);
        REQUIRE(row != nullptr);
        lv_obj_t* label = lv_obj_find_by_name(row, "printer_name");
        REQUIRE(label != nullptr);
        return lv_label_get_text(label);
    }

    static void check_on_screen(lv_obj_t* card) {
        lv_area_t a;
        lv_obj_get_coords(card, &a);
        const int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default());
        const int32_t h = lv_display_get_vertical_resolution(lv_display_get_default());
        INFO("card (" << a.x1 << "," << a.y1 << ")-(" << a.x2 << "," << a.y2 << ") in " << w << "x"
                      << h);
        CHECK(a.x1 >= 0);
        CHECK(a.y1 >= 0);
        CHECK(a.x2 < w);
        CHECK(a.y2 < h);
    }

    helix::Config* cfg_ = nullptr;
    helix::ui::PrinterSwitchMenu menu_;
    lv_obj_t* badge_ = nullptr;

  private:
    nlohmann::json saved_data_;
    std::string saved_active_;
};

} // namespace

TEST_CASE_METHOD(SwitchMenuFixture, "Printer switch menu: short names keep the minimum width",
                 "[context_menu][fit][printer_switch]") {
    set_printers({"K1", "U1"});
    lv_obj_set_pos(badge_, 10, 400);
    lv_obj_t* card = show();
    CHECK(lv_obj_get_width(card) == 160);
    CHECK(drawn_name("p0") == "K1");
}

TEST_CASE_METHOD(SwitchMenuFixture, "Printer switch menu: the card widens to show a long name",
                 "[context_menu][fit][printer_switch]") {
    const std::string name = "Voron 2.4 350 (Enclosure Build)";
    set_printers({name, "Snapmaker U1"});
    lv_obj_set_pos(badge_, 10, 400);
    lv_obj_t* card = show();
    CHECK(lv_obj_get_width(card) > 160);
    CHECK(lv_obj_get_width(card) <= TEST_DISPLAY_WIDTH / 2);
    CHECK(drawn_name("p0") == name);
    CHECK(drawn_name("p1") == "Snapmaker U1");
    check_on_screen(card);
}

TEST_CASE_METHOD(SwitchMenuFixture, "Printer switch menu: a name past half the screen ellipsizes",
                 "[context_menu][fit][printer_switch]") {
    const std::string name(90, 'W');
    set_printers({name});
    lv_obj_set_pos(badge_, 10, 400);
    lv_obj_t* card = show();
    CHECK(lv_obj_get_width(card) == TEST_DISPLAY_WIDTH / 2);
    CHECK(drawn_name("p0") != name);
    check_on_screen(card);
}

TEST_CASE_METHOD(SwitchMenuFixture,
                 "Printer switch menu: a badge at the right of a bottom navbar opens above it",
                 "[context_menu][fit][printer_switch]") {
    ScopedResolution portrait(lv_display_get_default(), 480, 800);
    lv_obj_set_size(test_screen(), 480, 800);
    set_printers({"Voron 2.4 350 (Enclosure Build)", "FlashForge Adventurer 5M Pro"});
    lv_obj_set_pos(badge_, 410, 730);
    lv_obj_t* card = show();
    check_on_screen(card);

    lv_area_t c;
    lv_area_t b;
    lv_obj_get_coords(card, &c);
    lv_obj_get_coords(badge_, &b);
    CHECK(c.y2 < b.y1);
    CHECK(c.x2 >= b.x1);
    CHECK(lv_obj_get_width(card) <= 240);
}
