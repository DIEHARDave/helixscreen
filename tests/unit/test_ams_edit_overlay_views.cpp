// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// View-level tests for the AMS slot editor overlay redesign: identity dropdown
// contents (tracked vs untracked), managed-state subject, pre-selection, and
// view transitions. Uses the full-UI fixture (real XML tree) plus the
// AmsEditOverlayViewTestAccess friend shim.

#include "ui_ams_edit_overlay.h"
#include "ui_icon_codepoints.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/log_capture.h"
#include "../test_helpers/scoped_breakpoint.h"
#include "ams_backend_mock.h"
#include "ams_error.h"
#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "display_numbering.h"
#include "display_settings_manager.h"
#include "filament_favorites.h"
#include "lane_source_store.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "spoolman_manager.h"
#include "spoolman_slot_saver.h"
#include "src/ui/panel_widgets/active_spool_widget.h"

#include <algorithm>
#include <memory>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using namespace helix::ui;

class AmsEditOverlayViewTestAccess {
  public:
    explicit AmsEditOverlayViewTestAccess(AmsEditOverlay& overlay) : overlay_(overlay) {}

    lv_obj_t* widget(const char* name) {
        return overlay_.find_widget(name);
    }
    void set_working_info(const SlotInfo& info) {
        overlay_.working_info_ = info;
    }
    void call_update_ui() {
        overlay_.update_ui();
    }
    int view() {
        return lv_subject_get_int(&overlay_.view_mode_subject_);
    }
    void call_set_view(int v) {
        overlay_.set_view(v);
    }
    int is_managed() {
        return lv_subject_get_int(&overlay_.is_managed_subject_);
    }
    void set_cached_spools(std::vector<SpoolInfo> spools) {
        overlay_.cached_spools_ = std::move(spools);
        // Mirror populate_picker's precompute so direct-drive tests exercise
        // the same filter path as production.
        overlay_.cached_searchables_.clear();
        overlay_.cached_searchables_.reserve(overlay_.cached_spools_.size());
        for (const auto& spool : overlay_.cached_spools_) {
            overlay_.cached_searchables_.push_back(build_searchable_text(spool));
        }
    }
    void call_render_spool_list(const std::string& filter) {
        overlay_.render_spool_list(filter);
    }
    int picker_state() {
        return lv_subject_get_int(&overlay_.picker_state_subject_);
    }
    /// What the XML value_changed callback does for each keystroke.
    void type_picker_search(const std::string& text) {
        overlay_.picker_search_debounce_.schedule(text);
    }
    bool picker_search_pending() {
        return overlay_.picker_search_debounce_.pending();
    }
    void call_enter_editor() {
        overlay_.enter_editor();
    }
    void call_handle_save() {
        overlay_.handle_save();
    }
    void call_switch_to_picker() {
        overlay_.switch_to_picker();
    }
    void call_handle_spool_selected(int spool_id) {
        overlay_.handle_spool_selected(spool_id);
    }
    void call_switch_to_editor() {
        overlay_.set_view(AmsEditOverlay::VIEW_EDITOR);
    }
    /// Pick @p option in the named dropdown the way a touch does: set the
    /// widget's selection and send it value_changed, so the XML-registered
    /// callback runs. False when the option is not in the list.
    bool pick_dropdown(const char* name, const std::string& option) {
        lv_obj_t* dd = overlay_.find_widget(name);
        REQUIRE(dd != nullptr);
        const std::string options = lv_dropdown_get_options(dd);
        int index = 0;
        size_t at = 0;
        while (true) {
            const size_t nl = options.find('\n', at);
            if (options.substr(at, nl == std::string::npos ? nl : nl - at) == option) {
                lv_dropdown_set_selected(dd, static_cast<uint32_t>(index));
                lv_obj_send_event(dd, LV_EVENT_VALUE_CHANGED, nullptr);
                return true;
            }
            if (nl == std::string::npos) {
                return false;
            }
            at = nl + 1;
            ++index;
        }
    }
    AmsEditOverlay::SpoolEditSelection sel() {
        return overlay_.sel_;
    }
    void set_sel(const AmsEditOverlay::SpoolEditSelection& sel) {
        overlay_.sel_ = sel;
        overlay_.rebuild_option_lists();
    }
    void call_apply_weight(float grams) {
        overlay_.apply_weight(grams);
    }
    void set_tag(const helix::ams::TagReading& tag) {
        overlay_.tag_ = tag;
    }
    void call_handle_reset_to_tag() {
        overlay_.handle_reset_to_tag();
    }
    void call_handle_add_to_spoolman() {
        overlay_.handle_add_to_spoolman();
    }
    /// Type into a More field: lv_textarea_set_text sends value_changed, so
    /// the XML-registered callback does the rest.
    void set_detail_field(const char* name, const char* text) {
        lv_obj_t* w = overlay_.find_widget(name);
        REQUIRE(w != nullptr);
        lv_textarea_set_text(w, text);
    }
    /// Shown on screen: the widget and every ancestor up to the overlay root
    /// are free of the hidden flag.
    bool shown(const char* name) {
        lv_obj_t* w = overlay_.find_widget(name);
        REQUIRE(w != nullptr);
        for (lv_obj_t* o = w; o && o != overlay_.overlay_root_; o = lv_obj_get_parent(o)) {
            if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
                return false;
            }
        }
        return true;
    }
    std::string detail_comment() const {
        return overlay_.detail_working_.comment;
    }
    int detail_id() const {
        return overlay_.detail_working_.id;
    }
    std::string dropdown_options(const char* name) {
        lv_obj_t* dd = overlay_.find_widget(name);
        return dd ? lv_dropdown_get_options(dd) : std::string();
    }
    std::string dropdown_selected(const char* name) {
        lv_obj_t* dd = overlay_.find_widget(name);
        if (!dd) {
            return {};
        }
        char buf[64] = {0};
        lv_dropdown_get_selected_str(dd, buf, sizeof(buf) - 1);
        return buf;
    }
    void set_details_color(uint32_t rgb) {
        overlay_.details_color_ = rgb;
        overlay_.details_color_set_ = true;
    }
    SlotInfo working_info() {
        return overlay_.working_info_;
    }
    // Simulate the async Spoolman fetch in enter_spool_edit() that overwrites
    // detail_original_/detail_working_ wholesale with the fetched record
    // (spool_weight_g = empty-spool CORE weight, not the filament total) and
    // repopulates the on-screen fields — exactly what the fetch callback does.
    void seed_detail_fetch(const SpoolInfo& spool) {
        overlay_.detail_original_ = spool;
        overlay_.detail_working_ = spool;
        overlay_.populate_detail_fields();
    }
    bool is_dirty() {
        return overlay_.is_dirty();
    }
    static bool save_is_disabled(int view, bool save_in_flight, bool dirty) {
        return AmsEditOverlay::save_is_disabled(view, save_in_flight, dirty);
    }
    int save_disabled() {
        return lv_subject_get_int(&overlay_.save_disabled_subject_);
    }
    /// What the identity prompt's "It's a new spool" button runs.
    void call_do_spoolman_save(helix::SpoolmanSlotSaver::LinkIntent intent) {
        overlay_.do_spoolman_save(intent);
    }
    void call_open_color_view() {
        overlay_.open_color_view();
    }
    void call_apply_color(uint32_t rgb) {
        overlay_.apply_color(rgb);
    }
    uint32_t details_color() {
        return overlay_.details_color_;
    }
    bool details_color_set() {
        return overlay_.details_color_set_;
    }
    void call_update_tag_badges() {
        overlay_.update_tag_badges();
    }
    /// The touch box: the widget plus its ext_click_area, after layout.
    lv_area_t click_area(const char* name) {
        lv_obj_t* w = overlay_.find_widget(name);
        REQUIRE(w != nullptr);
        lv_obj_update_layout(w);
        lv_area_t a;
        lv_obj_get_click_area(w, &a);
        return a;
    }
    bool has_tag() const {
        return overlay_.tag_.has_value();
    }
    void call_handle_back() {
        overlay_.handle_back();
    }
    void call_handle_weight_tap() {
        overlay_.handle_weight_tap();
    }
    void set_save_in_flight(bool in_flight) {
        overlay_.save_in_flight_ = in_flight;
        overlay_.update_sync_button_state();
    }
    /// What a cover (keypad, scanner pushed on top) does to screen-scoped work.
    void drop_screen_lifetime() {
        overlay_.lifetime_.invalidate();
    }
    std::string field_text(const char* name) {
        lv_obj_t* w = overlay_.find_widget(name);
        REQUIRE(w != nullptr);
        const char* t = lv_textarea_get_text(w);
        return t ? t : "";
    }
    void click(const char* name) {
        lv_obj_t* w = overlay_.find_widget(name);
        REQUIRE(w != nullptr);
        lv_obj_send_event(w, LV_EVENT_CLICKED, nullptr);
    }
    static void build_spool_patches(const SpoolInfo& original, const SpoolInfo& edited,
                                    nlohmann::json& spool_patch, nlohmann::json& filament_patch) {
        SpoolmanSlotSaver::build_spool_patches(original, edited, spool_patch, filament_patch);
    }

  private:
    AmsEditOverlay& overlay_;
};

