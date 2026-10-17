// SPDX-License-Identifier: GPL-3.0-or-later
#if HELIX_HAS_SNAPMAKER

#include "ams_backend_multiace.h"

#include "ace_slot_parse.h"
#include "ams_fault_event.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "snapmaker_exceptions.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix {

namespace {

/// multiACE's console faults: the U1's coded-exception shape with module id
/// 525 (FEEDING) and a code in the 200 band (its ace.py#_ace_error registry).
constexpr int kMultiAceFaultId = 525;
constexpr int kMultiAceCodeFirst = 200;
constexpr int kMultiAceCodeLast = 299;

std::string head_extruder_name(int head) {
    return head == 0 ? std::string("extruder") : fmt::format("extruder{}", head);
}

} // namespace

AmsBackendMultiAce::AmsBackendMultiAce(IMoonrakerAPI* api, helix::IMoonrakerClient* client)
    : AmsBackendSnapmaker(api, client) {}

// ============================================================================
// Engine state
// ============================================================================

bool AmsBackendMultiAce::active_locked() const {
    return api_version_ == multiace::kApiVersion &&
           (mode_ == multiace::Mode::MULTI || mode_ == multiace::Mode::HEAD);
}

bool AmsBackendMultiAce::ace_fed_locked(int head) const {
    if (head < 0 || head >= kHeads || !active_locked()) {
        return false;
    }
    const auto h = static_cast<size_t>(head);
    return multiace::head_feed(mode_, head_manual_[h], head_feeder_[h]) == multiace::HeadFeed::ACE;
}

std::optional<std::pair<int, int>> AmsBackendMultiAce::routed_bay_locked(int head) const {
    if (head < 0 || head >= kHeads) {
        return std::nullopt;
    }
    const auto& src = head_source_[static_cast<size_t>(head)];
    if (!src || src->load_failed || src->ace < 0 || src->slot < 0) {
        return std::nullopt;
    }
    return std::make_pair(src->ace, src->slot);
}

