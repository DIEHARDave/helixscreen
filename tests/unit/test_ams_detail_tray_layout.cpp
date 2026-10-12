// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The AMS panel's dry-box unit as laid out: spools stand in the box, labels sit
// above its lid (or back wall), the readout is a chip under the box, and under
// per-lane lids each lane shows its own humidity behind a droplet.

#include "ui_ams_detail.h"
#include "ui_ams_sidebar.h"
#include "ui_ams_slot.h"
#include "ui_endless_spool_arrows.h"
#include "ui_filament_path_canvas.h"
#include "ui_panel_ams.h"
#include "ui_spool_canvas.h"

#include "../test_fixtures.h"
#include "../test_helpers/ams_panel_test_access.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_tray_projection.h"
#include "filament_tube_stroker.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "src/ui/ui_filament_path_internal.h"
#include "theme_manager.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
namespace tray = helix::ui::tray;

namespace {

void register_ams_xml_once() {
    static bool done = false;
    if (done)
        return;
    ui_spool_canvas_register();
    ui_ams_slot_register();
    ui_filament_path_canvas_register();
    ui_endless_spool_arrows_register();
    helix::ui::AmsOperationSidebar::register_callbacks_static();
    lv_xml_register_component_from_file("A:ui_xml/components/ams_unit_detail.xml");
    lv_xml_register_component_from_file("A:ui_xml/components/ams_loaded_card.xml");
    lv_xml_register_component_from_file("A:ui_xml/components/ams_environment_indicator.xml");
    lv_xml_register_component_from_file("A:ui_xml/components/ams_sidebar.xml");
    lv_xml_register_component_from_file("A:ui_xml/ams_panel.xml");
    done = true;
}

class AmsTrayPanelFixture : public XMLTestFixture {
  public:
    void build(const char* env_mode, int gates = 4) {
        auto mock = std::make_unique<AmsBackendMock>(gates);
        mock->set_afc_mode(true);
        mock->set_environment_mode(env_mode);
        REQUIRE(mock->start().success());
        AmsState::instance().set_backend(std::move(mock));
        AmsState::instance().init_subjects(true);
        AmsState::instance().sync_from_backend();
        register_ams_xml_once();

        panel_ = std::make_unique<AmsPanel>(state(), &api());
        panel_->init_subjects();
        panel_obj_ = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ams_panel", nullptr));
        REQUIRE(panel_obj_ != nullptr);
        panel_->setup(panel_obj_, test_screen());
        panel_->on_activate();
        lv_obj_update_layout(test_screen());
        process_lvgl(50);
        panel_->refresh_slots();
        process_lvgl(20);
        lv_obj_update_layout(test_screen());

        container_ = lv_obj_find_by_name(panel_obj_, "slot_container");
        slot_grid_ = lv_obj_find_by_name(panel_obj_, "slot_grid");
        REQUIRE(container_ != nullptr);
        REQUIRE(slot_grid_ != nullptr);
        lv_area_t c;
        lv_obj_get_coords(container_, &c);
        origin_ = {c.x1, c.y1};
        REQUIRE(helix::ui::ams_detail_tray_geometry(box_, lid_, lid_h_, half_));
    }

    ~AmsTrayPanelFixture() override {
        if (panel_) {
            panel_->on_deactivate(DeactivateReason::NavigateAway);
            panel_->clear_panel_reference();
        }
        if (panel_obj_)
            lv_obj_delete(panel_obj_);
        process_lvgl(10);
        AmsState::instance().set_backend(nullptr);
    }

    lv_obj_t* slot(int i) const {
        return lv_obj_get_child(slot_grid_, i);
    }

    float top() const {
        return tray::unit_top_y(box_, lid_h_, lid_ != tray::LidMode::None);
    }