namespace {

void close_editor_overlay() {
    NavigationManager::instance().go_back();
    UpdateQueue::instance().drain();
    lv_timer_handler(); // the overlay close callback runs on the next tick
}

SlotInfo untracked_slot() {
    SlotInfo info;
    info.slot_index = 0;
    info.spoolman_id = 0;
    info.brand = "Generic";
    info.material = "PETG";
    info.color_rgb = 0xFF6600;
    info.color_name = "Orange";
    return info;
}

SlotInfo tracked_slot() {
    SlotInfo info;
    info.slot_index = 0;
    info.spoolman_id = 7;
    info.brand = "Bambu Lab";
    info.material = "ASA";
    info.spool_name = "Bambu Lab ASA";
    info.color_rgb = 0x8A949E;
    info.color_name = "Gray ASA";
    return info;
}

// A slot as apply_spool_to_slot() leaves it: spool_name is the Spoolman
// filament name alone, with brand and material in their own fields. Nothing
// about the name repeats the other two, so the summary has to join all three.
SlotInfo tracked_slot_spoolman_named() {
    SlotInfo info;
    info.slot_index = 0;
    info.spoolman_id = 42;
    info.brand = "Polymaker";
    info.material = "PLA";
    info.spool_name = "Ambrosia Pink";
    info.color_rgb = 0xFFB6C1;
    return info;
}

// A Spoolman-linked lane on AFC older than v1.2.0: the firmware publishes
// spool_id but no filament_name, so the slot carries the link and nothing to
// name it with. Seen live on a BoxTurtle at 192.168.1.112 (spool #106).
SlotInfo tracked_slot_named_only_in_cache() {
    SlotInfo info;
    info.slot_index = 0;
    info.spoolman_id = 106;
    info.material = "PLA";
    info.color_rgb = 0x333333;
    // brand and spool_name deliberately empty — only the cache knows them.
    return info;
}

// Copy of untracked_slot() with weights explicitly marked unknown (-1
// sentinel, also SlotInfo's default) — models a slot with no Spoolman/manual
// weight data on record.
SlotInfo untracked_slot_without_weights() {
    SlotInfo info = untracked_slot();
    info.total_weight_g = -1.0f;
    info.remaining_weight_g = -1.0f;
    return info;
}

// Mirrors the show_for_slot() + drain + process_lvgl sequence used by every
// test in this file, seeded with a weightless slot. Takes the fixture so it
// can reach test_screen()/process_lvgl() (both LVGLUITestFixture members).
// TEST_MIRROR_OK: this helper CALLS the production show_for_slot() (see the
//                 REQUIRE below) — it factors out the repeated call+pump
//                 sequence, it does not reimplement what show_for_slot() does.
void show_overlay_for_mock_slot_without_weights(LVGLUITestFixture& fixture) {
    auto& overlay = get_ams_edit_overlay();
    REQUIRE(overlay.show_for_slot(fixture.test_screen(), 0, untracked_slot_without_weights(),
                                  nullptr, nullptr));
    UpdateQueue::instance().drain();
    fixture.process_lvgl(10);
}

void show_overlay_for_mock_tracked_slot(LVGLUITestFixture& fixture) {
    auto& overlay = get_ams_edit_overlay();
    // api=nullptr keeps the async Spoolman re-fetch out of the picture.
    REQUIRE(overlay.show_for_slot(fixture.test_screen(), 0, tracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    fixture.process_lvgl(10);
}

void show_overlay_for_mock_untracked_slot(LVGLUITestFixture& fixture) {
    auto& overlay = get_ams_edit_overlay();
    REQUIRE(overlay.show_for_slot(fixture.test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    fixture.process_lvgl(10);
}

/// The linked spool both refetch cases start from, on the server and on the
/// backend's slot 0. The mock inventory already carries id 7 and a GET serves
/// the first match, so this states the fields on that record rather than
/// pushing a second one the reads would never reach.
void seed_linked_spool(MoonrakerAPIMock& api, MoonrakerClientMock& client,
                       AmsBackendMock& backend) {
    auto& spools = client.spoolman_mock().get_mock_spools();
    auto it =
        std::find_if(spools.begin(), spools.end(), [](const SpoolInfo& s) { return s.id == 7; });
    if (it == spools.end()) {
        SpoolInfo added;
        added.id = 7;
        spools.push_back(added);
        it = std::prev(spools.end());
    }
    it->filament_id = 3;
    it->vendor = "Bambu Lab";
    it->material = "ASA";
    it->color_hex = "8A949E";
    it->initial_weight_g = 1000.0;
    it->remaining_weight_g = 850.0;
    backend.sync_external_identity(0, tracked_slot());

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);
    get_printer_state().capabilities_state().set_spoolman_available(true);
    SpoolmanManager::instance().set_api(&api);
    UpdateQueue::instance().drain();
}

/// Fill the lane's Spoolman record from the server, so a case measures the
/// save's own read rather than the first record the lane ever gets.
void poll_lane_once(helix::ams::LaneId lane) {
    SpoolmanManager::instance().refresh_spoolman_weights();
    UpdateQueue::instance().drain();
    REQUIRE(helix::ams::lane_sources(lane).spoolman.has_value());
    REQUIRE(helix::ams::lane_sources(lane).spoolman->brand == "Bambu Lab");
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "show_for_slot skips the identity-sync fetch when Spoolman is unavailable",
                 "[ams_edit_overlay][card][spoolman]") {
    // A Spoolman-less printer must not fire a doomed identity-sync fetch (one
    // RPC + warn log) every time the slot editor opens. show_for_slot() gates
    // the open-time re-fetch on printer_has_spoolman, matching enter_spool_edit.
    //
    // The mock is fully ENABLED here, so if the fetch fired it would return
    // authoritative data and overwrite working_info_ (brand/material). The gate
    // under test is the UI-side availability subject, not the mock's own flag —
    // so an untouched working_info_ proves the fetch never ran.
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    client.set_mock_spoolman_enabled(true);

    // Authoritative record whose vendor/material differ from the slot's initial
    // values, so a fired fetch is observable as a working_info_ change.
    client.spoolman_mock().get_mock_spools().clear();
    SpoolInfo authoritative;
    authoritative.id = 7;
    authoritative.vendor = "SpoolmanVendor";
    authoritative.material = "SpoolmanPETG";
    client.spoolman_mock().get_mock_spools().push_back(authoritative);

    SlotInfo slot = tracked_slot(); // spoolman_id = 7
    slot.brand = "SlotBrand";
    slot.material = "SlotPLA";

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    SECTION("Spoolman unavailable -> no fetch, working_info_ untouched") {
        lv_subject_set_int(spoolman_subj, 0);

        REQUIRE(overlay.show_for_slot(test_screen(), 0, slot, &api, nullptr));
        UpdateQueue::instance().drain();
        process_lvgl(10);

        CHECK(access.working_info().brand == "SlotBrand");
        CHECK(access.working_info().material == "SlotPLA");

        close_editor_overlay();
    }

    SECTION("Spoolman available -> fetch fires, working_info_ synced") {
        lv_subject_set_int(spoolman_subj, 1);

        REQUIRE(overlay.show_for_slot(test_screen(), 0, slot, &api, nullptr));
        UpdateQueue::instance().drain();
        process_lvgl(10);

        CHECK(access.working_info().brand == "SpoolmanVendor");
        CHECK(access.working_info().material == "SpoolmanPETG");

        close_editor_overlay();
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "picker no longer offers a standalone unlink entry",
                 "[ams_edit_overlay][spool_edit][picker]") {
    // The picker_unlink_entry is retired — the single unlink path is the Save
    // Save with the toggle off (covered above).
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    show_overlay_for_mock_tracked_slot(*this);
    access.call_switch_to_picker();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK(access.widget("picker_unlink_entry") == nullptr);

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "color view from spool-edit stages the pending color and returns there",
                 "[ams_edit_overlay][color_view]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_enter_editor();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_open_color_view();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(access.view() == AmsEditOverlay::VIEW_COLOR);

    uint32_t before = access.working_info().color_rgb;
    access.call_apply_color(0x1E88E5);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK(access.view() == AmsEditOverlay::VIEW_EDITOR);
    CHECK(access.details_color() == 0x1E88E5);
    CHECK(access.details_color_set());
    CHECK(access.working_info().color_rgb == before); // slot untouched until Save

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "custom color view opens on the slot's black, not the grey sentinel",
                 "[ams_edit_overlay][color][1608]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    SlotInfo black = untracked_slot();
    black.color_rgb = 0x000000;
    black.color_name = "Black";
    REQUIRE(overlay.show_for_slot(test_screen(), 0, black, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    access.call_enter_editor();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    access.call_open_color_view();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* hex_input = access.widget("ams_color_hex_input");
    REQUIRE(hex_input != nullptr);
    CHECK(std::string(lv_textarea_get_text(hex_input)) == "#000000");

    close_editor_overlay();

    // A sentinel slot keeps its grey starting point: the picker needs some
    // seed, and the sentinel grey is what the slot's own summary already shows.
    SlotInfo sentinel = untracked_slot();
    sentinel.color_rgb = AMS_DEFAULT_SLOT_COLOR;
    sentinel.color_name.clear();
    REQUIRE(overlay.show_for_slot(test_screen(), 0, sentinel, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    access.call_enter_editor();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    access.call_open_color_view();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // close_editor_overlay() tears the widget tree down, so re-find the input
    // in the rebuilt tree rather than reusing the pointer from the first half.
    hex_input = access.widget("ams_color_hex_input");
    REQUIRE(hex_input != nullptr);
    CHECK(std::string(lv_textarea_get_text(hex_input)) == "#808080");

    close_editor_overlay();
}

namespace {
std::vector<SpoolInfo> two_spools() {
    SpoolInfo a;
    a.id = 11;
    a.vendor = "Polymaker";
    a.material = "PLA";
    a.color_hex = "1A1A2E";
    SpoolInfo b;
    b.id = 22;
    b.vendor = "eSUN";
    b.material = "PETG";
    b.color_hex = "00FF00";
    return {a, b};
}
} // namespace

// ============================================================================
// OverlayConsumerCommitFixture — for tests whose completion callback must
// mirror the production consumers (AmsPanel / AmsOverviewPanel): the overlay
// no longer pre-fires the server active-spool sync, so the consumer's
// commit_slot_edit() owns it, and the wiring that commit needs (a backend +
// the API registered on AmsState) has to exist in the test too. Wiring shape
// mirrors OverlayCommitFixture (test_ams_edit_overlay_active_spool.cpp) /
// CommitFixture (test_ams_state_commit_slot.cpp); no Config isolation —
// commit_slot_edit persists nothing (AmsBackendMock ignores the persist flag).
// ============================================================================

struct OverlayConsumerCommitFixture : LVGLUITestFixture {
    MoonrakerClientMock client;
    MoonrakerAPIMock api;
    AmsBackendMock* backend = nullptr;

    OverlayConsumerCommitFixture() : api(client, get_printer_state()) {
        auto& ams = AmsState::instance();
        ams.clear_backends();
        ams.deinit_subjects();
        // AmsState::init_subjects observes PrinterState's print-state subject;
        // it must exist first or the observer attaches to nothing.
        get_printer_state().init_subjects(false);
        ams.init_subjects(false);

        // A previous test file's SpoolmanManager::deinit_subjects() may have
        // latched its shutdown flag — unlatch it (see CommitFixture).
        SpoolmanManager::instance().init_subjects();
        SpoolmanManager::clear_identity_cache();

        auto owned = std::make_unique<AmsBackendMock>(4);
        backend = owned.get();
        ams.set_backend(std::move(owned));
        ams.set_moonraker_api(&api);
    }

    ~OverlayConsumerCommitFixture() override {
        // Detach the mocks BEFORE members are destroyed and while LVGL still
        // runs (base-class teardown has not happened yet).
        auto& ams = AmsState::instance();
        ams.set_moonraker_api(nullptr);
        ams.clear_backends();
        // Drain while AmsState's subjects are still alive; queued backend-event
        // syncs from this test must not leak into the next one.
        UpdateQueue::instance().drain();
        ams.deinit_subjects();
        SpoolmanManager::clear_identity_cache();
        // The manager holds a raw pointer, and this fixture's api outlives it
        // by one member destruction at most.
        SpoolmanManager::instance().set_api(nullptr);
    }

    /// The production backend-slot completion-consumer body (AmsPanel /
    /// AmsOverviewPanel): capture the pre-edit slot BEFORE the commit — its
    /// unlink arm (clear the server active spool) needs the old link.
    void commit_like_consumer(const AmsEditOverlay::EditResult& r) {
        if (!r.saved || r.slot_index < 0)
            return;
        AmsBackend* commit_backend = AmsState::instance().get_backend();
        REQUIRE(commit_backend);
        SlotInfo original = commit_backend->get_slot_info(r.slot_index);
        AmsError err = AmsState::instance().commit_slot_edit(r.slot_index, original, r.slot_info);
        REQUIRE(err.success());
    }
};

TEST_CASE_METHOD(LVGLUITestFixture, "picker pre-selects the first row for unlinked slots",
                 "[ams_edit_overlay][picker][preselect]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.set_cached_spools(two_spools());
    access.call_render_spool_list("");
    UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* list = access.widget("picker_spool_list");
    REQUIRE(list != nullptr);
    REQUIRE(lv_obj_get_child_count(list) == 2);
    CHECK(lv_obj_has_state(lv_obj_get_child(list, 0), LV_STATE_CHECKED));
    CHECK_FALSE(lv_obj_has_state(lv_obj_get_child(list, 1), LV_STATE_CHECKED));

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "picker row names a spool by its filament name",
                 "[ams_edit_overlay][picker][spool_label]") {
    // Two spools of one vendor and material differ only by Spoolman's filament
    // name, so the row's primary label (the only text a compact row shows)
    // has to carry it.
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr,
                                  /*open_on_picker=*/true));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    SpoolInfo clear;
    clear.id = 12;
    clear.vendor = "Kingroon";
    clear.material = "PETG";
    clear.filament_name = "Kingroon Basic PETG Clear";
    clear.remaining_weight_g = 750;
    SpoolInfo white = clear;
    white.id = 13;
    white.filament_name = "Kingroon Basic PETG White";
    SpoolInfo unnamed;
    unnamed.id = 14;
    unnamed.vendor = "Polymaker";
    unnamed.material = "PLA";
    access.set_cached_spools({clear, white, unnamed});

    // Medium renders compact single-line rows, Large the two-line rich rows.
    const UiBreakpoint bp = GENERATE(UiBreakpoint::Medium, UiBreakpoint::Large);
    CAPTURE(to_int(bp));
    helix::test::ScopedBreakpoint scoped_bp(bp);
    access.call_render_spool_list("");
    UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* list = access.widget("picker_spool_list");
    REQUIRE(list != nullptr);
    REQUIRE(lv_obj_get_child_count(list) == 3);

    const auto row_for = [list](int spool_id) -> lv_obj_t* {
        for (uint32_t i = 0; i < lv_obj_get_child_count(list); ++i) {
            lv_obj_t* row = lv_obj_get_child(list, static_cast<int32_t>(i));
            if (reinterpret_cast<intptr_t>(lv_obj_get_user_data(row)) == spool_id) {
                return row;
            }
        }
        return nullptr;
    };
    const auto name_of = [&row_for](int spool_id) -> std::string {
        lv_obj_t* row = row_for(spool_id);
        REQUIRE(row != nullptr);
        lv_obj_t* label = lv_obj_find_by_name(row, "spool_name");
        REQUIRE(label != nullptr);
        return lv_label_get_text(label);
    };

    // The vendor and material the filament name already contains are not repeated.
    CHECK(name_of(12) == "#12 Kingroon Basic PETG Clear");
    CHECK(name_of(13) == "#13 Kingroon Basic PETG White");
    CHECK(name_of(14) == "#14 Polymaker PLA");

    // The row prints the filament name once, whatever the layout.
    const auto visible_text = [](lv_obj_t* root) {
        std::string text;
        std::vector<lv_obj_t*> stack{root};
        while (!stack.empty()) {
            lv_obj_t* obj = stack.back();
            stack.pop_back();
            if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
                continue;
            }
            if (lv_obj_check_type(obj, &lv_label_class)) {
                text += lv_label_get_text(obj);
                text += '\n';
            }
            for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
                stack.push_back(lv_obj_get_child(obj, static_cast<int32_t>(i)));
            }
        }
        return text;
    };
    const auto occurrences = [](const std::string& haystack, const std::string& needle) {
        size_t count = 0;
        for (size_t pos = haystack.find(needle); pos != std::string::npos;
             pos = haystack.find(needle, pos + needle.size())) {
            ++count;
        }
        return count;
    };
    CHECK(occurrences(visible_text(row_for(12)), "Basic PETG Clear") == 1);
    CHECK(occurrences(visible_text(row_for(13)), "Basic PETG White") == 1);

    // A label that holds the name but lays out zero-sized shows nothing.
    lv_obj_update_layout(list);
    lv_obj_t* label = lv_obj_find_by_name(row_for(12), "spool_name");
    CHECK(lv_obj_get_height(label) > 0);
    CHECK(lv_obj_get_width(label) > 0);
    // The name leaves room for the weight beside or below it, inside the
    // details box that holds them both.
    lv_obj_t* weight = lv_obj_find_by_name(row_for(12), "spool_weight");
    lv_area_t details_area;
    lv_area_t weight_area;
    lv_obj_get_coords(lv_obj_get_parent(lv_obj_get_parent(weight)), &details_area);
    lv_obj_get_coords(weight, &weight_area);
    CHECK(weight_area.x2 <= details_area.x2);

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "picker search burst renders once, after the pause",
                 "[ams_edit_overlay][picker][search]") {
    // Regression: every value_changed used to rebuild the whole spool list
    // synchronously - at a few hundred spools that froze the keyboard for the
    // length of the rebuild, per keystroke (K2 Plus report 2026-08-30).
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr,
                                  /*open_on_picker=*/true));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.set_cached_spools(two_spools());
    access.call_render_spool_list("");
    UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* list = access.widget("picker_spool_list");
    REQUIRE(list != nullptr);
    REQUIRE(lv_obj_get_child_count(list) == 2);

    // Type a burst. Nothing may render synchronously or mid-burst: the list
    // keeps the previous content and the debounce stays armed.
    access.type_picker_search("e");
    access.type_picker_search("es");
    access.type_picker_search("esu");
    access.type_picker_search("esun");
    UpdateQueue::instance().drain();
    process_lvgl(10); // well under the 300ms delay
    CHECK(access.picker_search_pending());
    CHECK(lv_obj_get_child_count(list) == 2);

    // After the pause: ONE render with the final query ("esun" -> eSUN only).
    process_lvgl(320);
    UpdateQueue::instance().drain();
    CHECK_FALSE(access.picker_search_pending());
    CHECK(lv_obj_get_child_count(list) == 1);
    CHECK(access.picker_state() == 2);

    // Clearing applies immediately - no debounce wait, no timer.
    access.type_picker_search("");
    UpdateQueue::instance().drain();
    CHECK_FALSE(access.picker_search_pending());
    CHECK(lv_obj_get_child_count(list) == 2);

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "picker zero-match shows no-match state, not fetch-error retry",
                 "[ams_edit_overlay][picker][search]") {
    // The empty-filter result and the failed fetch used to share state 1, so
    // a term that matches nothing flashed "No spools found - tap to retry"
    // mid-word. A successful fetch with no matches is state 3: informational,
    // not an error, and not clickable.
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr,
                                  /*open_on_picker=*/true));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.set_cached_spools(two_spools());
    access.call_render_spool_list("zzzz nothing matches this");
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK(access.picker_state() == 3);
    lv_obj_t* no_match = access.widget("picker_no_match");
    REQUIRE(no_match != nullptr);
    CHECK_FALSE(lv_obj_has_flag(no_match, LV_OBJ_FLAG_HIDDEN));
    lv_obj_t* retry = access.widget("picker_empty");
    REQUIRE(retry != nullptr);
    CHECK(lv_obj_has_flag(retry, LV_OBJ_FLAG_HIDDEN));

    lv_obj_t* list = access.widget("picker_spool_list");
    REQUIRE(list != nullptr);
    CHECK(lv_obj_get_child_count(list) == 0);

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "picker-entry spool selection commits and closes the editor",
                 "[ams_edit_overlay][picker][header_save]") {
    // Task #13: when the editor is opened directly on the picker (context-menu
    // "Select spool"), choosing a spool is a one-tap commit — apply + close the
    // whole overlay via the header-Save commit path, firing completion with the
    // applied spool. Spoolman is left unavailable so commit_and_close takes the
    // synchronous local-close branch (no async PATCH seam needed here).
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 0);

