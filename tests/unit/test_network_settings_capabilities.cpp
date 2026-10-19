// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_network_settings_capabilities.cpp
 * @brief The Forget control, the WiFi radio toggle and the country row are
 *        shown only when the backend can perform them
 *        (prestonbrown/helixscreen#1429).
 */

#include "ui_overlay_network_settings.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/network_settings_overlay_test_access.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "system_settings_manager.h"
#include "wifi_backend_mock.h"
#include "wifi_country.h"
#include "wifi_manager.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using Access = NetworkSettingsOverlayTestAccess;

namespace {

/// A backend that owns neither the credential store nor the radio, the shape
/// of a printer whose network daemon manages WiFi.
class NoForgetNoToggleBackend : public WifiBackendMock {
  public:
    bool supports_forget() const override {
        return false;
    }
    bool supports_radio_toggle() const override {
        return false;
    }
    bool supports_country() const override {
        return false;
    }
};

class CapabilityFixture : public LVGLUITestFixture {
  protected:
    ScopedRuntimeConfig scoped_config;

    lv_obj_t* open_overlay(NetworkSettingsOverlay& overlay, std::unique_ptr<WifiBackend> backend) {
        get_runtime_config()->test_mode = true;
        get_runtime_config()->use_real_wifi = false;
        auto wm = std::make_shared<helix::WiFiManager>(std::move(backend));
        wm->init_self_reference(wm);
        overlay.init_subjects();
        overlay.register_callbacks();
        Access::wifi_manager(overlay) = wm;
        lv_obj_t* root = overlay.create(test_screen());
        REQUIRE(root != nullptr);
        overlay.on_activate();
        return root;
    }

    static bool hidden(lv_obj_t* root, const char* name) {
        lv_obj_t* obj = lv_obj_find_by_name(root, name);
        REQUIRE(obj != nullptr);
        return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
};

} // namespace

TEST_CASE_METHOD(CapabilityFixture,
                 "Network settings offers Forget and the radio toggle when supported",
                 "[network_settings][1429]") {
    NetworkSettingsOverlay overlay;
    lv_obj_t* root = open_overlay(overlay, std::make_unique<WifiBackendMock>());

    CHECK_FALSE(hidden(root, "forget_network_icon"));
    CHECK_FALSE(hidden(root, "wlan_toggle"));
}

TEST_CASE_METHOD(
    CapabilityFixture,
    "Network settings hides Forget and the radio toggle when the backend cannot do them",
    "[network_settings][1429]") {
    NetworkSettingsOverlay overlay;
    lv_obj_t* root = open_overlay(overlay, std::make_unique<NoForgetNoToggleBackend>());

    CHECK(hidden(root, "forget_network_icon"));
    CHECK(hidden(root, "wlan_toggle"));
}

TEST_CASE_METHOD(CapabilityFixture,
                 "Network settings hints at 5 GHz under a world-domain dual-band radio",
                 "[network_settings][wifi_country]") {
    helix::SystemSettingsManager::instance().set_wifi_country("");
    NetworkSettingsOverlay overlay;
    lv_obj_t* root = open_overlay(overlay, std::make_unique<WifiBackendMock>());

    // The country is read on the control lane; the hint lands with the answer.
    CHECK_FALSE(hidden(root, "wifi_country_container"));
    REQUIRE(wait_until([&] { return !hidden(root, "wifi_country_hint"); }));
    lv_obj_t* dropdown = lv_obj_find_by_name(root, "wifi_country_dropdown");
    REQUIRE(dropdown != nullptr);
    CHECK(lv_dropdown_get_selected(dropdown) == 0);
}

TEST_CASE_METHOD(CapabilityFixture,
                 "Network settings shows the system's country and drops the hint",
                 "[network_settings][wifi_country]") {
    helix::SystemSettingsManager::instance().set_wifi_country("");
    auto backend = std::make_unique<WifiBackendMock>();
    backend->set_country_for_test("DE");
    NetworkSettingsOverlay overlay;
    lv_obj_t* root = open_overlay(overlay, std::move(backend));

    const int de = helix::wifi::country::dropdown_index("DE");
    REQUIRE(
        wait_until([&] { return lv_subject_get_int(&Access::wifi_country_index(overlay)) == de; }));
    CHECK(hidden(root, "wifi_country_hint"));
    lv_obj_t* dropdown = lv_obj_find_by_name(root, "wifi_country_dropdown");
    REQUIRE(dropdown != nullptr);
    CHECK(static_cast<int>(lv_dropdown_get_selected(dropdown)) == de);
}

TEST_CASE_METHOD(CapabilityFixture,
                 "Network settings hides the country row when the backend cannot set it",
                 "[network_settings][wifi_country]") {
    NetworkSettingsOverlay overlay;
    lv_obj_t* root = open_overlay(overlay, std::make_unique<NoForgetNoToggleBackend>());
    helix::ui::UpdateQueue::instance().drain();

    CHECK(hidden(root, "wifi_country_container"));
}
