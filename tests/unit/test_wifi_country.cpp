// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// WiFi regulatory country: the ISO table and parsers, the startup policy that
// applies a saved country without overriding one the system set, and the
// rescan a regulatory domain change triggers.

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/wifi_manager_test_access.h"
#include "../ui_test_utils.h"
#include "async_lifetime_guard.h"
#include "config.h"
#include "http_executor.h"
#include "system_settings_manager.h"
#include "wifi_backend_mock.h"
#include "wifi_country.h"
#include "wifi_manager.h"

#include <atomic>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::Config;
using helix::SystemSettingsManager;
using helix::WiFiManager;
using helix::WiFiManagerTestAccess;
using helix::http::HttpExecutor;
namespace country = helix::wifi::country;

// ============================================================================
// Pure: table, parsers, policy
// ============================================================================

TEST_CASE("country table is ISO 3166 alpha-2, unique and round-trips the dropdown",
          "[wifi][wifi_country]") {
    REQUIRE(country::table_size() == 249);
    std::set<std::string> codes;
    for (size_t i = 0; i < country::table_size(); ++i) {
        const std::string code = country::table_entry(i).code;
        CHECK(code.size() == 2);
        CHECK(codes.insert(code).second);
        // Index 0 is the not-set row, so the table starts at 1.
        CHECK(country::dropdown_index(code) == static_cast<int>(i) + 1);
        CHECK(country::code_at_dropdown_index(static_cast<int>(i) + 1) == code);
    }
    CHECK(country::dropdown_index("") == 0);
    CHECK(country::dropdown_index("00") == 0);
    CHECK(country::code_at_dropdown_index(0).empty());
    CHECK(country::code_at_dropdown_index(static_cast<int>(country::table_size()) + 1).empty());

    const std::string options = country::dropdown_options("Not set");
    CHECK(options.rfind("Not set\n", 0) == 0);
    CHECK(options.find("\nUnited States (US)") != std::string::npos);
}

TEST_CASE("country normalize accepts table codes in any case and nothing else",
          "[wifi][wifi_country]") {
    CHECK(country::normalize("us") == "US");
    CHECK(country::normalize("De") == "DE");
    CHECK(country::normalize("00").empty());
    CHECK(country::normalize("ZZ").empty());
    CHECK(country::normalize("USA").empty());
    CHECK(country::normalize("U").empty());
    CHECK(country::is_world(""));
    CHECK(country::is_world("00"));
    CHECK_FALSE(country::is_world("US"));
}

TEST_CASE("country parsers read wpa GET, iw reg get and REGDOM-CHANGE", "[wifi][wifi_country]") {
    CHECK(country::parse_wpa_get_country("US") == "US");
    CHECK(country::parse_wpa_get_country("de\n") == "DE");
    CHECK(country::parse_wpa_get_country("FAIL\n").empty());
    CHECK(country::parse_wpa_get_country("").empty());

    // The global section comes first; a self-managed phy's own line follows it.
    const std::string iw = "global\n"
                           "country US: DFS-FCC\n"
                           "\t(2400 - 2472 @ 40), (N/A, 30), (N/A)\n"
                           "\n"
                           "phy#0 (self-managed)\n"
                           "country DE: DFS-ETSI\n";
    CHECK(country::parse_iw_reg_get(iw) == "US");
    CHECK(country::parse_iw_reg_get("global\ncountry 00: DFS-UNSET\n").empty());
    CHECK(country::parse_iw_reg_get("").empty());

    CHECK(country::parse_regdom_change_alpha2(
              "<3>CTRL-EVENT-REGDOM-CHANGE init=USER type=COUNTRY alpha2=US") == "US");
    CHECK(country::parse_regdom_change_alpha2("<3>CTRL-EVENT-REGDOM-CHANGE init=CORE type=WORLD")
              .empty());
}

TEST_CASE("country plan applies a saved choice and never overrides the system's own",
          "[wifi][wifi_country]") {
    SECTION("saved, radio disagrees: apply it") {
        const auto p = country::plan("DE", "");
        CHECK(p.apply == "DE");
        CHECK(p.display == "DE");
    }
    SECTION("saved, radio already agrees: nothing to send") {
        const auto p = country::plan("de", "DE");
        CHECK(p.apply.empty());
        CHECK(p.display == "DE");
    }
    SECTION("nothing saved, system set a country: show it, leave it") {
        const auto p = country::plan("", "GB");
        CHECK(p.apply.empty());
        CHECK(p.display == "GB");
    }
    SECTION("nothing saved, world: not set") {
        const auto p = country::plan("", "00");
        CHECK(p.apply.empty());
        CHECK(p.display.empty());
    }
}

TEST_CASE("country 5 GHz hint shows only for a dual-band radio in the world domain",
          "[wifi][wifi_country]") {
    CHECK(country::should_hint_5ghz(true, ""));
    CHECK(country::should_hint_5ghz(true, "00"));
    CHECK_FALSE(country::should_hint_5ghz(true, "US"));
    CHECK_FALSE(country::should_hint_5ghz(false, ""));
}

