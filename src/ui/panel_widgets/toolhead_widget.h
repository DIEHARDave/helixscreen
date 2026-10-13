// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_coalesced_timer.h"
#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "panel_widget.h"
#include "src/ui/panel_widgets/toolhead_layout.h"
#include "src/ui/panel_widgets/toolhead_plate.h"

#include <optional>

namespace helix {

class PrinterState;

/// Home widget for the toolhead's position and homing. Measures what each rung would
/// draw, decides the rung with decide_toolhead_layout(), and publishes the verdict and
/// the readings as subjects that panel_widget_toolhead.xml binds. Wide and up draw a
/// top-down plate with the crosshair and a Z bar on a plain object of that component.
/// A tap opens the Motion panel; the Full rung adds Home All while an axis is unhomed.
class ToolheadWidget : public PanelWidget {
  public:
    explicit ToolheadWidget(PrinterState& printer_state);
    ~ToolheadWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    bool fits_at(int width_px, int height_px) const override;
    const char* id() const override {
        return "toolhead";
    }

    static void clicked_cb(lv_event_t* e);

    /// Send G28. Does nothing unless Home All is on screen, which it is only while
    /// idle with an axis not homed.
    void home_all();

    /// The plate the canvas would draw now, or nullopt while it is not on screen.
    [[nodiscard]] std::optional<ToolheadPlateGeometry> plate_geometry() const;

  private:
    struct LiveState;
    LiveState read_live() const;
    ToolheadVerdict decide(int width_px, int height_px, bool printing, bool homed) const;
    void publish_state(const LiveState& live);
    void publish_values(const LiveState& live);
    void on_state_changed();
    void schedule_refresh();
    void refresh();
    void resolve_bed();
    bool uses_positions() const;
    bool plate_visible() const;
    ToolheadPlateInput plate_input(const LiveState& live, int box_w, int box_h) const;
    void open_motion_panel();

    PrinterState& printer_state_;
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    lv_obj_t* plate_ = nullptr;

    ToolheadVerdict verdict_;
    /// The state the verdict was decided for; a change re-decides, because the
    /// content being measured changed.
    bool decided_printing_ = false;
    bool decided_homed_ = false;
    bool moving_ = false;
    BedDimensions bed_{};
    bool circular_ = false;

    ObserverGuard homed_observer_;
    ObserverGuard print_active_observer_;
    ObserverGuard position_observers_[6];
    ObserverGuard velocity_observer_;
    ObserverGuard bed_moves_observer_;
    ObserverGuard build_volume_observer_;
    ObserverGuard tools_version_observer_;
    ObserverGuard active_tool_observer_;

    /// Every position and homing observer funnels here, so the readouts and the canvas
    /// refresh at about 4 Hz however fast the printer reports.
    ui::CoalescedTimer refresh_timer_{250};

    // Declared last so it is torn down first, expiring every token the
    // observers captured before any of them is destroyed.
    helix::AsyncLifetimeGuard lifetime_;
};

void register_toolhead_widget();

} // namespace helix