int AmsBackendMultiAce::unit_position_locked(int ace) const {
    for (size_t i = 0; i < units_.size(); ++i) {
        if (units_[i].idx == ace) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::optional<int> AmsBackendMultiAce::head_for_global_locked(int global) const {
    const auto bay = multiace::bay_of(global);
    if (!bay || !active_locked() || unit_position_locked(bay->first) < 0) {
        return std::nullopt;
    }
    std::array<multiace::HeadFeed, kHeads> feed{};
    for (int h = 0; h < kHeads; ++h) {
        const auto i = static_cast<size_t>(h);
        feed[i] = multiace::head_feed(mode_, head_manual_[i], head_feeder_[i]);
    }
    return multiace::head_for_bay(mode_, bay->first, bay->second, head_ace_, feed);
}

const multiace::Unit* AmsBackendMultiAce::unit_for_index_locked(int unit_index) const {
    if (!active_locked() || unit_index < 1 || unit_index > static_cast<int>(units_.size())) {
        return nullptr;
    }
    return &units_[static_cast<size_t>(unit_index - 1)];
}

void AmsBackendMultiAce::apply_extension_locked(const nlohmann::json& status, bool& changed) {
    const auto it = status.find("ace");
    if (it == status.end()) {
        return;
    }
    const multiace::StatusDelta d = multiace::parse_ace_status(*it);

    if (d.api_version) {
        api_version_ = d.api_version;
        if (*d.api_version != multiace::kApiVersion && !api_mismatch_logged_) {
            spdlog::warn("{} multiACE reports engine API {}, this screen speaks {}: showing the "
                         "U1's own heads only",
                         backend_log_tag(), *d.api_version, multiace::kApiVersion);
            api_mismatch_logged_ = true;
        }
    }
    if (d.mode) {
        mode_ = *d.mode;
    }
    if (d.head_ace) {
        head_ace_ = *d.head_ace;
    }
    if (d.head_feeder) {
        head_feeder_ = *d.head_feeder;
    }
    if (d.head_manual) {
        head_manual_ = *d.head_manual;
    }
    if (d.head_source) {
        head_source_ = *d.head_source;
    }
    if (d.aces) {
        units_ = *d.aces;
    }
    if (d.swap_in_progress) {
        swap_in_progress_ = *d.swap_in_progress;
    }
    if (d.unload_all_active) {
        unload_all_active_ = *d.unload_all_active;
    }
    if (d.swap_phase) {
        swap_phase_ = *d.swap_phase;
    }
    if (d.last_swap_result) {
        last_swap_ = *d.last_swap_result;
    }
    if (d.event_seq) {
        event_seq_ = *d.event_seq;
    }

    if (d.ace_heads && active_locked()) {
        // The engine's own list of ACE-driven heads, against ours from the
        // per-head flags. A disagreement means the two readings of its mode
        // drifted; the flags are what the engine's commands act on.
        std::vector<int> ours;
        for (int h = 0; h < kHeads; ++h) {
            if (ace_fed_locked(h)) {
                ours.push_back(h);
            }
        }
        if (ours != *d.ace_heads) {
            spdlog::debug("{} ace_heads disagrees with the per-head flags", backend_log_tag());
        }
    }

    if (d.api_version || d.mode || d.head_ace || d.head_feeder || d.head_manual || d.head_source ||
        d.aces) {
        rebuild_units_locked();
        changed = true;
    }
}

void AmsBackendMultiAce::rebuild_units_locked() {
    system_info_.units.resize(1);
    int total = kHeads;
    if (active_locked()) {
        std::array<multiace::HeadFeed, kHeads> feed{};
        for (int h = 0; h < kHeads; ++h) {
            const auto i = static_cast<size_t>(h);
            feed[i] = multiace::head_feed(mode_, head_manual_[i], head_feeder_[i]);
        }
        for (size_t pos = 0; pos < units_.size(); ++pos) {
            const multiace::Unit& u = units_[pos];
            AmsUnit unit;
            unit.unit_index = static_cast<int>(pos) + 1;
            unit.name = fmt::format("ACE {}", u.idx + 1);
            unit.display_name = unit.name;
            unit.slot_count = multiace::kBaysPerUnit;
            unit.first_slot_global_index = multiace::bay_global(u.idx, 0);
            unit.connected = u.connected;
            unit.firmware_version = u.firmware;
            if (u.temp || u.humidity) {
                EnvironmentData env;
                env.temperature_c = u.temp.value_or(0.0f);
                env.humidity_pct = u.humidity.value_or(0.0f);
                env.has_humidity = u.humidity.has_value();
                unit.environment = env;
            }
            if (mode_ == multiace::Mode::HEAD) {
                unit.topology = PathTopology::HUB;
                unit.hub_tool_label =
                    multiace::head_for_bay(mode_, u.idx, 0, head_ace_, feed).value_or(-1);
            } else {
                unit.topology = PathTopology::PARALLEL;
            }

            for (int s = 0; s < multiace::kBaysPerUnit; ++s) {
                SlotInfo slot;
                slot.slot_index = s;
                slot.global_index = multiace::bay_global(u.idx, s);
                const auto bay = std::find_if(u.bays.begin(), u.bays.end(),
                                              [s](const multiace::Bay& b) { return b.index == s; });
                if (bay != u.bays.end()) {
                    slot.status = bay->status;
                    slot.material = bay->material;
                    slot.brand = bay->brand;
                    if (bay->color) {
                        slot.color_rgb = *bay->color;
                    }
                }
                const auto head = multiace::head_for_bay(mode_, u.idx, s, head_ace_, feed);
                if (head) {
                    slot.mapped_tool = *head;
                    slot.extruder_name = head_extruder_name(*head);
                    if (routed_bay_locked(*head) == std::make_pair(u.idx, s)) {
                        slot.status = SlotStatus::LOADED;
                    }
                }
                unit.slots.push_back(std::move(slot));
            }
            total = std::max(total, unit.first_slot_global_index + unit.slot_count);
            system_info_.units.push_back(std::move(unit));
        }
    }
    system_info_.total_slots = total;
}

// ============================================================================
// Base hooks
// ============================================================================

bool AmsBackendMultiAce::head_is_upstream_fed_locked(int head) const {
    return ace_fed_locked(head);
}

bool AmsBackendMultiAce::preload_finish_ends_unload_locked(int head) const {
    // multiACE's FEED_AUTO unload leaves an ACE head's channel at
    // preload_finish: the filament is back in the ACE, not at the head's port.
    return ace_fed_locked(head);
}

SlotInfo* AmsBackendMultiAce::cached_slot_locked(int slot_index) {
    if (slot_index >= kHeads) {
        return nullptr;
    }
    return AmsBackendSnapmaker::cached_slot_locked(slot_index);
}

// ============================================================================
// Queries
// ============================================================================

PathTopology AmsBackendMultiAce::get_unit_topology(int unit_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (unit_index >= 0 && unit_index < static_cast<int>(system_info_.units.size())) {
        return system_info_.units[static_cast<size_t>(unit_index)].topology;
    }
    return get_topology();
}

PathSegment AmsBackendMultiAce::get_slot_filament_segment(int slot_index) const {
    int routed_head = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (slot_index < kHeads) {
            routed_head = slot_index;
        } else {
            const SlotInfo* slot = system_info_.get_slot_global(slot_index);
            const auto head = head_for_global_locked(slot_index);
            if (!slot) {
                return PathSegment::NONE;
            }
            if (head && routed_bay_locked(*head) == multiace::bay_of(slot_index)) {
                routed_head = *head;
            } else {
                // Filament an ACE holds back sits at its gate.
                return slot->is_present() ? PathSegment::PREP : PathSegment::NONE;
            }
        }
    }
    return AmsBackendSnapmaker::get_slot_filament_segment(routed_head);
}

bool AmsBackendMultiAce::needs_unload_before_load(const AmsSystemInfo& info,
                                                  int target_slot) const {
    if (multiace::bay_of(target_slot)) {
        return false;
    }
    return AmsBackendSnapmaker::needs_unload_before_load(info, target_slot);
}

bool AmsBackendMultiAce::can_unload_from_toolhead(int slot_index) const {
    int head = slot_index;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (slot_index >= kHeads) {
            const auto h = head_for_global_locked(slot_index);
            if (!h || routed_bay_locked(*h) != multiace::bay_of(slot_index)) {
                return false;
            }
            head = *h;
        }
    }
    return AmsBackendSnapmaker::can_unload_from_toolhead(head);
}

