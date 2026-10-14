// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_context_menu.h"
#include "ui_observer_guard.h"

#include "ams_error.h"
#include "async_lifetime_guard.h"
#include "display_numbering.h"
#include "panel_widget.h"
#include "src/ui/panel_widgets/tool_switcher_layout.h"
#include "subject_managed_panel.h"

#include <memory>
#include <string>
#include <vector>

namespace helix {

class PrinterState;
class ToolSwitcherTestAccess;
struct ToolInfo;

/// Draws its own measured layout (decide_tool_switcher_layout()), so it takes no
/// part in the TileSizing ladder.
class ToolSwitcherWidget : public PanelWidget {
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

    /// Single-select list of the printer's tools and Park, raised by a tap on
    /// the face or the mounted segment. Picking a row issues the change; a tap
    /// outside it chooses nothing.
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
    /// Each head's material as publish_heads() last read it from its slot, so
    /// a temperature change rewrites a detail line without reading the slots.
    std::vector<std::string> head_materials_;

    ObserverGuard active_tool_observer_;
    ObserverGuard tool_count_observer_;
    ObserverGuard print_state_observer_;
    ObserverGuard spool_latch_observer_;
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
    /// A rail segment's tap: mount that head, or raise the sheet when it is
    /// the mounted one.
    void handle_segment_tapped(int tool_index);

    /// Null every cached widget pointer without touching observers or the
    /// widget tree. Shared by detach() and the raw-delete hook.
    void forget_tile_widgets();

    void on_hooked_root_deleted() override;

    /// Measure the cached box, decide, and publish everything the XML binds.
    void publish();
    /// The mode and geometry for the cached box, and the label form the
    /// segments or face have room for.
    void publish_layout();
    /// Every head's segment text and colour, the detail rows, the face and
    /// whether the cells hold a second line, from ToolState, the filament
    /// system and the temperatures.
    void publish_heads();
    /// Head @p i's new temperature: its segment's temperature (and second
    /// line in the Code form), its detail line, the face's second line when
    /// it is mounted, and whether the second lines still fit. Labels, colours
    /// and materials keep what publish_heads() last published.
    void publish_head_temp(int i);
    /// Whether every cell shown holds its second line.
    void publish_two_line();
    /// Observe each head's extruder temperature; the set follows the tools.
    void rebuild_head_temps();
    /// Head @p i's temperature, empty while unknown.
    std::string head_temp_text(int i) const;
    /// Whether head @p i reads hot.
    bool head_hot(int i) const;
    /// @p tool's detail line: its name, material and temperature, in the full
    /// form for the @p main line where the content box has room for it.
    std::string detail_head_text(const ToolInfo& tool, bool main) const;

    /// @p tool's label in @p form, in the connected printer's tool noun.
    static std::string tool_text(const ToolInfo& tool, ui::ToolLabelForm form);
    /// What a wide segment calls @p tool: its configured name, else "Tool 1".
    static std::string name_text(const ToolInfo& tool);
    /// What a narrow segment calls @p tool: the name it answers to in G-code
    /// and in the slicer, configured name or not. The sheet's G-code form, so
    /// the rail and the sheet never name one head two ways.
    static std::string short_text(const ToolInfo& tool);
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

    /// Publish tool_switcher_change_blocked: tool_change_refusal(), or spools
    /// on the bed. Called from publish() and the print-state and latch
    /// observers, so a recycled instance re-applies it on attach as well as on
    /// state change.
    void refresh_print_gating();

    /// Issue the change and report any refusal. Static so the confirmation
    /// modal's stateless event callback shares the one on_error path.
    static void dispatch_tool_change(int tool_index);

    /// Whether spools lying on the bed refuse every move, telling the user so
    /// when they do. The tile is disabled then too; this catches the tap that
    /// lands first.
    [[nodiscard]] bool refuse_while_spools_on_bed() const;

    /// Whether the connected backend can dock the mounted tool, which is what
    /// puts a Park action beside the tools.
    static bool park_offered();
    /// Publish park_offered() for the picker's Park row.
    void refresh_park_support();
    /// Dock the mounted tool, refusing while a print holds the machine or
    /// nothing is mounted. Every Park tap lands here.
    void handle_park_requested();

  public:
    static void tool_segment_cb(lv_event_t* e);
    static void tool_face_cb(lv_event_t* e);
    static void tool_park_cb(lv_event_t* e);
};

void register_tool_switcher_widget();

} // namespace helix
