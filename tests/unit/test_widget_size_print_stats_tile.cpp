// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_print_stats_tile.cpp
 * @brief The print stats tile draws what its ladder promises at each rung: the published
 * verdict and texts, which parts carry HIDDEN, and when the lifetime line appears.
 *
 * The decision is print_stats_tile_layout.h's and the sums are print_activity.h's; these
 * tests pin their wiring to the real fonts, XML and history manager.
 *
 * Run with: ./build/bin/helix-tests "[widget_size][print_stats]"
 */

#include "ui_nav_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/print_history_manager_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "locale_formats.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "moonraker_history_api.h"
#include "panel_widget_manager.h"
#include "print_activity.h"
#include "print_history_manager.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/print_stats_widget.h"
#include "system_settings_manager.h"

#include <array>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

struct Box {
    int w;
    int h;
    SizeRung rung;
};

// Just above the smallest box each rung needs with the fixture's fonts (heading 34px,
// small 22px) and the tile's 3px padding. The Strip is the medium tier's 2x1 cell.
constexpr Box kStrip{114, 54, SizeRung::Strip};
constexpr Box kCell{114, 142, SizeRung::Cell};
constexpr Box kWide{236, 95, SizeRung::Wide};
constexpr Box kFull{236, 182, SizeRung::Full};

/// Answers lifetime totals from the test when it holds some, else from the mock.
class TotalsHistoryAPI : public MoonrakerHistoryAPI {
  public:
    explicit TotalsHistoryAPI(helix::IMoonrakerClient& client) : MoonrakerHistoryAPI(client) {}

    void get_history_totals(HistoryTotalsCallback on_success, ErrorCallback on_error) override {
        if (totals) {
            on_success(*totals);
            return;
        }
        MoonrakerHistoryAPI::get_history_totals(std::move(on_success), std::move(on_error));
    }

    std::optional<PrintHistoryTotals> totals;
};

class TotalsMoonrakerAPI : public MoonrakerAPI {
  public:
    TotalsMoonrakerAPI(helix::IMoonrakerClient& client, helix::PrinterState& state)
        : MoonrakerAPI(client, state) {
        history_api_ = std::make_unique<TotalsHistoryAPI>(client);
    }

    TotalsHistoryAPI& totals_api() {
        return static_cast<TotalsHistoryAPI&>(*history_api_);
    }
};

struct PrintStatsTileFixture : public LVGLUITestFixture {
    PrintStatsTileFixture() : client(MoonrakerClientMock::PrinterType::VORON_24, 1000.0) {
        client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<TotalsMoonrakerAPI>(client, state());
        manager = std::make_unique<PrintHistoryManager>(api.get(), &client);
        set_moonraker_api(api.get());
        set_print_history_manager(manager.get());
        PanelWidgetManager::instance().init_widget_subjects();
        // The staleness watcher is queued with the socket down and stales any cache it
        // finds when it runs, so drain it and bring the socket up first.
        settle();
        lv_subject_set_int(state().network_state().get_printer_connection_state_subject(),
                           static_cast<int>(ConnectionState::CONNECTED));
        settle();
    }
    ~PrintStatsTileFixture() override {
        SystemSettingsManager::instance().set_language("en");
        drain();
        set_print_history_manager(nullptr);
        set_moonraker_api(nullptr);
        manager.reset();
        api.reset();
        client.disconnect();
    }

    static void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
    void settle() {
        drain();
        process_lvgl(30);
    }

    /// The totals the mock reports, read the way the tile reads them.
    PrintHistoryTotals mock_totals() {
        std::optional<PrintHistoryTotals> got;
        api->history().get_history_totals([&](const PrintHistoryTotals& t) { got = t; },
                                          [](const MoonrakerError&) {});
        REQUIRE(got.has_value());
        return *got;
    }

    /// Install @p jobs as the whole cached history.
    void seed(std::vector<PrintHistoryJob> jobs) {
        PrintHistoryManagerTestAccess::set_loaded_jobs(
            *manager, std::move(jobs), HistoryScope::RECENT, PrintHistoryManager::kRecentJobLimit);
        settle();
    }

