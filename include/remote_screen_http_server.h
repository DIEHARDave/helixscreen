// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace helix {

class ScreenFrameStore;

struct ScreenServerConfig {
    /// "127.0.0.1" behind an nginx route, "0.0.0.0" when the server faces the LAN.
    std::string bind_host = "127.0.0.1";
    int port = 7130; ///< 0 = ephemeral (tests).
    bool control_allowed = false;
    /// Facing the LAN with nothing in front: control needs a paired browser.
    bool lan_mode = false;
    std::string page_path = "assets/remote_screen/index.html";
};

/// What the server needs from the app. Called on the HTTP thread, so an
/// implementation that touches LVGL queues the work to the UI thread.
struct ScreenServerHooks {
    /// Invalidate the whole display so the next flush repaints every pixel.
    std::function<void()> request_full_redraw;
    std::function<bool()> is_printing;
};

/**
 * @brief The built-in screen server: the UI as a JPEG for Mainsail/Fluidd, on its own port.
 *
 * Routes, all relative to the mount point (nginx strips its prefix):
 *   GET /snapshot.jpg  one JPEG of the current screen (query strings ignored)
 *   GET /info          {"w","h","control"} so the page can size itself
 *   GET /              the control page
 *
 * One libhv worker thread. A request may block for up to kFirstFrameWaitMs while
 * a full repaint lands; stop() wakes it.
 */
class ScreenHttpServer {
  public:
    ScreenHttpServer(std::shared_ptr<ScreenFrameStore> store, ScreenServerHooks hooks);
    ~ScreenHttpServer();

    ScreenHttpServer(const ScreenHttpServer&) = delete;
    ScreenHttpServer& operator=(const ScreenHttpServer&) = delete;

    /// Bind and start serving. False, with nothing running, when the port cannot be bound.
    bool start(const ScreenServerConfig& cfg);
    /// Idempotent.
    void stop();
    /// The bound port, or 0 when stopped.
    int port() const;

    void set_control_allowed(bool allowed);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::shared_ptr<ScreenFrameStore> store_;
    ScreenServerHooks hooks_;
    ScreenServerConfig cfg_;
    std::atomic<bool> control_allowed_{false};
    int port_ = 0;
};

} // namespace helix
