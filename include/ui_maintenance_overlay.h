// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "overlay_base.h"
#include "static_panel_registry.h"

namespace helix {

/// The maintenance meters' detail overlay, opened from the home tile: a card per meter
/// with its used hours and a Reset that asks first, the total print time, and the
/// service interval for each meter, where Off hides it. Everything binds
/// maintenance_model's subjects; the callbacks write straight through it.
class MaintenanceOverlay : public OverlayBase {
  public:
    const char* get_name() const override {
        return "Maintenance";
    }
    const char* xml_component() const override {
        return "maintenance_overlay";
    }

    void register_callbacks() override;
};

inline MaintenanceOverlay& get_maintenance_overlay() {
    return lazy_global<MaintenanceOverlay>("MaintenanceOverlay");
}

} // namespace helix