    /// A job that finished @p days_ago local days back, ten seconds after that day began.
    static PrintHistoryJob job(int days_ago, double print_seconds, double total_seconds = 0.0) {
        const ActivityDayStarts starts = local_day_starts(std::time(nullptr));
        PrintHistoryJob j;
        j.job_id = "job-" + std::to_string(days_ago) + "-" + std::to_string(print_seconds);
        j.filename = "a.gcode";
        j.status = PrintJobStatus::COMPLETED;
        j.start_time = starts[static_cast<size_t>(kActivityDays - 1 - days_ago)] + 10.0;
        j.end_time = j.start_time + print_seconds;
        j.print_duration = print_seconds;
        j.total_duration = total_seconds > 0 ? total_seconds : print_seconds;
        return j;
    }

    MoonrakerClientMock client;
    std::unique_ptr<TotalsMoonrakerAPI> api;
    std::unique_ptr<PrintHistoryManager> manager;
};

int subject_int(const char* name) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
    REQUIRE(subject != nullptr);
    return lv_subject_get_int(subject);
}

std::string subject_text(const char* name) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
    REQUIRE(subject != nullptr);
    return lv_subject_get_string(subject);
}

/// Whether an object draws: it and every ancestor up to the widget root is unhidden.
bool shown(PanelWidgetHarness<PrintStatsWidget>& h, const char* name) {
    lv_obj_t* obj = h.child(name);
    REQUIRE(obj != nullptr);
    for (lv_obj_t* o = obj; o != nullptr; o = lv_obj_get_parent(o)) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN))
            return false;
    }
    return true;
}

} // namespace

TEST_CASE_METHOD(PrintStatsTileFixture, "print stats tile: medium-tier budgets land on their rung",
                 "[widget_size][print_stats]") {
    seed({job(0, 3600)});
    PanelWidgetHarness<PrintStatsWidget> h(test_screen());
    for (const Box& b : {kStrip, kCell, kWide, kFull}) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        CHECK(subject_int("print_stats_rung") == static_cast<int>(b.rung));
    }
}

TEST_CASE_METHOD(PrintStatsTileFixture,
                 "print stats tile: the hero sums print_duration over the seven bars",
                 "[widget_size][print_stats]") {
    seed({job(0, 3600, 99999), job(0, 5400, 99999)});
    PanelWidgetHarness<PrintStatsWidget> h(test_screen());
    h.resize(1, 1, kFull.w, kFull.h);
    CHECK(subject_text("print_stats_hero") == format_print_hours(2.5));
    CHECK(subject_text("print_stats_hero_label") == "this week");
    CHECK(subject_int("print_stats_day_6") == 100);
    CHECK(subject_int("print_stats_day_5") == 0);
    CHECK(shown(h, "ps_bars"));
}

TEST_CASE_METHOD(PrintStatsTileFixture,
                 "print stats tile: no prints this week keeps the lifetime line",
                 "[widget_size][print_stats]") {
    seed({job(10, 3600)});
    PanelWidgetHarness<PrintStatsWidget> h(test_screen());
    h.resize(1, 1, kWide.w, kWide.h);
    h.widget().on_activate();
    settle();
    CHECK(subject_text("print_stats_hero") == "0 h");
    CHECK(subject_text("print_stats_hero_label") == "No prints this week");
    CHECK_FALSE(subject_text("print_stats_support_text").empty());
    CHECK(shown(h, "ps_support"));
}

TEST_CASE_METHOD(PrintStatsTileFixture, "print stats tile: no history at all",
                 "[widget_size][print_stats]") {
    client.fail_next("server.history.totals");
    seed({});
    PanelWidgetHarness<PrintStatsWidget> h(test_screen());
    h.resize(1, 1, kFull.w, kFull.h);
    h.widget().on_activate();
    settle();
    CHECK(subject_int("print_stats_content") == static_cast<int>(PrintStatsContent::NoHistory));
    CHECK(subject_text("print_stats_hero_label") == "No prints yet");
    CHECK_FALSE(shown(h, "ps_hero"));
    CHECK_FALSE(shown(h, "ps_bars"));
    CHECK_FALSE(shown(h, "ps_support"));
}

