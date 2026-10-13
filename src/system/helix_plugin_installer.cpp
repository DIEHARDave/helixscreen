// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "helix_plugin_installer.h"

#include "config.h"
#include "helix_fs.h"
#include "host_identity.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace hfs = helix::fs;

namespace helix {

// ============================================================================
// Process Execution Helpers
// ============================================================================

/**
 * @brief Wait result from wait_for_child_with_timeout()
 */
struct WaitResult {
    bool timed_out = false;
    bool error = false;
    bool spawn_failed = false;
    int exit_code = -1;
    std::string error_message;
    std::string output;
};

// install.sh prints a handful of lines; anything past this is noise.
constexpr size_t MAX_SCRIPT_OUTPUT_BYTES = 16 * 1024;

/**
 * @brief Append whatever the child has written so far; fd is non-blocking.
 */
static void drain_output(int fd, std::string& out) {
    char buf[512];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            if (out.size() < MAX_SCRIPT_OUTPUT_BYTES) {
                out.append(buf,
                           std::min(static_cast<size_t>(n), MAX_SCRIPT_OUTPUT_BYTES - out.size()));
            }
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return;
    }
}

/**
 * @brief Log install.sh's output line by line.
 *
 * The script's own messages are the only record of why it failed, so a
 * failure logs them at warn; a success keeps them at debug.
 */
static void log_script_output(const std::string& output, bool succeeded) {
    for (const auto& line : script_output_lines(output)) {
        if (succeeded) {
            spdlog::debug("[PluginInstaller] install.sh: {}", line);
        } else {
            spdlog::warn("[PluginInstaller] install.sh: {}", line);
        }
    }
}

/**
 * @brief Wait for child process with timeout, handling EINTR
 *
 * Uses non-blocking waitpid with polling to implement timeout.
 * Properly handles EINTR interruptions from signals.
 *
 * @param pid Child process ID to wait for
 * @param timeout_seconds Maximum time to wait
 * @param operation_name Name for logging ("Installation" or "Uninstallation")
 * @param out_fd Non-blocking read end of the child's stdout/stderr, drained
 *               while waiting so a full pipe can never stall the child
 * @return WaitResult with exit code, captured output, or error info
 */
static WaitResult wait_for_child_with_timeout(pid_t pid, int timeout_seconds,
                                              const char* operation_name, int out_fd) {
    constexpr int POLL_INTERVAL_MS = 100;
    WaitResult result;

    int status = 0;
    auto start_time = std::chrono::steady_clock::now();

    while (true) {
        drain_output(out_fd, result.output);
        pid_t wait_result = waitpid(pid, &status, WNOHANG);

        if (wait_result == pid) {
            // Child exited
            drain_output(out_fd, result.output);
            result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            return result;
        }

        if (wait_result < 0) {
            // Handle EINTR - signal interrupted waitpid, just retry
            if (errno == EINTR) {
                continue;
            }
            // Actual error
            result.error = true;
            result.error_message = strerror(errno);
            spdlog::error("[PluginInstaller] waitpid error: {}", result.error_message);
            return result;
        }

        // Check timeout
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed > std::chrono::seconds(timeout_seconds)) {
            spdlog::error("[PluginInstaller] {} timed out after {} seconds", operation_name,
                          timeout_seconds);
            // Kill the child process
            kill(pid, SIGTERM);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            kill(pid, SIGKILL); // Force kill if still running

            // Reap the zombie (blocking, but child should be dead)
            waitpid(pid, &status, 0);

            result.timed_out = true;
            return result;
        }

        // Sleep before next poll
        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
    }
}

/**
 * @brief Run install.sh in the given mode, capturing and logging its output.
 *
 * fork/exec instead of popen(): sh receives the script as a path argument,
 * not a command string, so nothing inside script_path is interpreted.
 * Running it through /bin/sh also works when the file has no exec bit:
 * Moonraker's type:web self-update extracts the release zip with Python's
 * zipfile, which does not restore unix modes. --no-restart: NoNewPrivileges
 * keeps the script from sudo; the caller restarts Moonraker over its API.
 */
