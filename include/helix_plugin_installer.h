// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file helix_plugin_installer.h
 * @brief Auto-installer for helix_print Moonraker plugin
 *
 * Handles detection and installation of the helix_print plugin:
 * - For local Moonraker (localhost): Auto-install via bundled install.sh
 * - For remote Moonraker: Show curl command for manual installation
 *
 * The plugin enables server-side G-code modification, which is faster and
 * safer than client-side modification on memory-constrained devices.
 */

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Forward declaration

namespace helix {
class IMoonrakerAPI;
struct HelixPluginInstallerTestAccess; // test-only friend (tests/test_helpers/)

// ============================================================================
// Plugin Install State
// ============================================================================

/**
 * @brief State of the plugin installation process
 */
enum class PluginInstallState {
    IDLE,            ///< No installation in progress
    INSTALLING,      ///< Installation is running
    SUCCESS,         ///< Installation completed successfully
    NEEDS_ATTENTION, ///< Uninstall removed the plugin, but left a config file that needs a look
    FAILED           ///< Installation failed
};

/**
 * @brief Outcome of a local uninstall attempt
 *
 * Mirrors install.sh's own --uninstall / --uninstall-auto exit codes: 0
 * maps to SUCCESS, 2 to NEEDS_ATTENTION, anything else to FAILED.
 */
enum class UninstallOutcome {
    SUCCESS,         ///< Plugin removed; nothing left in the config to clean up
    NEEDS_ATTENTION, ///< Plugin removed, but a config file was skipped or failed
                     ///< during the phase-tracking strip and needs a look
    FAILED           ///< Uninstall failed; the plugin may still be present
};

// ============================================================================
// HelixPluginInstaller Class
// ============================================================================

/**
 * @brief Manages helix_print plugin detection and installation
 *
 * @note Thread Safety:
 *       - uninstall_local(): MUST be called from main thread only. It blocks
 *         during script execution and invokes its callback inline. For
 *         non-blocking UI, wrap in std::thread or std::async.
 *       - install_local_sync(): Blocks during script execution; invokes no
 *         callback, so it is safe to call from a worker thread.
 *       - get_state() / is_installing(): Thread-safe (atomic read).
 *       - All other methods: Main thread only.
 *
 * Usage:
 *   1. Create installer and set API
 *   2. Check if plugin is missing: !PrinterState::service_has_helix_plugin()
 *   3. Check if should prompt: installer.should_prompt_install()
 *   4. For local: installer.install_local_sync()
 *   5. For remote: show dialog with installer.get_remote_install_command()
 */
class HelixPluginInstaller {
  public:
    using UninstallCallback =
        std::function<void(UninstallOutcome outcome, const std::string& message)>;

    HelixPluginInstaller() = default;
    ~HelixPluginInstaller() = default;

    // Non-copyable, non-movable (atomic members are not movable)
    HelixPluginInstaller(const HelixPluginInstaller&) = delete;
    HelixPluginInstaller& operator=(const HelixPluginInstaller&) = delete;
    HelixPluginInstaller(HelixPluginInstaller&&) = delete;
    HelixPluginInstaller& operator=(HelixPluginInstaller&&) = delete;

    // === Configuration ===

    /**
     * @brief Set the IMoonrakerAPI instance for plugin status checks
     */
    void set_api(IMoonrakerAPI* api);

    /**
     * @brief Set the WebSocket URL (for localhost detection)
     *
     * Normally derived from IMoonrakerAPI, but can be set directly for testing.
     */
    void set_websocket_url(const std::string& url);

    // === Detection ===

    /**
     * @brief Check if Moonraker is running on localhost
     *
     * Uses the WebSocket URL to determine if we're connected locally.
     * Local connections can use auto-install via bundled install.sh.
     *
     * @return true if connected to localhost/127.0.0.1/::1
     */
    [[nodiscard]] bool is_local_moonraker() const;

    // === Installation ===

    /**
     * @brief Result of synchronous installation
     */
    struct SyncInstallResult {
        bool success = false;
        std::string message;
    };

    /**
     * @brief Attempt local auto-installation (synchronous, no callback)
     *
     * This method blocks during script execution (up to 60s timeout).
     * Returns a result struct instead of using callbacks, which avoids
     * std::function-related crashes on ARM/glibc static builds.
     *
     * @return Result with success flag and message
     */
    [[nodiscard]] SyncInstallResult install_local_sync();

    /**
     * @brief Attempt local auto-uninstallation
     *
     * Runs the bundled install.sh script with --uninstall-auto. Only works
     * when connected to local Moonraker.
     *
     * @param callback Called with the outcome once the script exits
     */
    void uninstall_local(UninstallCallback callback);

