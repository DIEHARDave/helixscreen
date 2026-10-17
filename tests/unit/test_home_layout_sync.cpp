// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_home_layout_sync.cpp
 * @brief HomeLayoutSync publish / fetch through the mock Moonraker database,
 *        and the connection locality verdict it is gated on.
 */

#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/log_capture.h"
#include "../test_helpers/moonraker_manager_test_access.h"
#include "app_globals.h"
#include "config.h"
#include "connection_state.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "home_layout_share.h"
#include "home_layout_sync.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "moonraker_manager.h"
#include "panel_widget_config.h"
#include "printer_network_state.h"
#include "printer_state.h"

#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::home_layout::kKey;
using helix::home_layout::kNamespace;
using json = nlohmann::json;

namespace {

json widget(const char* id, int col, int row, int colspan, int rowspan) {
    return json{{"id", id},   {"enabled", true},    {"col", col},
                {"row", row}, {"colspan", colspan}, {"rowspan", rowspan}};
}

/// A home layout saved on a 12x8 grid.
json saved_home_12x8(const char* second_widget = "fan") {
    return json{
        {"pages",
         json::array({json{{"id", "main"},
                           {"widgets", json::array({widget("printer_image", 0, 0, 4, 4),
                                                    widget("temperature", 4, 0, 2, 2),
                                                    widget(second_widget, 6, 0, 2, 2)})}}})},
        {"main_page_index", 0},
        {"next_page_id", 1},
        {"grid", "12x8"}};
}

class HomeLayoutSyncFixture : public LVGLUITestFixture {
  public:
    HomeLayoutSyncFixture() : client_(MoonrakerClientMock::PrinterType::VORON_24) {
        api_ = std::make_unique<MoonrakerAPIMock>(client_, get_printer_state());
        global_config_ = Config::get_instance();
        REQUIRE(global_config_ != nullptr);
        saved_active_id_ = ConfigTestAccess::active_printer_id(*global_config_);
        ConfigTestAccess::active_printer_id(*global_config_) = "printer_a";
        get_printer_state().clear_moonraker_locality();
        drain();
        HomeLayoutSync::instance().init(*api_);
    }

    ~HomeLayoutSyncFixture() override {
        HomeLayoutSync::instance().set_seed_listener(nullptr);
        HomeLayoutSync::instance().shutdown();
        get_printer_state().clear_moonraker_locality();
        drain();
        ConfigTestAccess::active_printer_id(*global_config_) = saved_active_id_;
    }

    static void drain() {
        helix::ui::UpdateQueue::instance().drain();
    }

    void set_locality(bool remote) {
        get_printer_state().set_moonraker_is_remote(remote);
        drain();
    }

    /// A loaded home PanelWidgetConfig over a private Config holding @p saved.
    PanelWidgetConfig& home_config(const json& saved) {
        ConfigTestAccess::data(local_config_) = json::object();
        ConfigTestAccess::data(local_config_)["printers"]["default"]["panel_widgets"]["home"] =
            saved;
        ConfigTestAccess::active_printer_id(local_config_) = "default";
        configs_.push_back(std::make_unique<PanelWidgetConfig>("home", local_config_));
        configs_.back()->load();
        return *configs_.back();
    }

    bool published() {
        return client_.mock_db().contains(kNamespace) &&
               client_.mock_db()[kNamespace].contains(kKey);
    }

    json published_value() {
        return client_.mock_db_get(kNamespace, kKey);
    }

    void forget_published() {
        if (client_.mock_db().contains(kNamespace)) {
            client_.mock_db()[kNamespace].erase(kKey);
        }
    }

    void publish_remote_layout(const json& value) {
        client_.mock_db_set(kNamespace, kKey, value);
    }

    json valid_published_payload() {
        PanelWidgetConfig& source = home_config(saved_home_12x8());
        return home_layout::build_payload(source.published_pages(), "12x8", "1.0.0", 1700000000);
    }

    MoonrakerClientMock client_;
    std::unique_ptr<MoonrakerAPIMock> api_;
    Config* global_config_ = nullptr;
    std::string saved_active_id_;
    Config local_config_;
    std::vector<std::unique_ptr<PanelWidgetConfig>> configs_;
};

} // namespace

TEST_CASE_METHOD(HomeLayoutSyncFixture,
                 "home_layout_sync: publishes nothing while locality is unknown",
                 "[home_layout_sync]") {
    REQUIRE(get_printer_state().moonraker_locality() == MoonrakerLocality::Unknown);
    PanelWidgetConfig& wc = home_config(saved_home_12x8());
    wc.save();
    drain();
    CHECK_FALSE(published());

    HomeLayoutSync::instance().on_connected();
    drain();
    CHECK_FALSE(published());
}

