// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "remote_screen_http_server.h"

#include "http_listen_socket.h"
#include "hv/HttpServer.h"
#include "json_utils.h"
#include "remote_screen_server_sink.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <iterator>
#include <unistd.h>
#include <utility>

#include "hv/json.hpp"

namespace helix {

struct ScreenHttpServer::Impl {
    hv::HttpService router;
    hv::HttpServer server;
};

ScreenHttpServer::ScreenHttpServer(std::shared_ptr<ScreenFrameStore> store, ScreenServerHooks hooks)
    : store_(std::move(store)), hooks_(std::move(hooks)) {}

ScreenHttpServer::~ScreenHttpServer() {
    stop();
}

bool ScreenHttpServer::start(const ScreenServerConfig& cfg) {
    if (impl_) {
        return true;
    }
    cfg_ = cfg;
    control_allowed_.store(cfg.control_allowed);
    const SnapshotRateCap cap = snapshot_rate_cap(remote_screen_host_is_constrained());

    int port = 0;
    const int fd = bind_tcp_listener(cfg.bind_host.c_str(), cfg.port, &port);
    if (fd < 0) {
        spdlog::warn("[RemoteScreen] Cannot listen on {}:{} - screen server not started",
                     cfg.bind_host, cfg.port);
        return false;
    }

    auto impl = std::make_unique<Impl>();

    impl->router.GET("/snapshot.jpg", [this, cap](HttpRequest*, HttpResponse* resp) {
        const uint64_t gen = store_->generation();
        if (store_->note_request() == ScreenFrameStore::Need::FullRedraw) {
            if (hooks_.request_full_redraw) {
                hooks_.request_full_redraw();
            }
            store_->wait_for_generation_after(gen, std::chrono::milliseconds(kFirstFrameWaitMs));
        }
        const bool printing = hooks_.is_printing && hooks_.is_printing();
        const auto snap = store_->snapshot(printing, cap);
        resp->headers["Cache-Control"] = "no-store";
        if (!snap) {
            resp->headers["Retry-After"] = "1";
            return static_cast<int>(HTTP_STATUS_SERVICE_UNAVAILABLE);
        }
        resp->content_type = IMAGE_JPEG;
        resp->body.assign(reinterpret_cast<const char*>(snap->jpeg->data()), snap->jpeg->size());
        return static_cast<int>(HTTP_STATUS_OK);
    });

    impl->router.GET("/info", [this](HttpRequest*, HttpResponse* resp) {
        const auto [w, h] = store_->frame_size();
        nlohmann::json j = {{"w", w}, {"h", h}, {"control", control_allowed_.load()}};
        resp->headers["Cache-Control"] = "no-store";
        resp->content_type = APPLICATION_JSON;
        resp->body = json_util::safe_dump(j);
        return static_cast<int>(HTTP_STATUS_OK);
    });

    impl->router.GET("/", [this](HttpRequest*, HttpResponse* resp) {
        std::ifstream in(cfg_.page_path, std::ios::binary);
        if (!in) {
            resp->content_type = TEXT_PLAIN;
            resp->body = "screen page not installed\n";
            return static_cast<int>(HTTP_STATUS_NOT_FOUND);
        }
        resp->content_type = TEXT_HTML;
        resp->body.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return static_cast<int>(HTTP_STATUS_OK);
    });

    impl->server.registerHttpService(&impl->router);
    impl->server.setListenFD(fd);
    impl->server.setPort(0); // leave libhv's own Listen() path disabled
    impl->server.setThreadNum(1);
    if (impl->server.start() != 0) {
        spdlog::warn("[RemoteScreen] Screen server failed to start on port {}", port);
        ::close(fd);
        return false;
    }

    impl_ = std::move(impl);
    port_ = port;
    spdlog::info("[RemoteScreen] Screen server listening on {}:{}", cfg.bind_host, port);
    return true;
}

void ScreenHttpServer::stop() {
    if (!impl_) {
        return;
    }
    // A handler blocked in its first-frame wait would hold stop() for the whole wait.
    store_->abort_waits();
    impl_->server.stop();
    impl_.reset();
    port_ = 0;
    spdlog::info("[RemoteScreen] Screen server stopped");
}

int ScreenHttpServer::port() const {
    return port_;
}

void ScreenHttpServer::set_control_allowed(bool allowed) {
    control_allowed_.store(allowed);
}

} // namespace helix