    /**
     * @brief The command a user runs on a remote Moonraker host to install the plugin
     *
     * plugin_remote_install_command() for this app's own release, so the
     * plugin installed matches the app talking to it.
     */
    [[nodiscard]] std::string get_remote_install_command() const;

    /**
     * @brief Get path to bundled install.sh script
     *
     * Searches for install.sh relative to the executable.
     *
     * @return Path to script, or empty if not found
     */
    [[nodiscard]] std::string get_install_script_path() const;

    // === Preference Management ===

    /**
     * @brief Check if we should prompt for plugin installation
     *
     * Returns false with beta features off, before the first-run wizard is
     * done, once the user checked "don't ask again", and once the prompt is
     * settled for this session.
     *
     * @return true if we should show the install prompt
     */
    [[nodiscard]] bool should_prompt_install() const;

    /**
     * @brief Ask no more for the rest of this session
     *
     * Called when the prompt is shown, whatever the user answers, and when the
     * plugin is uninstalled: the plugin then reads as absent, which is no
     * reason to offer it back. Process-wide, because each panel owns its own
     * installer. Main thread only.
     */
    static void settle_prompt_for_session();

    /**
     * @brief Save preference to not prompt again
     *
     * Called when user dismisses the install dialog with "don't ask again".
     */
    void set_install_declined();

    // === State ===

    /**
     * @brief Get current installation state
     */
    [[nodiscard]] PluginInstallState get_state() const;

    /**
     * @brief Check if installation is currently in progress
     */
    [[nodiscard]] bool is_installing() const;

  private:
    friend struct HelixPluginInstallerTestAccess;

    IMoonrakerAPI* api_ = nullptr;
    std::string websocket_url_;
    std::atomic<PluginInstallState> state_{PluginInstallState::IDLE};

    // Where install.sh is looked for. Empty means beside the executable, plus the
    // system share dir; a set root is searched alone, so nothing outside it runs.
    std::string script_search_root_;
    // How long install.sh may run before it is killed.
    int script_timeout_seconds_ = 60;

    // Set by settle_prompt_for_session(); cleared only by a new process.
    static bool prompt_settled_for_session_;

    // Config key for "don't ask again" preference
    static constexpr const char* kPrefInstallDeclined = "/plugin_install_declined";
};

/**
 * @brief The git ref holding the plugin of app version @p version
 *
 * A release version names its tag (1.2.0-beta.3 is v1.2.0-beta.3; build
 * metadata after '+' is dropped). Anything else, such as a "dev" build, is main.
 */
std::string plugin_release_ref(std::string_view version);

/**
 * @brief The shell one-liner that installs the plugin of git ref @p ref
 *
 * Fetches that ref's moonraker-plugin/remote-install.sh and runs it with sh,
 * pinned to the same ref, so the plugin files and the installer that places
 * them both come from it.
 */
std::string plugin_remote_install_command(std::string_view ref);

/**
 * @brief What the boot-time install prompt does when it is checked
 */
enum class PluginPromptStep {
    Stop,          ///< the plugin is not known absent: nothing to offer
    WaitForJob,    ///< a job holds the machine; ask again when it lets go
    WaitForScreen, ///< a modal or the print status overlay is up; ask again shortly
    Offer,         ///< open it now (preferences permitting)
};

/**
 * @brief Decide the prompt's next step
 *
 * The prompt never competes with a print: not while a job holds the machine,
 * and not over what follows one. A job that has just let go hands the screen to
 * its completion flow, which may not have put its modal up yet, so the first
 * check after a wait for the job always waits for the screen once more.
 *
 * @param plugin_absent discovery found the plugin absent (helix_plugin_installed == 0)
 * @param job_holds job_holds_machine() of the current print lifecycle
 * @param waited_for_job the previous step was WaitForJob
 * @param modal_up a modal is on screen
 * @param print_status_open the print status overlay is in the navigation stack
 */
constexpr PluginPromptStep decide_plugin_prompt(bool plugin_absent, bool job_holds,
                                                bool waited_for_job, bool modal_up,
                                                bool print_status_open) {
    if (!plugin_absent) {
        return PluginPromptStep::Stop;
    }
    if (job_holds) {
        return PluginPromptStep::WaitForJob;
    }
    if (waited_for_job || modal_up || print_status_open) {
        return PluginPromptStep::WaitForScreen;
    }
    return PluginPromptStep::Offer;
}

/**
 * @brief Split install.sh output into log lines: color escapes removed,
 *        blank lines dropped.
 */
std::vector<std::string> script_output_lines(const std::string& output);

} // namespace helix
