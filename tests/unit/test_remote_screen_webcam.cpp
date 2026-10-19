// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_remote_screen_webcam.cpp
 * @brief The webcam entry the screen server registers in Moonraker, and the round
 *        trip through the mock's registry.
 */

#include "../lvgl_test_fixture.h"
#include "host_identity.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "remote_screen_webcam.h"
#include "webcam_selection.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using json = nlohmann::json;

TEST_CASE("webcam item: view-only with route is mjpegstreamer-adaptive, relative snapshot, "
          "target_fps 2",
          "[remote_screen][webcam]") {
    auto item = remote_screen_webcam_item(true, false, "/helixscreen/screen/", "10.0.0.5", 7130);
    REQUIRE(item.has_value());
    CHECK((*item)["name"] == kRemoteScreenWebcamName);
    CHECK((*item)["service"] == "mjpegstreamer-adaptive");
    CHECK((*item)["snapshot_url"] == "/helixscreen/screen/snapshot.jpg");
    CHECK((*item)["target_fps"] == 2);
    CHECK((*item)["location"] == "printer");
    CHECK((*item)["enabled"] == true);
}

TEST_CASE("webcam item: control with route is iframe pointing at the route",
          "[remote_screen][webcam]") {
    auto item = remote_screen_webcam_item(true, true, "/helixscreen/screen/", "10.0.0.5", 7130);
    REQUIRE(item.has_value());
    CHECK((*item)["service"] == "iframe");
    CHECK((*item)["stream_url"] == "/helixscreen/screen/");
    CHECK((*item)["snapshot_url"] == "/helixscreen/screen/snapshot.jpg");
}

TEST_CASE("webcam item: LAN mode uses http://ip:port", "[remote_screen][webcam]") {
    auto view = remote_screen_webcam_item(true, false, "", "192.168.1.40", 7130);
    REQUIRE(view.has_value());
    CHECK((*view)["snapshot_url"] == "http://192.168.1.40:7130/snapshot.jpg");
    auto control = remote_screen_webcam_item(true, true, "", "192.168.1.40", 7131);
    REQUIRE(control.has_value());
    CHECK((*control)["stream_url"] == "http://192.168.1.40:7131/");
}

TEST_CASE("webcam item: disabled is nullopt", "[remote_screen][webcam]") {
    CHECK_FALSE(remote_screen_webcam_item(false, false, "/r/", "10.0.0.5", 7130).has_value());
    CHECK_FALSE(remote_screen_webcam_item(false, true, "", "10.0.0.5", 7130).has_value());
    // LAN mode with no address a browser could reach: registering would only mislead.
    CHECK_FALSE(remote_screen_webcam_item(true, false, "", "", 7130).has_value());
}

TEST_CASE("select_lan_ipv4 skips 127.x, 169.254.x and IPv6", "[remote_screen][webcam]") {
    CHECK(select_lan_ipv4({"127.0.0.1", "::1", "fe80::1", "169.254.3.4", "192.168.1.9",
                           "10.0.0.2"}) == "192.168.1.9");
    CHECK(select_lan_ipv4({"127.0.0.1", "fe80::1"}).empty());
    CHECK(select_lan_ipv4({}).empty());
}

TEST_CASE("is_own_screen_webcam matches the registered name only", "[remote_screen][webcam]") {
    WebcamInfo own;
    own.name = kRemoteScreenWebcamName;
    WebcamInfo other;
    other.name = "Nozzle";
    CHECK(webcam::is_own_screen_webcam(own));
    CHECK_FALSE(webcam::is_own_screen_webcam(other));
}

TEST_CASE_METHOD(LVGLTestFixture, "mock: post then list round-trips; delete removes it",
                 "[remote_screen][webcam]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    PrinterState state;
    state.init_subjects(false);
    MoonrakerAPI api(client, state);

    auto names = [&] {
        std::vector<std::string> out;
        api.timelapse().get_webcam_list(
            [&](const std::vector<WebcamInfo>& cams) {
                for (const auto& c : cams)
                    out.push_back(c.name);
            },
            [](const MoonrakerError&) {});
        return out;
    };
    auto has = [](const std::vector<std::string>& v, const std::string& n) {
        return std::find(v.begin(), v.end(), n) != v.end();
    };

    auto item = remote_screen_webcam_item(true, false, "/helixscreen/screen/", "", 7130);
    REQUIRE(item.has_value());
    bool posted = false;
    api.timelapse().post_webcam(*item, [&] { posted = true; }, [](const MoonrakerError&) {});
    CHECK(posted);
    CHECK(has(names(), kRemoteScreenWebcamName));

    // A second post replaces rather than duplicates.
    api.timelapse().post_webcam(*item, [] {}, [](const MoonrakerError&) {});
    const auto listed = names();
    CHECK(std::count(listed.begin(), listed.end(), kRemoteScreenWebcamName) == 1);

    bool deleted = false;
    api.timelapse().delete_webcam(
        kRemoteScreenWebcamName, [&] { deleted = true; }, [](const MoonrakerError&) {});
    CHECK(deleted);
    CHECK_FALSE(has(names(), kRemoteScreenWebcamName));
}