bool AmsBackendMultiAce::slot_has_filament_at_toolhead(int slot_index) const {
    int head = slot_index;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (slot_index >= kHeads) {
            const auto h = head_for_global_locked(slot_index);
            if (!h || routed_bay_locked(*h) != multiace::bay_of(slot_index)) {
                return false;
            }
            head = *h;
        }
    }
    return AmsBackendSnapmaker::slot_has_filament_at_toolhead(head);
}

bool AmsBackendMultiAce::slot_is_actively_loaded(int slot_index) const {
    if (slot_index < kHeads) {
        return AmsBackendSnapmaker::slot_is_actively_loaded(slot_index);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const SlotInfo* slot = system_info_.get_slot_global(slot_index);
    return slot && slot->status == SlotStatus::LOADED;
}

// ============================================================================
// Filament operations
// ============================================================================

AmsError AmsBackendMultiAce::do_load_filament(int slot_index) {
    int head = -1;
    std::optional<std::pair<int, int>> bay;
    std::optional<std::pair<int, int>> routed;
    bool bay_empty = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (slot_index >= 0 && slot_index < kHeads) {
            head = ace_fed_locked(slot_index) ? slot_index : -1;
        } else if (const auto h = head_for_global_locked(slot_index)) {
            head = *h;
            bay = multiace::bay_of(slot_index);
            routed = routed_bay_locked(head);
            const SlotInfo* slot = system_info_.get_slot_global(slot_index);
            bay_empty = !slot || slot->status == SlotStatus::EMPTY;
        } else if (multiace::bay_of(slot_index)) {
            return AmsErrorHelper::not_supported("Loading an ACE bay no toolhead is wired to");
        }
    }

    if (head < 0) {
        // A feeder or manual head, no engine contract, or no slot at all: the
        // U1's own path, which validates the index.
        return AmsBackendSnapmaker::do_load_filament(slot_index);
    }

    const bool head_loaded = AmsBackendSnapmaker::slot_has_filament_at_toolhead(head);
    if (!bay) {
        if (head_loaded) {
            return AmsErrorHelper::slot_ineligible(
                lane_noun(), slot_index,
                filament_op_eligibility_reason(FilamentOpEligibility::AlreadyLoaded));
        }
        return dispatch_feed(multiace::load_gcode(head, std::nullopt), EVENT_LOAD_COMPLETE);
    }

    // The engine swaps bays mid-print itself, as the sliced file asks; a bay
    // change from the screen while a print holds the heads is refused.
    if (auto err = refuse_if_printing(/*paused_blocks=*/true); !err.success()) {
        return err;
    }
    if (bay_empty) {
        return AmsErrorHelper::slot_not_available(lane_noun(), slot_index);
    }
    if (routed == bay && head_loaded) {
        return AmsErrorHelper::slot_ineligible(
            lane_noun(), slot_index,
            filament_op_eligibility_reason(FilamentOpEligibility::AlreadyLoaded));
    }

    // ACE_LOAD_HEAD answers OK and feeds nothing into a head that already
    // holds filament, so a head holding another bay is unloaded in the same
    // script. Klipper stops the script at the first line that raises.
    std::string gcode = multiace::load_gcode(head, bay);
    if (head_loaded || routed) {
        gcode = multiace::unload_gcode(head) + "\n" + gcode;
    }
    return dispatch_feed(std::move(gcode), EVENT_LOAD_COMPLETE);
}

