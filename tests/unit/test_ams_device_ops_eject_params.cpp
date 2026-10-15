// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_device_ops_eject_params.cpp
 * @brief The Device Operations eject-params rows follow the backend capability.
 *
 * Run with: ./build/bin/helix-tests "[ams][device-ops][eject-params]"
 */

#include "ui_ams_device_operations_overlay.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "static_panel_registry.h"

#include <lvgl/lvgl.h>

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::get_ams_device_operations_overlay;

namespace {

class EjectParamsMock : public AmsBackendMock {
  public:
    EjectParamsMock(int slots, bool configurable)
        : AmsBackendMock(slots), configurable_(configurable) {}

    [[nodiscard]] BackendTraits traits() const override {
        BackendTraits t = AmsBackendMock::traits();
        t.supports_configurable_eject_params = configurable_;
        return t;
    }

  private:
    bool configurable_;
};

class EjectParamsFixture : public LVGLUITestFixture {
  public:
    EjectParamsFixture() {
        helix::ui::destroy_static_panels();
        helix::ui::UpdateQueue::instance().drain();
        AmsState::instance().deinit_subjects();
    }

    ~EjectParamsFixture() override {
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

    /// Install a backend with the given capability, build the overlay through show(), and
    /// return the value of the eject-params visibility subject.
    int eject_params_subject_value(bool configurable) {
        auto owned = std::make_unique<EjectParamsMock>(4, configurable);
        owned->set_operation_delay(0);
        REQUIRE(owned->start().success());
        auto& ams = AmsState::instance();
        ams.set_backend(std::move(owned));
        ams.init_subjects(true);
        ams.sync_from_backend();

        auto& overlay = get_ams_device_operations_overlay();
        overlay.init_subjects();
        overlay.show(test_screen());
        settle();

        lv_subject_t* subject = lv_xml_get_subject(nullptr, "ams_device_ops_has_eject_params");
        REQUIRE(subject != nullptr);
        return lv_subject_get_int(subject);
    }

    /// True when the named eject row is visible in the built overlay.
    bool row_visible(const char* name) {
        lv_obj_t* row = lv_obj_find_by_name(get_ams_device_operations_overlay().get_root(), name);
        REQUIRE(row != nullptr);
        return !lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN);
    }
};

} // namespace

TEST_CASE_METHOD(EjectParamsFixture, "eject params rows follow the backend capability",
                 "[ams][device-ops][eject-params]") {
    SECTION("a backend with configurable eject params shows the rows") {
        CHECK(eject_params_subject_value(true) == 1);
        CHECK(row_visible("row_qidi_eject_distance"));
        CHECK(row_visible("row_qidi_eject_velocity"));
    }

    SECTION("a backend without them hides the rows") {
        CHECK(eject_params_subject_value(false) == 0);
        CHECK_FALSE(row_visible("row_qidi_eject_distance"));
        CHECK_FALSE(row_visible("row_qidi_eject_velocity"));
    }

    SECTION("the vendor-named subject is gone") {
        eject_params_subject_value(true);
        CHECK(lv_xml_get_subject(nullptr, "ams_device_ops_is_qidi") == nullptr);
    }
}
