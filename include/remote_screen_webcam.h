// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <string>

#include "hv/json.hpp"

namespace helix {

class IMoonrakerAPI;

/**
 * @brief The Moonraker webcam entry for the built-in screen server.
 *
 * One entry: an iframe of the control page when control is on (tap-to-control
 * inside Mainsail/Fluidd's webcam panel), a polled snapshot feed when view only.
 * Behind nginx the URLs are relative to @p route; facing the LAN they are
 * absolute on @p lan_ip:@p port.
 *
 * @return the item for server.webcams.post_item, or nullopt when no entry should
 *         exist: sharing off, or LAN mode with no address a browser could reach.
 */
std::optional<nlohmann::json> remote_screen_webcam_item(bool enabled, bool control,
                                                        const std::string& route,
                                                        const std::string& lan_ip, int port);

/// Make Moonraker's webcam list match the screen server: post the entry while it is
/// serving (refreshing the address on every connect), delete a listed one otherwise.
void sync_remote_screen_webcam(IMoonrakerAPI& api);

} // namespace helix
