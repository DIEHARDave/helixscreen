// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_plugin_install_prompt.cpp
 * @brief The HelixPrint plugin install prompt Print Select shows after discovery
 *
 * The prompt opens when discovery reports the plugin absent (0), and only then:
 * never on -1 (not probed yet) or 1 (installed). An install from it must leave
 * Moonraker able to load the plugin, which takes a Moonraker restart, because
 * install.sh always runs with --no-restart. It never opens while a job holds the
 * machine, and at most once a session: not again after it was answered, and not
 * after the user uninstalled the plugin from Advanced. These cases drive the
 * real panels' observers over a mock Moonraker client, with install.sh replaced
 * by a stub.
 */

#include "ui_modal.h"
#include "ui_panel_advanced.h"
#include "ui_panel_print_select.h"

#include "../test_helpers/advanced_panel_test_access.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/helix_plugin_installer_test_access.h"
#include "../test_helpers/plugin_script_sandbox.h"
#include "../test_helpers/print_select_panel_fixture.h"
#include "../test_helpers/print_select_panel_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "config.h"
#include "observer_factory.h"
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
        HelixPluginInstallerTestAccess::reset_prompt_session();
        REQUIRE(prompt() == nullptr);
    }

    ~PromptFixture() override {
        HelixPluginInstallerTestAccess::reset_prompt_session();
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

TEST_CASE_METHOD(PromptFixture, "the install prompt stays closed while a job holds the machine",
                 "[plugin_installer][print_select]") {
    test::set_wire_state(get_printer_state(), PrintJobState::PRINTING);
    drain();
    REQUIRE(job_holds_machine(get_printer_state().print_state().get_print_lifecycle()));

    set_plugin_installed(false);

    CHECK(prompt() == nullptr);
}

TEST_CASE_METHOD(PromptFixture, "an answered install prompt does not reopen this session",
                 "[plugin_installer][print_select]") {
    set_plugin_installed(false);
    REQUIRE(prompt() != nullptr);
    click("btn_cancel");
    REQUIRE(prompt() == nullptr);

    // A reconnect reports the plugin absent again; a printer switch or a
    // Moonraker restart can pass through installed on the way.
    set_plugin_installed(false);
    CHECK(prompt() == nullptr);
    set_plugin_installed(true);
    set_plugin_installed(false);
    CHECK(prompt() == nullptr);
}

TEST_CASE_METHOD(PromptFixture, "uninstalling from Advanced does not open the install prompt",
                 "[plugin_installer][print_select][advanced]") {
    helix::ui::modal_init_subjects();
    AdvancedPanel advanced{get_printer_state(), nullptr};
    helix::ui::AdvancedPanelTestAccess::set_uninstall_runner(
        advanced, [](HelixPluginInstaller::UninstallCallback done) {
            done(UninstallOutcome::SUCCESS, "removed");
        });
    helix::ui::AdvancedPanelTestAccess::set_moonraker_restarter(advanced, []() {});
    set_plugin_installed(true);

    helix::ui::AdvancedPanelTestAccess::tap_uninstall_row(advanced);
    drain();
    lv_obj_t* confirm = ModalStack::instance().top_dialog();
    REQUIRE(confirm != nullptr);
    lv_obj_t* uninstall = lv_obj_find_by_name(confirm, "btn_primary");
    REQUIRE(uninstall != nullptr);
    lv_obj_send_event(uninstall, LV_EVENT_CLICKED, nullptr);
    drain();

    REQUIRE(get_printer_state().plugin_status_state().helix_plugin_state() == 0);
    CHECK(prompt() == nullptr);
}

namespace {

struct NotifyCounter {
    int notifications = 0;
};

} // namespace

TEST_CASE_METHOD(PromptFixture, "a repeated absent report does not notify plugin observers",
                 "[plugin_installer][print_select]") {
    // What a reconnect's discovery does: it reports the same 0 again. The
    // subject notifies only on a change, so observers see one transition.
    NotifyCounter counter;
    auto guard = helix::ui::observe<int>(
        get_printer_state().plugin_status_state().get_helix_plugin_installed_subject(), &counter,
        [](NotifyCounter* c, int) { ++c->notifications; },
        get_printer_state().get_subjects_lifetime());
    drain();
    const int at_attach = counter.notifications;

    set_plugin_installed(false);
    set_plugin_installed(false);
    set_plugin_installed(false);

    CHECK(counter.notifications == at_attach + 1);
}
