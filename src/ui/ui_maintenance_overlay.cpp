// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_maintenance_overlay.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"

#include "src/ui/panel_widgets/maintenance_model.h"

namespace helix {

namespace {

template <int Meter> void on_interval_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MaintenanceOverlay] on_interval_changed");
    maintenance_model::set_interval_choice(Meter, ui::event_selected(e));
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace

void MaintenanceOverlay::register_callbacks() {
    register_xml_callbacks({
        {"maintenance_reset_cb",
         [](lv_event_t* e) {
             LVGL_SAFE_EVENT_CB_BEGIN("[MaintenanceOverlay] maintenance_reset_cb");
             if (auto meter = ui::event_user_int(e))
                 maintenance_model::confirm_reset(*meter);
             LVGL_SAFE_EVENT_CB_END();
         }},
        {"maintenance_interval_0_cb", on_interval_changed<0>},
        {"maintenance_interval_1_cb", on_interval_changed<1>},
        {"maintenance_interval_2_cb", on_interval_changed<2>},
    });
}

} // namespace helix