    bool fired = false;
    AmsEditOverlay::EditResult captured;
    REQUIRE(overlay.show_for_slot(
        test_screen(), 0, untracked_slot(), nullptr,
        [&](const AmsEditOverlay::EditResult& r) {
            fired = true;
            captured = r;
        },
        /*open_on_picker=*/true));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(access.view() == AmsEditOverlay::VIEW_SPOOL_PICKER);

    access.set_cached_spools(two_spools());
    access.call_render_spool_list("");
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Select spool #22 (eSUN PETG) from the picker.
    access.call_handle_spool_selected(22);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Committed + closed in one step, completion fired with the applied spool.
    CHECK(fired);
    CHECK(captured.saved);
    CHECK(captured.slot_info.spoolman_id == 22);
    CHECK(captured.slot_info.material == "PETG");

    UpdateQueue::instance().drain();
    process_lvgl(10);
}

TEST_CASE_METHOD(OverlayConsumerCommitFixture,
                 "the editor header names the position in the active backend's own word",
                 "[ams_edit_overlay][i18n]") {
    // The fixture registers an AmsBackendMock, which reports Happy Hare, whose
    // noun is Gate. The title a user reads above the editor has to be the same
    // word the panel behind it used for the thing they tapped.
    REQUIRE(backend->lane_noun() == helix::ui::LaneNoun::Gate);

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    SlotInfo info = untracked_slot();
    info.slot_index = 2;
    REQUIRE(overlay.show_for_slot(test_screen(), 2, info, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // header_bar's title carries text_transform="uppercase", so the rendered
    // string is the composed title in caps.
    lv_obj_t* title = access.widget("header_title");
    REQUIRE(title != nullptr);
    CHECK(std::string(lv_label_get_text(title)) == "GATE 3");

    close_editor_overlay();
}

TEST_CASE_METHOD(OverlayConsumerCommitFixture,
                 "picker-entry relink to a different spool never prompts or PATCHes the old spool",
                 "[ams_edit_overlay][filament_picker][picker][header_save]") {
    // Task #16 regression: a slot linked to spool A, opened directly on the
    // picker, then switched to a DIFFERENT spool B is a pure RELINK — not an
    // edit of A's identity. The old "Different filament?" confirm + identity
    // PATCH would clobber the wrong Spoolman record. Assert: no confirm modal,
    // completion fires once with spool B, and NO spool/filament PATCH is sent.

    // Seed the currently-linked spool A (id 7) so the open-time re-fetch has an
    // authoritative record and leaves original_info_ as A's identity.
    SpoolInfo linked_a;
    linked_a.id = 7;
    linked_a.filament_id = 3;
    linked_a.vendor = "Bambu Lab";
    linked_a.material = "ASA";
    linked_a.color_hex = "8A949E";
    client.spoolman_mock().get_mock_spools().push_back(linked_a);
    // The backend mirrors the overlay's initial info, the way a live backend
    // holds the tracked slot the editor was opened on.
    backend->sync_external_identity(0, tracked_slot());

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1); // is_spoolman_available() -> true
    get_printer_state().capabilities_state().set_spoolman_available(true);

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    bool fired = false;
    AmsEditOverlay::EditResult captured;
    REQUIRE(overlay.show_for_slot(
        test_screen(), 0, tracked_slot(), &api,
        [&](const AmsEditOverlay::EditResult& r) {
            fired = true;
            captured = r;
            commit_like_consumer(r); // the consumer commit owns the server sync
        },
        /*open_on_picker=*/true));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(access.view() == AmsEditOverlay::VIEW_SPOOL_PICKER);

    access.set_cached_spools(two_spools());
    access.call_render_spool_list("");
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Switch the link to spool #22 (eSUN PETG) — a different material/color.
    access.call_handle_spool_selected(22);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // No "Different filament?" confirm modal interposed.
    CHECK(ModalStack::instance().stack_empty());
    // Committed + closed in one step with the newly linked spool.
    CHECK(fired);
    CHECK(captured.saved);
    CHECK(captured.slot_info.spoolman_id == 22);
    CHECK(captured.slot_info.material == "PETG");
    // The old spool A was never touched: no identity/weight PATCH, no filament
    // repoint, no new filament created.
    CHECK(client.spoolman_mock().spool_updates.empty());
    CHECK(client.spoolman_mock().filament_updates.empty());
    // The new spool is registered as active on the server.
    CHECK(client.spoolman_mock().get_mock_active_spool_id() == 22);

    get_printer_state().capabilities_state().set_spoolman_available(false); // restore clean slate
    UpdateQueue::instance().drain();
    process_lvgl(10);
}

TEST_CASE_METHOD(OverlayConsumerCommitFixture,
                 "two-step relink header Save never prompts or PATCHes the old spool",
                 "[ams_edit_overlay][filament_picker][picker][header_save]") {
    // Task #16, two-step variant: reach the picker via Change Filament (not the
    // one-tap entry), pick a different spool, return to the editor, then tap
    // header Save. Same relink semantics: no confirm, no PATCH of the old spool.

    SpoolInfo linked_a;
    linked_a.id = 7;
    linked_a.filament_id = 3;
    linked_a.vendor = "Bambu Lab";
    linked_a.material = "ASA";
    linked_a.color_hex = "8A949E";
    client.spoolman_mock().get_mock_spools().push_back(linked_a);
    backend->sync_external_identity(0, tracked_slot());

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);
    get_printer_state().capabilities_state().set_spoolman_available(true);

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    bool fired = false;
    AmsEditOverlay::EditResult captured;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api,
                                  [&](const AmsEditOverlay::EditResult& r) {
                                      fired = true;
                                      captured = r;
                                      commit_like_consumer(r);
                                  }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_switch_to_picker(); // Change-Filament entry: clears the shortcut
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(access.view() == AmsEditOverlay::VIEW_SPOOL_PICKER);

    access.set_cached_spools(two_spools());
    access.call_render_spool_list("");
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_handle_spool_selected(22); // stages the relink, returns to the editor
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(access.view() == AmsEditOverlay::VIEW_EDITOR);
    REQUIRE_FALSE(fired); // not committed yet
    REQUIRE(access.working_info().spoolman_id == 22);

    access.call_handle_save(); // header Save on the editor
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK(ModalStack::instance().stack_empty()); // no identity confirm
    CHECK(fired);
    CHECK(captured.saved);
    CHECK(captured.slot_info.spoolman_id == 22);
    CHECK(client.spoolman_mock().spool_updates.empty());
    CHECK(client.spoolman_mock().filament_updates.empty());
    CHECK(client.spoolman_mock().get_mock_active_spool_id() == 22);

    get_printer_state().capabilities_state().set_spoolman_available(false); // restore clean slate
    UpdateQueue::instance().drain();
    process_lvgl(10);
}

