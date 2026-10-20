// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "printer_network_state.h"

#include <optional>
#include <string>

#include "hv/json.hpp"

namespace helix {

class IMoonrakerAPI;

/**
 * @brief The Moonraker webcam entry for the built-in screen server.
 *
 * One entry: an iframe of the control page when control is on (tap-to-control
 * inside Mainsail/Fluidd's webcam panel), the MJPEG stream when view only.
 * Behind nginx the URLs are relative to @p route; facing the LAN they are
 * absolute on @p lan_ip:@p port.
 *
 * @return the item for server.webcams.post_item, or nullopt when no entry should
 *         exist: sharing off, or LAN mode with no address a browser could reach.
 */
std::optional<nlohmann::json> remote_screen_webcam_item(const std::string& name, bool enabled,
                                                        bool control, const std::string& route,
                                                        const std::string& lan_ip, int port);

/// The entry's name: kRemoteScreenWebcamName on the Moonraker of this host, with
/// @p hostname added on another host's Moonraker so two screens never share one entry.
std::string remote_screen_webcam_name(bool moonraker_on_this_host, const std::string& hostname);

/// The nginx route to use: @p route only when Moonraker runs on this host. The route is
/// this host's nginx guarding this host's Moonraker; a screen pointed at another
/// printer faces the LAN with pairing.
std::string remote_screen_effective_route(bool moonraker_remote, const std::string& route);

/// Make Moonraker's webcam list match the screen server: post the entry while it is
/// serving (refreshing the address on every connect), delete a listed one otherwise.
/// Does nothing until @p locality says where that Moonraker runs, since the entry's
/// name and URLs depend on it.
void sync_remote_screen_webcam(IMoonrakerAPI& api, MoonrakerLocality locality);

} // namespace helix
