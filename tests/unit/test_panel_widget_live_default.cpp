// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_panel_widget_live_default.cpp
 * @brief An unedited default home layout follows the printer's capabilities.
 *
 * Every case drives PanelWidgetManager::populate_widgets() on a measured grid
 * container, with the capability subjects the shipped default_layout.json
 * consults pinned, and changes a capability the way discovery does: by setting
 * its subject and letting the gate observers' queued rebuild run. The spool
 * slot of the medium table is the probe throughout: with `ams_slot_count` at 0
 * it seats led and filament, with an AMS it seats the `ams` tile 2x1 and
 * passes over both.
 */

#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/scope_exit.h"
#include "../test_helpers/scoped_config_write_counter.h"
#include "../test_helpers/scoped_int_subject.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "../test_helpers/update_queue_test_access.h"
#include "ams_state.h"
#include "config.h"
#include "connection_state.h"
#include "default_layout_resolver.h"
#include "grid_layout.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "src/ui/panel_widgets/print_status_widget.h"
#include "theme_manager.h"
#include "tool_state.h"

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::test::ScopedIntSubject;

namespace {

constexpr int kConnected = static_cast<int>(ConnectionState::CONNECTED);
constexpr int kDisconnected = static_cast<int>(ConnectionState::DISCONNECTED);
constexpr int kReady = static_cast<int>(KlippyState::READY);
constexpr int kStartup = static_cast<int>(KlippyState::STARTUP);

/// A panel resolution and the measured home content box at it.
struct Geometry {
    const char* name;
    int panel_w, panel_h;
    int content_w, content_h;
    UiBreakpoint bp;
    int cols, rows;
};

// The 800x480 home content box is a 12x8 grid; 600 wide at the same tier is a
// 10x8 one, the grid a UI scale or a narrower panel gives the medium table.
constexpr Geometry kMedium12x8{"medium 12x8", 800, 480, 710, 466, UiBreakpoint::Medium, 12, 8};
constexpr Geometry kMedium10x8{"medium 10x8", 800, 480, 600, 466, UiBreakpoint::Medium, 10, 8};

/// Forces mock backends: NetworkWidget's attach() otherwise builds the real
/// platform WiFi backend against this machine's state.
struct TestModeGuard {
    RuntimeConfig* rc;
    bool prev;
    explicit TestModeGuard(RuntimeConfig* r) : rc(r), prev(r->test_mode) {
        rc->test_mode = true;
    }
    ~TestModeGuard() {
        rc->test_mode = prev;
    }
};

class LiveDefaultFixture : public LVGLUITestFixture {
  public:
    LiveDefaultFixture() : test_mode_(get_runtime_config()) {
        PanelWidgetManager::instance().init_widget_subjects();
        ToolState::instance().deinit_subjects();
        ToolState::instance().init_subjects(false);
        ToolTopology topo;
        topo.tool_count = 1;
        topo.active_tool = 0;
        ToolState::instance().set_ams_topology(topo);
        state().temperature_state().init_extruders({"extruder"});
        if (lv_xml_get_subject(nullptr, "ams_slot_count") == nullptr) {
            AmsState::instance().init_subjects(true);
        }
        helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    }

    ~LiveDefaultFixture() override {
        auto& mgr = PanelWidgetManager::instance();
        for (const auto& id : panels_) {
            PanelWidgetManager::clear_gate_observers(id);
            mgr.clear_panel_config(id);
        }
        PrintStatusWidget::destroy_formatter_for_test();
        ToolState::instance().deinit_subjects();
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
        res_.reset();
        if (lv_display_t* d = lv_display_get_default()) {
            theme_manager_refresh_layout_constants(d);
        }
    }

    /// Pin every subject the shipped table consults: no AMS, an LED, a
    /// filament sensor, nothing else, and a READY printer.
    void pin_capabilities(int ams_slots) {
        pin("ams_slot_count", ams_slots);
        pin("home_dryer_available", 0);
        pin("printer_has_chamber", 0);
        pin("led_controllable", 1);
        pin("filament_sensor_count", 1);
        pin("printer_has_spoolman", 0);
        pin("printer_connection_state", kConnected);
        pin("klippy_state", kReady);
    }

    void pin(const char* name, int value) {
        pins_.push_back(std::make_unique<ScopedIntSubject>(name, value));
    }