TEST_CASE_METHOD(OverlayConsumerCommitFixture,
                 "active_spool widget completion routes the edit through commit_slot_edit",
                 "[ams_edit_overlay][active_spool][commit]") {
    // Final-review find: ActiveSpoolWidget is the sixth completion consumer of
    // the shared edit overlay. Its backend-slot arm used to write the backend
    // directly (apply_user_edit) — no server active-spool sync, no identity
    // invalidation. The consumer commit owns both; this drives the widget's
    // own click → editor → Save path and asserts the server sync fired.

    // The mock backend starts with slot 0 loaded and current by construction;
    // give that lane a Spoolman link the way a live one carries it.
    SlotInfo seeded = backend->get_slot_info(0);
    seeded.spoolman_id = 169;
    seeded.material = "PLA";
    backend->sync_external_identity(0, seeded);

    // Spoolman "unavailable" so the editor's Save takes the synchronous
    // local-close branch (no async PATCH seam).
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 0);
    get_printer_state().capabilities_state().set_spoolman_available(false);

    // Build the home-panel component + controller, then tap it the way a user
    // does — the widget's own completion wiring is the code under test.
    lv_obj_t* comp =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "panel_widget_active_spool", nullptr));
    REQUIRE(comp != nullptr);
    ActiveSpoolWidget widget(&api);
    widget.attach(comp, test_screen());

    lv_obj_t* btn = lv_obj_find_by_name(comp, "spoolman_btn");
    REQUIRE(btn != nullptr);
    lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // The editor opened on the loaded slot; stage an edit and header-Save the
    // overlay the widget opened.
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    // A linked spool owns the material, so the edit moves the colour, which is
    // the user's to change.
    SlotInfo edited = seeded;
    edited.color_rgb = 0x1E5AA8;
    access.set_working_info(edited);
    access.call_handle_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // REQUIRED: the edit reached the backend slot through commit_slot_edit...
    const SlotInfo after = backend->get_slot_info(0);
    REQUIRE(after.color_rgb == 0x1E5AA8u);
    REQUIRE(after.spoolman_id == 169);
    // ...AND the server active-spool sync fired — the old direct-write arm
    // never did, which is exactly the branch regression this pins.
    REQUIRE(client.spoolman_mock().get_mock_active_spool_id() == 169);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "editing the linked spool's identity (same spool) still prompts to confirm",
                 "[ams_edit_overlay][filament_picker]") {
    // Contrast with the relink cases: when spoolman_id is UNCHANGED and the
    // material identity changes, this is a genuine edit of the linked spool —
    // the "Different filament?" confirmation MUST still interpose (task #16 must
    // not swallow it). Completion does not fire until the user answers.
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    // Seed spool 7 so the open-time re-fetch leaves original_info_ as ASA.
    SpoolInfo linked_a;
    linked_a.id = 7;
    linked_a.filament_id = 3;
    linked_a.vendor = "Bambu Lab";
    linked_a.material = "ASA";
    linked_a.color_hex = "8A949E";
    client.spoolman_mock().get_mock_spools().push_back(linked_a);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);
    get_printer_state().capabilities_state().set_spoolman_available(true);
    UpdateQueue::instance().drain(); // flush the queued availability update
    REQUIRE(get_printer_state().is_spoolman_available());

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    bool fired = false;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api,
                                  [&](const AmsEditOverlay::EditResult&) { fired = true; }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Same linked spool (id 7), but change the material identity in place.
    SlotInfo edited = access.working_info();
    edited.material = "PLA"; // ASA -> PLA on the SAME spool 7
    access.set_working_info(edited);

    access.call_handle_save(); // header Save on the editor
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // The relink short-circuit must NOT swallow a same-spool identity edit: it
    // still routes into the confirm path (no early close). Either the confirm
    // modal is up (completion pending) or the fallback save fired against the
    // linked spool — but never a silent no-op relink close.
    const bool confirm_pending = !ModalStack::instance().stack_empty() && !fired;
    const bool save_ran = !client.spoolman_mock().spool_updates.empty() ||
                          !client.spoolman_mock().filament_updates.empty();
    CHECK((confirm_pending || save_ran));

    // Dismiss any modal so teardown is clean.
    if (!ModalStack::instance().stack_empty()) {
        Modal::hide(Modal::get_top());
        UpdateQueue::instance().drain();
        process_lvgl(10);
    }
    if (!fired) {
        close_editor_overlay();
    }
    get_printer_state().capabilities_state().set_spoolman_available(false); // restore clean slate
}

TEST_CASE_METHOD(LVGLUITestFixture, "a linked slot still saves a color while Spoolman is offline",
                 "[ams_edit_overlay][spool_edit][1653]") {
    // Colour is the lane's own, not the spool's, so the read-only identity does
    // not reach it: a swatch picked with Spoolman down still saves.
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 0);
    get_printer_state().capabilities_state().set_spoolman_available(false);
    UpdateQueue::instance().drain();

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    bool fired = false;
    AmsEditOverlay::EditResult captured;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), nullptr,
                                  [&](const AmsEditOverlay::EditResult& r) {
                                      fired = true;
                                      captured = r;
                                  }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_enter_editor();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(access.view() == AmsEditOverlay::VIEW_EDITOR);

    access.set_details_color(0x112233);
    access.call_handle_save(); // header Save on spool-edit finishes the edit
    UpdateQueue::instance().drain();
    process_lvgl(10);

    REQUIRE(fired);
    CHECK(captured.slot_info.color_rgb == 0x112233U);
    CHECK(captured.slot_info.material == tracked_slot().material);
    CHECK(captured.slot_info.brand == tracked_slot().brand);
}

