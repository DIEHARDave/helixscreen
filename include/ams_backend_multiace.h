// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#if HELIX_HAS_SNAPMAKER

#include "ams_backend_snapmaker.h"
#include "multiace_status.h"

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace helix {

/**
 * @file ams_backend_multiace.h
 * @brief A Snapmaker U1 with multiACE: ACE Pro units upstream of its heads.
 *
 * The U1 stays the printer. Its four heads, `T<n>`, channel_state, pre-print
 * remap, preferences and resume all stay AmsBackendSnapmaker's, on global
 * slots 0..3. Each ACE unit multiACE reports becomes one more unit whose four
 * bays sit at multiace::bay_global(ace, bay); a bay feeds the head its mode
 * routes it to. The engine is driven through its gcode contract only
 * (docs/devel/FILAMENT_BACKEND_MULTIACE.md).
 *
 * Until a frame shows api_version 1 and a multi or head mode, this backend
 * answers exactly as the base does: no ACE units, the stock feed path.
 */
class AmsBackendMultiAce : public AmsBackendSnapmaker {
  public:
    AmsBackendMultiAce(IMoonrakerAPI* api, helix::IMoonrakerClient* client);

    /// Unit 0 is the heads (PARALLEL). An ACE unit is a HUB in head mode, its
    /// four bays merging into the one head wired to it, and PARALLEL in multi
    /// mode, bay s feeding head s.
    [[nodiscard]] PathTopology get_unit_topology(int unit_index) const override;

    [[nodiscard]] PathSegment get_slot_filament_segment(int slot_index) const override;

    /// A bay into a head holding another bay is not a swap for the screen to
    /// sequence: do_load_filament() sends the head's unload ahead of the load.
    [[nodiscard]] bool needs_unload_before_load(const AmsSystemInfo& info,
                                                int target_slot) const override;

    [[nodiscard]] bool can_unload_from_toolhead(int slot_index) const override;
    [[nodiscard]] bool slot_has_filament_at_toolhead(int slot_index) const override;
    [[nodiscard]] bool slot_is_actively_loaded(int slot_index) const override;

    /// Unit 0 has no dryer; every ACE unit has the ACE Pro's.
    [[nodiscard]] DryerInfo get_dryer_info(int unit = 0) const override;
    AmsError start_drying(float temp_c, int duration_min, int fan_pct = -1, int unit = 0) override;
    AmsError stop_drying(int unit = 0) override;
    AmsError update_drying(float temp_c = -1, int duration_min = -1, int fan_pct = -1,
                           int unit = 0) override;
    [[nodiscard]] std::vector<DryingPreset> get_drying_presets() const override;

    /// multiACE raises its own faults coded (id 525, a 200-band code, the head
    /// as index); they are filed as a filament error on that head. Everything
    /// else is the base's.
    [[nodiscard]] std::optional<helix::ErrorEvent>
    classify_error(const std::string& raw_line, const helix::ClassifyContext& ctx) const override;

  protected:
    AmsError do_load_filament(int slot_index) override;
    AmsError do_unload_filament(int slot_index) override;
    AmsError do_select_slot(int slot_index) override;
    AmsError do_filament_batch(const std::vector<int>& slots, bool load) override;

    [[nodiscard]] bool head_is_upstream_fed_locked(int head) const override;
    [[nodiscard]] bool preload_finish_ends_unload_locked(int head) const override;
    void apply_extension_locked(const nlohmann::json& status, bool& changed) override;

    /// Bays are painted from each frame and have no lane records to repaint
    /// from; a lane repaint would blank them.
    SlotInfo* cached_slot_locked(int slot_index) override;

    const char* backend_log_tag() const override {
        return "[AMS multiACE]";
    }

  private:
    static constexpr int kHeads = multiace::kHeadCount;

    /// The engine contract is in force: api_version 1 in multi or head mode.
    /// Caller holds mutex_.
    [[nodiscard]] bool active_locked() const;

    /// Head @p head is fed by an ACE. Caller holds mutex_.
    [[nodiscard]] bool ace_fed_locked(int head) const;

    /// The bay routed to @p head, when one is and its load did not fail.
    /// Caller holds mutex_.
    [[nodiscard]] std::optional<std::pair<int, int>> routed_bay_locked(int head) const;

    /// The head bay @p global feeds, when @p global names a bay of a unit
    /// this backend shows. Caller holds mutex_.
    [[nodiscard]] std::optional<int> head_for_global_locked(int global) const;

    /// Position in units_ of the unit whose idx is @p ace, or -1.
    [[nodiscard]] int unit_position_locked(int ace) const;

    /// The ACE unit an AmsUnit index names, or nullptr for unit 0 and indices
    /// beyond the units shown. Caller holds mutex_.
    [[nodiscard]] const multiace::Unit* unit_for_index_locked(int unit_index) const;

    /// Replaces every unit after unit 0 from the held engine state, and
    /// total_slots with them. Caller holds mutex_.
    void rebuild_units_locked();

    // Engine state, merged across delta frames. All under mutex_.
    std::optional<int> api_version_;
    multiace::Mode mode_ = multiace::Mode::UNKNOWN;
    std::array<int, kHeads> head_ace_{0, 1, 2, 3};
    std::array<bool, kHeads> head_feeder_{};
    std::array<bool, kHeads> head_manual_{};
    std::array<std::optional<multiace::HeadSource>, kHeads> head_source_{};
    std::vector<multiace::Unit> units_;
    bool swap_in_progress_ = false;
    bool unload_all_active_ = false;
    std::string swap_phase_;
    multiace::LastSwap last_swap_;
    int64_t event_seq_ = -1;
    /// api_version mismatch already logged, so a stream of frames logs once.
    bool api_mismatch_logged_ = false;
};

} // namespace helix

#endif // HELIX_HAS_SNAPMAKER