AmsError AmsBackendMultiAce::do_unload_filament(int slot_index) {
    int head = slot_index;
    bool ace_head = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (head < 0) {
            head = system_info_.current_slot;
        }
        if (head >= kHeads) {
            const auto h = head_for_global_locked(head);
            if (!h || routed_bay_locked(*h) != multiace::bay_of(head)) {
                return AmsErrorHelper::not_loaded();
            }
            head = *h;
        }
        ace_head = ace_fed_locked(head);
    }
    if (!ace_head) {
        return AmsBackendSnapmaker::do_unload_filament(head);
    }
    return dispatch_feed(multiace::unload_gcode(head), EVENT_UNLOAD_COMPLETE);
}

AmsError AmsBackendMultiAce::do_select_slot(int slot_index) {
    if (slot_index < kHeads) {
        return AmsBackendSnapmaker::do_select_slot(slot_index);
    }
    int head = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto h = head_for_global_locked(slot_index);
        if (h && routed_bay_locked(*h) == multiace::bay_of(slot_index)) {
            head = *h;
        }
    }
    if (head < 0) {
        // Selecting a bay is a tool change to its head, which prints that bay
        // only once it is the head's routed bay.
        return AmsErrorHelper::slot_ineligible(
            lane_noun(), slot_index,
            filament_op_eligibility_reason(FilamentOpEligibility::NotLoaded));
    }
    return do_change_tool(head);
}

AmsError AmsBackendMultiAce::do_filament_batch(const std::vector<int>& slots, bool load) {
    std::vector<int> heads;
    std::vector<bool> ace_heads;
    std::vector<int> loaded_heads;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_locked()) {
            for (int s : slots) {
                int head = s;
                if (s >= kHeads) {
                    const auto h = head_for_global_locked(s);
                    if (load || !h || routed_bay_locked(*h) != multiace::bay_of(s)) {
                        return AmsErrorHelper::not_supported("Batch load from ACE bays");
                    }
                    head = *h;
                }
                if (std::find(heads.begin(), heads.end(), head) == heads.end()) {
                    heads.push_back(head);
                    ace_heads.push_back(ace_fed_locked(head));
                }
            }
        }
    }
    const bool any_ace = std::find(ace_heads.begin(), ace_heads.end(), true) != ace_heads.end();
    if (!any_ace) {
        return AmsBackendSnapmaker::do_filament_batch(slots, load);
    }
    if (load) {
        return AmsErrorHelper::not_supported("Batch load into ACE-fed toolheads");
    }

    for (int h = 0; h < kHeads; ++h) {
        if (AmsBackendSnapmaker::slot_has_filament_at_toolhead(h)) {
            loaded_heads.push_back(h);
        }
    }
    std::vector<int> sorted = heads;
    std::sort(sorted.begin(), sorted.end());
    if (!loaded_heads.empty() && sorted == loaded_heads) {
        // The engine's own unload-all, whose sequencing covers every loaded
        // head, ACE-fed or not.
        return dispatch_feed(multiace::unload_all_gcode(), EVENT_UNLOAD_COMPLETE);
    }

    std::string script;
    for (size_t i = 0; i < heads.size(); ++i) {
        if (!script.empty()) {
            script += "\n";
        }
        script += ace_heads[i] ? multiace::unload_gcode(heads[i])
                               : batch_feed_gcode({heads[i]}, /*load=*/false);
    }
    return dispatch_feed(std::move(script), EVENT_UNLOAD_COMPLETE);
}

