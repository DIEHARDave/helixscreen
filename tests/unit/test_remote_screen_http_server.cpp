// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_remote_screen_http_server.cpp
 * @brief ScreenHttpServer's view routes over a real loopback socket.
 */

#include "../test_helpers/raw_http_client.h"
#include "remote_screen_http_server.h"
#include "remote_screen_server_sink.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using helix::test::raw_http;
using namespace std::chrono_literals;

namespace {

struct Frame {
    std::vector<uint8_t> px;
    RemoteScreenFrame f;
    Frame(int w, int h) : px(static_cast<size_t>(w) * h * 4, 0x7F) {
        f.px_map = px.data();
        f.px_map_len = px.size();
        f.x2 = w - 1;
        f.y2 = h - 1;
        f.disp_w = w;
        f.disp_h = h;
        f.src_stride = static_cast<uint32_t>(w * 4);
        f.src_format = RemoteScreenPixelFormat::BGRA8888;
    }
};

struct ServerFixture {
    std::shared_ptr<ScreenFrameStore> store = std::make_shared<ScreenFrameStore>();
    std::atomic<int> redraws{0};
    std::unique_ptr<ScreenHttpServer> server;

    explicit ServerFixture(ScreenServerConfig cfg = {}) {
        ScreenServerHooks hooks;
        hooks.request_full_redraw = [this] { redraws.fetch_add(1); };
        hooks.is_printing = [] { return false; };
        server = std::make_unique<ScreenHttpServer>(store, hooks);
        cfg.bind_host = "127.0.0.1";
        cfg.port = 0;
        REQUIRE(server->start(cfg));
        REQUIRE(server->port() > 0);
    }

    void capture(int w = 32, int h = 24) {
        Frame frame(w, h);
        store->on_frame(frame.f);
    }
};

} // namespace

TEST_CASE("http: snapshot returns image/jpeg with no-store after a frame",
          "[remote_screen][http]") {
    ServerFixture fx;
    fx.store->note_request();
    fx.capture();
    auto r = raw_http(fx.server->port(), "GET", "/snapshot.jpg");
    CHECK(r.status == 200);
    CHECK(r.has_header("content-type: image/jpeg"));
    CHECK(r.has_header("cache-control: no-store"));
    REQUIRE(r.body.size() > 4);
    CHECK(static_cast<uint8_t>(r.body[0]) == 0xFF);
    CHECK(static_cast<uint8_t>(r.body[1]) == 0xD8);
}

TEST_CASE("http: snapshot with no frame yet is 503 with Retry-After 1", "[remote_screen][http]") {
    ServerFixture fx;
    const auto start = std::chrono::steady_clock::now();
    auto r = raw_http(fx.server->port(), "GET", "/snapshot.jpg");
    const auto took = std::chrono::steady_clock::now() - start;
    CHECK(r.status == 503);
    CHECK(r.has_header("retry-after: 1"));
    CHECK(took < std::chrono::milliseconds(kFirstFrameWaitMs + 300));
}

TEST_CASE("http: first snapshot calls request_full_redraw once", "[remote_screen][http]") {
    ServerFixture fx;
    // The redraw lands while the first request waits for it.
    std::thread flusher([&] {
        while (fx.redraws.load() == 0) {
            std::this_thread::sleep_for(2ms);
        }
        fx.capture();
    });
    auto first = raw_http(fx.server->port(), "GET", "/snapshot.jpg");
    flusher.join();
    CHECK(first.status == 200);
    auto second = raw_http(fx.server->port(), "GET", "/snapshot.jpg");
    CHECK(second.status == 200);
    CHECK(fx.redraws.load() == 1);
}

TEST_CASE("http: snapshot?ts=123 is served like snapshot.jpg", "[remote_screen][http]") {
    ServerFixture fx;
    fx.store->note_request();
    fx.capture();
    auto r = raw_http(fx.server->port(), "GET", "/snapshot.jpg?ts=123");
    CHECK(r.status == 200);
    CHECK(r.has_header("content-type: image/jpeg"));
}

TEST_CASE("http: info reports w, h and control", "[remote_screen][http]") {
    ScreenServerConfig cfg;
    cfg.control_allowed = true;
    ServerFixture fx(cfg);
    fx.store->note_request();
    fx.capture(48, 20);
    auto r = raw_http(fx.server->port(), "GET", "/info");
    REQUIRE(r.status == 200);
    auto j = nlohmann::json::parse(r.body, nullptr, false);
    REQUIRE(j.is_object());
    CHECK(j["w"] == 48);
    CHECK(j["h"] == 20);
    CHECK(j["control"] == true);

    fx.server->set_control_allowed(false);
    auto off =
        nlohmann::json::parse(raw_http(fx.server->port(), "GET", "/info").body, nullptr, false);
    CHECK(off["control"] == false);
}

TEST_CASE("http: index serves the page file, 404 when it is missing", "[remote_screen][http]") {
    const std::string page = "/tmp/helix_remote_screen_page_test.html";
    {
        std::ofstream out(page);
        out << "<html>screen</html>";
    }
    ScreenServerConfig cfg;
    cfg.page_path = page;
    ServerFixture fx(cfg);
    auto r = raw_http(fx.server->port(), "GET", "/");
    CHECK(r.status == 200);
    CHECK(r.has_header("content-type: text/html"));
    CHECK(r.body == "<html>screen</html>");
    std::remove(page.c_str());

    auto missing = raw_http(fx.server->port(), "GET", "/");
    CHECK(missing.status == 404);
}

TEST_CASE("http: stop() while a request waits returns promptly", "[remote_screen][http]") {
    ServerFixture fx;
    std::thread client([&] {
        // No frame ever arrives, so the handler sits in its first-frame wait.
        (void)raw_http(fx.server->port(), "GET", "/snapshot.jpg");
    });
    while (fx.redraws.load() == 0) {
        std::this_thread::sleep_for(1ms);
    }
    const auto start = std::chrono::steady_clock::now();
    fx.server->stop();
    // Well inside the first-frame wait: stop() wakes the handler instead of waiting it out.
    CHECK(std::chrono::steady_clock::now() - start <
          std::chrono::milliseconds(kFirstFrameWaitMs / 2));
    client.join();
}

TEST_CASE("http: a port already in use fails start cleanly", "[remote_screen][http]") {
    ServerFixture first;
    auto store = std::make_shared<ScreenFrameStore>();
    ScreenHttpServer second(store, {});
    ScreenServerConfig cfg;
    cfg.bind_host = "127.0.0.1";
    cfg.port = first.server->port();
    CHECK_FALSE(second.start(cfg));
    CHECK(second.port() == 0);
}