// ============================================================================
// WiFiManager: persist, apply at startup, rescan on a domain change
// ============================================================================

namespace {

/// Mock that counts scan triggers, so a test can see a rescan go out.
class CountingCountryBackend : public WifiBackendMock {
  public:
    WiFiError trigger_scan() override {
        triggers_++;
        return WiFiError(WiFiResult::SUCCESS);
    }
    int triggers() const {
        return triggers_.load();
    }

  private:
    std::atomic<int> triggers_{0};
};

struct CountryFixture : LVGLTestFixture {
    CountingCountryBackend* backend = nullptr;
    std::shared_ptr<WiFiManager> wm;
    helix::AsyncLifetimeGuard guard;

    CountryFixture() {
        Config::get_instance();
        SystemSettingsManager::instance().set_wifi_country("");
        HttpExecutor::fast().start();
    }

    /// Builds the manager; its READY handler runs the startup apply.
    void start(const std::string& system_country) {
        auto b = std::make_unique<CountingCountryBackend>();
        backend = b.get();
        backend->set_country_for_test(system_country);
        wm = std::make_shared<WiFiManager>(std::move(b));
        wm->init_self_reference(wm);
        REQUIRE(WiFiManagerTestAccess::settle(*wm));
    }

    ~CountryFixture() override {
        if (wm) {
            wm->stop_scan();
            WiFiManagerTestAccess::settle(*wm);
            wm.reset();
        }
        helix::ui::UpdateQueue::instance().drain();
        SystemSettingsManager::instance().set_wifi_country("");
    }
};

std::string radio_country(WifiBackendMock& backend) {
    std::string code;
    REQUIRE(backend.get_country(code).success());
    return code;
}

} // namespace

TEST_CASE_METHOD(CountryFixture, "setting the country saves it and applies it to the radio",
                 "[wifi][wifi_country]") {
    start("");
    bool done = false;
    bool ok = false;
    wm->set_country_async("de", guard.token(), [&](bool success) {
        done = true;
        ok = success;
    });
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    REQUIRE(done);
    CHECK(ok);
    CHECK(SystemSettingsManager::instance().get_wifi_country() == "DE");
    CHECK(radio_country(*backend) == "DE");
    CHECK(backend->set_country_calls() == 1);
}

TEST_CASE_METHOD(CountryFixture, "clearing the country forgets the choice and leaves the radio",
                 "[wifi][wifi_country]") {
    start("");
    wm->set_country_async("FR", guard.token(), nullptr);
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    REQUIRE(backend->set_country_calls() == 1);

    wm->set_country_async("", guard.token(), nullptr);
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    CHECK(SystemSettingsManager::instance().get_wifi_country().empty());
    CHECK(radio_country(*backend) == "FR");
    CHECK(backend->set_country_calls() == 1);
}

TEST_CASE_METHOD(CountryFixture, "a saved country is applied at startup when the radio differs",
                 "[wifi][wifi_country]") {
    SystemSettingsManager::instance().set_wifi_country("JP");
    start("");
    CHECK(radio_country(*backend) == "JP");
    CHECK(backend->set_country_calls() == 1);
}

TEST_CASE_METHOD(CountryFixture, "a saved country the radio already has is not sent again",
                 "[wifi][wifi_country]") {
    SystemSettingsManager::instance().set_wifi_country("JP");
    start("JP");
    CHECK(backend->set_country_calls() == 0);
}

TEST_CASE_METHOD(CountryFixture, "with nothing saved, the system's country is shown and kept",
                 "[wifi][wifi_country]") {
    start("GB");
    CHECK(backend->set_country_calls() == 0);
    CHECK(radio_country(*backend) == "GB");

    std::optional<WiFiManager::CountryState> got;
    wm->get_country_async(guard.token(), [&](const WiFiManager::CountryState& s) { got = s; });
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    REQUIRE(got.has_value());
    CHECK(got->read_ok);
    CHECK(got->stored.empty());
    CHECK(got->system == "GB");
    CHECK(got->display == "GB");
}

TEST_CASE_METHOD(CountryFixture, "a regulatory domain change rescans an open scan session",
                 "[wifi][wifi_country]") {
    start("");
    wm->start_scan([](const std::vector<WiFiNetwork>&) {});
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    WiFiManagerTestAccess::fire_scan_complete(*wm);
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    const int before = backend->triggers();
    REQUIRE(before >= 1);

    // The mock dispatches REGDOM_CHANGE from set_country, as wpa_supplicant does.
    wm->set_country_async("US", guard.token(), nullptr);
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    CHECK(backend->triggers() == before + 1);
}

TEST_CASE_METHOD(CountryFixture, "a regulatory domain change with no scan session sends no scan",
                 "[wifi][wifi_country]") {
    start("");
    wm->set_country_async("US", guard.token(), nullptr);
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    CHECK(radio_country(*backend) == "US");
    CHECK(backend->triggers() == 0);
}