    /// Set a subject the way discovery does. The gate observers queue their
    /// rebuild; run_rebuild() runs it.
    static void set(const char* name, int value) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        lv_subject_set_int(s, value);
    }

    void use_geometry(const Geometry& g) {
        lv_display_t* disp = lv_display_get_default();
        REQUIRE(disp != nullptr);
        res_.reset();
        res_ = std::make_unique<ScopedResolution>(disp, g.panel_w, g.panel_h);
        theme_manager_refresh_layout_constants(disp);
        lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
        REQUIRE(bp_subj != nullptr);
        REQUIRE(lv_subject_get_int(bp_subj) == static_cast<int>(g.bp));
        const GridDimensions dims = GridLayout::get_dimensions(g.bp, g.content_w, g.content_h);
        REQUIRE(dims.cols == g.cols);
        REQUIRE(dims.rows == g.rows);
        container_ = make_container(g.content_w, g.content_h);
    }

    lv_obj_t* make_container(int content_w, int content_h) {
        lv_obj_t* c = lv_obj_create(test_screen());
        lv_obj_set_size(c, content_w, content_h);
        lv_obj_set_style_pad_all(c, 0, 0);
        lv_obj_set_style_border_width(c, 0, 0);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        return c;
    }

    /// A panel id with no saved layout, so it loads as a fresh default.
    std::string fresh_panel(const std::string& id) {
        Config* cfg = Config::get_instance();
        cfg->set<nlohmann::json>(cfg->df() + "panel_widgets/" + id, nlohmann::json());
        auto& mgr = PanelWidgetManager::instance();
        mgr.get_widget_config(id).mark_dirty();
        mgr.clear_panel_config(id);
        if (std::find(panels_.begin(), panels_.end(), id) == panels_.end()) {
            panels_.push_back(id);
        }
        return id;
    }

    void populate(const std::string& panel) {
        PanelWidgetManager::instance().populate_widgets(panel, container_, 0);
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
        lv_obj_update_layout(container_);
    }

    /// Observe the panel's gates; the rebuild repopulates this container.
    void observe_gates(const std::string& panel) {
        PanelWidgetManager::instance().setup_gate_observers(panel, [this, panel]() {
            ++rebuilds;
            populate(panel);
        });
        run_rebuild(); // the registration itself fires every observer once
        rebuilds = 0;
    }

    void run_rebuild() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
        process_lvgl(20);
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    /// Let a save held by save_soon() come due.
    void settle_saves() {
        process_lvgl(static_cast<int>(PanelWidgetConfig::SAVE_SETTLE_MS) + 100);
    }

    PanelWidgetConfig& config(const std::string& panel) {
        return PanelWidgetManager::instance().get_widget_config(panel);
    }

    const PanelWidgetEntry* entry(const std::string& panel, const char* id) {
        for (const auto& e : config(panel).page_entries(0)) {
            if (e.id == id) {
                return &e;
            }
        }
        return nullptr;
    }

    bool placed(const std::string& panel, const char* id) {
        const auto* e = entry(panel, id);
        return e && e->is_placed();
    }

    /// The tile LVGL lays out for @p id, at the cell and span it renders at.
    void check_tile(const char* id, int col, int row, int colspan, int rowspan) {
        lv_obj_t* obj = lv_obj_find_by_name(container_, id);
        if (obj == nullptr) {
            FAIL_CHECK("widget '" << id << "' missing from the grid");
            return;
        }
        CAPTURE(id, col, row, colspan, rowspan);
        CHECK(lv_obj_get_style_grid_cell_column_pos(obj, LV_PART_MAIN) == col);
        CHECK(lv_obj_get_style_grid_cell_row_pos(obj, LV_PART_MAIN) == row);
        CHECK(lv_obj_get_style_grid_cell_column_span(obj, LV_PART_MAIN) == colspan);
        CHECK(lv_obj_get_style_grid_cell_row_span(obj, LV_PART_MAIN) == rowspan);
    }

