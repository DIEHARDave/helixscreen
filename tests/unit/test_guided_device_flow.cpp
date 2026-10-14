// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_guided_device_flow.cpp
 * @brief Device actions that name a guided flow start it; every other action executes.
 *
 * Run with: ./build/bin/helix-tests "[ams][guided-flow]"
 */

#include "ui_ams_device_section_detail_overlay.h"
#include "ui_guided_device_flow.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "static_panel_registry.h"

#include <lvgl/lvgl.h>

#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::printer::ActionType;
using helix::printer::DeviceAction;
using helix::printer::DeviceSection;
using helix::ui::get_ams_device_section_detail_overlay;

namespace {

class GuidedFlowFixture : public LVGLUITestFixture {
  public:
    AmsBackendMock* backend = nullptr;

    GuidedFlowFixture() {
        helix::ui::destroy_static_panels();
        helix::ui::UpdateQueue::instance().drain();
        auto& ams = AmsState::instance();
        ams.deinit_subjects();

        auto owned = std::make_unique<AmsBackendMock>(4);
        backend = owned.get();
        backend->set_operation_delay(0);
        REQUIRE(backend->start().success());
        ams.set_backend(std::move(owned));
        ams.init_subjects(true);
        ams.sync_from_backend();
    }

    ~GuidedFlowFixture() override {
        helix::ui::unregister_guided_device_flow("t");
        NavigationManager::instance().go_back();
        settle();
        helix::ui::destroy_static_panels();
        settle();
        AmsState::instance().set_backend(nullptr);
    }

    void settle() {
        for (int i = 0; i < 4; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
        process_lvgl(10);
    }

    /// Build the overlay for a "calibration" section holding @p actions and return the
    /// button named @p id.
    lv_obj_t* open_section_and_find(std::vector<DeviceAction> actions, const char* id) {
        backend->set_device_sections({DeviceSection{"calibration", "Calibration", 0, ""}});
        backend->set_device_actions(std::move(actions));
        auto& overlay = get_ams_device_section_detail_overlay();
        overlay.init_subjects();
        overlay.show(test_screen(), "calibration", "Calibration");
        settle();
        lv_obj_t* btn = lv_obj_find_by_name(overlay.get_root(), id);
        REQUIRE(btn != nullptr);
        return btn;
    }

    static DeviceAction button(const char* id, const char* flow) {
        DeviceAction a = DeviceAction::button(id, id, "calibration");
        a.guided_flow = flow;
        return a;
    }

    void click(lv_obj_t* btn) {
        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
        settle();
    }

    std::string last_executed() const {
        return backend->get_last_executed_action().first;
    }
};

} // namespace

TEST_CASE_METHOD(GuidedFlowFixture,
                 "the section overlay starts a registered flow and executes the rest",
                 "[ams][guided-flow]") {
    int starts = 0;
    std::string started_label;
    helix::ui::register_guided_device_flow("t", {nullptr, [&](const std::string& label) {
                                                     ++starts;
                                                     started_label = label;
                                                 }});

    lv_obj_t* guided = open_section_and_find(
        {button("guided_action", "t"), button("plain_action", "")}, "guided_action");
    click(guided);
    CHECK(starts == 1);
    CHECK(started_label == "guided_action");
    CHECK(last_executed().empty());

    click(lv_obj_find_by_name(get_ams_device_section_detail_overlay().get_root(), "plain_action"));
    CHECK(starts == 1);
    CHECK(last_executed() == "plain_action");
}

TEST_CASE_METHOD(GuidedFlowFixture, "an unregistered guided flow never executes its action",
                 "[ams][guided-flow]") {
    lv_obj_t* btn =
        open_section_and_find({button("calibrate_cutter", "cfs_cutter")}, "calibrate_cutter");

    SECTION("before any registration") {
        click(btn);
        CHECK(last_executed().empty());
    }

#if HELIX_HAS_CFS
    SECTION("after registration the confirmation gates the action") {
        helix::ui::register_cfs_device_flows();
        click(btn);
        // A registered CFS flow asks before it moves the printer; the sweep has not run.
        CHECK(last_executed().empty());
        CHECK(helix::ui::find_guided_device_flow("cfs_cutter") != nullptr);
    }
#endif
}

TEST_CASE_METHOD(GuidedFlowFixture, "a flow's status attach runs once per section build",
                 "[ams][guided-flow]") {
    int attaches = 0;
    helix::ui::register_guided_device_flow("t", {[&](lv_obj_t* container) {
                                                     REQUIRE(container != nullptr);
                                                     ++attaches;
                                                 },
                                                 [](const std::string&) {}});

    open_section_and_find({button("a", "t"), button("b", "t")}, "a");
    CHECK(attaches == 1);
}
