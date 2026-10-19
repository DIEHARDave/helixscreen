// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "remote_screen_service.h"

#include "ui_timer_guard.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "data_root_resolver.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "remote_screen_http_server.h"
#include "remote_screen_manager.h"
#include "remote_screen_server_sink.h"
#include "remote_screen_settings_manager.h"
#include "remote_screen_webcam.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <cstring>
#include <fstream>

namespace helix {

namespace {
/// How often a stopped viewer's buffers are checked for release.
constexpr uint32_t kIdleCheckMs = 2000;
} // namespace

bool remote_screen_firmware_provides() {
    const char* mode = std::getenv("HELIX_REMOTE_SCREEN");
    if (mode && std::strcmp(mode, "firmware") == 0) {
        return true;
    }
    const char* fb0 = std::getenv("HELIX_REMOTE_SCREEN_FB0");
    return fb0 && *fb0;
}

std::string remote_screen_route(const std::string& route_file) {
    std::ifstream in(route_file);
    std::string line;
    if (!in || !std::getline(in, line)) {
        return {};
    }
    const auto first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = line.find_last_not_of(" \t\r");
    return line.substr(first, last - first + 1);
}

RemoteScreenService& RemoteScreenService::instance() {
    static RemoteScreenService inst;
    return inst;
}

RemoteScreenService::RemoteScreenService() : store_(std::make_shared<ScreenFrameStore>()) {}

RemoteScreenService::~RemoteScreenService() {
    cancel_idle_timer();
}

bool RemoteScreenService::available() const {
#ifdef __ANDROID__
    return false;
#else
    return !remote_screen_firmware_provides();
#endif
}

void RemoteScreenService::init(RemoteScreenManager* screens) {
    auto& settings = RemoteScreenSettingsManager::instance();
    settings.set_available(available());
    if (!available()) {
        spdlog::info("[RemoteScreen] The firmware serves the screen; built-in server off");
        return;
    }
    if (screens && !sink_attached_) {
        screens->add_sink(std::make_unique<ScreenServerSink>(store_));
        screens->start();
        sink_attached_ = true;
    }
    settings.set_change_handler([this] { apply_settings(); });
    apply_settings();
}

void RemoteScreenService::watch_print_state() {
    auto& ps = get_printer_state();
    print_observer_ = ui::observe<int>(
        ps.print_state().get_print_active_subject(), this,
        [](RemoteScreenService* self, int active) { self->printing_.store(active != 0); },
        ps.print_state().get_static_subjects_lifetime());
}

void RemoteScreenService::apply_settings() {
    apply_server();
    if (IMoonrakerAPI* api = get_moonraker_api()) {
        sync_remote_screen_webcam(*api);
    }
}

void RemoteScreenService::apply_server() {
    auto& settings = RemoteScreenSettingsManager::instance();
    const bool want = available() && settings.get_enabled();

    if (!want) {
        if (server_) {
            server_->stop();
            server_.reset();
        }
        cancel_idle_timer();
        store_->release();
        return;
    }

    const std::string route = remote_screen_route(writable_path(".remote_screen_route"));
    const bool lan_mode = route.empty();
    if (server_ && lan_mode != lan_mode_) {
        server_->stop();
        server_.reset();
    }
    if (server_) {
        server_->set_control_allowed(settings.get_control());
        return;
    }

    ScreenServerHooks hooks;
    hooks.request_full_redraw = [] {
        ui::queue_update("remote_screen::full_redraw", [] {
            if (lv_obj_t* screen = lv_screen_active()) {
                lv_obj_invalidate(screen);
            }
            lv_obj_invalidate(lv_layer_top());
        });
    };
    hooks.is_printing = [this] { return printing_.load(); };

    ScreenServerConfig cfg;
    cfg.bind_host = lan_mode ? "0.0.0.0" : "127.0.0.1";
    cfg.port = settings.get_port();
    cfg.control_allowed = settings.get_control();
    cfg.lan_mode = lan_mode;
    cfg.page_path = asset_path("assets/remote_screen/index.html");

    auto server = std::make_unique<ScreenHttpServer>(store_, hooks);
    if (!server->start(cfg)) {
        return;
    }
    server_ = std::move(server);
    lan_mode_ = lan_mode;

    if (!idle_timer_) {
        idle_timer_ = lv_timer_create(
            [](lv_timer_t* t) {
                auto* self = static_cast<RemoteScreenService*>(lv_timer_get_user_data(t));
                if (self->store_->release_if_idle()) {
                    spdlog::debug("[RemoteScreen] Viewer gone; frame buffers released");
                }
            },
            kIdleCheckMs, this);
    }
}

void RemoteScreenService::cancel_idle_timer() {
    ui::lv_timer_cancel_safe(idle_timer_);
    idle_timer_ = nullptr;
}

void RemoteScreenService::shutdown() {
    RemoteScreenSettingsManager::instance().set_change_handler(nullptr);
    print_observer_.reset();
    if (server_) {
        server_->stop();
        server_.reset();
    }
    cancel_idle_timer();
    store_->release();
}

bool RemoteScreenService::running() const {
    return server_ != nullptr;
}

int RemoteScreenService::port() const {
    return server_ ? server_->port() : 0;
}

} // namespace helix
