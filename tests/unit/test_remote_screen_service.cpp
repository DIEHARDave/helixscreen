// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_remote_screen_service.cpp
 * @brief The screen server's settings and the service that starts and stops it.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/raw_http_client.h"
#include "config.h"
#include "remote_screen_server_sink.h"
#include "remote_screen_service.h"
#include "remote_screen_settings_manager.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Fresh settings with the server on an ephemeral port, restored on exit.
struct RemoteScreenFixture : LVGLTestFixture {
    RemoteScreenFixture() {
        ::unsetenv("HELIX_REMOTE_SCREEN");
        ::unsetenv("HELIX_REMOTE_SCREEN_FB0");
        auto& data = ConfigTestAccess::data(*Config::get_instance());
        data.erase("remote_screen");
        Config::get_instance()->set<int>("/remote_screen/port", 0);
        auto& m = RemoteScreenSettingsManager::instance();
        m.deinit_subjects();
        m.init_subjects();
    }
    ~RemoteScreenFixture() override {
        RemoteScreenService::instance().shutdown();
        ::unsetenv("HELIX_REMOTE_SCREEN");
        ::unsetenv("HELIX_REMOTE_SCREEN_FB0");
        ConfigTestAccess::data(*Config::get_instance()).erase("remote_screen");
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "settings: defaults are off, port 7130",
                 "[remote_screen][settings]") {
    ConfigTestAccess::data(*Config::get_instance()).erase("remote_screen");
    auto& m = RemoteScreenSettingsManager::instance();
    m.deinit_subjects();
    m.init_subjects();
    CHECK_FALSE(m.get_enabled());
    CHECK_FALSE(m.get_control());
    CHECK(m.get_port() == 7130);
}

TEST_CASE_METHOD(RemoteScreenFixture, "settings: control cannot be on while view is off",
                 "[remote_screen][settings]") {
    auto& m = RemoteScreenSettingsManager::instance();
    m.set_control(true);
    CHECK_FALSE(m.get_control());

    m.set_enabled(true);
    m.set_control(true);
    CHECK(m.get_control());

    m.set_enabled(false);
    CHECK_FALSE(m.get_control());
    CHECK(lv_subject_get_int(m.subject_control()) == 0);
}

TEST_CASE_METHOD(RemoteScreenFixture,
                 "settings: a hand-edited config with control but no view loads off",
                 "[remote_screen][settings]") {
    Config::get_instance()->set<bool>("/remote_screen/enabled", false);
    Config::get_instance()->set<bool>("/remote_screen/control", true);
    auto& m = RemoteScreenSettingsManager::instance();
    m.deinit_subjects();
    m.init_subjects();
    CHECK_FALSE(m.get_control());
}

TEST_CASE("route: missing, blank and present route files", "[remote_screen][settings]") {
    const std::string path = "/tmp/helix_remote_screen_route_test";
    std::remove(path.c_str());
    CHECK(remote_screen_route(path).empty());
    { std::ofstream(path) << "  \n"; }
    CHECK(remote_screen_route(path).empty());
    { std::ofstream(path) << "/helixscreen/screen/\n"; }
    CHECK(remote_screen_route(path) == "/helixscreen/screen/");
    std::remove(path.c_str());
}

TEST_CASE("firmware_provides: both env spellings", "[remote_screen][settings]") {
    ::unsetenv("HELIX_REMOTE_SCREEN");
    ::unsetenv("HELIX_REMOTE_SCREEN_FB0");
    CHECK_FALSE(remote_screen_firmware_provides());
    ::setenv("HELIX_REMOTE_SCREEN", "firmware", 1);
    CHECK(remote_screen_firmware_provides());
    ::setenv("HELIX_REMOTE_SCREEN", "builtin", 1);
    CHECK_FALSE(remote_screen_firmware_provides());
    ::unsetenv("HELIX_REMOTE_SCREEN");
    ::setenv("HELIX_REMOTE_SCREEN_FB0", "/dev/fb0", 1);
    CHECK(remote_screen_firmware_provides());
    ::unsetenv("HELIX_REMOTE_SCREEN_FB0");
}

TEST_CASE_METHOD(RemoteScreenFixture, "service: enabling view starts the server live",
                 "[remote_screen][settings]") {
    auto& svc = RemoteScreenService::instance();
    svc.init(nullptr);
    CHECK_FALSE(svc.running());

    // The settings change alone starts it: the toggle's callback knows no server.
    RemoteScreenSettingsManager::instance().set_enabled(true);
    REQUIRE(svc.running());
    REQUIRE(svc.port() > 0);
    auto r = helix::test::raw_http(svc.port(), "GET", "/info");
    CHECK(r.status == 200);
}

TEST_CASE_METHOD(RemoteScreenFixture,
                 "service: disabling view frees the store and stops the server",
                 "[remote_screen][settings]") {
    auto& svc = RemoteScreenService::instance();
    RemoteScreenSettingsManager::instance().set_enabled(true);
    svc.init(nullptr);
    REQUIRE(svc.running());

    // A viewer has been served a frame.
    std::vector<uint8_t> px(16 * 16 * 4, 0x33);
    RemoteScreenFrame f;
    f.px_map = px.data();
    f.px_map_len = px.size();
    f.x2 = 15;
    f.y2 = 15;
    f.disp_w = 16;
    f.disp_h = 16;
    f.src_stride = 64;
    f.src_format = RemoteScreenPixelFormat::BGRA8888;
    svc.store()->note_request();
    svc.store()->on_frame(f);
    REQUIRE(svc.store()->snapshot(false, {5, 2}).has_value());
    REQUIRE(svc.store()->allocated_bytes() > 0);

    RemoteScreenSettingsManager::instance().set_enabled(false);
    CHECK_FALSE(svc.running());
    CHECK(svc.port() == 0);
    CHECK(svc.store()->allocated_bytes() == 0);
    CHECK_FALSE(svc.store()->wants_frames());
}

TEST_CASE_METHOD(RemoteScreenFixture, "service: firmware-provided screen never starts",
                 "[remote_screen][settings]") {
    ::setenv("HELIX_REMOTE_SCREEN", "firmware", 1);
    RemoteScreenSettingsManager::instance().set_enabled(true);
    auto& svc = RemoteScreenService::instance();
    CHECK_FALSE(svc.available());
    svc.init(nullptr);
    svc.apply_settings();
    CHECK_FALSE(svc.running());
}