    /// The spool slot holds the AMS tile, and its other candidates are off.
    void check_ams_slot(const std::string& panel) {
        const auto* ams = entry(panel, "ams");
        REQUIRE(ams != nullptr);
        CHECK(ams->is_placed());
        CHECK(ams->col == 8);
        CHECK(ams->row == 0);
        CHECK(ams->colspan == 4);
        CHECK(ams->rowspan == 2);
        check_tile("ams", 8, 0, 4, 2);
        CHECK_FALSE(entry(panel, "filament")->enabled);
        CHECK_FALSE(entry(panel, "led")->enabled);
        CHECK(lv_obj_find_by_name(container_, "filament") == nullptr);
    }

    /// The spool slot holds led and filament, and the AMS is off.
    void check_env_fil_slot(const std::string& panel) {
        CHECK_FALSE(entry(panel, "ams")->enabled);
        CHECK(lv_obj_find_by_name(container_, "ams") == nullptr);
        CHECK(placed(panel, "led"));
        CHECK(placed(panel, "filament"));
        check_tile("led", 8, 0, 2, 2);
        check_tile("filament", 10, 0, 2, 2);
    }

    /// Every enabled widget on the main page holds a cell.
    void check_none_enabled_unplaced(const std::string& panel) {
        for (const auto& e : config(panel).page_entries(0)) {
            if (e.enabled) {
                INFO("'" << e.id << "' is enabled with no cell");
                CHECK(e.has_grid_position());
            }
        }
    }

    /// Move two placed widgets onto each other's cells, as edit mode or
    /// `ctl home` does.
    void swap_temperatures(const std::string& panel) {
        const PanelWidgetEntry nozzle = *entry(panel, "temperature");
        const PanelWidgetEntry bed = *entry(panel, "bed_temperature");
        REQUIRE(nozzle.is_placed());
        REQUIRE(bed.is_placed());
        auto& cfg = config(panel);
        cfg.place_entry("temperature", 0, bed.col, bed.row, bed.colspan, bed.rowspan);
        cfg.place_entry("bed_temperature", 0, nozzle.col, nozzle.row, nozzle.colspan,
                        nozzle.rowspan);
        cfg.save();
    }

    nlohmann::json saved_node(const std::string& panel) {
        Config* cfg = Config::get_instance();
        return cfg->get<nlohmann::json>(cfg->df() + "panel_widgets/" + panel, nlohmann::json());
    }

    int rebuilds = 0;

  protected:
    TestModeGuard test_mode_;
    std::unique_ptr<ScopedResolution> res_;
    lv_obj_t* container_ = nullptr;
    std::vector<std::unique_ptr<ScopedIntSubject>> pins_;
    std::vector<std::string> panels_;
};

/// Drops the gate observers before the pinned subjects are put back.
struct GateObserverScope {
    std::string panel;
    ~GateObserverScope() {
        PanelWidgetManager::clear_gate_observers(panel);
    }
};

} // namespace

TEST_CASE_METHOD(LiveDefaultFixture, "live default: an AMS discovered after READY takes the slot",
                 "[live_default]") {
    use_geometry(kMedium12x8);
    require_font_tokens_distinct();
    pin_capabilities(0);
    ScopedConfigWriteCounter writes;
    const std::string panel = fresh_panel("live_default_k2_race");

    populate(panel);
    REQUIRE(config(panel).is_unedited_default());
    check_env_fil_slot(panel);

    GateObserverScope scope{panel};
    observe_gates(panel);
    set("ams_slot_count", 4);
    run_rebuild();
    REQUIRE(rebuilds >= 1);
    check_ams_slot(panel);
    CHECK(config(panel).is_unedited_default());
    check_none_enabled_unplaced(panel);

    // And back: the backend going away again is a capability change too.
    set("ams_slot_count", 0);
    run_rebuild();
    check_env_fil_slot(panel);
}

TEST_CASE_METHOD(LiveDefaultFixture, "live default: a moved widget makes the layout the user's",
                 "[live_default]") {
    use_geometry(kMedium12x8);
    pin_capabilities(0);
    ScopedConfigWriteCounter writes;
    const std::string panel = fresh_panel("live_default_moved");
    populate(panel);
    REQUIRE(config(panel).is_unedited_default());

    swap_temperatures(panel);
    REQUIRE_FALSE(config(panel).is_unedited_default());
    const auto before = config(panel).page_entries(0);

    GateObserverScope scope{panel};
    observe_gates(panel);
    set("ams_slot_count", 4);
    run_rebuild();
    REQUIRE(rebuilds >= 1);
    CHECK_FALSE(entry(panel, "ams")->is_placed());
    CHECK(placed(panel, "filament"));
    CHECK(placed(panel, "led"));
    CHECK(config(panel).page_entries(0) == before);
}

