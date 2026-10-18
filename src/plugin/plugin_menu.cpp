// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin_menu.h"

#if HELIX_HAS_PLUGINS
#include "plugin_host.h"
#endif

namespace helix::plugin_menu {

int row_cap(MenuKind kind) {
    switch (kind) {
    case MenuKind::PrintFile:
        return 8;
    case MenuKind::FilamentSlot:
        return 4;
    case MenuKind::Spool:
        return 6;
    case MenuKind::Tile:
        return 4;
    }
    return 0;
}

#if HELIX_HAS_PLUGINS

using helix::plugin::PluginHost;

bool any_declared(MenuKind kind, std::string_view plugin_id) {
    PluginHost* host = PluginHost::live();
    return host && host->menu_declared(kind, plugin_id);
}

std::vector<Entry> resolve(MenuKind kind, const json& context, std::string_view only_plugin) {
    PluginHost* host = PluginHost::live();
    return host ? host->resolve_menu(kind, context, only_plugin) : std::vector<Entry>{};
}

void activate(const Entry& e, const json& context) {
    if (PluginHost* host = PluginHost::live())
        host->activate_menu(e, context);
}

#else

bool any_declared(MenuKind, std::string_view) {
    return false;
}

std::vector<Entry> resolve(MenuKind, const json&, std::string_view) {
    return {};
}

void activate(const Entry&, const json&) {}

#endif // HELIX_HAS_PLUGINS

} // namespace helix::plugin_menu
