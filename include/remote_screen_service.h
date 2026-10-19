// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "lvgl/lvgl.h"

#include <atomic>
#include <memory>
#include <string>

namespace helix {

class RemoteScreenManager;
class ScreenFrameStore;
class ScreenHttpServer;

/// The firmware already runs a screen server of its own (a platform hook exported
/// HELIX_REMOTE_SCREEN=firmware, or set up the fb0 mirror it serves from).
bool remote_screen_firmware_provides();

/// The nginx mount the installer set up, read from @p route_file
/// ("/helixscreen/screen/"); empty when the file is absent or blank.
std::string remote_screen_route(const std::string& route_file);

/**
 * @brief Owns the built-in screen server for the life of the app (main thread).
 *
 * Starts and stops the HTTP server from RemoteScreenSettingsManager, live. The
 * capture sink is attached once and costs nothing while no browser asks.
 */
class RemoteScreenService {
  public:
    static RemoteScreenService& instance();

    RemoteScreenService(const RemoteScreenService&) = delete;
    RemoteScreenService& operator=(const RemoteScreenService&) = delete;

    /// Attach the capture sink to @p screens (null in tests) and start the server
    /// if sharing is on. No-op when the firmware provides its own server.
    void init(RemoteScreenManager* screens);
    /// Track whether a print is running, which lowers the frame rate cap.
    void watch_print_state();
    /// Start, stop or reconfigure the server to match the settings.
    void apply_settings();
    void shutdown();

    bool available() const;
    bool running() const;
    /// Bound port while running, else 0.
    int port() const;
    std::shared_ptr<ScreenFrameStore> store() const {
        return store_;
    }

  private:
    RemoteScreenService();
    ~RemoteScreenService();

    void apply_server();
    void cancel_idle_timer();

    std::shared_ptr<ScreenFrameStore> store_;
    std::unique_ptr<ScreenHttpServer> server_;
    bool lan_mode_ = false;
    bool sink_attached_ = false;
    lv_timer_t* idle_timer_ = nullptr;
    std::atomic<bool> printing_{false};
    ObserverGuard print_observer_;
};

} // namespace helix