TEST_CASE_METHOD(PrintStatsTileFixture,
                 "print stats tile: the lifetime line appears when totals land after the week",
                 "[widget_size][print_stats]") {
    const PrintHistoryTotals totals = mock_totals();
    seed({job(0, 3600)});
    client.defer_next("server.history.totals");
    PanelWidgetHarness<PrintStatsWidget> h(test_screen());
    h.resize(1, 1, kWide.w, kWide.h);
    h.widget().on_activate();
    settle();
    CHECK(subject_text("print_stats_hero") == format_print_hours(1.0));
    CHECK(subject_text("print_stats_support_text").empty());
    CHECK_FALSE(shown(h, "ps_support"));
    const int32_t width_before = lv_obj_get_width(h.root());
    const int32_t height_before = lv_obj_get_height(h.root());

    client.fire_deferred("server.history.totals");
    settle();
    CHECK(subject_text("print_stats_support_text") ==
          format_lifetime_line({totals.total_jobs, static_cast<double>(totals.total_time) / 3600.0,
                                totals.total_filament_used},
                               true));
    CHECK(shown(h, "ps_support"));
    CHECK(subject_int("print_stats_rung") == static_cast<int>(SizeRung::Wide));
    CHECK(lv_obj_get_width(h.root()) == width_before);
    CHECK(lv_obj_get_height(h.root()) == height_before);
}

TEST_CASE_METHOD(PrintStatsTileFixture,
                 "print stats tile: the lifetime line never comes from the job cache",
                 "[widget_size][print_stats]") {
    client.fail_next("server.history.totals");
    seed({job(0, 3600), job(1, 3600), job(2, 3600)});
    PanelWidgetHarness<PrintStatsWidget> h(test_screen());
    h.widget().on_activate();
    for (const Box& b : {kCell, kWide}) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        settle();
        CHECK(subject_text("print_stats_support_text").empty());
        CHECK_FALSE(shown(h, "ps_support"));
    }
    CHECK(subject_text("print_stats_hero") == format_print_hours(3.0));
}

TEST_CASE_METHOD(PrintStatsTileFixture, "print stats tile: filament shows from Wide, in meters",
                 "[widget_size][print_stats]") {
    PrintHistoryTotals totals;
    totals.total_jobs = 128;
    totals.total_time = 412 * 3600;
    totals.total_filament_used = 412300.0;
    api->totals_api().totals = totals;
    seed({job(0, 3600)});
    PanelWidgetHarness<PrintStatsWidget> h(test_screen());
    h.resize(1, 1, kWide.w, kWide.h);
    h.widget().on_activate();
    settle();
    const std::string wide_text = subject_text("print_stats_support_text");
    const std::string suffix = " · 412 m";
    REQUIRE(wide_text.size() > suffix.size());
    CHECK(wide_text.substr(wide_text.size() - suffix.size()) == suffix);
    h.resize(1, 1, kCell.w, kCell.h);
    CHECK(subject_text("print_stats_support_text") == format_print_count(128));
}

TEST_CASE_METHOD(PrintStatsTileFixture, "print stats tile: day labels only at Full",
                 "[widget_size][print_stats]") {
    seed({job(0, 3600)});
    PanelWidgetHarness<PrintStatsWidget> h(test_screen());
    h.resize(1, 1, kWide.w, kWide.h);
    CHECK(subject_int("print_stats_show_day_labels") == 0);
    CHECK_FALSE(shown(h, "ps_day_label_6"));
    h.resize(1, 1, kFull.w, kFull.h);
    CHECK(subject_int("print_stats_show_day_labels") == 1);
    CHECK(shown(h, "ps_day_label_6"));

    std::tm today{};
    const std::time_t now = std::time(nullptr);
    localtime_r(&now, &today);
    static const char* const kInitials[] = {"S", "M", "T", "W", "T", "F", "S"};
    CHECK(subject_text("print_stats_day_label_6") == kInitials[today.tm_wday]);
}

