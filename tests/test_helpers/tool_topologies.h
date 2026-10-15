// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "tool_state.h"

#include <string>

namespace helix::test {

/// @p heads heads, one tool each, every tool printing through its own
/// extruder ("extruder", "extruder1", ...) and sourcing lane i.
inline ToolTopology make_head_topology(int heads, int active_tool = 0) {
    ToolTopology topo;
    topo.tool_count = heads;
    topo.active_tool = active_tool;
    for (int i = 0; i < heads; ++i) {
        topo.tool_to_slot.push_back(i);
        topo.tool_extruder.push_back(i == 0 ? std::string("extruder")
                                            : "extruder" + std::to_string(i));
    }
    return topo;
}

/// A seven-head changer whose AFC units publish eighteen lanes, each its own
/// tool: four heads fed directly, then hubs of four, four and six lanes into
/// extruder4, extruder5 and extruder6. Lane i is slot i.
inline ToolTopology make_hub_topology(int active_tool = 0) {
    const char* extruders[] = {"extruder",  "extruder1", "extruder2", "extruder3", "extruder4",
                               "extruder4", "extruder4", "extruder4", "extruder5", "extruder5",
                               "extruder5", "extruder5", "extruder6", "extruder6", "extruder6",
                               "extruder6", "extruder6", "extruder6"};
    ToolTopology topo;
    topo.tool_count = 18;
    topo.active_tool = active_tool;
    for (int i = 0; i < 18; ++i) {
        topo.tool_to_slot.push_back(i);
        topo.tool_extruder.emplace_back(extruders[i]);
    }
    return topo;
}

} // namespace helix::test
