// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_manual_pull_prompt.h"

#include "ui_error_reporting.h"
#include "ui_modal.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "config.h"
#include "display_numbering.h"
#include "filament_sensor_manager.h"
#include "observer_factory.h"
#include "static_subject_registry.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <string>

namespace {

/// Held file-static rather than on an owner because the two surfaces that arm
/// this (FilamentPanel, AmsOperationSidebar) have different lifetimes and either
/// may be gone by the time the sensor edge lands.
ObserverGuard s_toolhead_observer;
bool s_armed = false;
bool s_registered = false;
/// The open manual-unload notice, so a second tap replaces it.
lv_obj_t* s_manual_unload_notice = nullptr;

/// Toolhead-detected subject values. -1 is "this printer has no such sensor",
/// which is a third answer and not a synonym for 0.
constexpr int TOOLHEAD_CLEAR = 0;
constexpr int TOOLHEAD_DETECTED = 1;

void fire(const char* via) {
    // Disarm BEFORE notifying. NOTIFY_* is synchronous, and tearing the observer
    // down first is what stops a second edge arriving mid-toast from doubling it.
    helix::ui::disarm_manual_pull_prompt();
    spdlog::info("[ManualPull] Filament clear of the toolhead (via {}) — prompting manual removal",
                 via);
    NOTIFY_INFO(lv_tr("Filament is clear of the toolhead. Pull it out the rest of the way."));
}

} // namespace

namespace helix::ui {

void disarm_manual_pull_prompt() {
    s_armed = false;
    s_toolhead_observer.reset();
}

void arm_manual_pull_prompt() {
    disarm_manual_pull_prompt();

    auto& sensors = FilamentSensorManager::instance();
    lv_subject_t* toolhead = sensors.get_toolhead_detected_subject();
    s_armed = true;

    // Only worth watching when the sensor currently SEES filament. Anything else
    // (already clear, or no sensor at all) can produce no meaningful 1 -> 0 edge,
    // and observe<int> fires once on registration — hooking it up in those
    // states would toast immediately, before the retract has moved anything.
    const bool watchable = toolhead && lv_subject_get_int(toolhead) == TOOLHEAD_DETECTED;
    if (watchable) {
        s_toolhead_observer = observe<int>(
            toolhead, &sensors,
            [](FilamentSensorManager*, int detected) {
                if (detected == TOOLHEAD_CLEAR) {
                    fire("toolhead sensor");
                }
            },
            sensors.get_subjects_lifetime());
    }

    if (!s_registered) {
        s_registered = true;
        StaticSubjectRegistry::instance().register_deinit("ManualPullPrompt", []() {
            disarm_manual_pull_prompt();
            s_registered = false;
            spdlog::trace("[ManualPull] Observers released");
        });
    }

    spdlog::debug("[ManualPull] Armed (toolhead sensor {})",
                  watchable ? "watching" : "unavailable — will prompt on completion");
}

void manual_pull_unload_finished() {
    if (!s_armed) {
        return;
    }
    fire("unload complete");
}

bool is_creator5_printer() {
    auto* cfg = Config::get_instance();
    return cfg && cfg->get_preset().rfind("creator5", 0) == 0;
}

void dispatch_creator5_load(int slot) {
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend || slot < 0) {
        NOTIFY_WARNING(lv_tr("Multi-Filament System not available"));
        return;
    }
    spdlog::info("[ManualPull] Creator 5 load of slot {} - firmware heats and feeds", slot);
    if (AmsError error = backend->load_filament(slot); !error.success()) {
        notify_ams_error(error);
    }
}

void show_manual_unload_notice(int tool) {
    if (s_manual_unload_notice) {
        modal_hide(s_manual_unload_notice);
        s_manual_unload_notice = nullptr;
    }

    const std::string title = tool >= 0
                                  ? fmt::format(fmt::runtime(lv_tr("Unload {}")), tool_label(tool))
                                  : std::string(lv_tr("Unload Filament"));

    AlertOptions opts;
    opts.on_dismiss = [] { s_manual_unload_notice = nullptr; };
    s_manual_unload_notice = modal_alert(
        title.c_str(),
        lv_tr("The Creator 5 has no automatic unload. "
              "Cut the filament at the head and pull it out by hand."),
        ModalSeverity::Info, lv_tr("OK"), [] { s_manual_unload_notice = nullptr; }, opts);

    if (!s_manual_unload_notice) {
        spdlog::error("[ManualPull] Failed to create manual unload notice");
        return;
    }
    spdlog::info("[ManualPull] Creator 5 manual unload notice shown ({})", title);
}

} // namespace helix::ui