TEST_CASE_METHOD(PrintStatsTileFixture, "print stats tile: tap opens the history dashboard",
                 "[widget_size][print_stats]") {
    helix::ui::destroy_static_panels();
    std::array<lv_obj_t*, UI_PANEL_COUNT> panels{};
    for (auto& p : panels)
        p = lv_obj_create(test_screen());
    NavigationManager::instance().set_panels(panels.data());

    seed({job(0, 3600)});
    {
        PanelWidgetHarness<PrintStatsWidget> h(test_screen());
        h.resize(1, 1, kWide.w, kWide.h);
        REQUIRE(NavigationManager::instance().overlay_stack_names().empty());
        lv_obj_send_event(h.root(), LV_EVENT_CLICKED, nullptr);
        drain();
        CHECK(NavigationManager::instance().overlay_stack_names().size() == 1);
    }
    drain();
    NavigationManager::instance().shutdown();
    helix::ui::destroy_static_panels();
}

TEST_CASE_METHOD(PrintStatsTileFixture, "print stats tile: a language switch recomposes the label",
                 "[widget_size][print_stats][i18n]") {
    seed({job(0, 3600)});
    PanelWidgetHarness<PrintStatsWidget> h(test_screen());
    h.resize(1, 1, kWide.w, kWide.h);
    REQUIRE(subject_text("print_stats_hero_label") == "this week");

    SystemSettingsManager::instance().set_language("de");
    settle();
    const std::string german = lv_tr("this week");
    CHECK(german != "this week");
    CHECK(subject_text("print_stats_hero_label") == german);

    SystemSettingsManager::instance().set_language("en");
    settle();
    CHECK(subject_text("print_stats_hero_label") == "this week");
}

TEST_CASE_METHOD(PrintStatsTileFixture, "print stats tile: bars and lines stay inside the tile",
                 "[widget_size][print_stats]") {
    PrintHistoryTotals totals;
    totals.total_jobs = 88888;
    totals.total_time = 8888ULL * 3600;
    totals.total_filament_used = 88888000.0;
    api->totals_api().totals = totals;
    seed({job(0, 3600), job(1, 7200), job(3, 1800)});
    PanelWidgetHarness<PrintStatsWidget> h(test_screen());
    h.widget().on_activate();
    for (const Box& b : {kCell, kWide, kFull}) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        settle();
        h.resize(1, 1, b.w, b.h);
        lv_area_t tile;
        lv_obj_get_coords(h.root(), &tile);
        for (const char* name : {"ps_hero", "ps_hero_label", "ps_bars", "ps_bar_6", "ps_support"}) {
            INFO(name);
            lv_obj_t* obj = h.child(name);
            REQUIRE(obj != nullptr);
            lv_area_t a;
            lv_obj_get_coords(obj, &a);
            CHECK(a.x1 >= tile.x1);
            CHECK(a.y1 >= tile.y1);
            CHECK(a.x2 <= tile.x2);
            CHECK(a.y2 <= tile.y2);
        }
        lv_obj_t* bar = h.child("ps_bar_6");
        CHECK(lv_obj_get_height(bar) >= lv_font_get_line_height(lv_obj_get_style_text_font(
                                            h.child("ps_support"), LV_PART_MAIN)));
    }
}

TEST_CASE_METHOD(PrintStatsTileFixture, "print stats tile: fits_at refuses below the strip",
                 "[widget_size][print_stats]") {
    PrintStatsWidget probe;
    CHECK_FALSE(probe.fits_at(60, 30));
    CHECK(probe.fits_at(kStrip.w, kStrip.h));
}

TEST_CASE_METHOD(PrintStatsTileFixture,
                 "print stats tile: the Strip shows hero and label on one line",
                 "[widget_size][print_stats]") {
    seed({job(0, 3600)});
    PanelWidgetHarness<PrintStatsWidget> h(test_screen());
    h.resize(1, 1, kStrip.w, kStrip.h);
    settle();
    h.resize(1, 1, kStrip.w, kStrip.h);
    lv_area_t hero, label, tile;
    lv_obj_get_coords(h.child("ps_hero"), &hero);
    lv_obj_get_coords(h.child("ps_hero_label"), &label);
    lv_obj_get_coords(h.root(), &tile);
    CHECK(label.x1 >= hero.x2);
    CHECK(label.x2 <= tile.x2);
    CHECK(label.y2 <= tile.y2);
    CHECK(hero.y2 <= tile.y2);
}
