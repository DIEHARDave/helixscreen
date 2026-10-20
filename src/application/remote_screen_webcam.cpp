// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "remote_screen_webcam.h"

#include "app_globals.h"
#include "data_root_resolver.h"
#include "host_identity.h"
#include "i_moonraker_api.h"
#include "printer_state.h"
#include "remote_screen_service.h"
#include "remote_screen_settings_manager.h"
#include "webcam_selection.h"

#include <spdlog/spdlog.h>

#include <unistd.h>

namespace helix {

std::string remote_screen_webcam_name(bool moonraker_on_this_host, const std::string& hostname) {
    if (moonraker_on_this_host || hostname.empty()) {
        return kRemoteScreenWebcamName;
    }
    return std::string(kRemoteScreenWebcamName) + " (" + hostname + ")";
}

std::string remote_screen_effective_route(bool moonraker_remote, const std::string& route) {
    return moonraker_remote ? std::string() : route;
}

std::optional<nlohmann::json> remote_screen_webcam_item(const std::string& name, bool enabled,
                                                        bool control, const std::string& route,
                                                        const std::string& lan_ip, int port) {
    if (!enabled) {
        return std::nullopt;
    }
    std::string base;
    if (!route.empty()) {
        base = route.back() == '/' ? route : route + "/";
    } else if (!lan_ip.empty()) {
        base = "http://" + lan_ip + ":" + std::to_string(port) + "/";
    } else {
        return std::nullopt;
    }
    const std::string snapshot = base + "snapshot.jpg";
    nlohmann::json item = {
        {"name", name},
        {"location", "printer"},
        {"enabled", true},
        {"snapshot_url", snapshot},
    };
    if (control) {
        item["service"] = "iframe";
        item["stream_url"] = base;
    } else {
        item["service"] = "mjpegstreamer";
        item["stream_url"] = base + "stream.mjpg";
        item["target_fps"] = 2;
    }
    return item;
}

void sync_remote_screen_webcam(IMoonrakerAPI& api, MoonrakerLocality locality) {
    if (locality == MoonrakerLocality::Unknown) {
        return;
    }
    auto& service = RemoteScreenService::instance();
    const auto& settings = RemoteScreenSettingsManager::instance();
    const bool remote = locality == MoonrakerLocality::Remote;
    char host[256] = {};
    if (gethostname(host, sizeof(host) - 1) != 0) {
        host[0] = '\0';
    }
    const std::string name = remote_screen_webcam_name(!remote, host);
    const std::string route = remote_screen_effective_route(
        remote, remote_screen_route(writable_path(".remote_screen_route")));
    const auto item =
        remote_screen_webcam_item(name, service.running(), settings.get_control(), route,
                                  select_lan_ipv4(local_interface_ips()), service.port());
    if (item) {
        api.timelapse().post_webcam(
            *item, [] { spdlog::info("[RemoteScreen] Registered the screen as a webcam"); },
            [](const MoonrakerError& err) {
                spdlog::warn("[RemoteScreen] Could not register the screen webcam: {}",
                             err.message);
            });
        return;
    }
    // Deleting a missing entry is an error, so look before deleting.
    IMoonrakerAPI* api_ptr = &api;
    api.timelapse().get_webcam_list(
        [api_ptr, name](const std::vector<WebcamInfo>& cams) {
            for (const auto& cam : cams) {
                if (cam.name == name) {
                    api_ptr->timelapse().delete_webcam(
                        name, [] { spdlog::info("[RemoteScreen] Removed the screen webcam"); },
                        [](const MoonrakerError& err) {
                            spdlog::warn("[RemoteScreen] Could not remove the screen webcam: {}",
                                         err.message);
                        });
                    return;
                }
            }
        },
        [](const MoonrakerError& err) {
            spdlog::debug("[RemoteScreen] Webcam list unavailable: {}", err.message);
        });
}

} // namespace helix
