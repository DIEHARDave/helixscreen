// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_plugin_install_prompt.cpp
 * @brief The HelixPrint plugin install prompt Print Select shows after discovery
 *
 * The prompt opens when discovery reports the plugin absent (0), and only then:
 * never on -1 (not probed yet) or 1 (installed). An install from it must leave
 * Moonraker able to load the plugin, which takes a Moonraker restart, because
 * install.sh always runs with --no-restart. These cases drive the real panel's
 * observer over a mock Moonraker client, with install.sh replaced by a stub.
 */

#include "ui_modal.h"
#include "ui_panel_print_select.h"

#include "../test_helpers/config_test_access.h"
#include "../test_helpers/plugin_script_sandbox.h"
#include "../test_helpers/print_select_panel_fixture.h"
#include "../test_helpers/print_select_panel_test_access.h"
#include "config.h"
#include "printer_state.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// The prompt's preferences open: beta features on, the wizard done, never
/// declined. The whole config is put back afterwards.
struct PromptConfigScope {
    PromptConfigScope() : cfg(Config::get_instance()), saved(ConfigTestAccess::data(*cfg)) {
        cfg->set<bool>("/beta_features", true);
        cfg->set<bool>("/wizard_completed", true);
        cfg->set<bool>(cfg->df() + "wizard_completed", true);
        cfg->set<bool>("/plugin_install_declined", false);
    }
    ~PromptConfigScope() {
        ConfigTestAccess::data(*cfg) = saved;
    }
    PromptConfigScope(const PromptConfigScope&) = delete;
    PromptConfigScope& operator=(const PromptConfigScope&) = delete;

    Config* cfg;
    json saved;
};

struct PromptFixture : PrintSelectGlobalStateReset, PrintSelectPanelFixture {
    PromptConfigScope prefs;
    test::PluginScriptSandbox scripts{PrintSelectPanelTestAccess::plugin_installer(*panel_)};

    PromptFixture() {
        REQUIRE(prompt() == nullptr);
    }

    void set_plugin_installed(bool installed) {
        get_printer_state().set_helix_plugin_installed(installed);
        drain();
    }

    /// The open install prompt, or null.
    lv_obj_t* prompt() {
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        if (dialog && lv_obj_find_by_name(dialog, "btn_install")) {
            return dialog;
        }
        return nullptr;
    }

    void click(const char* name) {
        lv_obj_t* dialog = prompt();
        REQUIRE(dialog != nullptr);
        lv_obj_t* button = lv_obj_find_by_name(dialog, name);
        REQUIRE(button != nullptr);
        // The modal's handlers run inside send_event; no render pass is needed.
        lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
        drain();
    }

    std::string result_message() {
        lv_obj_t* dialog = prompt();
        REQUIRE(dialog != nullptr);
        lv_obj_t* label = lv_obj_find_by_name(dialog, "result_message");
        REQUIRE(label != nullptr);
        return lv_label_get_text(label);
    }

    int moonraker_restarts() const {
        return mock_client_.call_count("server.restart");
    }
};

} // namespace

TEST_CASE_METHOD(PromptFixture, "the install prompt opens only when the plugin is known absent",
                 "[plugin_installer][print_select]") {
    SECTION("installed: no prompt") {
        set_plugin_installed(true);
        CHECK(prompt() == nullptr);
    }
    SECTION("absent: the prompt opens") {
        set_plugin_installed(false);
        CHECK(prompt() != nullptr);
    }
}

TEST_CASE_METHOD(PromptFixture, "the install prompt stays closed once declined",
                 "[plugin_installer][print_select]") {
    prefs.cfg->set<bool>("/plugin_install_declined", true);
    set_plugin_installed(false);
    CHECK(prompt() == nullptr);
}

TEST_CASE_METHOD(PromptFixture, "the install prompt stays closed with beta features off",
                 "[plugin_installer][print_select]") {
    prefs.cfg->set<bool>("/beta_features", false);
    set_plugin_installed(false);
    CHECK(prompt() == nullptr);
}

TEST_CASE_METHOD(PromptFixture, "an install from the prompt restarts Moonraker to load the plugin",
                 "[plugin_installer][print_select]") {
    scripts.write_script("exit 0");
    set_plugin_installed(false);
    REQUIRE(prompt() != nullptr);
    const int restarts_before = moonraker_restarts();

    click("btn_install");

    CHECK(scripts.script_args() == "--no-restart --auto ");
    CHECK(moonraker_restarts() == restarts_before + 1);
    CHECK(result_message().find("restarting") != std::string::npos);
}

TEST_CASE_METHOD(PromptFixture, "a failed install from the prompt restarts nothing",
                 "[plugin_installer][print_select]") {
    scripts.write_script("exit 1");
    set_plugin_installed(false);
    REQUIRE(prompt() != nullptr);
    const int restarts_before = moonraker_restarts();

    click("btn_install");

    CHECK(moonraker_restarts() == restarts_before);
    CHECK(result_message().find("restarting") == std::string::npos);
}
