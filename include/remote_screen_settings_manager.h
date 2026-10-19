// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"
#include "persisted_setting.h"
#include "subject_managed_panel.h"

#include <functional>

namespace helix {

/// Default port of the built-in screen server.
constexpr int kRemoteScreenDefaultPort = 7130;

/**
 * @brief The built-in screen server's settings (prestonbrown/helixscreen#1825).
 *
 * Two opt-ins, both off by default: share the screen as a webcam (view), then
 * let a browser drive it (control). Control is only ever on while view is.
 */
class RemoteScreenSettingsManager {
  public:
    static RemoteScreenSettingsManager& instance();

    /// The static PersistedSetting table this manager is built from.
    static settings::TableView persisted_table();

    RemoteScreenSettingsManager(const RemoteScreenSettingsManager&) = delete;
    RemoteScreenSettingsManager& operator=(const RemoteScreenSettingsManager&) = delete;

    void init_subjects();
    void deinit_subjects();

    bool get_enabled() const {
        return settings_.get_bool(Key::Enabled);
    }
    /// Turning view off turns control off with it.
    void set_enabled(bool enabled);

    bool get_control() const {
        return settings_.get_bool(Key::Control);
    }
    /// Ignored (stays off) while view is off.
    void set_control(bool control);

    /// `/remote_screen/port`; not shown in the UI. 0 asks for an ephemeral port.
    int get_port() const;

    /// Called after view or control changes. The server's owner installs it, so the
    /// settings UI never has to know a server exists (the firmware build has none).
    void set_change_handler(std::function<void()> handler) {
        change_handler_ = std::move(handler);
    }

    /// Whether the rows show at all: false where the firmware serves the screen.
    /// Starts false; RemoteScreenService decides at startup.
    void set_available(bool available);

    lv_subject_t* subject_enabled() {
        return settings_.subject(Key::Enabled);
    }
    lv_subject_t* subject_control() {
        return settings_.subject(Key::Control);
    }

  private:
    RemoteScreenSettingsManager();
    ~RemoteScreenSettingsManager() = default;

    enum class Key : uint8_t { Enabled, Control, COUNT };

    SubjectManager subjects_;
    settings::PersistedSettings<Key, static_cast<size_t>(Key::COUNT)> settings_;
    void notify_changed();

    std::function<void()> change_handler_;
    lv_subject_t available_subject_{};
    bool subjects_initialized_ = false;
};

} // namespace helix