    /// The environment chip stands under the box, right of the last lane's tube,
    /// above the selector and inside the unit, clear of every lane badge and spool.
    void check_env_chip_clear(int gates) {
        lv_obj_t* chip = lv_obj_find_by_name(panel_obj_, "env_indicator");
        lv_obj_t* canvas = lv_obj_find_by_name(panel_obj_, "path_canvas");
        REQUIRE(chip != nullptr);
        REQUIRE(canvas != nullptr);
        REQUIRE_FALSE(lv_obj_has_flag(chip, LV_OBJ_FLAG_HIDDEN));
        lv_obj_update_layout(test_screen());
        lv_area_t ca, unit, hub;
        lv_obj_get_coords(chip, &ca);
        lv_obj_get_coords(container_, &unit);
        int32_t tube_x1 = 0, tube_x2 = 0;
        REQUIRE(ui_filament_path_canvas_get_lane_span(canvas, gates - 1, &tube_x1, &tube_x2));
        REQUIRE(ui_filament_path_canvas_get_hub_box(canvas, &hub));
        CAPTURE(ca.x1, ca.y1, ca.x2, ca.y2, tube_x2, hub.y1, unit.x2);
        CHECK(ca.x1 > tube_x2);
        // The tube's span reaches past the lane's centre by the tube's own half width.
        lv_obj_t* last_spool = lv_obj_find_by_name(slot(gates - 1), "spool_container");
        REQUIRE(last_spool != nullptr);
        lv_area_t sa;
        lv_obj_get_coords(last_spool, &sa);
        const int32_t lane_x = (sa.x1 + sa.x2) / 2;
        const auto* data = helix::ui::fpath::get_data(canvas);
        REQUIRE(data != nullptr);
        CHECK(tube_x2 - lane_x >= data->theme.tube_gauge / 2);
        CHECK((float)(ca.y1 - origin_.y) > box_.fb);
        CHECK(ca.y2 < hub.y1);
        CHECK(ca.x2 <= unit.x2);
        int checked = 0;
        lv_obj_t* badges = lv_obj_find_by_name(panel_obj_, "badge_layer");
        std::vector<lv_obj_t*> obstacles;
        for (uint32_t i = 0; badges && i < lv_obj_get_child_count(badges); ++i)
            obstacles.push_back(lv_obj_get_child(badges, (int32_t)i));
        for (int i = 0; i < gates; ++i)
            if (lv_obj_t* o = lv_obj_find_by_name(slot(i), "spool_graphic"))
                obstacles.push_back(o);
        for (lv_obj_t* o : obstacles) {
            lv_area_t oa;
            lv_obj_get_coords(o, &oa);
            CAPTURE(oa.x1, oa.y1, oa.x2, oa.y2);
            CHECK_FALSE((oa.x1 <= ca.x2 && oa.x2 >= ca.x1 && oa.y1 <= ca.y2 && oa.y2 >= ca.y1));
            ++checked;
        }
        REQUIRE(checked >= 2 * gates);
    }

    /// Every lane's number badge lies on the box front.
    void check_badges_on_front(int gates) {
        lv_obj_t* badges = lv_obj_find_by_name(panel_obj_, "badge_layer");
        REQUIRE(badges != nullptr);
        REQUIRE(lv_obj_get_child_count(badges) == (uint32_t)gates);
        for (int i = 0; i < gates; ++i) {
            CAPTURE(i);
            lv_area_t ba;
            lv_obj_get_coords(lv_obj_get_child(badges, i), &ba);
            CHECK((float)(ba.y1 - origin_.y) >= box_.ft - 1);
            CHECK((float)(ba.y2 - origin_.y) <= box_.fb + 1);
        }
    }

    std::unique_ptr<AmsPanel> panel_;
    lv_obj_t* panel_obj_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* slot_grid_ = nullptr;
    lv_point_t origin_{};
    tray::TrayBox box_{};
    tray::LidMode lid_ = tray::LidMode::None;
    float lid_h_ = 0;
    float half_ = 0;
};

} // namespace