static WaitResult run_install_script(const std::string& script_path, const char* mode_flag,
                                     int timeout_seconds, const char* operation_name) {
    WaitResult result;
    int fds[2];
    if (pipe(fds) != 0) {
        result.error = result.spawn_failed = true;
        result.error_message = strerror(errno);
        spdlog::error("[PluginInstaller] pipe failed: {}", result.error_message);
        return result;
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);

    pid_t pid = fork();
    if (pid < 0) {
        result.error = result.spawn_failed = true;
        result.error_message = strerror(errno);
        spdlog::error("[PluginInstaller] Fork failed: {}", result.error_message);
        close(fds[0]);
        close(fds[1]);
        return result;
    }

    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        execl("/bin/sh", "sh", script_path.c_str(), "--no-restart", mode_flag, nullptr);
        _exit(127);
    }

    close(fds[1]);
    result = wait_for_child_with_timeout(pid, timeout_seconds, operation_name, fds[0]);
    close(fds[0]);
    log_script_output(result.output, !result.timed_out && !result.error && result.exit_code == 0);
    return result;
}

std::vector<std::string> script_output_lines(const std::string& output) {
    std::vector<std::string> lines;
    std::string line;
    auto flush = [&] {
        if (line.find_first_not_of(" \t\r") != std::string::npos) {
            lines.push_back(line);
        }
        line.clear();
    };
    for (size_t i = 0; i < output.size(); ++i) {
        const char c = output[i];
        if (c == '\033') {
            // A CSI sequence: ESC [ parameters, then a final byte in @..~
            if (i + 1 < output.size() && output[i + 1] == '[') {
                i += 2;
                while (i < output.size() && (output[i] < '@' || output[i] > '~')) {
                    ++i;
                }
            }
            continue;
        }
        if (c == '\n') {
            flush();
        } else {
            line.push_back(c);
        }
    }
    flush();
    return lines;
}

// ============================================================================
// HelixPluginInstaller Implementation
// ============================================================================

void HelixPluginInstaller::set_api(IMoonrakerAPI* api) {
    api_ = api;

    // Try to get WebSocket URL from API's client
    if (api_ && api_->get_connection_state() == ConnectionState::CONNECTED) {
        websocket_url_ = api_->get_websocket_url();
    }
}

void HelixPluginInstaller::set_websocket_url(const std::string& url) {
    websocket_url_ = url;
}

bool HelixPluginInstaller::is_local_moonraker() const {
    if (websocket_url_.empty()) {
        return false;
    }

    // Canonical predicate (own hostname + interface IPs, not just loopback
    // literals) — installing into a printer whose address resolves to THIS
    // machine is exactly the local case.
    //
    // A URL that fails to parse yields an empty host, and is_loopback_literal()
    // reads empty as loopback — which would call a remote printer local. A parse
    // failure is not evidence of locality, so it is rejected here rather than in
    // the shared predicate, where an unset host does legitimately mean local.
    const std::string host = extract_host_from_websocket_url(websocket_url_);
    if (host.empty()) {
        return false;
    }
    return is_moonraker_on_same_host(host);
}

