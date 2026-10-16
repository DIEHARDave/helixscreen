// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_panel_toolhead_tap.cpp
 * @brief On a one-head-per-tool path, a head tap mounts that tool and a tube
 *        tap opens the slot menu, never a tool change.
 */

#include "ui_filament_path_canvas.h"
#include "ui_modal.h"
#include "ui_panel_ams.h"

#include "../test_fixtures.h"
#include "../test_helpers/ams_panel_test_access.h"
#include "../test_helpers/modal_answer.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/tool_switcher_rig.h"
#include "safety_settings_manager.h"
#include "tool_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::parallel_tap_at;
using helix::ui::ParallelTapTarget;

// ============================================================================
// parallel_tap_at(): pure
//
// Three columns at x = 100, 200, 300; tool_scale 10, so a column is 30 px
// either side of its x and the head band 40 px either side of toolhead_y 400.
// The lanes own everything above sensor_y 250.
// ============================================================================

namespace {
constexpr int32_t kSlotX[] = {100, 200, 300};
constexpr int32_t kToolheadY = 400;
constexpr int32_t kSensorY = 250;
constexpr int32_t kScale = 10;

helix::ui::ParallelTap tap(int32_t x, int32_t y) {
    return parallel_tap_at({x, y}, kToolheadY, kSensorY, kScale, kSlotX, 3);
}
} // namespace

TEST_CASE("parallel_tap_at: a head glyph answers as its toolhead", "[ams][path][hit_test]") {
    const auto t = tap(200, 410);
    CHECK(t.target == ParallelTapTarget::Toolhead);
    CHECK(t.slot == 1);
    CHECK(tap(325, 365).target == ParallelTapTarget::Toolhead);
    CHECK(tap(325, 365).slot == 2);
}

TEST_CASE("parallel_tap_at: a tube or spool answers as its lane", "[ams][path][hit_test]") {
    for (int32_t y : {10, 120, 249}) {
        CAPTURE(y);
        const auto t = tap(100, y);
        CHECK(t.target == ParallelTapTarget::Lane);
        CHECK(t.slot == 0);
    }
}

TEST_CASE("parallel_tap_at: between the sensor and the head band is nothing",
          "[ams][path][hit_test]") {
    CHECK(tap(200, 300).target == ParallelTapTarget::None);
    CHECK(tap(200, 360).target == ParallelTapTarget::None);
    CHECK(tap(200, 440).target == ParallelTapTarget::None);
}

TEST_CASE("parallel_tap_at: between columns is nothing", "[ams][path][hit_test]") {
    CHECK(tap(150, 400).target == ParallelTapTarget::None);
    CHECK(tap(150, 100).target == ParallelTapTarget::None);
}

TEST_CASE("parallel_tap_at: a head reaching above the sensor row still answers as the head",
          "[ams][path][hit_test]") {
    // Sensor row inside the head band: the band wins.
    const auto t = parallel_tap_at({200, 380}, kToolheadY, 390, kScale, kSlotX, 3);
    CHECK(t.target == ParallelTapTarget::Toolhead);
    CHECK(t.slot == 1);
}

// ============================================================================
// AmsPanel: what each tap does
// ============================================================================

namespace {

struct ToolheadTapFixture : public XMLTestFixture {
    ToolheadTapFixture() {
        saved_confirm = SafetySettingsManager::instance().get_tool_change_require_confirmation();
        test::set_wire_state(state(), PrintJobState::STANDBY);
        ensure_ams_widgets_registered();
        panel = std::make_unique<AmsPanel>(state(), &api());
        panel->init_subjects();
        panel_obj = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ams_panel", nullptr));
        REQUIRE(panel_obj != nullptr);
        panel->setup(panel_obj, test_screen());
        panel->on_activate();
        lv_obj_update_layout(test_screen());
        process_lvgl(50);
        REQUIRE(rig.backend->get_topology() == PathTopology::PARALLEL);
        REQUIRE(ToolState::instance().active_tool_index() == 0);
    }