TEST_CASE_METHOD(LiveDefaultFixture,
                 "live default: per-widget config keeps it live and survives a re-resolve",
                 "[live_default]") {
    use_geometry(kMedium12x8);
    pin_capabilities(0);
    ScopedConfigWriteCounter writes;
    const std::string panel = fresh_panel("live_default_led_config");
    populate(panel);
    REQUIRE(placed(panel, "led"));

    const nlohmann::json strip{{"strip", "neopixel chamber"}};
    config(panel).set_widget_config("led", strip);
    CHECK(config(panel).is_unedited_default());

    GateObserverScope scope{panel};
    observe_gates(panel);
    set("ams_slot_count", 4);
    run_rebuild();
    check_ams_slot(panel);
    CHECK(config(panel).get_widget_config("led") == strip);

    set("ams_slot_count", 0);
    run_rebuild();
    check_env_fil_slot(panel);
    CHECK(config(panel).get_widget_config("led") == strip);
}

TEST_CASE_METHOD(LiveDefaultFixture,
                 "live default: a default block whose hash no longer matches stays put",
                 "[live_default]") {
    use_geometry(kMedium12x8);
    pin_capabilities(0);
    ScopedConfigWriteCounter writes;
    const std::string panel = fresh_panel("live_default_stale_hash");
    populate(panel);
    config(panel).flush_pending_save();

    // An older build moved a widget and kept the default block it could not
    // read, so the block's hash describes an arrangement that is gone.
    Config* cfg = Config::get_instance();
    const std::string path = cfg->df() + "panel_widgets/" + panel;
    nlohmann::json node = saved_node(panel);
    REQUIRE(node.contains("default"));
    for (auto& w : node["pages"][0]["widgets"]) {
        if (w["id"] == "notifications") {
            w["row"] = w["row"].get<int>() + 2;
        }
    }
    cfg->set<nlohmann::json>(path, node);
    config(panel).mark_dirty();
    REQUIRE_FALSE(config(panel).is_unedited_default());
    REQUIRE_FALSE(config(panel).default_signature().empty());

    GateObserverScope scope{panel};
    observe_gates(panel);
    set("ams_slot_count", 4);
    run_rebuild();
    CHECK_FALSE(entry(panel, "ams")->is_placed());
    CHECK(placed(panel, "filament"));
}

TEST_CASE_METHOD(LiveDefaultFixture,
                 "live default: nothing re-resolves until the printer is connected and READY",
                 "[live_default]") {
    use_geometry(kMedium12x8);
    pin_capabilities(0);
    ScopedConfigWriteCounter writes;
    const std::string panel = fresh_panel("live_default_not_ready");
    populate(panel);
    REQUIRE(config(panel).is_unedited_default());

    GateObserverScope scope{panel};
    observe_gates(panel);

    SECTION("disconnected") {
        set("printer_connection_state", kDisconnected);
        set("ams_slot_count", 4);
        run_rebuild();
        CHECK_FALSE(entry(panel, "ams")->is_placed());
        CHECK(placed(panel, "filament"));
        CHECK_FALSE(PanelWidgetManager::instance().refresh_live_default(panel));
    }
    SECTION("connected, Klipper starting up") {
        set("klippy_state", kStartup);
        set("ams_slot_count", 4);
        run_rebuild();
        CHECK_FALSE(entry(panel, "ams")->is_placed());
        CHECK(placed(panel, "filament"));

        // READY arriving with nothing else changing is what applies it.
        rebuilds = 0;
        set("klippy_state", kReady);
        run_rebuild();
        CHECK(rebuilds >= 1);
        check_ams_slot(panel);
    }
}