TEST_CASE_METHOD(AmsTrayPanelFixture, "AMS unit with a reading: spools in the box under one lid",
                 "[ams][tray][ui_integration]") {
    build("passive");
    REQUIRE(lid_ == tray::LidMode::Unit);
    const int32_t space_md = theme_manager_get_spacing("space_md");
    const float skew = tray::DEPTH_SKEW * box_.depth;

    float first_x = 0, last_x = 0;
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        lv_obj_t* spool = lv_obj_find_by_name(slot(i), "spool_graphic");
        REQUIRE(spool != nullptr);
        lv_area_t a;
        lv_obj_get_coords(spool, &a);
        const float cx = (a.x1 + a.x2 + 1) / 2.0f - origin_.x;
        const float cy = (a.y1 + a.y2 + 1) / 2.0f - origin_.y;
        const float ry = tray::SPOOL_FLANGE_RADIUS * lv_area_get_width(&a);
        const tray::PointF want = tray::spool_center(box_, cx - skew / 2, ry);
        CHECK(std::fabs(cx - want.x) <= 1);
        CHECK(std::fabs(cy - want.y) <= 1);
        if (i == 0)
            first_x = cx - skew / 2;
        last_x = cx - skew / 2;

        lv_obj_t* label = lv_obj_find_by_name(slot(i), "material_label");
        REQUIRE(label != nullptr);
        lv_area_t la;
        lv_obj_get_coords(label, &la);
        CHECK(std::fabs((float)(la.y2 - origin_.y) - (top() - space_md)) <= 1);
    }
    // The box ends where the outer lane lids would.
    CHECK(box_.fl == Catch::Approx(first_x - half_).margin(0.01));
    CHECK(box_.fr == Catch::Approx(last_x + half_).margin(0.01));

    // The readout is a chip under the box beside the last tube, and costs the row
    // no width.
    check_env_chip_clear(4);
    check_badges_on_front(4);
    lv_obj_t* unit = lv_obj_get_parent(container_);
    CHECK(lv_obj_get_width(container_) == lv_obj_get_content_width(unit));
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS unit with no climate data: no lid, labels over the wall and the spools",
                 "[ams][tray][ui_integration]") {
    build("off");
    REQUIRE(lid_ == tray::LidMode::None);
    const int32_t space_md = theme_manager_get_spacing("space_md");
    const float wall_top = box_.ft - box_.back_extra - box_.rise;
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        lv_obj_t* label = lv_obj_find_by_name(slot(i), "material_label");
        lv_obj_t* spool = lv_obj_find_by_name(slot(i), "spool_graphic");
        REQUIRE(label != nullptr);
        REQUIRE(spool != nullptr);
        lv_area_t la, sa;
        lv_obj_get_coords(label, &la);
        lv_obj_get_coords(spool, &sa);
        const float spool_top = (sa.y1 + sa.y2 + 1) / 2.0f - origin_.y -
                                tray::SPOOL_FLANGE_RADIUS * lv_area_get_width(&sa);
        CHECK((float)(la.y2 - origin_.y) <= wall_top - space_md + 1);
        CHECK((float)(la.y2 - origin_.y) <= spool_top - space_md + 1);
    }
}