TEST_CASE_METHOD(LVGLUITestFixture, "header-picker selection returns to the editor without closing",
                 "[ams_edit_overlay][picker]") {
    // Contrast with the picker-entry shortcut: reaching the picker via the header
    // Spoolman button (switch_to_picker clears opened_on_picker_) keeps the two-step
    // flow — a selection returns to the editor for review, no completion.
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);

    bool fired = false;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr,
                                  [&](const AmsEditOverlay::EditResult&) { fired = true; }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_switch_to_picker(); // Change-Filament entry: clears the shortcut
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(access.view() == AmsEditOverlay::VIEW_SPOOL_PICKER);

    access.set_cached_spools(two_spools());
    access.call_render_spool_list("");
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_handle_spool_selected(22);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK(access.view() == AmsEditOverlay::VIEW_EDITOR);
    CHECK_FALSE(fired); // no commit — the editor's header Save commits later
    CHECK(access.working_info().spoolman_id == 22); // staged, awaiting review

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "picker always offers the setup entry",
                 "[ams_edit_overlay][picker][setup_entry]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr,
                                  /*open_on_picker=*/true));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* entry = access.widget("picker_setup_entry");
    REQUIRE(entry != nullptr);
    // Present regardless of picker fetch state (loading/empty/content) —
    // it is the only path forward when the spool list is empty.
    CHECK_FALSE(lv_obj_has_flag(entry, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(entry, LV_OBJ_FLAG_CLICKABLE));

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "picker pre-selects the current spool when linked",
                 "[ams_edit_overlay][picker][preselect]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    SlotInfo linked = tracked_slot();
    linked.spoolman_id = 22;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, linked, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.set_cached_spools(two_spools());
    access.call_render_spool_list("");
    UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* list = access.widget("picker_spool_list");
    REQUIRE(list != nullptr);
    REQUIRE(lv_obj_get_child_count(list) == 2);
    CHECK_FALSE(lv_obj_has_state(lv_obj_get_child(list, 0), LV_STATE_CHECKED));
    CHECK(lv_obj_has_state(lv_obj_get_child(list, 1), LV_STATE_CHECKED));

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "header Save shows on the editor, hides on picker/color",
                 "[ams_edit_overlay][views]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    auto* hide_subj = lv_xml_get_subject(nullptr, "ams_edit_save_hidden");
    REQUIRE(hide_subj != nullptr);
    auto* save_dis = lv_xml_get_subject(nullptr, "ams_edit_save_disabled");
    REQUIRE(save_dis != nullptr);
    REQUIRE(lv_subject_get_int(hide_subj) == 0); // editor: visible

    access.call_set_view(AmsEditOverlay::VIEW_SPOOL_PICKER);
    REQUIRE(lv_subject_get_int(hide_subj) == 1); // picker: hidden

    // Editor: Save stays VISIBLE and is disabled while nothing is dirty —
    // the dropdowns stage into working_info_ only through Save.
    access.call_set_view(AmsEditOverlay::VIEW_EDITOR);
    REQUIRE(lv_subject_get_int(hide_subj) == 0);
    REQUIRE_FALSE(access.is_dirty());
    CHECK(lv_subject_get_int(save_dis) == 1);

    access.call_set_view(AmsEditOverlay::VIEW_COLOR);
    REQUIRE(lv_subject_get_int(hide_subj) == 1); // color: hidden

    access.call_set_view(AmsEditOverlay::VIEW_EDITOR);
    REQUIRE(lv_subject_get_int(hide_subj) == 0);
    // Overview: dirty-gated — a clean slot keeps Save disabled.
    CHECK(lv_subject_get_int(save_dis) == 1);

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "weightless slot opens clean — no fabricated weights",
                 "[ams_edit_overlay][dirty]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    show_overlay_for_mock_slot_without_weights(*this); // SlotInfo with total/remaining = -1
    access.call_update_ui();

    REQUIRE_FALSE(access.is_dirty());
    auto* save_dis = lv_xml_get_subject(nullptr, "ams_edit_save_disabled");
    REQUIRE(save_dis != nullptr);
    REQUIRE(lv_subject_get_int(save_dis) == 1);             // Save stays disabled
    REQUIRE(access.working_info().total_weight_g <= 0);     // untouched
    REQUIRE(access.working_info().remaining_weight_g <= 0); // untouched

    close_editor_overlay();
}

TEST_CASE("build_spool_patches splits spool-level vs filament-level fields",
          "[ams_edit_overlay][spool_details]") {
    SpoolInfo original;
    original.id = 42;
    original.filament_id = 7;
    original.remaining_weight_g = 500.0;
    original.spool_weight_g = 140.0;
    original.price = 19.99;
    original.lot_nr = "LOT-A";
    original.location = "Shelf A";
    original.comment = "";
    original.color_hex = "#FF0000";

    SpoolInfo edited = original;
    edited.remaining_weight_g = 450.0;
    edited.spool_weight_g = 200.0;
    edited.price = 24.99;
    edited.lot_nr = "LOT-B";
    edited.location = "Shelf B";
    edited.comment = "dried 4h";
    edited.color_hex = "#00FF00";

    nlohmann::json spool_patch;
    nlohmann::json filament_patch;
    AmsEditOverlayViewTestAccess::build_spool_patches(original, edited, spool_patch,
                                                      filament_patch);

    CHECK(spool_patch["remaining_weight"] == Catch::Approx(450.0));
    CHECK(spool_patch["price"] == Catch::Approx(24.99));
    CHECK(spool_patch["lot_nr"] == "LOT-B");
    CHECK(spool_patch["location"] == "Shelf B");
    CHECK(spool_patch["comment"] == "dried 4h");
    CHECK(spool_patch["spool_weight"] == Catch::Approx(200.0));

    CHECK(filament_patch.count("spool_weight") == 0);
    CHECK(filament_patch["color_hex"] == "#00FF00");
    CHECK(filament_patch.count("remaining_weight") == 0);

    // No changes -> both empty
    nlohmann::json empty_spool;
    nlohmann::json empty_filament;
    AmsEditOverlayViewTestAccess::build_spool_patches(original, original, empty_spool,
                                                      empty_filament);
    CHECK(empty_spool.empty());
    CHECK(empty_filament.empty());
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "a failed Spoolman save keeps the editor open and commits nothing",
                 "[ams_edit_overlay][spool_edit]") {
    // A save that did not reach Spoolman has changed nothing anywhere, so the
    // editor stays on the user's edit rather than closing on a local commit
    // they would read as saved.
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SpoolInfo linked;
    linked.id = 7;
    linked.filament_id = 3;
    linked.vendor = "Generic";
    linked.material = "PLA";
    linked.color_hex = "112233";
    client.spoolman_mock().get_mock_spools().push_back(linked);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);
    get_printer_state().capabilities_state().set_spoolman_available(true);
    UpdateQueue::instance().drain();

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    bool fired = false;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api,
                                  [&](const AmsEditOverlay::EditResult&) { fired = true; }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Every Spoolman request from here on fails.
    client.set_mock_spoolman_enabled(false);
    helix::TextLogCapture log;

    // A brand move on a linked spool changes which filament the spool points
    // at, so it PROMPTS (Update in Spoolman / Unlink) exactly like a material
    // move - not silently writing. The prompt owns this save; nothing is
    // written until a button answers it.
    SlotInfo edited = access.working_info();
    edited.brand = "Sunlu";
    access.set_working_info(edited);
    access.call_handle_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK_FALSE(ModalStack::instance().stack_empty()); // the Update/Unlink prompt
    CHECK_FALSE(log.contains("Spoolman save failed")); // no write was attempted
    CHECK_FALSE(fired);

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "a failed Spoolman write keeps the editor open on the staged edit",
                 "[ams_edit_overlay][spoolman][failure]") {
    // A weight-only edit (spool-level, no identity move) reaches the Spoolman
    // write with no prompt - and with every request failing, nothing is
    // written anywhere, so the editor stays on the user's edit rather than
    // closing on a local commit they would read as saved.
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SpoolInfo linked;
    linked.id = 7;
    linked.filament_id = 3;
    linked.vendor = "Bambu Lab";
    linked.material = "ASA";
    linked.color_hex = "8A949E";
    client.spoolman_mock().get_mock_spools().push_back(linked);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);
    get_printer_state().capabilities_state().set_spoolman_available(true);
    UpdateQueue::instance().drain();

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    bool fired = false;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api,
                                  [&](const AmsEditOverlay::EditResult&) { fired = true; }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    client.set_mock_spoolman_enabled(false);
    helix::TextLogCapture log;

    SlotInfo weighed = access.working_info();
    weighed.remaining_weight_g = 500;
    access.set_working_info(weighed);
    access.call_handle_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    // The toast is queued from inside the deferred save answer.
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK_FALSE(fired);
    CHECK(access.working_info().remaining_weight_g == 500);
    CHECK(log.contains("Spoolman save failed; nothing written"));
    // Save is available again: the edit is still staged and still unsaved.
    CHECK(access.save_disabled() == 0);

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "an incomplete new spool keeps the editor open and names what is missing",
                 "[ams_edit_overlay][spool_edit]") {
    // "It's a new spool" creates a filament, and Spoolman identifies one by
    // vendor, material and colour. With a field missing there is nothing to
    // create, so the editor stays open rather than closing on a save that
    // wrote nothing.
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);
    get_printer_state().capabilities_state().set_spoolman_available(true);
    UpdateQueue::instance().drain();

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    bool fired = false;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api,
                                  [&](const AmsEditOverlay::EditResult&) { fired = true; }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    SlotInfo edited = access.working_info();
    edited.brand = "";
    edited.color_rgb = 0xE53935;
    access.set_working_info(edited);

    helix::TextLogCapture log;
    access.call_do_spoolman_save(SpoolmanSlotSaver::LinkIntent::CreateAndRebind);
    UpdateQueue::instance().drain();
    process_lvgl(10);
    // The toast is queued from inside the deferred save answer.
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK_FALSE(fired);
    // The save stopped for the missing field rather than as a generic failure,
    // which is what decides the sentence the user is shown.
    CHECK(log.contains("missing a field it is identified by"));
    CHECK_FALSE(log.contains("Spoolman save failed"));
    CHECK(client.spoolman_mock().created_spools.empty());
    CHECK(client.spoolman_mock().created_filaments.empty());

    NavigationManager::instance().go_back();
    UpdateQueue::instance().drain();
    process_lvgl(10);
}

TEST_CASE_METHOD(OverlayConsumerCommitFixture,
                 "a successful Spoolman save re-reads the linked spool onto the lane before the "
                 "next poll",
                 "[ams_edit_overlay][spoolman][1653]") {
    // A linked spool resolves its identity and its weights from the lane's
    // Spoolman record, which only a fetch writes, and the poll is a whole
    // interval wide. The save is the moment that record is known stale, so it
    // re-reads the spool it just wrote.
    seed_linked_spool(api, client, *backend);
    const helix::ams::LaneId lane = backend->lane_id(0);
    poll_lane_once(lane);
    REQUIRE(helix::ams::lane_sources(lane).spoolman->remaining_weight_g == 850.0F);

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    bool fired = false;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api,
                                  [&](const AmsEditOverlay::EditResult& r) {
                                      fired = true;
                                      commit_like_consumer(r);
                                  }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // A weight move is an edit Spoolman has to write, and it never prompts.
    SlotInfo edited = access.working_info();
    edited.remaining_weight_g = 600.0F;
    access.set_working_info(edited);

    access.call_handle_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(fired);
    REQUIRE(ModalStack::instance().stack_empty());

    // The save reached Spoolman. Separated from the lane check below so a
    // failure names which of the two halves broke.
    const auto& spools = client.spoolman_mock().get_mock_spools();
    const auto served =
        std::find_if(spools.begin(), spools.end(), [](const SpoolInfo& s) { return s.id == 7; });
    REQUIRE(served != spools.end());
    REQUIRE(served->remaining_weight_g == 600.0);

    // The read is issued after the commit, and its answer is filed from a
    // later drain.
    UpdateQueue::instance().drain();
    process_lvgl(10);

    const auto record = helix::ams::lane_sources(lane).spoolman;
    REQUIRE(record.has_value());
    REQUIRE(record->remaining_weight_g.has_value());
    CHECK(*record->remaining_weight_g == 600.0F);

    get_printer_state().capabilities_state().set_spoolman_available(false); // restore clean slate
    UpdateQueue::instance().drain();
    process_lvgl(10);
}