TEST_CASE_METHOD(LiveDefaultFixture,
                 "live default: each new signature saves once and an unchanged one writes nothing",
                 "[live_default]") {
    use_geometry(kMedium12x8);
    pin_capabilities(0);
    ScopedConfigWriteCounter writes;
    const std::string panel = fresh_panel("live_default_saves");
    populate(panel);
    GateObserverScope scope{panel};
    observe_gates(panel);
    settle_saves();

    // A gate the table never consults: its flips rebuild the panel but cannot
    // change the signature.
    const auto consulted =
        helix::layout::subjects_named_by(PanelWidgetConfig::load_default_table());
    std::string unconsulted;
    for (const auto& def : get_all_widget_defs()) {
        if (def.hardware_gate_subject &&
            std::find(consulted.begin(), consulted.end(), def.hardware_gate_subject) ==
                consulted.end() &&
            lv_xml_get_subject(nullptr, def.hardware_gate_subject) != nullptr) {
            unconsulted = def.hardware_gate_subject;
            break;
        }
    }
    REQUIRE_FALSE(unconsulted.empty());
    pin(unconsulted.c_str(), 0);
    run_rebuild();
    settle_saves();

    const std::string first_signature = config(panel).default_signature();
    std::set<std::string> signatures{first_signature};
    for (int flip = 1; flip <= 10; ++flip) {
        const int before = writes.writes();
        set("ams_slot_count", flip % 2 == 1 ? 4 : 0);
        run_rebuild();
        settle_saves();
        INFO("flip " << flip);
        CHECK(writes.writes() - before == 1);
        signatures.insert(config(panel).default_signature());
    }
    CHECK(signatures.size() == 2);
    CHECK(config(panel).default_signature() == first_signature);

    const int before = writes.writes();
    set(unconsulted.c_str(), 1);
    run_rebuild();
    set(unconsulted.c_str(), 0);
    run_rebuild();
    PanelWidgetManager::instance().populate_widgets(panel, container_, 0);
    settle_saves();
    CHECK(writes.writes() == before);
    CHECK(config(panel).default_signature() == first_signature);
}

TEST_CASE_METHOD(LiveDefaultFixture, "live default: Reset after an edit makes it live again",
                 "[live_default]") {
    use_geometry(kMedium12x8);
    pin_capabilities(0);
    ScopedConfigWriteCounter writes;
    const std::string panel = fresh_panel("live_default_reset");
    populate(panel);
    swap_temperatures(panel);
    REQUIRE_FALSE(config(panel).is_unedited_default());

    config(panel).reset_to_defaults();
    config(panel).save();
    populate(panel);
    REQUIRE(config(panel).is_unedited_default());
    REQUIRE_FALSE(config(panel).default_signature().empty());

    GateObserverScope scope{panel};
    observe_gates(panel);
    set("ams_slot_count", 4);
    run_rebuild();
    check_ams_slot(panel);
}

TEST_CASE_METHOD(LiveDefaultFixture,
                 "live default: a printer switch resolves the new printer once it is READY",
                 "[live_default]") {
    use_geometry(kMedium12x8);
    pin_capabilities(0);
    ScopedConfigWriteCounter writes;
    Config* cfg = Config::get_instance();
    std::string& active = ConfigTestAccess::active_printer_id(*cfg);
    const std::string saved_active = active;
    helix::test::ScopeExit restore_active([&] { active = saved_active; });

    const std::string panel = "live_default_switch";
    active = "live_default_printer_a";
    fresh_panel(panel);
    populate(panel);
    config(panel).flush_pending_save();
    const std::string path_a = cfg->df() + "panel_widgets/" + panel;
    const nlohmann::json node_a = cfg->get<nlohmann::json>(path_a, nlohmann::json());
    REQUIRE(node_a.contains("default"));

    GateObserverScope scope{panel};
    observe_gates(panel);

    // The retarget: the link drops, every cached layout reloads from the new
    // printer's path, and the new printer's capabilities arrive before READY.
    set("printer_connection_state", kDisconnected);
    run_rebuild();
    active = "live_default_printer_b";
    fresh_panel(panel);
    PanelWidgetManager::instance().clear_all_panel_configs();
    populate(panel);
    set("ams_slot_count", 4);
    run_rebuild();
    CHECK_FALSE(entry(panel, "ams")->is_placed());

    set("printer_connection_state", kConnected);
    run_rebuild();
    check_ams_slot(panel);
    config(panel).flush_pending_save();

    CHECK(cfg->get<nlohmann::json>(path_a, nlohmann::json()) == node_a);
    const nlohmann::json node_b =
        cfg->get<nlohmann::json>(cfg->df() + "panel_widgets/" + panel, nlohmann::json());
    REQUIRE(node_b.contains("default"));
    CHECK(node_b["default"]["signature"] != node_a["default"]["signature"]);
}