HelixPluginInstaller::SyncInstallResult HelixPluginInstaller::install_local_sync() {
    // NOTE: This method is designed to be called from a background thread.
    // It does NOT use std::function to avoid ARM/glibc static linking issues.

    if (!is_local_moonraker()) {
        spdlog::warn("[PluginInstaller] Cannot auto-install on remote Moonraker");
        return {false, lv_tr("Auto-install only works on local Moonraker")};
    }

    std::string script_path = get_install_script_path();
    if (script_path.empty()) {
        return {false, lv_tr("Install script not found. Use the remote install command instead.")};
    }

    state_.store(PluginInstallState::INSTALLING);
    spdlog::info("[PluginInstaller] Starting local installation: {} --no-restart --auto",
                 script_path);

    constexpr int INSTALL_TIMEOUT_SECONDS = 60;
    WaitResult result =
        run_install_script(script_path, "--auto", INSTALL_TIMEOUT_SECONDS, "Installation");

    if (result.spawn_failed) {
        state_.store(PluginInstallState::FAILED);
        return {false, lv_tr("Failed to start installer: ") + result.error_message};
    }

    if (result.timed_out) {
        state_.store(PluginInstallState::FAILED);
        return {false, lv_tr("Installation timed out. The script may be stuck.")};
    }

    if (result.error) {
        state_.store(PluginInstallState::FAILED);
        return {false, lv_tr("Installation failed: ") + result.error_message};
    }

    if (result.exit_code == 0) {
        state_.store(PluginInstallState::SUCCESS);
        spdlog::info("[PluginInstaller] Installation completed successfully");
        return {true, lv_tr("Plugin installed successfully. Moonraker is restarting...")};
    }

    state_.store(PluginInstallState::FAILED);
    spdlog::error("[PluginInstaller] Installation failed (exit code {})", result.exit_code);
    return {false, lv_tr("Installation failed. Check logs for details.")};
}

namespace {
// install.sh's --uninstall / --uninstall-auto exit code for "the plugin was
// removed, but a config file was skipped or failed during the phase-tracking
// strip" (moonraker-plugin/install.sh, moonraker-plugin/strip_phase_tracking.py).
constexpr int UNINSTALL_EXIT_NEEDS_ATTENTION = 2;
} // namespace

void HelixPluginInstaller::uninstall_local(UninstallCallback callback) {
    if (!is_local_moonraker()) {
        spdlog::warn("[PluginInstaller] Cannot auto-uninstall on remote Moonraker");
        if (callback) {
            callback(UninstallOutcome::FAILED,
                     lv_tr("Auto-uninstall only works on local Moonraker"));
        }
        return;
    }

    std::string script_path = get_install_script_path();
    if (script_path.empty()) {
        if (callback) {
            callback(UninstallOutcome::FAILED, lv_tr("Uninstall script not found."));
        }
        return;
    }

    state_.store(PluginInstallState::INSTALLING);
    spdlog::info(
        "[PluginInstaller] Starting local uninstallation: {} --no-restart --uninstall-auto",
        script_path);

    constexpr int UNINSTALL_TIMEOUT_SECONDS = 60;
    WaitResult result = run_install_script(script_path, "--uninstall-auto",
                                           UNINSTALL_TIMEOUT_SECONDS, "Uninstallation");

    if (result.spawn_failed) {
        state_.store(PluginInstallState::FAILED);
        if (callback) {
            callback(UninstallOutcome::FAILED,
                     lv_tr("Failed to start uninstaller: ") + result.error_message);
        }
        return;
    }

    if (result.timed_out) {
        state_.store(PluginInstallState::FAILED);
        if (callback) {
            callback(UninstallOutcome::FAILED,
                     lv_tr("Uninstallation timed out. The script may be stuck."));
        }
        return;
    }

    if (result.error) {
        state_.store(PluginInstallState::FAILED);
        if (callback) {
            callback(UninstallOutcome::FAILED,
                     lv_tr("Uninstallation failed: ") + result.error_message);
        }
        return;
    }

    if (result.exit_code == 0) {
        state_.store(PluginInstallState::SUCCESS);
        spdlog::info("[PluginInstaller] Uninstallation completed successfully");
        if (callback) {
            callback(UninstallOutcome::SUCCESS, lv_tr("Plugin uninstalled successfully."));
        }
    } else if (result.exit_code == UNINSTALL_EXIT_NEEDS_ATTENTION) {
        state_.store(PluginInstallState::NEEDS_ATTENTION);
        spdlog::warn("[PluginInstaller] Uninstallation removed the plugin but left a config "
                     "file needing attention (exit code {})",
                     result.exit_code);
        if (callback) {
            callback(UninstallOutcome::NEEDS_ATTENTION,
                     lv_tr("Plugin removed. A config file needs attention - check the "
                           "HelixScreen log for details."));
        }
    } else {
        state_.store(PluginInstallState::FAILED);
        spdlog::error("[PluginInstaller] Uninstallation failed (exit code {})", result.exit_code);
        if (callback) {
            callback(UninstallOutcome::FAILED,
                     lv_tr("Uninstallation failed. Check logs for details."));
        }
    }
}