// ============================================================================
// Dryer
// ============================================================================

DryerInfo AmsBackendMultiAce::get_dryer_info(int unit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const multiace::Unit* u = unit_for_index_locked(unit)) {
        return u->dryer;
    }
    return DryerInfo{.supported = false};
}

AmsError AmsBackendMultiAce::start_drying(float temp_c, int duration_min, int fan_pct, int unit) {
    (void)fan_pct;
    auto err = check_preconditions();
    if (!err.success()) {
        return err;
    }
    int ace = -1;
    DryerInfo dryer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (const multiace::Unit* u = unit_for_index_locked(unit)) {
            ace = u->idx;
            dryer = u->dryer;
        }
    }
    if (ace < 0) {
        return AmsErrorHelper::not_supported("Drying on a unit with no ACE");
    }
    if (auto refusal = helix::ace::check_dryer_request(dryer, temp_c, duration_min)) {
        return *refusal;
    }
    spdlog::info("{} Drying ACE {}: {}°C for {} minutes", backend_log_tag(), ace + 1, temp_c,
                 duration_min);
    return execute_gcode(multiace::dry_gcode(ace, static_cast<int>(temp_c), duration_min));
}

AmsError AmsBackendMultiAce::stop_drying(int unit) {
    int ace = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (const multiace::Unit* u = unit_for_index_locked(unit)) {
            ace = u->idx;
        }
    }
    if (ace < 0) {
        return AmsErrorHelper::not_supported("Drying on a unit with no ACE");
    }
    spdlog::info("{} Stopping drying on ACE {}", backend_log_tag(), ace + 1);
    return execute_gcode(multiace::stop_dry_gcode(ace));
}

AmsError AmsBackendMultiAce::update_drying(float temp_c, int duration_min, int fan_pct, int unit) {
    auto err = stop_drying(unit);
    if (!err.success()) {
        return err;
    }
    const DryerInfo dryer = get_dryer_info(unit);
    const auto [target_temp, target_duration] =
        helix::ace::restart_request(dryer, temp_c, duration_min);
    return start_drying(target_temp, target_duration, fan_pct, unit);
}

std::vector<DryingPreset> AmsBackendMultiAce::get_drying_presets() const {
    return get_default_drying_presets();
}

// ============================================================================
// Errors
// ============================================================================

std::optional<helix::ErrorEvent>
AmsBackendMultiAce::classify_error(const std::string& raw_line,
                                   const helix::ClassifyContext& ctx) const {
    if (!helix::is_bang_line(raw_line)) {
        return AmsBackendSnapmaker::classify_error(raw_line, ctx);
    }
    const std::string detail = helix::strip_bang_prefix(raw_line);
    const auto code = snapmaker::decode_exception_code(detail);
    if (!code || code->id != kMultiAceFaultId || code->code < kMultiAceCodeFirst ||
        code->code > kMultiAceCodeLast) {
        return AmsBackendSnapmaker::classify_error(raw_line, ctx);
    }

    const std::string token =
        fmt::format("{:04d}-{:04d}-{:04d}-{:04d}", code->level, code->id, code->index, code->code);
    std::string message = detail;
    if (const auto at = message.find(token); at != std::string::npos) {
        message.erase(at, token.size());
    }
    const auto first = message.find_first_not_of(" :");
    message = first == std::string::npos ? std::string{} : message.substr(first);
    while (!message.empty() && message.back() == ' ') {
        message.pop_back();
    }

    const int head = std::clamp(code->index, 0, kHeads - 1);
    std::vector<helix::RecoveryAction> actions;
    actions.push_back({lv_tr("OK"), "", "ams_backend_multiace::fault_dismiss", "", false});
    helix::ErrorEvent e = helix::make_ams_fault_event(
        helix::ErrorSource::SNAPMAKER, lv_tr("Filament operation failed"),
        fmt::format("{}: {}", AmsErrorHelper::position_label(tool_noun(), head), message),
        std::move(actions));
    e.code = token;
    e.raw_detail = detail;
    // Idle, the fault is the engine refusing a command the user just sent.
    e.command_refusal = !ctx.is_printing && !ctx.is_paused;
    return e;
}

} // namespace helix

#endif // HELIX_HAS_SNAPMAKER