TEST_CASE_METHOD(
    LiveDefaultFixture,
    "live default: a new page or an added widget freezes it and retained entries survive",
    "[live_default]") {
    use_geometry(kMedium12x8);
    pin_capabilities(0);
    ScopedConfigWriteCounter writes;
    const std::string panel = fresh_panel("live_default_freeze");
    populate(panel);
    REQUIRE(config(panel).is_unedited_default());
    auto& mgr = PanelWidgetManager::instance();

    SECTION("a new page") {
        REQUIRE(config(panel).add_page() == 1);
        CHECK_FALSE(config(panel).is_unedited_default());
        set("ams_slot_count", 4);
        CHECK_FALSE(mgr.refresh_live_default(panel));
        CHECK_FALSE(entry(panel, "ams")->is_placed());
    }
    SECTION("a plugin widget placed from the catalog") {
        RuntimeWidgetDef def;
        def.id = "live_default-plug_tile";
        def.display_name = "Plug";
        def.icon = "puzzle_outline";
        helix::test::ScopeExit unregister(
            [] { unregister_runtime_widget_def("live_default-plug_tile"); });
        REQUIRE(register_runtime_widget_def(def));
        REQUIRE(config(panel).place_entry("live_default-plug_tile", 0, 0, 0, 2, 2) >= 0);
        CHECK_FALSE(config(panel).is_unedited_default());
        set("ams_slot_count", 4);
        CHECK_FALSE(mgr.refresh_live_default(panel));
        CHECK_FALSE(entry(panel, "ams")->is_placed());
    }
    SECTION("an unplaced retained entry rides through a re-resolve") {
        config(panel).flush_pending_save();
        Config* cfg = Config::get_instance();
        nlohmann::json node = saved_node(panel);
        const nlohmann::json retained{{"id", "absent-plugin_tile"},
                                      {"enabled", false},
                                      {"col", -1},
                                      {"row", -1},
                                      {"colspan", 2},
                                      {"rowspan", 2}};
        node["pages"][0]["widgets"].push_back(retained);
        cfg->set<nlohmann::json>(cfg->df() + "panel_widgets/" + panel, node);
        config(panel).mark_dirty();
        REQUIRE(config(panel).is_unedited_default());
        REQUIRE(config(panel).pages()[0].retained.size() == 1);

        set("ams_slot_count", 4);
        CHECK(mgr.refresh_live_default(panel));
        CHECK(entry(panel, "ams")->is_placed());
        REQUIRE(config(panel).pages()[0].retained.size() == 1);
        CHECK(config(panel).pages()[0].retained[0] == retained);
        config(panel).flush_pending_save();
        const nlohmann::json saved = saved_node(panel);
        const auto& widgets = saved["pages"][0]["widgets"];
        INFO(saved.dump());
        CHECK(std::any_of(widgets.begin(), widgets.end(),
                          [&](const nlohmann::json& w) { return w == retained; }));
    }
}

TEST_CASE_METHOD(LiveDefaultFixture,
                 "live default: crash-loop safe mode re-resolves what it shows and writes nothing",
                 "[live_default]") {
    use_geometry(kMedium12x8);
    pin_capabilities(0);
    ScopedRuntimeConfig runtime;
    get_runtime_config()->crash_loop_safe_mode = true;
    ScopedConfigWriteCounter writes;
    const std::string panel = fresh_panel("live_default_safe_mode");
    populate(panel);
    check_env_fil_slot(panel);

    GateObserverScope scope{panel};
    observe_gates(panel);
    set("ams_slot_count", 4);
    run_rebuild();
    check_ams_slot(panel);
    settle_saves();
    CHECK(writes.writes() == 0);
}