std::string HelixPluginInstaller::get_remote_install_command() const {
    return std::string("curl -sSL ") + REMOTE_INSTALL_URL + " | bash";
}

std::string HelixPluginInstaller::get_install_script_path() const {
    // Try to find install.sh relative to the executable

    // Get executable path
    std::string exe_dir;

#ifdef __APPLE__
    char path[1024];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        exe_dir = std::string(hfs::parent_path(path));
    }
#else
    // Linux: read /proc/self/exe
    char path[1024];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len != -1) {
        path[len] = '\0';
        exe_dir = std::string(hfs::parent_path(path));
    }
#endif

    if (exe_dir.empty()) {
        return "";
    }

    // Try common locations relative to executable
    std::vector<std::string> search_paths = {
        exe_dir + "/moonraker-plugin/install.sh",    // Deployed alongside binary
        exe_dir + "/../moonraker-plugin/install.sh", // Development build
        exe_dir + "/../../moonraker-plugin/install.sh",
        exe_dir + "/../share/helix/moonraker-plugin/install.sh", // Installed
        "/usr/share/helix/moonraker-plugin/install.sh"};

    for (const auto& candidate : search_paths) {
        if (!hfs::exists(candidate)) {
            continue;
        }
        // canonical() resolves symlinks and normalizes the path; nullopt when
        // the path is inaccessible
        auto canonical_path = hfs::canonical(candidate);
        if (!canonical_path) {
            spdlog::warn("[PluginInstaller] Failed to resolve {}: {}", candidate,
                         std::strerror(errno));
            continue;
        }

        // Security check: ensure the resolved path still points to install.sh
        // This prevents symlink attacks where a malicious symlink points elsewhere
        if (hfs::filename(*canonical_path) != "install.sh") {
            spdlog::warn("[PluginInstaller] Skipping {} - resolved to unexpected file: {}",
                         candidate, *canonical_path);
            continue;
        }

        // No exec-bit check: the script runs via /bin/sh, and a release
        // unpacked by Moonraker's zip extractor carries no unix modes.
        return *canonical_path;
    }

    spdlog::warn("[PluginInstaller] install.sh not found below exe dir {}", exe_dir);
    return "";
}

bool HelixPluginInstaller::should_prompt_install() const {
    Config* config = Config::get_instance();

    // Gate plugin install prompt behind beta_features flag
    if (!config->is_beta_features_enabled()) {
        spdlog::debug("[PluginInstaller] Beta features disabled, skipping plugin prompt");
        return false;
    }

    // Don't show plugin prompt until the first-run wizard is complete.
    // The prompt will be triggered when wizard completes and connects to Moonraker,
    // which fires PrintSelectPanel's connection observer.
    if (config->is_wizard_required()) {
        spdlog::debug("[PluginInstaller] Wizard not complete, deferring plugin prompt");
        return false;
    }

    // If user previously declined, don't prompt
    bool declined = config->get<bool>(PREF_INSTALL_DECLINED, false);
    return !declined;
}

void HelixPluginInstaller::set_install_declined() {
    Config* config = Config::get_instance();
    config->set<bool>(PREF_INSTALL_DECLINED, true);
    config->save();
    spdlog::debug("[PluginInstaller] User declined plugin install prompt");
}

PluginInstallState HelixPluginInstaller::get_state() const {
    return state_.load();
}

bool HelixPluginInstaller::is_installing() const {
    return state_.load() == PluginInstallState::INSTALLING;
}

} // namespace helix