TEST_CASE_METHOD(HomeLayoutSyncFixture,
                 "home_layout_sync: publishes on save when Local, payload parses back",
                 "[home_layout_sync]") {
    set_locality(false);
    REQUIRE(get_printer_state().moonraker_locality() == MoonrakerLocality::Local);
    PanelWidgetConfig& wc = home_config(saved_home_12x8());
    REQUIRE(wc.grid_signature() == "12x8");

    wc.save();
    drain();
    REQUIRE(published());

    const json value = published_value();
    CHECK(value["schema"] == home_layout::kSchema);
    CHECK(value["grid"] == "12x8");
    CHECK_FALSE(value.contains("next_page_id"));
    const auto parsed = home_layout::parse_payload(value);
    REQUIRE(parsed.has_value());
    CHECK(parsed->cols == 12);
    CHECK(parsed->rows == 8);
    const json& widgets = parsed->pages_payload["pages"][0]["widgets"];
    std::vector<std::string> ids;
    for (const auto& w : widgets) {
        // load() appends the registry's other widgets disabled and unplaced.
        if (w.value("enabled", false) && w.value("col", -1) >= 0) {
            ids.push_back(w["id"].get<std::string>());
        }
    }
    CHECK(ids == std::vector<std::string>{"printer_image", "temperature", "fan"});
    CHECK(widgets[2]["col"] == 6);
}

TEST_CASE_METHOD(HomeLayoutSyncFixture,
                 "home_layout_sync: a panel other than home is not published",
                 "[home_layout_sync]") {
    set_locality(false);
    ConfigTestAccess::data(local_config_) = json::object();
    ConfigTestAccess::data(local_config_)["printers"]["default"]["panel_widgets"]["controls"] =
        saved_home_12x8();
    ConfigTestAccess::active_printer_id(local_config_) = "default";
    PanelWidgetConfig other("controls", local_config_);
    other.load();
    other.save();
    drain();
    CHECK_FALSE(published());
}

TEST_CASE_METHOD(HomeLayoutSyncFixture, "home_layout_sync: remote client never publishes",
                 "[home_layout_sync]") {
    set_locality(true);
    REQUIRE(get_printer_state().moonraker_locality() == MoonrakerLocality::Remote);
    PanelWidgetConfig& wc = home_config(saved_home_12x8());
    wc.save();
    drain();
    CHECK_FALSE(published());

    HomeLayoutSync::instance().on_connected();
    drain();
    CHECK_FALSE(published());
}

TEST_CASE_METHOD(HomeLayoutSyncFixture, "home_layout_sync: identical consecutive saves post once",
                 "[home_layout_sync]") {
    set_locality(false);
    PanelWidgetConfig& wc = home_config(saved_home_12x8());
    wc.save();
    drain();
    REQUIRE(published());

    // An unchanged layout is not posted again, so the key stays gone.
    forget_published();
    wc.save();
    drain();
    CHECK_FALSE(published());

    SECTION("a changed layout posts again") {
        PanelWidgetConfig& changed = home_config(saved_home_12x8("led"));
        changed.save();
        drain();
        CHECK(published());
    }

    SECTION("a new connection posts again") {
        HomeLayoutSync::instance().on_disconnected();
        wc.save();
        drain();
        CHECK(published());
    }
}

TEST_CASE_METHOD(HomeLayoutSyncFixture,
                 "home_layout_sync: on_connected as Remote fetches and take_seed returns it once",
                 "[home_layout_sync]") {
    publish_remote_layout(valid_published_payload());
    set_locality(true);

    std::vector<std::string> told;
    HomeLayoutSync::instance().set_seed_listener(
        [&told](const std::string& printer_id) { told.push_back(printer_id); });

    HomeLayoutSync::instance().on_connected();
    // Nothing is handed over until the reply reaches the main thread.
    CHECK(told.empty());
    drain();
    CHECK(told == std::vector<std::string>{"printer_a"});

    CHECK_FALSE(HomeLayoutSync::instance().take_seed("printer_b").has_value());
    auto seed = HomeLayoutSync::instance().take_seed("printer_a");
    REQUIRE(seed.has_value());
    CHECK(seed->cols == 12);
    CHECK(seed->rows == 8);
    CHECK_FALSE(HomeLayoutSync::instance().take_seed("printer_a").has_value());
}

TEST_CASE_METHOD(HomeLayoutSyncFixture,
                 "home_layout_sync: a reply for a printer no longer active is dropped",
                 "[home_layout_sync]") {
    publish_remote_layout(valid_published_payload());
    set_locality(true);
    bool told = false;
    HomeLayoutSync::instance().set_seed_listener([&told](const std::string&) { told = true; });

    HomeLayoutSync::instance().on_connected();
    // The mock answers at once; the reply is queued for the main thread.
    ConfigTestAccess::active_printer_id(*global_config_) = "printer_b";
    drain();

    CHECK_FALSE(told);
    CHECK_FALSE(HomeLayoutSync::instance().take_seed("printer_a").has_value());
    CHECK_FALSE(HomeLayoutSync::instance().take_seed("printer_b").has_value());
}

