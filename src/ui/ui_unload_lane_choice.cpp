// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_unload_lane_choice.h"

#include "ui_filament_slot_picker.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix::ui {

std::vector<helix::AvailableSlot>
unload_lane_choices(const std::vector<int>& candidates,
                    const std::vector<helix::AvailableSlot>& slots) {
    std::vector<helix::AvailableSlot> choices;
    for (int slot : candidates) {
        auto it = std::find_if(slots.begin(), slots.end(), [slot](const helix::AvailableSlot& s) {
            return s.backend_index == 0 && s.slot_index == slot;
        });
        if (it != slots.end()) {
            choices.push_back(*it);
        }
    }
    return choices;
}

bool choose_unload_lane(helix::AmsBackend* backend, lv_obj_t* anchor,
                        std::function<void(int)> on_chosen) {
    if (!backend || !anchor) {
        return false;
    }
    const std::vector<int> candidates = backend->unload_lane_candidates();
    if (candidates.empty()) {
        return false;
    }
    const std::vector<helix::AvailableSlot> choices =
        unload_lane_choices(candidates, AmsState::instance().collect_available_slots());
    if (choices.empty()) {
        spdlog::warn("[UnloadLane] {} lane candidate(s) but none published as a slot",
                     candidates.size());
        return false;
    }

    // One question at a time, so one picker serves every Unload button. Never
    // destroyed: its destructor deletes LVGL objects, and a static's destructor
    // runs after lv_deinit(), which already freed the screen holding them.
    static FilamentSlotPicker* picker = new FilamentSlotPicker();
    spdlog::info("[UnloadLane] Asking which of {} lane(s) is in the toolhead", choices.size());
    FilamentSlotPicker::Options options;
    options.title = lv_tr("Which lane is in the toolhead?");
    options.highlight_trigger = false;
    picker->show(
        lv_screen_active(), anchor, choices, FilamentSlotPicker::Selection{},
        [on_chosen = std::move(on_chosen)](const FilamentSlotPicker::Selection& sel) {
            if (sel.slot_index >= 0 && on_chosen) {
                on_chosen(sel.slot_index);
            }
        },
        options);
    return true;
}

} // namespace helix::ui
