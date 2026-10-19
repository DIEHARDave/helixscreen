// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_network_settings_wlan_toggle.cpp
 * @brief The WiFi switch shows the tapped state at once and takes no further tap
 *        until the radio has answered.
 */

#include "ui_overlay_network_settings.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/network_settings_overlay_test_access.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "http_executor.h"
#include "lvgl/src/misc/lv_event_private.h"
#include "wifi_backend_mock.h"
#include "wifi_manager.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "../catch_amalgamated.hpp"

using Access = NetworkSettingsOverlayTestAccess;

namespace {

/// Radio calls park once armed, until the test releases them.
class GatedRadioBackend : public WifiBackendMock {
  public:
    WiFiError set_radio_enabled(bool on) override {
        if (armed_.load()) {
            entered_.store(true);
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return released_; });
            if (fail_) {
                return WiFiError(WiFiResult::RF_KILL_BLOCKED, "gated failure",
                                 "Could not turn WiFi radio on");
            }
        }
        return WifiBackendMock::set_radio_enabled(on);
    }

    void arm(bool fail) {
        fail_ = fail;
        armed_.store(true);
    }
    bool entered() const {
        return entered_.load();
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        cv_.notify_all();
    }

  private:
    std::atomic<bool> armed_{false};
    std::atomic<bool> entered_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    bool released_ = false;
    bool fail_ = false;
};

bool drain_until(const std::function<bool()>& pred, int timeout_ms = 5000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        helix::ui::UpdateQueue::instance().drain();
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    helix::ui::UpdateQueue::instance().drain();
    return pred();
}

class WlanToggleFixture : public LVGLUITestFixture {
  protected:
    ScopedRuntimeConfig scoped_config;
    GatedRadioBackend* backend = nullptr;
    std::shared_ptr<helix::WiFiManager> wm;

    /// Opens the overlay over a radio that is off.
    lv_obj_t* open_overlay(NetworkSettingsOverlay& overlay) {
        helix::http::HttpExecutor::fast().start();
        get_runtime_config()->test_mode = true;
        get_runtime_config()->use_real_wifi = false;
        auto owned = std::make_unique<GatedRadioBackend>();
        backend = owned.get();
        wm = std::make_shared<helix::WiFiManager>(std::move(owned));
        wm->init_self_reference(wm);
        helix::ui::UpdateQueue::instance().drain();
        backend->set_radio_enabled(false);

        overlay.init_subjects();
        overlay.register_callbacks();
        Access::wifi_manager(overlay) = wm;
        lv_obj_t* root = overlay.create(test_screen());
        REQUIRE(root != nullptr);
        overlay.on_activate();
        helix::ui::UpdateQueue::instance().drain();
        return root;
    }

    /// What a tap does to a switch: flip CHECKED, then hand the change to this
    /// overlay's handler (the XML callback resolves the app's global overlay).
    static void tap(NetworkSettingsOverlay& overlay, lv_obj_t* sw) {
        if (lv_obj_has_state(sw, LV_STATE_CHECKED)) {
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
        lv_event_t e{};
        e.code = LV_EVENT_VALUE_CHANGED;
        e.original_target = sw;
        e.current_target = sw;
        Access::wlan_toggle_changed(overlay, &e);
    }

    static bool locked(lv_obj_t* sw) {
        return lv_obj_has_state(sw, LV_STATE_DISABLED);
    }
};

} // namespace

TEST_CASE_METHOD(WlanToggleFixture,
                 "The WiFi switch holds the tapped state and stays locked until the radio answers",
                 "[network_settings][wlan_toggle]") {
    NetworkSettingsOverlay overlay;
    lv_obj_t* root = open_overlay(overlay);
    lv_obj_t* sw = lv_obj_find_by_name(root, "wlan_toggle");
    REQUIRE(sw != nullptr);
    REQUIRE_FALSE(lv_obj_has_state(sw, LV_STATE_CHECKED));
    REQUIRE_FALSE(locked(sw));

    backend->arm(/*fail=*/false);
    tap(overlay, sw);

    REQUIRE(drain_until([this] { return backend->entered(); }));
    CHECK(lv_obj_has_state(sw, LV_STATE_CHECKED));
    CHECK(locked(sw));

    backend->release();
    REQUIRE(drain_until([sw] { return !locked(sw); }));
    CHECK(lv_obj_has_state(sw, LV_STATE_CHECKED));

    overlay.cleanup();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(WlanToggleFixture,
                 "A WiFi switch whose radio change fails unlocks showing the radio's state",
                 "[network_settings][wlan_toggle]") {
    NetworkSettingsOverlay overlay;
    lv_obj_t* root = open_overlay(overlay);
    lv_obj_t* sw = lv_obj_find_by_name(root, "wlan_toggle");
    REQUIRE(sw != nullptr);

    backend->arm(/*fail=*/true);
    tap(overlay, sw);
    REQUIRE(drain_until([this] { return backend->entered(); }));
    CHECK(locked(sw));

    backend->release();
    REQUIRE(drain_until([sw] { return !locked(sw); }));
    CHECK_FALSE(lv_obj_has_state(sw, LV_STATE_CHECKED));

    overlay.cleanup();
    helix::ui::UpdateQueue::instance().drain();
}