TEST_CASE_METHOD(OverlayConsumerCommitFixture,
                 "an updated spool is refiled on its lane before the next poll",
                 "[ams_edit_overlay][spoolman][1653]") {
    // A spool created by the save has never been fetched, so the lane still
    // carries the spool it was bound to before. The read follows the id the
    // commit just bound, not the one the editor opened on.
    seed_linked_spool(api, client, *backend);
    const helix::ams::LaneId lane = backend->lane_id(0);
    poll_lane_once(lane);

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    AmsEditOverlay::EditResult captured;
    bool fired = false;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api,
                                  [&](const AmsEditOverlay::EditResult& r) {
                                      fired = true;
                                      captured = r;
                                      commit_like_consumer(r);
                                  }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // A material move is an identity change, so Save asks whose spool this is.
    SlotInfo edited = access.working_info();
    edited.material = "PETG";
    access.set_working_info(edited);

    access.call_handle_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    REQUIRE_FALSE(ModalStack::instance().stack_empty()); // "Different filament?"
    lv_obj_t* dlg = ModalStack::instance().top_dialog();
    REQUIRE(dlg != nullptr);
    lv_obj_t* confirm_btn = lv_obj_find_by_name(dlg, "btn_primary");
    REQUIRE(confirm_btn != nullptr);
    lv_obj_send_event(confirm_btn, LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    REQUIRE(fired);
    // "Update in Spoolman" REPOINTS the spool at the matching filament: the
    // spool keeps its id and the lane's record now states the new material.
    REQUIRE(captured.slot_info.spoolman_id != 0);
    REQUIRE(captured.slot_info.spoolman_id == 7);

    UpdateQueue::instance().drain();
    process_lvgl(10);

    const auto record = helix::ams::lane_sources(lane).spoolman;
    REQUIRE(record.has_value());
    CHECK(record->spoolman_id == captured.slot_info.spoolman_id);
    CHECK(record->material == "PETG");

    get_printer_state().capabilities_state().set_spoolman_available(false); // restore clean slate
    UpdateQueue::instance().drain();
    process_lvgl(10);
}

TEST_CASE("AmsEditOverlay::save_is_disabled holds Save shut while a save is in flight",
          "[ams_edit_overlay][spool_edit]") {
    // The rule as a rule, with no editor around it: the async seam a live save
    // runs through is what makes the in-flight moment unobservable otherwise.
    using Access = AmsEditOverlayViewTestAccess;

    SECTION("the editor gates on the edit") {
        CHECK(Access::save_is_disabled(AmsEditOverlay::VIEW_EDITOR, false, false));
        CHECK_FALSE(Access::save_is_disabled(AmsEditOverlay::VIEW_EDITOR, false, true));
    }

    SECTION("a save in flight holds it shut on any view") {
        CHECK(Access::save_is_disabled(AmsEditOverlay::VIEW_EDITOR, true, true));
        CHECK(Access::save_is_disabled(AmsEditOverlay::VIEW_EDITOR, true, false));
    }
}

// ============================================================================
// Identity dropdown form: pure selection rules + option publishing
// ============================================================================

TEST_CASE("selection_from_slot splits family/variant/brand", "[ams_edit_overlay][dropdown]") {
    SlotInfo info;
    info.material = "PLA-CF";
    info.brand = "eSUN";
    info.catalog_id = "esun-pla-cf-1kg";
    const auto sel = AmsEditOverlay::selection_from_slot(info);
    CHECK(sel.family == "PLA");
    CHECK(sel.variant == "CF");
    CHECK(sel.brand == "eSUN");
    CHECK(sel.product_id == "esun-pla-cf-1kg");

    // Unknown values pass through verbatim so an untouched open+save
    // round-trips them (the extra selected option).
    info.material = "MysteryBlend";
    info.brand = "NoName Co";
    const auto unknown = AmsEditOverlay::selection_from_slot(info);
    CHECK(unknown.family == "MysteryBlend");
    CHECK(unknown.variant == "Basic");
    CHECK(unknown.brand == "NoName Co");

    // An empty material opens on the common case; an empty brand is Generic.
    info.material.clear();
    info.brand.clear();
    const auto blank = AmsEditOverlay::selection_from_slot(info);
    CHECK(blank.family == "PLA");
    CHECK(blank.brand == "Generic");
}

TEST_CASE_METHOD(LVGLUITestFixture, "apply_family_change pins the brand/variant reset rules",
                 "[ams_edit_overlay][dropdown]") {
    auto catalog = helix::printer::FilamentCatalog::load_full();

    AmsEditOverlay::SpoolEditSelection sel;
    sel.family = "PLA";
    sel.variant = "Silk";
    sel.brand = "Generic";
    sel.product_id = "generic-silk-pla";

    // A brand that makes the new family keeps the brand: pick one that makes
    // both PLA and PETG from the catalog itself, so the case cannot rot when
    // the catalog changes.
    const auto petg_makers = catalog.brands_in_family("PETG");
    const auto pla_makers = catalog.brands_in_family("PLA");
    std::string dual;
    for (const auto& b : petg_makers) {
        if (std::find(pla_makers.begin(), pla_makers.end(), b) != pla_makers.end() &&
            b != "Generic") {
            dual = b;
            break;
        }
    }
    REQUIRE_FALSE(dual.empty());
    AmsEditOverlay::SpoolEditSelection branded = sel;
    branded.brand = dual;
    const auto kept = AmsEditOverlay::apply_family_change(branded, "PETG", catalog);
    CHECK(kept.family == "PETG");
    CHECK(kept.variant == "Basic");
    CHECK(kept.brand == dual);
    CHECK(kept.product_id.empty());

    // A brand that does not make the new family falls back to Generic.
    AmsEditOverlay::SpoolEditSelection odd;
    odd.family = "PPA";
    odd.variant = "CF";
    odd.brand = "OnlyPPA Inc";
    const auto dropped = AmsEditOverlay::apply_family_change(odd, "PLA", catalog);
    CHECK(dropped.brand == "Generic");
    CHECK(dropped.variant == "Basic");
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "editor dropdowns publish families, variants, Generic-first brands",
                 "[ams_edit_overlay][dropdown]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    SlotInfo info = untracked_slot(); // PETG
    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Material dropdown: the slot's family is selected, the list carries the
    // families and ends in the browse entry.
    const std::string materials = access.dropdown_options("dropdown_material");
    CHECK(materials.find("PETG") != std::string::npos);
    CHECK(access.dropdown_selected("dropdown_material") == "PETG");
    const std::string browse = "Browse catalog...";
    CHECK(materials.size() > browse.size() + 1);
    CHECK(materials.compare(materials.size() - browse.size(), browse.size(), browse) == 0);

    // Brand dropdown: Generic leads and the browse entry trails.
    const std::string brands = access.dropdown_options("dropdown_brand");
    CHECK(brands.find("Generic\n") == 0);
    CHECK(access.dropdown_selected("dropdown_brand") == "Generic");

    close_editor_overlay();
}

namespace {
/// The option list's entries, in order.
std::vector<std::string> split_options(const std::string& options) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= options.size()) {
        const size_t nl = options.find('\n', at);
        out.push_back(options.substr(at, nl == std::string::npos ? nl : nl - at));
        if (nl == std::string::npos) {
            break;
        }
        at = nl + 1;
    }
    return out;
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "every identity dropdown stages its pick through value_changed",
                 "[ams_edit_overlay][dropdown][callback]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(access.save_disabled() == 1);

    // Material, through the stacked layout's dropdown.
    REQUIRE(access.pick_dropdown("dropdown_material", "PLA"));
    process_lvgl(5);
    CHECK(access.sel().family == "PLA");
    CHECK(access.save_disabled() == 0);

    // Variant, through the split layout's twin: both layouts are wired.
    const auto variants = split_options(access.dropdown_options("dropdown_variant_split"));
    REQUIRE(variants.size() > 1);
    REQUIRE(access.pick_dropdown("dropdown_variant_split", variants[1]));
    process_lvgl(5);
    CHECK(access.sel().variant == variants[1]);

    // Brand: the first maker after Generic, which reveals the product row.
    const auto brands = split_options(access.dropdown_options("dropdown_brand"));
    REQUIRE(brands.size() > 2);
    REQUIRE(brands[0] == "Generic");
    REQUIRE(access.pick_dropdown("dropdown_brand", brands[1]));
    process_lvgl(5);
    CHECK(access.sel().brand == brands[1]);

    // Product: any line that brand makes for the family.
    const auto products = split_options(access.dropdown_options("dropdown_product"));
    if (!products.empty() && !products[0].empty()) {
        REQUIRE(access.pick_dropdown("dropdown_product", products[0]));
        process_lvgl(5);
        CHECK_FALSE(access.sel().product_id.empty());
    }

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "unknown material shows as its own selected family option",
                 "[ams_edit_overlay][dropdown]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    SlotInfo info = untracked_slot();
    info.material = "MysteryBlend";
    info.brand = "NoName Co";
    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // The catalog does not know the family, so it is the dropdown's selected
    // entry and
    // an untouched open+save changes nothing (selection not moved).
    CHECK(access.dropdown_selected("dropdown_material") == "MysteryBlend");
    CHECK(access.sel().family == "MysteryBlend");
    CHECK(access.sel().brand == "NoName Co");
    CHECK_FALSE(access.is_dirty());
    access.call_handle_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    // Round-tripped: the commit carried the unknown values verbatim.
    CHECK(access.working_info().material == "MysteryBlend");
    CHECK(access.working_info().brand == "NoName Co");
}