TEST_CASE_METHOD(HomeLayoutSyncFixture,
                 "home_layout_sync: a reply that lands after a disconnect is dropped",
                 "[home_layout_sync]") {
    publish_remote_layout(valid_published_payload());
    set_locality(true);
    HomeLayoutSync::instance().on_connected();
    HomeLayoutSync::instance().on_disconnected();
    drain();
    CHECK_FALSE(HomeLayoutSync::instance().take_seed("printer_a").has_value());
}

TEST_CASE_METHOD(HomeLayoutSyncFixture,
                 "home_layout_sync: missing key or unparseable value yields no seed and no error "
                 "toast",
                 "[home_layout_sync]") {
    set_locality(true);
    SECTION("missing key") {}
    SECTION("unparseable value") {
        publish_remote_layout(json{{"schema", 99}, {"grid", "nonsense"}});
    }
    SECTION("not an object") {
        publish_remote_layout("garbage");
    }

    LogCapture capture;
    bool told = false;
    HomeLayoutSync::instance().set_seed_listener([&told](const std::string&) { told = true; });
    HomeLayoutSync::instance().on_connected();
    drain();

    CHECK_FALSE(told);
    CHECK_FALSE(HomeLayoutSync::instance().take_seed("printer_a").has_value());
    for (auto level : capture.levels_for("[HomeLayoutSync]")) {
        CHECK(level < spdlog::level::warn);
    }
    for (auto level : capture.levels_for("database")) {
        CHECK(level < spdlog::level::warn);
    }
}

TEST_CASE_METHOD(HomeLayoutSyncFixture, "home_layout_sync: fetch_now hands the layout to done",
                 "[home_layout_sync]") {
    set_locality(true);
    std::vector<bool> results;
    auto record = [&results](std::optional<home_layout::ParsedLayout> layout) {
        results.push_back(layout.has_value());
    };

    HomeLayoutSync::instance().fetch_now(record);
    drain();
    publish_remote_layout(valid_published_payload());
    HomeLayoutSync::instance().fetch_now(record);
    drain();
    // The manual fetch does not leave a seed behind.
    CHECK_FALSE(HomeLayoutSync::instance().take_seed("printer_a").has_value());
    // A printer switch in flight still answers, with nothing.
    HomeLayoutSync::instance().fetch_now(record);
    ConfigTestAccess::active_printer_id(*global_config_) = "printer_b";
    drain();

    CHECK(results == std::vector<bool>{false, true, false});
}

TEST_CASE_METHOD(HomeLayoutSyncFixture,
                 "home_layout_sync: locality: Unknown after clear, Local/Remote after set, cleared "
                 "on disconnect edge",
                 "[home_layout_sync]") {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, "moonraker_locality");
    REQUIRE(subject != nullptr);
    PrinterState& ps = get_printer_state();

    CHECK(ps.moonraker_locality() == MoonrakerLocality::Unknown);
    CHECK(lv_subject_get_int(subject) == 0);

    ps.set_moonraker_is_remote(false);
    // The verdict is readable as soon as it is made; the subject follows the queue.
    CHECK(ps.moonraker_locality() == MoonrakerLocality::Local);
    drain();
    CHECK(lv_subject_get_int(subject) == 1);
    CHECK_FALSE(ps.is_moonraker_remote());

    ps.set_moonraker_is_remote(true);
    drain();
    CHECK(ps.moonraker_locality() == MoonrakerLocality::Remote);
    CHECK(lv_subject_get_int(subject) == 2);
    CHECK(ps.is_moonraker_remote());

    ps.clear_moonraker_locality();
    CHECK(ps.moonraker_locality() == MoonrakerLocality::Unknown);
    drain();
    CHECK(lv_subject_get_int(subject) == 0);
    // is_moonraker_remote() keeps the last verdict, as its callers expect.
    CHECK(ps.is_moonraker_remote());
}

TEST_CASE_METHOD(HomeLayoutSyncFixture,
                 "home_layout_sync: a non-CONNECTED connection edge clears the verdict and the "
                 "held seed",
                 "[home_layout_sync]") {
    publish_remote_layout(valid_published_payload());
    set_locality(true);
    HomeLayoutSync::instance().on_connected();
    drain();

    const int edge = GENERATE(static_cast<int>(ConnectionState::DISCONNECTED),
                              static_cast<int>(ConnectionState::RECONNECTING),
                              static_cast<int>(ConnectionState::FAILED));
    INFO("edge " << edge);
    MoonrakerManager manager;
    MoonrakerManagerTestAccess::queue_connection_state(
        manager, static_cast<int>(ConnectionState::CONNECTED), edge);
    manager.process_notifications();
    drain();

    CHECK(get_printer_state().moonraker_locality() == MoonrakerLocality::Unknown);
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "moonraker_locality")) == 0);
    CHECK_FALSE(HomeLayoutSync::instance().take_seed("printer_a").has_value());
}