TEST_CASE_METHOD(AmsTrayPanelFixture, "AMS per-lane sensors: each lane's humidity behind a droplet",
                 "[ams][tray][ui_integration]") {
    build("slot");
    REQUIRE(lid_ == tray::LidMode::PerLane);
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        lv_obj_t* row = ui_ams_slot_get_lane_humidity(slot(i));
        REQUIRE(row != nullptr);
        CHECK_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));
        // Temperature above, then the droplet and humidity.
        lv_obj_t* temp_row = lv_obj_find_by_name(row, "lane_temp_row");
        lv_obj_t* hum_row = lv_obj_find_by_name(row, "lane_humidity_row");
        REQUIRE(temp_row != nullptr);
        REQUIRE(hum_row != nullptr);
        CHECK_FALSE(lv_obj_has_flag(temp_row, LV_OBJ_FLAG_HIDDEN));
        CHECK(lv_obj_get_index(temp_row) < lv_obj_get_index(hum_row));
        CHECK(std::strcmp(lv_obj_get_name(lv_obj_get_child(temp_row, 0)), "lane_temp_icon") == 0);
        const char* temp = lv_label_get_text(lv_obj_get_child(temp_row, 1));
        REQUIRE(temp != nullptr);
        CHECK(std::strstr(temp, "\xC2\xB0"
                                "C") != nullptr);
        REQUIRE(lv_obj_get_child_count(hum_row) == 2);
        // The droplet comes first, then the value.
        CHECK(std::strcmp(lv_obj_get_name(lv_obj_get_child(hum_row, 0)), "lane_humidity_icon") ==
              0);
        const char* value = lv_label_get_text(lv_obj_get_child(hum_row, 1));
        REQUIRE(value != nullptr);
        CHECK(std::strchr(value, '%') != nullptr);

        // Above its material label.
        lv_obj_t* label = lv_obj_find_by_name(slot(i), "material_label");
        REQUIRE(label != nullptr);
        lv_area_t ra, la;
        lv_obj_get_coords(row, &ra);
        lv_obj_get_coords(label, &la);
        CHECK(ra.y2 <= la.y1);
    }
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS tray: a dark-mode switch repaints the box in the new colors",
                 "[ams][tray][theme][ui_integration]") {
    build("off");
    REQUIRE(lid_ == tray::LidMode::None);
    REQUIRE_FALSE(helix::ui::reduced_effects());

    // A point on the back wall just under its top edge, midway along it:
    // between the two middle spools.
    const tray::TrayFaces f = tray::tray_faces(box_);
    const int x = (int)std::lround((f.back_wall[0].x + f.back_wall[1].x) / 2);
    const int y = (int)std::lround((f.back_wall[0].y + f.back_wall[1].y) / 2) + 4;

    auto back_pixel = [&]() {
        lv_draw_buf_t* snap = lv_snapshot_take(container_, LV_COLOR_FORMAT_ARGB8888);
        REQUIRE(snap != nullptr);
        const uint8_t* px = snap->data + y * snap->header.stride + x * 4;
        const lv_color_t c = lv_color_make(px[2], px[1], px[0]);
        lv_draw_buf_destroy(snap);
        return c;
    };
    auto back_token = [](bool dark) {
        const char* hex = lv_xml_get_const(lv_xml_component_get_scope("ams_unit_detail"),
                                           dark ? "tray_back_dark" : "tray_back_light");
        REQUIRE(hex != nullptr);
        return theme_manager_parse_hex_color(hex);
    };
    auto near = [](lv_color_t a, lv_color_t b) {
        return std::abs(a.red - b.red) <= 2 && std::abs(a.green - b.green) <= 2 &&
               std::abs(a.blue - b.blue) <= 2;
    };

    const bool dark = theme_manager_is_dark_mode();
    REQUIRE_FALSE(near(back_token(dark), back_token(!dark)));
    REQUIRE(near(back_pixel(), back_token(dark)));

    theme_manager_toggle_dark_mode();
    process_lvgl(20);
    CHECK(near(back_pixel(), back_token(!dark)));

    theme_manager_toggle_dark_mode();
    process_lvgl(20);
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS unit detail: the unit is centered and its tubes drop from the spools",
                 "[ams][tray][ui_integration]") {
    const char* mode = GENERATE("passive", "off");
    CAPTURE(mode);
    build(mode);

    // The unit: the box's faces.
    const tray::TrayFaces f = tray::tray_faces(box_);
    float lo = 1e9f, hi = -1e9f;
    for (const auto* face : {f.back_wall, f.floor, f.left_wall, f.front, f.right_side}) {
        for (int k = 0; k < 4; k++) {
            lo = std::min(lo, face[k].x);
            hi = std::max(hi, face[k].x);
        }
    }
    int32_t left = origin_.x + (int32_t)std::lround(lo);
    int32_t right = origin_.x + (int32_t)std::lround(hi);
    lv_obj_t* row = lv_obj_get_parent(container_);
    lv_area_t rc;
    lv_obj_get_content_coords(row, &rc);
    INFO("unit " << left << ".." << right << " in row " << rc.x1 << ".." << rc.x2);
    CHECK(std::abs((left - rc.x1) - (rc.x2 - right)) <= 4);

    // The path canvas reads the laid-out spools, so each lane sits under its spool.
    lv_obj_t* canvas = lv_obj_find_by_name(panel_obj_, "path_canvas");
    REQUIRE(canvas != nullptr);
    const auto* data = helix::ui::fpath::get_data(canvas);
    REQUIRE(data != nullptr);
    lv_area_t cc;
    lv_obj_get_coords(canvas, &cc);
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        lv_obj_t* spool = lv_obj_find_by_name(slot(i), "spool_graphic");
        REQUIRE(spool != nullptr);
        lv_area_t a;
        lv_obj_get_coords(spool, &a);
        const int32_t lane_x = cc.x1 + helix::ui::fpath::get_slot_x(data, i, cc.x1);
        CHECK(std::abs(lane_x - (a.x1 + a.x2) / 2) <= 2);
    }
}