TEST_CASE_METHOD(LVGLUITestFixture, "weight pill keypad stages remaining grams",
                 "[ams_edit_overlay][weight]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    SlotInfo info = untracked_slot();
    info.remaining_weight_g = 750;
    info.total_weight_g = 1000;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_apply_weight(600);
    UpdateQueue::instance().drain();
    CHECK(access.working_info().remaining_weight_g == 600);
    CHECK(access.working_info().total_weight_g == 1000); // known total untouched
    CHECK(access.is_dirty());
    close_editor_overlay();

    // A weightless slot: the same number is the honest first total.
    SlotInfo bare = untracked_slot();
    bare.remaining_weight_g = -1;
    bare.total_weight_g = -1;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, bare, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    access.call_apply_weight(750);
    UpdateQueue::instance().drain();
    CHECK(access.working_info().remaining_weight_g == 750);
    CHECK(access.working_info().total_weight_g == 750);
    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "Reset to tag restores identity, not just the colour",
                 "[ams_edit_overlay][dropdown][tag]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    // The slot opens DRIFTED from the tag (the values firmware last showed);
    // only a working reset can bring the tag's values back.
    helix::ams::TagReading tag;
    tag.material = "PETG";
    tag.brand = "Polymaker";
    tag.color_rgb = 0x112233;
    tag.total_weight_g = 1000;

    SlotInfo info = untracked_slot();
    info.material = "PLA";
    info.brand = "eSUN";
    info.color_rgb = 0xFF6600;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    access.set_tag(tag);

    // The user moves the selection away, then resets.
    AmsEditOverlay::SpoolEditSelection moved = access.sel();
    moved.family = "ABS";
    access.set_sel(moved);
    access.call_handle_reset_to_tag();
    UpdateQueue::instance().drain();

    CHECK(access.working_info().material == "PETG");
    CHECK(access.working_info().brand == "Polymaker");
    CHECK(access.working_info().color_rgb == 0x112233);
    CHECK(access.working_info().total_weight_g == 1000);
    CHECK(access.sel().family == "PETG");
    CHECK(access.sel().brand == "Polymaker");

    // The reset moved the slot off its opened-on values, so Save is live.
    CHECK(access.is_dirty());

    close_editor_overlay();

    // The mirror case: a slot that OPENED on the tag's values resets clean.
    SlotInfo aligned = untracked_slot();
    aligned.material = *tag.material;
    aligned.brand = *tag.brand;
    aligned.color_rgb = *tag.color_rgb;
    aligned.total_weight_g = *tag.total_weight_g;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, aligned, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    access.set_tag(tag);
    AmsEditOverlay::SpoolEditSelection drift = access.sel();
    drift.family = "ABS";
    access.set_sel(drift);
    access.call_handle_reset_to_tag();
    UpdateQueue::instance().drain();
    CHECK(access.working_info().material == "PETG");
    CHECK_FALSE(access.is_dirty());

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "a dropdown pick refreshes the temperature line",
                 "[ams_edit_overlay][dropdown][r10]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    SlotInfo info = untracked_slot(); // PETG opens with PETG's range
    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    auto* temps = lv_xml_get_subject(nullptr, "ams_edit_temp_text");
    REQUIRE(temps != nullptr);
    const std::string petg_range = lv_subject_get_string(temps);
    REQUIRE(petg_range.find("PETG") == std::string::npos); // range text, not name
    CHECK(!petg_range.empty());

    // Move the family to PLA through the dropdown path; the temps line must
    // follow the pick immediately, not only after Save.
    AmsEditOverlay::SpoolEditSelection sel = access.sel();
    REQUIRE(sel.family == "PETG");
    REQUIRE(access.pick_dropdown("dropdown_material", "PLA"));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(access.sel().family == "PLA");
    CHECK(lv_subject_get_string(temps) != petg_range);

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "a More-section edit lights Save and stages on it",
                 "[ams_edit_overlay][more]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    SlotInfo info = untracked_slot();
    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE_FALSE(access.is_dirty());

    // A Notes-only edit is a real edit: dirty, Save lit, and it survives
    // Save's staging.
    REQUIRE(access.save_disabled() == 1);
    access.set_detail_field("detail_field_comment", "keep dry");
    UpdateQueue::instance().drain();
    CHECK(access.is_dirty());
    CHECK(access.save_disabled() == 0);

    access.call_handle_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(access.detail_comment() == "keep dry");

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "a picker relink reseeds the More section to the new spool",
                 "[ams_edit_overlay][more][relink]") {
    // The More fields must describe the spool the slot NOW names, and the
    // Save PATCH must target that spool's record, never the one the editor
    // opened on.
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SpoolInfo spool_b;
    spool_b.id = 22;
    spool_b.filament_id = 9;
    spool_b.vendor = "eSUN";
    spool_b.material = "PETG";
    spool_b.color_hex = "FF6600";
    spool_b.location = "Shelf B";
    client.spoolman_mock().get_mock_spools().push_back(spool_b);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);
    get_printer_state().capabilities_state().set_spoolman_available(true);
    UpdateQueue::instance().drain();

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Relink to spool B through the picker path.
    access.set_cached_spools({spool_b});
    access.call_handle_spool_selected(22);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // The More section now carries B's identity, not A's.
    CHECK(access.detail_id() == 22);
    CHECK(access.working_info().spoolman_id == 22);

    // A Notes edit on B saves against B's record.
    access.set_detail_field("detail_field_comment", "on B");
    UpdateQueue::instance().drain();
    access.call_handle_save();
    UpdateQueue::instance().drain();
    process_lvgl(20);
    UpdateQueue::instance().drain();
    process_lvgl(20);

    bool patched_b = false;
    for (const auto& upd : client.spoolman_mock().spool_updates) {
        if (upd.spool_id == 22) {
            patched_b = true;
        }
    }
    CHECK(patched_b);

    close_editor_overlay();
    get_printer_state().capabilities_state().set_spoolman_available(false);
    UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(LVGLUITestFixture, "a combined material+notes save loses neither",
                 "[ams_edit_overlay][more][combined]") {
    // The identity prompt owns the save, and the Notes edit rides the
    // Update arm instead of being dropped.
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SpoolInfo linked;
    linked.id = 7;
    linked.filament_id = 3;
    linked.vendor = "Bambu Lab";
    linked.material = "ASA";
    linked.color_hex = "8A949E";
    client.spoolman_mock().get_mock_spools().push_back(linked);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);
    get_printer_state().capabilities_state().set_spoolman_available(true);
    UpdateQueue::instance().drain();

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    bool fired = false;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api,
                                  [&](const AmsEditOverlay::EditResult& r) { fired = r.saved; }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Move the material AND edit the notes in one save.
    SlotInfo edited = access.working_info();
    edited.material = "PETG";
    access.set_working_info(edited);
    access.set_detail_field("detail_field_comment", "combined edit");
    UpdateQueue::instance().drain();

    access.call_handle_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE_FALSE(ModalStack::instance().stack_empty()); // Update / Unlink prompt

    // "Update in Spoolman" is the primary button.
    lv_obj_t* dlg = ModalStack::instance().top_dialog();
    REQUIRE(dlg != nullptr);
    lv_obj_t* confirm = lv_obj_find_by_name(dlg, "btn_primary");
    REQUIRE(confirm != nullptr);
    lv_obj_send_event(confirm, LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    process_lvgl(30);
    UpdateQueue::instance().drain();
    process_lvgl(30);

    CHECK(fired);
    // The notes reached the spool record alongside the identity move.
    bool notes_reached_spool = false;
    for (const auto& upd : client.spoolman_mock().spool_updates) {
        if (upd.patch.dump().find("combined edit") != std::string::npos) {
            notes_reached_spool = true;
        }
    }
    CHECK(notes_reached_spool);

    if (!fired) {
        close_editor_overlay();
    }
    get_printer_state().capabilities_state().set_spoolman_available(false);
    UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(LVGLUITestFixture, "a linked slot's family change refreshes the temps line",
                 "[ams_edit_overlay][r10][linked]") {
    // A linked slot carries its spool's stored range; a family pick must
    // show the picked family's range, not the stored one.
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SpoolInfo linked;
    linked.id = 7;
    linked.filament_id = 3;
    linked.vendor = "Bambu Lab";
    linked.material = "ASA"; // ASA prints far hotter than PLA
    linked.color_hex = "8A949E";
    client.spoolman_mock().get_mock_spools().push_back(linked);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);
    get_printer_state().capabilities_state().set_spoolman_available(true);
    UpdateQueue::instance().drain();

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    // The linked spool's OWN range, as a fetched record leaves it: hot ASA
    // numbers that a PLA pick must not keep showing.
    SlotInfo info = tracked_slot();
    info.material = "ASA";
    info.nozzle_temp_min = 260;
    info.nozzle_temp_max = 290;
    info.bed_temp = 100;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, &api, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    auto* temps = lv_xml_get_subject(nullptr, "ams_edit_temp_text");
    REQUIRE(temps != nullptr);
    const std::string asa_range = lv_subject_get_string(temps); // ASA's stored range
    REQUIRE_FALSE(asa_range.empty());

    // Move the family to PLA through the dropdown; PLA's cooler range must
    // replace ASA's immediately.
    REQUIRE(access.pick_dropdown("dropdown_material", "PLA"));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    const std::string pla_range = lv_subject_get_string(temps);
    CHECK_FALSE(pla_range.empty()); // a range, not a blanked line
    CHECK(pla_range != asa_range);

    close_editor_overlay();
    get_printer_state().capabilities_state().set_spoolman_available(false);
    UpdateQueue::instance().drain();
}

namespace {
/// The mock's record for @p id, added when the inventory lacks one (a GET
/// serves the first match, so an existing record is edited, not shadowed).
SpoolInfo& mock_spool(MoonrakerClientMock& client, int id) {
    auto& spools = client.spoolman_mock().get_mock_spools();
    auto it =
        std::find_if(spools.begin(), spools.end(), [id](const SpoolInfo& s) { return s.id == id; });
    if (it == spools.end()) {
        SpoolInfo added;
        added.id = id;
        spools.push_back(added);
        return spools.back();
    }
    return *it;
}

void set_spoolman_available(bool available) {
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, available ? 1 : 0);
    get_printer_state().capabilities_state().set_spoolman_available(available);
    UpdateQueue::instance().drain();
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "a relink never carries the old spool's More fields over",
                 "[ams_edit_overlay][more][relink]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SpoolInfo& a = mock_spool(client, 7);
    a.filament_id = 3;
    a.vendor = "Bambu Lab";
    a.material = "ASA";
    a.color_hex = "8A949E";
    a.location = "Dry box 1";
    a.comment = "opened 5/3";
    SpoolInfo& b = mock_spool(client, 22);
    b.filament_id = 9;
    b.vendor = "eSUN";
    b.material = "PETG";
    b.color_hex = "FF6600";
    b.location = "Shelf B";
    b.comment = "";
    const SpoolInfo spool_b = b;
    set_spoolman_available(true);

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(access.field_text("detail_field_location") == "Dry box 1");

    access.set_cached_spools({spool_b});
    access.call_handle_spool_selected(22);
    // Before B's record lands the fields are blank, not A's.
    CHECK(access.field_text("detail_field_location").empty());
    CHECK(access.field_text("detail_field_comment").empty());
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(access.field_text("detail_field_location") == "Shelf B");

    access.call_handle_save();
    UpdateQueue::instance().drain();
    process_lvgl(20);
    UpdateQueue::instance().drain();

    for (const auto& upd : client.spoolman_mock().spool_updates) {
        const std::string body = upd.patch.dump();
        CHECK(body.find("Dry box 1") == std::string::npos);
        CHECK(body.find("opened 5/3") == std::string::npos);
    }

    if (overlay.get_root() && NavigationManager::instance().is_panel_in_stack(overlay.get_root())) {
        close_editor_overlay();
    }
    set_spoolman_available(false);
}

