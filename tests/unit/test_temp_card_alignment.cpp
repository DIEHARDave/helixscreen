// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_temp_card_alignment.cpp
 * @brief The print status temp card keeps its columns still: every row's "/"
 *        sits at one x, and each row's state glyph holds its x while the duty
 *        text changes width.
 *
 * The card runs on a multi-tool printer with the nozzle's tool digit showing,
 * at a compact breakpoint (label column hidden, status column sized to its
 * content) and at MEDIUM (label and status columns fixed).
 */

#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"
#include "printer_temperature_state.h"
#include "tool_state.h"

#include <array>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

namespace {

// ui_temp_display_create_cb() creates the separator as its second label.
constexpr int kSeparatorChild = 1;
constexpr int kStateHeating = 1;

constexpr std::array<const char*, 3> kRows = {"nozzle_row", "bed_row", "chamber_row"};
constexpr std::array<const char*, 3> kDisplays = {"nozzle_temp", "bed_temp",
                                                  "chamber_temp_display"};
constexpr std::array<const char*, 3> kDuty = {"print_nozzle_status", "print_bed_status",
                                              "print_chamber_status"};
constexpr std::array<const char*, 3> kState = {
    "print_nozzle_status_state", "print_bed_status_state", "print_chamber_status_state"};

struct Breakpoint {
    int value;
    int32_t card_width;
};

class TempCardFixture : public XMLTestFixture {
  public:
    TempCardFixture() : XMLTestFixture() {
        // The print status panel owns these; the card only binds them. The
        // registry is global and outlives this fixture, so the storage is static.
        static std::array<lv_subject_t, 3> state_subjects{};
        static std::array<lv_subject_t, 3> duty_subjects{};
        static std::array<std::array<char, 16>, 3> duty_bufs{};
        for (size_t i = 0; i < kRows.size(); i++) {
            if (!lv_xml_get_subject(nullptr, kState[i])) {
                lv_subject_init_int(&state_subjects[i], 0);
                lv_xml_register_subject(nullptr, kState[i], &state_subjects[i]);
            }
            if (!lv_xml_get_subject(nullptr, kDuty[i])) {
                lv_subject_init_string(&duty_subjects[i], duty_bufs[i].data(), nullptr,
                                       duty_bufs[i].size(), "");
                lv_xml_register_subject(nullptr, kDuty[i], &duty_subjects[i]);
            }
        }
        if (!lv_xml_get_subject(nullptr, "show_tool_badge"))
            helix::ToolState::instance().init_subjects(true);
        REQUIRE(register_component("components/heater_icon"));
        REQUIRE(register_component("components/temp_card_unified"));

        breakpoint_ = lv_xml_get_subject(nullptr, "ui_breakpoint");
        REQUIRE(breakpoint_ != nullptr);
        original_breakpoint_ = lv_subject_get_int(breakpoint_);
        has_chamber_ = lv_xml_get_subject(nullptr, "printer_has_chamber");
        REQUIRE(has_chamber_ != nullptr);
        original_has_chamber_ = lv_subject_get_int(has_chamber_);
        lv_subject_set_int(has_chamber_, 1);

        lv_subject_copy_string(lv_xml_get_subject(nullptr, "tool_badge_text"), "4");
        lv_subject_set_int(lv_xml_get_subject(nullptr, "show_tool_badge"), 1);
    }

    ~TempCardFixture() override {
        if (holder_ && lv_obj_is_valid(holder_))
            lv_obj_delete(holder_);
        lv_subject_set_int(lv_xml_get_subject(nullptr, "show_tool_badge"), 0);
        lv_subject_set_int(has_chamber_, original_has_chamber_);
        lv_subject_set_int(breakpoint_, original_breakpoint_);
    }

