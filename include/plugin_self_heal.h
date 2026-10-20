// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "helix_plugin_installer.h"

#include <functional>

namespace helix {
class PrinterState;
struct PluginSelfHealTestAccess; // test-only friend (tests/test_helpers/)

/**
 * @brief Keeps an installed HelixPrint plugin loading the code this app ships
 *
 * Once per app start, after discovery has reported the plugin: on this host,
 * install.sh --relink re-points a link left naming an install directory that
 * moved or was replaced, and Moonraker restarts when it re-pointed the link or
 * the running plugin reports a version other than the one bundled here. A
 * restart waits for any job to let go. A remote Moonraker is only reported in
 * the log. decide_plugin_self_heal() holds the rule; this class gathers its
 * facts and acts. Main thread only.
 */
class PluginSelfHeal {
  public:
    /// Asks Moonraker to restart; returns whether a restart was requested.
    using MoonrakerRestarter = std::function<bool()>;

    PluginSelfHeal(PrinterState& printer_state, HelixPluginInstaller& installer,
                   MoonrakerRestarter restarter);

    /// Discovery has reported the plugin (installed or not): run the check,
    /// the first time in this process.
    void on_plugin_reported();

    /// The print lifecycle moved: a restart waiting for the job may go now.
    void on_job_state_changed();

    /// The check is running, so a plugin read as absent may yet turn out installed.
    [[nodiscard]] bool checking() const {
        return checking_;
    }

  private:
    friend struct PluginSelfHealTestAccess;

    void act(bool local, PluginLinkCheck link);
    void restart_moonraker();

    PrinterState& printer_state_;
    HelixPluginInstaller& installer_;
    MoonrakerRestarter restarter_;
    bool restart_waiting_for_job_ = false;
    bool checking_ = false;
    AsyncLifetimeGuard lifetime_;

    // Set when the check starts; a failed relink waits for the next start.
    static bool ran_this_start_;
};

} // namespace helix