TEST_CASE_METHOD(
    LiveDefaultFixture,
    "live default: a grid change re-resolves an unedited default and parks an edited one",
    "[live_default]") {
    use_geometry(kMedium12x8);
    pin_capabilities(0);
    ScopedConfigWriteCounter writes;

    SECTION("unedited: resolved for the new grid") {
        const std::string panel = fresh_panel("live_default_grid_unedited");
        populate(panel);
        REQUIRE(config(panel).grid_signature() == "12x8");

        container_ = make_container(kMedium10x8.content_w, kMedium10x8.content_h);
        populate(panel);
        CHECK(config(panel).grid_signature() == "10x8");
        CHECK(config(panel).is_unedited_default());
        CHECK(config(panel).default_signature().find("|10|8|") != std::string::npos);
        check_none_enabled_unplaced(panel);
        for (const auto& e : config(panel).page_entries(0)) {
            if (e.is_placed()) {
                CAPTURE(e.id);
                lv_obj_t* tile = lv_obj_find_by_name(container_, e.id.c_str());
                REQUIRE(tile != nullptr);
                CHECK(lv_obj_get_style_grid_cell_column_pos(tile, LV_PART_MAIN) == e.col);
                CHECK(lv_obj_get_style_grid_cell_row_pos(tile, LV_PART_MAIN) == e.row);
            }
        }
        config(panel).flush_pending_save();
        const nlohmann::json node = saved_node(panel);
        CHECK_FALSE(node.contains("parked_grids"));
    }
    SECTION("edited: parked and ported as before") {
        const std::string panel = fresh_panel("live_default_grid_edited");
        populate(panel);
        swap_temperatures(panel);
        REQUIRE_FALSE(config(panel).is_unedited_default());

        container_ = make_container(kMedium10x8.content_w, kMedium10x8.content_h);
        populate(panel);
        CHECK(config(panel).grid_signature() == "10x8");
        CHECK(config(panel).default_signature().empty());
        const nlohmann::json node = saved_node(panel);
        REQUIRE(node.contains("parked_grids"));
        CHECK(node["parked_grids"].contains("12x8"));
    }
}

TEST_CASE_METHOD(LiveDefaultFixture,
                 "live default: an auto-placed widget does not make a fresh default the user's",
                 "[live_default]") {
    // On 10 columns the spool slot's 4-wide AMS tile runs off the grid, so the
    // first populate auto-places it and writes the position back.
    use_geometry(kMedium10x8);
    pin_capabilities(4);
    ScopedConfigWriteCounter writes;
    const std::string panel = fresh_panel("live_default_auto_placed");
    populate(panel);
    REQUIRE(placed(panel, "ams"));
    CHECK(config(panel).is_unedited_default());
    check_none_enabled_unplaced(panel);
    config(panel).flush_pending_save();
    {
        const nlohmann::json node = saved_node(panel);
        REQUIRE(node.contains("default"));
        CHECK(node["default"]["hash"] == helix::layout::arrangement_hash(node));
    }

    GateObserverScope scope{panel};
    observe_gates(panel);
    set("ams_slot_count", 0);
    run_rebuild();
    CHECK_FALSE(entry(panel, "ams")->enabled);
    CHECK(placed(panel, "filament"));
    CHECK(config(panel).is_unedited_default());
    check_none_enabled_unplaced(panel);
}

TEST_CASE_METHOD(LiveDefaultFixture,
                 "live default: every tier seats each enabled widget before and after a flip",
                 "[live_default]") {
    const Geometry geometries[] = {
        {"micro 12x8", 480, 272, 430, 264, UiBreakpoint::Micro, 12, 8},
        {"tiny 10x8", 480, 320, 418, 312, UiBreakpoint::Tiny, 10, 8},
        kMedium12x8,
        kMedium10x8,
        {"large 16x10", 1024, 600, 904, 584, UiBreakpoint::Large, 16, 10},
    };
    pin_capabilities(0);
    ScopedConfigWriteCounter writes;
    int n = 0;
    for (const auto& g : geometries) {
        for (int ams : {0, 4}) {
            INFO(g.name << ", ams_slot_count " << ams);
            use_geometry(g);
            set("ams_slot_count", ams);
            const std::string panel = fresh_panel("live_default_tier_" + std::to_string(n++));
            populate(panel);
            CHECK(config(panel).is_unedited_default());
            check_none_enabled_unplaced(panel);

            set("ams_slot_count", ams == 0 ? 4 : 0);
            populate(panel);
            CHECK(config(panel).is_unedited_default());
            check_none_enabled_unplaced(panel);
        }
    }
}
