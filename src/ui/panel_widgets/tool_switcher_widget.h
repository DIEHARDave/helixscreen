// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_context_menu.h"
#include "ui_observer_guard.h"

#include "ams_error.h"
#include "async_lifetime_guard.h"
#include "display_numbering.h"
#include "src/ui/panel_widgets/tiled_panel_widget.h"
#include "src/ui/panel_widgets/tool_switcher_layout.h"
#include "subject_managed_panel.h"

#include <memory>
#include <string>
#include <vector>

namespace helix {

class PrinterState;
class ToolSwitcherTestAccess;
struct ToolInfo;

class ToolSwitcherWidget : public TiledPanelWidget {
  public:
    /// Heads the per-head subject pools hold. Every pool is sized to this
    /// when the subjects are registered, before any XML binds a row; a printer
    /// with more heads reaches the rest through the picker.
    static constexpr int kPoolSize = 16;

    explicit ToolSwitcherWidget(PrinterState& printer_state);
    ~ToolSwitcherWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override {
        return "tool_switcher";
    }
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    /// Whether the content box (the tile less space_md each side) holds one
    /// touch target on both axes: the face every smaller box would fall to.
    bool fits_at(int width_px, int height_px) const override;
    bool has_overlay_open() const override {
        return picker_.is_visible();
    }

    // Static instance tracker for callbacks from static event handlers
    static ToolSwitcherWidget* s_active_instance;

  private:
    friend class ToolSwitcherTestAccess;

    /// Single-select list of the printer's tools, raised by a tap on the compact
    /// tile. Picking a row issues the change; a tap outside it chooses nothing.
    class ToolPicker : public helix::ui::ContextMenu {
        HELIX_CONTEXT_MENU_KIND(ToolPicker)

      public:
        explicit ToolPicker(ToolSwitcherWidget& owner) : owner_(owner) {}

      protected:
        const char* xml_component_name() const override {
            return "tool_switcher_picker";
        }
        void on_created(lv_obj_t* backdrop) override;

      private:
        ToolSwitcherWidget& owner_;
    };

    /// One head's extruder temperature, observed for the segment and detail text.
    struct HeadTemp {
        // The lifetime is declared before the observer so the observer is
        // destroyed first, while the subject it removes itself from still lives.
        SubjectLifetime lifetime;
        ObserverGuard observer;
        int deci = 0;
        bool known = false;
    };

    PrinterState& printer_state_;
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    ToolPicker picker_{*this};

    // Physical size the widget was last granted, cached for observer paths
    // (tool_count_observer_, on_active_tool_changed()) that fire later and
    // need to re-decide without a size to read.
    int current_width_px_ = 0;
    int current_height_px_ = 0;

    /// The verdict last published, and the label form the segments read in.
    ToolSwitcherLayout layout_{};
    ui::ToolLabelForm label_form_ = ui::ToolLabelForm::Full;

    /// Indexed by tool, sized to the tool count; rebuilt when it changes.
    std::vector<HeadTemp> head_temps_;

    ObserverGuard active_tool_observer_;
    ObserverGuard tool_count_observer_;
    ObserverGuard print_state_observer_;
    ObserverGuard slots_observer_;
    ObserverGuard extruders_observer_;

    // MUST stay declared LAST: reverse-declaration destruction makes this the
    // first member torn down, invalidating every captured token before any
    // observer destructs. Without this, queued observer callbacks captured
    // via tok.defer() see token.expired() == false after the observers are
    // already gone and dereference a half-destroyed widget. See temp_stack_widget.h
    // (commit 45abc8c2a, bundle AX3CKAKB).
    helix::AsyncLifetimeGuard lifetime_;

    void show_tool_picker();
    void handle_tool_selected(int tool_index);

    /// Null every cached widget pointer without touching observers or the
    /// widget tree. Shared by detach() and the raw-delete hook.
    void forget_tile_widgets();

    void on_hooked_root_deleted() override;

    /// Measure the cached box, decide, and publish everything the XML binds.
    void publish();
    /// The mode and geometry for the cached box, and the label form the
    /// segments or face have room for.
    void publish_layout();
    /// Every head's segment text and colour, the detail rows and the compact
    /// readout, from ToolState, the filament system and the temperatures.
    void publish_heads();
    /// Observe each head's extruder temperature; the set follows the tools.
    void rebuild_head_temps();

    /// @p tool's label in @p form, in the connected printer's tool noun.
    static std::string tool_text(const ToolInfo& tool, ui::ToolLabelForm form);
    /// What a narrow segment calls @p tool: "T<index>", the name it answers to
    /// in G-code and in the slicer, configured name or not. Wider surfaces keep
    /// tool_text()'s physical label for a configured name.
    static std::string short_text(const ToolInfo& tool);
    /// Budget the compact value at the widest label any tool carries.
    void refresh_label_budget();
    void on_active_tool_changed(int tool_index);

    /**
     * @brief Whether a running print blocks a tool change, and why.
     *
     * The same question every other filament surface asks — the filament panel,
     * the AMS sidebar and the AMS context menu all route through
     * helix::ui::print_blocks_filament_op(), the mirror of
     * AmsSubscriptionBackend::refuse_if_printing(). PRINTING always refuses;
     * PAUSED refuses only on a backend whose filament macro homes itself
     * (AD5X IFS). A tool change on a shared-toolhead AMS *is* a filament op, so
     * it gets the same gate rather than a policy of its own.
     *
     * @return AmsErrorHelper::print_active() when blocked, success() otherwise.
     */
    [[nodiscard]] AmsError tool_change_refusal() const;

    /// Publish tool_change_refusal() as tool_switcher_change_blocked. Called
    /// from publish() and the print-state observer, so a recycled instance
    /// re-applies it on attach as well as on state change.
    void refresh_print_gating();

    /// Issue the change and report any refusal. Static so the confirmation
    /// modal's stateless event callback shares the one on_error path.
    static void dispatch_tool_change(int tool_index);

    /// Whether the connected backend can dock the mounted tool, which is what
    /// puts a Park action beside the tools.
    static bool park_offered();
    /// Publish park_offered() for the picker's Park row.
    void refresh_park_support();
    /// Dock the mounted tool, refusing while a print holds the machine or
    /// nothing is mounted. Every Park tap lands here.
    void handle_park_requested();

  public:
    static void tool_compact_cb(lv_event_t* e);
    static void tool_park_cb(lv_event_t* e);
};

void register_tool_switcher_widget();

} // namespace helix