    void build(const Breakpoint& bp) {
        lv_subject_set_int(breakpoint_, bp.value);
        holder_ = lv_obj_create(test_screen());
        lv_obj_set_size(holder_, bp.card_width, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(holder_, 0, LV_PART_MAIN);
        card_ = static_cast<lv_obj_t*>(lv_xml_create(holder_, "temp_card_unified", nullptr));
        REQUIRE(card_ != nullptr);
    }

    void set_temps(int current_c, int target_c, size_t row) {
        auto& temps = state().temperature_state();
        lv_subject_t* current[] = {temps.get_active_extruder_temp_subject(),
                                   temps.get_bed_temp_subject(), temps.get_chamber_temp_subject()};
        lv_subject_t* target[] = {temps.get_active_extruder_target_subject(),
                                  temps.get_bed_target_subject(),
                                  lv_xml_get_subject(nullptr, "chamber_effective_target")};
        lv_subject_set_int(current[row], current_c * 10);
        lv_subject_set_int(target[row], target_c * 10);
    }

    static void set_status(size_t row, int state, const char* duty) {
        lv_subject_set_int(lv_xml_get_subject(nullptr, kState[row]), state);
        lv_subject_copy_string(lv_xml_get_subject(nullptr, kDuty[row]), duty);
    }

    void settle() {
        UpdateQueue::instance().drain();
        lv_obj_update_layout(holder_);
    }

    [[nodiscard]] lv_obj_t* named(lv_obj_t* parent, const char* name) const {
        lv_obj_t* obj = lv_obj_find_by_name(parent, name);
        REQUIRE(obj != nullptr);
        return obj;
    }

    [[nodiscard]] int32_t slash_x(size_t row) const {
        lv_obj_t* separator =
            lv_obj_get_child(named(named(card_, kRows[row]), kDisplays[row]), kSeparatorChild);
        REQUIRE(separator != nullptr);
        lv_area_t area;
        lv_obj_get_coords(separator, &area);
        return area.x1;
    }

    [[nodiscard]] int32_t heating_glyph_x(size_t row) const {
        lv_obj_t* glyph = named(named(card_, kRows[row]), "status_heating");
        REQUIRE_FALSE(lv_obj_has_flag(glyph, LV_OBJ_FLAG_HIDDEN));
        lv_area_t area;
        lv_obj_get_coords(glyph, &area);
        return area.x1;
    }

    lv_obj_t* card_ = nullptr;

  private:
    lv_obj_t* holder_ = nullptr;
    lv_subject_t* breakpoint_ = nullptr;
    lv_subject_t* has_chamber_ = nullptr;
    int original_breakpoint_ = 0;
    int original_has_chamber_ = 0;
};

// U1-sized compact card, and a MEDIUM card with the label column showing.
const Breakpoint kBreakpoints[] = {{2, 182}, {3, 320}};

} // namespace

TEST_CASE_METHOD(TempCardFixture, "temp card puts every row's slash at one x",
                 "[temp_card][align_current]") {
    for (const auto& bp : kBreakpoints) {
        build(bp);
        // Two-, three- and decimal-digit readings against short and long duty text,
        // with the tool digit beside the nozzle glyph only.
        const int temps[][3][2] = {
            {{25, 0}, {220, 220}, {100, 250}},
            {{220, 220}, {25, 0}, {45, 60}},
            {{100, 250}, {60, 60}, {25, 0}},
        };
        const char* duties[] = {"0%", "100%"};
        for (const auto& combo : temps) {
            for (const char* duty : duties) {
                for (size_t r = 0; r < kRows.size(); r++) {
                    set_temps(combo[r][0], combo[r][1], r);
                    set_status(r, kStateHeating, r == 0 ? duty : "35%");
                }
                settle();
                INFO("breakpoint " << bp.value << " nozzle " << combo[0][0] << " bed "
                                   << combo[1][0] << " chamber " << combo[2][0] << " duty "
                                   << duty);
                CHECK(slash_x(0) == slash_x(1));
                CHECK(slash_x(1) == slash_x(2));
            }
        }
        lv_obj_delete(lv_obj_get_parent(card_));
    }
}

TEST_CASE_METHOD(TempCardFixture, "temp card state glyph holds its x as the duty text changes",
                 "[temp_card][heater_status]") {
    for (const auto& bp : kBreakpoints) {
        build(bp);
        for (size_t r = 0; r < kRows.size(); r++)
            set_temps(200, 220, r);

        std::array<int32_t, 3> at_zero{};
        for (size_t r = 0; r < kRows.size(); r++)
            set_status(r, kStateHeating, "0%");
        settle();
        for (size_t r = 0; r < kRows.size(); r++)
            at_zero[r] = heating_glyph_x(r);

        for (size_t r = 0; r < kRows.size(); r++)
            set_status(r, kStateHeating, "100%");
        settle();
        for (size_t r = 0; r < kRows.size(); r++) {
            INFO("breakpoint " << bp.value << " row " << kRows[r]);
            CHECK(heating_glyph_x(r) == at_zero[r]);
            CHECK(at_zero[r] == at_zero[0]);
        }
        lv_obj_delete(lv_obj_get_parent(card_));
    }
}