TEST_CASE_METHOD(LVGLUITestFixture, "a late spool record never overwrites a relink or a pick",
                 "[ams_edit_overlay][more][relink]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    SpoolInfo& a = mock_spool(client, 7);
    a.vendor = "Bambu Lab";
    a.material = "ASA";
    a.location = "Dry box 1";
    set_spoolman_available(true);

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api, nullptr));
    // The open-time fetch is queued; the user picks before it lands.
    REQUIRE(access.pick_dropdown("dropdown_material", "PLA"));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(access.sel().family == "PLA"); // the record did not reset the pick
    CHECK(access.field_text("detail_field_location") == "Dry box 1");
    close_editor_overlay();

    // A relink made before A's record lands keeps B's identity.
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api, nullptr));
    SpoolInfo other;
    other.id = 31;
    other.vendor = "Sunlu";
    other.material = "PETG";
    access.set_cached_spools({other});
    access.call_handle_spool_selected(31);
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(access.working_info().spoolman_id == 31);
    CHECK(access.working_info().brand == "Sunlu");
    CHECK(access.field_text("detail_field_location") != "Dry box 1");
    close_editor_overlay();
    set_spoolman_available(false);
}

TEST_CASE_METHOD(LVGLUITestFixture, "picker-entry then Back builds the editor from this slot alone",
                 "[ams_edit_overlay][picker][session]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    // A previous session on a tag-read ASA slot.
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    helix::ams::TagReading tag;
    tag.material = "ASA";
    tag.brand = "Bambu Lab";
    access.set_tag(tag);
    close_editor_overlay();

    // Picker-entry on an untracked PETG slot, then Back to the editor.
    REQUIRE(overlay.show_for_slot(test_screen(), 1, untracked_slot(), nullptr, nullptr, true));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(access.view() == AmsEditOverlay::VIEW_SPOOL_PICKER);
    CHECK_FALSE(access.has_tag());
    access.call_handle_back();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK(access.view() == AmsEditOverlay::VIEW_EDITOR);
    CHECK(access.sel().family == "PETG");
    CHECK(access.sel().brand == "Generic");
    CHECK(access.dropdown_selected("dropdown_material") == "PETG");
    CHECK(access.dropdown_selected("dropdown_brand") == "Generic");
    auto* reset_visible = lv_xml_get_subject(nullptr, "ams_edit_reset_tag_visible");
    REQUIRE(reset_visible != nullptr);
    CHECK(lv_subject_get_int(reset_visible) == 0);
    CHECK_FALSE(access.is_dirty());

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "leaving the picker keeps the edits staged in the editor",
                 "[ams_edit_overlay][picker][session]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    REQUIRE(access.pick_dropdown("dropdown_material", "PLA"));
    access.set_detail_field("detail_field_comment", "keep dry");
    access.call_switch_to_picker();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // "Not in your spools? Set it up" returns to the editor, edits intact.
    access.click("picker_setup_entry");
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(access.view() == AmsEditOverlay::VIEW_EDITOR);
    CHECK(access.sel().family == "PLA");
    CHECK(access.detail_comment() == "keep dry");
    CHECK(access.is_dirty());

    access.set_detail_field("detail_field_comment", "");
    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "a save in flight holds Back and the keypad shut",
                 "[ams_edit_overlay][save][session]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    bool fired = false;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr,
                                  [&](const AmsEditOverlay::EditResult&) { fired = true; }));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    lv_obj_t* root = overlay.get_root();

    access.set_save_in_flight(true);
    access.call_handle_back();
    access.call_handle_weight_tap();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK_FALSE(fired);
    CHECK(NavigationManager::instance().is_panel_on_top(root));
    CHECK(access.save_disabled() == 1);
    close_editor_overlay();

    // The next session starts with Save governed by its own edits alone.
    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    access.call_apply_weight(321);
    CHECK(access.save_disabled() == 0);
    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "a Spoolman save completes even if the editor was covered",
                 "[ams_edit_overlay][save][session]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    SpoolInfo& a = mock_spool(client, 7);
    a.filament_id = 3;
    a.vendor = "Bambu Lab";
    a.material = "ASA";
    a.color_hex = "8A949E";
    a.comment = "";
    set_spoolman_available(true);

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    bool saved = false;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api,
                                  [&](const AmsEditOverlay::EditResult& r) { saved = r.saved; }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.set_detail_field("detail_field_comment", "covered");
    access.call_handle_save();
    access.drop_screen_lifetime(); // covered before the PATCH answers
    UpdateQueue::instance().drain();
    process_lvgl(20);
    UpdateQueue::instance().drain();
    process_lvgl(20);

    CHECK(saved);
    if (!saved) {
        close_editor_overlay();
    }
    set_spoolman_available(false);
}

TEST_CASE_METHOD(LVGLUITestFixture, "an untracked slot on a Spoolman printer shows the Add link",
                 "[ams_edit_overlay][more][add_link]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    set_spoolman_available(true);
    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(access.shown("add_to_spoolman_link"));
    CHECK_FALSE(access.shown("more_row")); // the logistics fields are Spoolman's
    close_editor_overlay();

    // Linked: the toggle shows and the link does not.
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK_FALSE(access.shown("add_to_spoolman_link"));
    CHECK(access.shown("more_row"));
    close_editor_overlay();

    // No Spoolman: neither.
    set_spoolman_available(false);
    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK_FALSE(access.shown("add_to_spoolman_link"));
    CHECK_FALSE(access.shown("more_bar"));
    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "a price the field rounds does not make the editor dirty",
                 "[ams_edit_overlay][more][price]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    SpoolInfo fetched;
    fetched.id = 7;
    fetched.price = 19.995; // shown as 20.00 (or 19.99)
    fetched.location = "Shelf A";
    access.seed_detail_fetch(fetched);
    UpdateQueue::instance().drain();
    CHECK_FALSE(access.is_dirty());
    CHECK(access.save_disabled() == 1);

    // A real edit of the price still counts.
    access.set_detail_field("detail_field_price", "21.50");
    CHECK(access.is_dirty());
    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "the brand list names Generic once",
                 "[ams_edit_overlay][dropdown]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    SlotInfo info = untracked_slot();
    info.material = "PLA";
    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    const auto brands = split_options(access.dropdown_options("dropdown_brand"));
    CHECK(std::count(brands.begin(), brands.end(), std::string("Generic")) == 1);
    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "a variant the catalog lacks shows as the selected entry",
                 "[ams_edit_overlay][dropdown]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    SlotInfo info = untracked_slot();
    info.material = "PLA-Zebra";
    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    REQUIRE(access.sel().family == "PLA");
    REQUIRE(access.sel().variant == "Zebra");
    CHECK(access.dropdown_selected("dropdown_variant") == "Zebra");
    CHECK_FALSE(access.is_dirty());
    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Browse catalog leaves the row intact and a pick sets the temps",
                 "[ams_edit_overlay][dropdown][browse]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    SlotInfo info = untracked_slot(); // PETG, Generic
    info.nozzle_temp_min = 199;
    info.nozzle_temp_max = 201;
    info.bed_temp = 33;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    auto* temps = lv_xml_get_subject(nullptr, "ams_edit_temp_text");
    REQUIRE(temps != nullptr);
    const std::string stored = lv_subject_get_string(temps);
    REQUIRE(stored.find("199") != std::string::npos);

    REQUIRE(access.pick_dropdown("dropdown_material", "Browse catalog..."));
    process_lvgl(10);
    // The sheet is up and the row still names the selection, not the entry.
    lv_obj_t* dlg = ModalStack::instance().top_dialog();
    REQUIRE(dlg != nullptr);
    CHECK(access.dropdown_selected("dropdown_material") == "PETG");

    // Pick the first product row the sheet lists, then Select.
    lv_obj_t* list = lv_obj_find_by_name(dlg, "product_list");
    REQUIRE(list != nullptr);
    const auto catalog = helix::printer::FilamentCatalog::load_full();
    lv_obj_t* row = nullptr;
    for (uint32_t i = 0; i < lv_obj_get_child_count(list); ++i) {
        lv_obj_t* child = lv_obj_get_child(list, static_cast<int32_t>(i));
        const char* name = lv_obj_get_name(child);
        if (name && catalog.resolve_id(name)) {
            row = child;
            break;
        }
    }
    REQUIRE(row != nullptr);
    lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* select = lv_obj_find_by_name(dlg, "btn_primary");
    REQUIRE(select != nullptr);
    lv_obj_send_event(select, LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK_FALSE(access.sel().product_id.empty());
    CHECK(std::string(lv_subject_get_string(temps)).find("199") == std::string::npos);
    close_editor_overlay();
}

namespace {
int subject_int(const char* name) {
    auto* subj = lv_xml_get_subject(nullptr, name);
    REQUIRE(subj != nullptr);
    return lv_subject_get_int(subj);
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "each field the tag states carries its own badge",
                 "[ams_edit_overlay][tag]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    SlotInfo info = untracked_slot();
    info.material = "PETG";
    info.brand = "Polymaker";
    info.nozzle_temp_min = 230;
    info.nozzle_temp_max = 250;
    info.bed_temp = 80;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, info, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    helix::ams::TagReading tag;
    tag.material = "PETG";
    tag.brand = "Polymaker";
    tag.color_rgb = 0x112233; // the slot shows another colour
    tag.nozzle_temp_min = 230;
    tag.nozzle_temp_max = 250;
    tag.bed_temp = 80;
    access.set_tag(tag);
    access.call_update_tag_badges();
    CHECK(subject_int("ams_edit_tag_badge_visible") == 1);
    CHECK(subject_int("ams_edit_tag_material") == 1);
    CHECK(subject_int("ams_edit_tag_brand") == 1);
    CHECK(subject_int("ams_edit_tag_temps") == 1);
    CHECK(subject_int("ams_edit_tag_color") == 0);
    CHECK(subject_int("ams_edit_tag_product") == 0);

    // Moving the material off the tag's drops that badge (and the temps it
    // brought) and keeps the brand's.
    REQUIRE(access.pick_dropdown("dropdown_material", "PLA"));
    process_lvgl(5);
    CHECK(subject_int("ams_edit_tag_material") == 0);
    CHECK(subject_int("ams_edit_tag_temps") == 0);
    CHECK(subject_int("ams_edit_tag_brand") == 1);
    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "the editor's non-swatch targets reach 44px",
                 "[ams_edit_overlay][touch]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    auto check_44 = [&](const char* name) {
        INFO(name);
        REQUIRE(access.shown(name));
        const lv_area_t a = access.click_area(name);
        CHECK(lv_area_get_width(&a) >= 44);
        CHECK(lv_area_get_height(&a) >= 44);
    };

    set_spoolman_available(true);
    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    auto* reset_visible = lv_xml_get_subject(nullptr, "ams_edit_reset_tag_visible");
    REQUIRE(reset_visible != nullptr);
    lv_subject_set_int(reset_visible, 1);
    process_lvgl(5);
    check_44("btn_custom_color");
    check_44("weight_pill");
    check_44("add_to_spoolman_link");
    check_44("btn_reset_tag");
    lv_subject_set_int(reset_visible, 0);
    close_editor_overlay();

    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    check_44("more_row");
    close_editor_overlay();
    set_spoolman_available(false);
}
