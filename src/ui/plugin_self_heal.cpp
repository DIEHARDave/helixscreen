// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin_self_heal.h"

#include "ui_toast_manager.h"

#include "http_executor.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "runtime_config.h"

#include <spdlog/spdlog.h>

namespace helix {

namespace {
// install.sh --relink walks /proc and greps the config; it never waits on Moonraker.
constexpr int kRelinkTimeoutSeconds = 30;
constexpr uint32_t kRestartToastMs = 4000;
} // namespace

bool PluginSelfHeal::ran_this_start_ = false;

PluginSelfHeal::PluginSelfHeal(PrinterState& printer_state, HelixPluginInstaller& installer,
                               MoonrakerRestarter restarter)
    : printer_state_(printer_state), installer_(installer), restarter_(std::move(restarter)) {}

void PluginSelfHeal::on_plugin_reported() {
    if (ran_this_start_ || printer_state_.plugin_status_state().helix_plugin_state() < 0) {
        return;
    }
    ran_this_start_ = true;

    // A mock Moonraker is not the one install.sh would find on this host.
    if (get_runtime_config()->should_mock_moonraker()) {
        return;
    }
    if (!installer_.is_local_moonraker()) {
        act(false, PluginLinkCheck::NotOurs);
        return;
    }
    const std::string script = installer_.get_install_script_path();
    if (script.empty()) {
        spdlog::info("[PluginSelfHeal] No bundled install.sh; the plugin link is not checked");
        return;
    }
    // The script blocks while it walks /proc and the config, so it runs off
    // this thread; its outcome comes back here.
    checking_ = true;
    auto tok = lifetime_.token();
    http::HttpExecutor::slow().submit([this, tok, script]() {
        const PluginLinkCheck link = relink_plugin(script, kRelinkTimeoutSeconds);
        tok.defer("PluginSelfHeal::relinked", [this, link]() { act(true, link); });
    });
}

void PluginSelfHeal::act(bool local, PluginLinkCheck link) {
    checking_ = false;
    // A link of ours means the user installed the plugin: one that reads as
    // absent failed to load, which this check repairs, so offering an install
    // would be wrong.
    if (link == PluginLinkCheck::Current || link == PluginLinkCheck::Relinked) {
        HelixPluginInstaller::settle_prompt_for_session();
    }
    PluginSelfHealFacts facts;
    facts.local = local;
    facts.link = link;
    facts.reported_version = printer_state_.plugin_status_state().version();
    facts.bundled_version = installer_.bundled_plugin_version();
    facts.job_holds = job_holds_machine(printer_state_.print_state().get_print_lifecycle());

    switch (decide_plugin_self_heal(facts)) {
    case PluginSelfHealStep::Nothing:
        if (link == PluginLinkCheck::Failed) {
            spdlog::info("[PluginSelfHeal] Could not check the plugin link; the next start "
                         "tries again");
        }
        return;
    case PluginSelfHealStep::ReportOnly:
        spdlog::info("[PluginSelfHeal] Moonraker runs HelixPrint plugin {} and this app ships {}; "
                     "run the install command from Advanced on the printer to match them",
                     facts.reported_version, facts.bundled_version);
        return;
    case PluginSelfHealStep::WaitForJob:
        spdlog::info("[PluginSelfHeal] The HelixPrint plugin needs a Moonraker restart (link {}, "
                     "running {}, bundled {}); waiting for the job to end",
                     link == PluginLinkCheck::Relinked ? "re-pointed" : "current",
                     facts.reported_version, facts.bundled_version);
        restart_waiting_for_job_ = true;
        return;
    case PluginSelfHealStep::RestartNow:
        spdlog::info("[PluginSelfHeal] Restarting Moonraker to load the HelixPrint plugin this "
                     "app ships (link {}, running {}, bundled {})",
                     link == PluginLinkCheck::Relinked ? "re-pointed" : "current",
                     facts.reported_version, facts.bundled_version);
        restart_moonraker();
        return;
    }
}

void PluginSelfHeal::on_job_state_changed() {
    if (!restart_waiting_for_job_ ||
        job_holds_machine(printer_state_.print_state().get_print_lifecycle())) {
        return;
    }
    restart_waiting_for_job_ = false;
    spdlog::info("[PluginSelfHeal] The job let go; restarting Moonraker for the HelixPrint plugin");
    restart_moonraker();
}

void PluginSelfHeal::restart_moonraker() {
    if (restarter_ && restarter_()) {
        ToastManager::instance().show(
            ToastSeverity::INFO, lv_tr("HelixPrint plugin updated. Moonraker is restarting..."),
            kRestartToastMs);
    }
}

} // namespace helix