TEST_CASE_METHOD(AmsTrayPanelFixture, "AMS unit: a spool name stacks directly over its material",
                 "[ams][tray][ui_integration][slot_name]") {
    build("passive");
    int named = 0;
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        lv_obj_t* name = lv_obj_find_by_name(slot(i), "name_label");
        lv_obj_t* material = lv_obj_find_by_name(slot(i), "material_label");
        REQUIRE(name != nullptr);
        REQUIRE(material != nullptr);
        if (lv_obj_has_flag(name, LV_OBJ_FLAG_HIDDEN))
            continue;
        ++named;
        lv_area_t na, ma;
        lv_obj_get_coords(name, &na);
        lv_obj_get_coords(material, &ma);
        // The tray lifts the material above the unit; the name rides with it.
        const int32_t gap = ma.y1 - na.y2 - 1;
        CHECK(gap >= 0);
        CHECK(gap <= lv_obj_get_style_pad_row(slot(i), LV_PART_MAIN));
    }
    REQUIRE(named > 0);
}

TEST_CASE_METHOD(AmsTrayPanelFixture, "AMS unit: a named slot's badge sits on the box front",
                 "[ams][tray][ui_integration][slot_name]") {
    build("passive");
    int named = 0;
    for (int i = 0; i < 4; ++i) {
        lv_obj_t* name = lv_obj_find_by_name(slot(i), "name_label");
        REQUIRE(name != nullptr);
        if (!lv_obj_has_flag(name, LV_OBJ_FLAG_HIDDEN))
            ++named;
    }
    REQUIRE(named > 0);
    check_badges_on_front(4);
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS unit: a chip taller than the room opens the least gap under the box",
                 "[ams][tray][ui_integration][env_chip]") {
    build("passive");
    lv_obj_t* chip = lv_obj_find_by_name(panel_obj_, "env_indicator");
    REQUIRE(chip != nullptr);
    lv_obj_update_layout(chip);
    // Taller than any room the box leaves above the selector at the test size.
    lv_obj_set_style_min_height(chip, lv_obj_get_height(chip) + 60, LV_PART_MAIN);
    AmsPanelTestAccess::create_slots(*panel_, 4);
    lv_obj_update_layout(test_screen());
    process_lvgl(50);
    REQUIRE(helix::ui::ams_detail_tray_geometry(box_, lid_, lid_h_, half_));
    const int32_t gap = lv_obj_get_style_pad_bottom(container_, LV_PART_MAIN);
    CHECK(gap > 0);
    check_env_chip_clear(4);

    // The least such gap: one pixel less and the chip no longer fits.
    lv_obj_t* canvas = lv_obj_find_by_name(panel_obj_, "path_canvas");
    lv_obj_set_style_pad_bottom(container_, gap - 1, LV_PART_MAIN);
    lv_obj_update_layout(test_screen());
    const tray::EnvChipSpot tighter =
        helix::ui::ams_detail_place_env_chip(chip, canvas, slot_grid_, 4, INT32_MAX, false);
    CHECK_FALSE(tighter.fits(lv_obj_get_width(chip), lv_obj_get_height(chip)));
}

TEST_CASE_METHOD(
    AmsTrayPanelFixture,
    "AMS unit at 5, 7 and 8 lanes: the chip clears the tubes and badges stay on the box",
    "[ams][tray][ui_integration][env_chip]") {
    for (int gates : {5, 7, 8}) {
        CAPTURE(gates);
        build("passive", gates);
        REQUIRE(lid_ == tray::LidMode::Unit);
        check_env_chip_clear(gates);
        check_badges_on_front(gates);
        panel_->on_deactivate(DeactivateReason::NavigateAway);
        panel_->clear_panel_reference();
        lv_obj_delete(panel_obj_);
        panel_obj_ = nullptr;
        panel_.reset();
        process_lvgl(10);
    }
}