    ~ToolheadTapFixture() override {
        ModalStack::instance().clear();
        panel->on_deactivate(DeactivateReason::NavigateAway);
        panel->clear_panel_reference();
        lv_obj_delete(panel_obj);
        process_lvgl(10);
        panel.reset();
        SafetySettingsManager::instance().set_tool_change_require_confirmation(saved_confirm);
    }

    void set_confirm(bool on) {
        SafetySettingsManager::instance().set_tool_change_require_confirmation(on);
    }

    ToolChangerRig rig{3};
    std::unique_ptr<AmsPanel> panel;
    lv_obj_t* panel_obj = nullptr;
    bool saved_confirm = true;
};

} // namespace

TEST_CASE_METHOD(ToolheadTapFixture, "a tap on another tool's head asks, then mounts it",
                 "[ams][path][toolhead_tap]") {
    set_confirm(true);
    AmsPanelTestAccess::tap_toolhead(*panel, 1);
    CHECK(rig.backend->change_tool_calls == 0);
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);
    CHECK_FALSE(AmsPanelTestAccess::context_menu_open(*panel));

    test::answer_top_modal("btn_primary");
    CHECK(rig.backend->change_tool_calls == 1);
    CHECK(rig.backend->last_tool == 1);
}

TEST_CASE_METHOD(ToolheadTapFixture, "with confirmation off a head tap mounts it at once",
                 "[ams][path][toolhead_tap]") {
    set_confirm(false);
    AmsPanelTestAccess::tap_toolhead(*panel, 2);
    CHECK(rig.backend->change_tool_calls == 1);
    CHECK(rig.backend->last_tool == 2);
    CHECK(ModalStack::instance().stack_empty());
}

TEST_CASE_METHOD(ToolheadTapFixture, "a tube tap never changes tool, whatever the setting",
                 "[ams][path][toolhead_tap]") {
    for (bool confirm : {false, true}) {
        CAPTURE(confirm);
        set_confirm(confirm);
        AmsPanelTestAccess::tap_lane(*panel, 1);
        CHECK(rig.backend->change_tool_calls == 0);
        CHECK(ModalStack::instance().stack_empty());
        CHECK(AmsPanelTestAccess::context_menu_open(*panel));
    }
}

TEST_CASE_METHOD(ToolheadTapFixture, "the mounted head's tap opens the slot menu",
                 "[ams][path][toolhead_tap]") {
    set_confirm(false);
    AmsPanelTestAccess::tap_toolhead(*panel, 0);
    CHECK(rig.backend->change_tool_calls == 0);
    CHECK(AmsPanelTestAccess::context_menu_open(*panel));
}

TEST_CASE_METHOD(ToolheadTapFixture, "a head tap while the filament system is busy is ignored",
                 "[ams][path][toolhead_tap]") {
    set_confirm(false);
    rig.backend->report_busy = true;
    AmsPanelTestAccess::tap_toolhead(*panel, 1);
    CHECK(rig.backend->change_tool_calls == 0);
    CHECK(ModalStack::instance().stack_empty());
    CHECK_FALSE(AmsPanelTestAccess::context_menu_open(*panel));
    CHECK(rig.warnings.empty());
}

TEST_CASE_METHOD(ToolheadTapFixture, "a head tap while printing is refused with a reason",
                 "[ams][path][toolhead_tap]") {
    set_confirm(true);
    test::set_wire_state(state(), PrintJobState::PRINTING);
    AmsPanelTestAccess::tap_toolhead(*panel, 1);
    CHECK(rig.backend->change_tool_calls == 0);
    CHECK(ModalStack::instance().stack_empty());
    REQUIRE(rig.warnings.size() == 1);
    CHECK(rig.warnings[0].find("Cannot run filament operation while printing") !=
          std::string::npos);
}

TEST_CASE_METHOD(ToolheadTapFixture, "a head tap with spools on the bed is refused",
                 "[ams][path][toolhead_tap]") {
    set_confirm(false);
    state().print_state().set_spool_latch(true);
    AmsPanelTestAccess::tap_toolhead(*panel, 1);
    CHECK(rig.backend->change_tool_calls == 0);
    CHECK(rig.warnings.size() == 1);
    state().print_state().set_spool_latch(false);
}
