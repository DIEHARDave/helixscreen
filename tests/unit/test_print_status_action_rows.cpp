// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_action_rows.cpp
 * @brief PrintStatusLayoutFitter's action-row decision, wired to live widgets
 *
 * The fitter decides from label text whether an action row's buttons fit with
 * each icon beside its label, and re-decides when a label resizes or a button
 * is shown. These tests drive those events and check both the published
 * subject and the buttons bound to it (prestonbrown/helixscreen#1307).
 */

#include "ui_breakpoint.h"
#include "ui_button.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "../test_helpers/scoped_breakpoint.h"
#include "../test_helpers/update_queue_test_access.h"
#include "print_status_layout_fitter.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::PrintStatusLayoutFitter;

namespace {

/// The fitter's subjects, owned by the test the way the panel owns them.
struct FitterSubjects {
    lv_subject_t density{}, aux_icon{}, aux_full{}, aux_short{}, fans_fit{}, graph_fits{};
    lv_subject_t row1_stacked{}, row1_tall{}, row2_stacked{}, row2_tall{};

    FitterSubjects() {
        for (lv_subject_t* s : all()) {
            lv_subject_init_int(s, 0);
        }
        lv_xml_register_subject(nullptr, "test_ps_row2_stacked", &row2_stacked);
    }
    ~FitterSubjects() {
        for (lv_subject_t* s : all()) {
            lv_subject_deinit(s);
        }
    }
    std::vector<lv_subject_t*> all() {
        return {&density,    &aux_icon,     &aux_full,  &aux_short,    &fans_fit,
                &graph_fits, &row1_stacked, &row1_tall, &row2_stacked, &row2_tall};
    }
    PrintStatusLayoutFitter::Subjects view() {
        return {&density,    &aux_icon,     &aux_full,  &aux_short,    &fans_fit,
                &graph_fits, &row1_stacked, &row1_tall, &row2_stacked, &row2_tall};
    }
};

/// One action button. @p content_min is the row's content sharing
/// (style_min_width="content"); without it the button keeps its even share
/// and its row never relays out on its own when the label grows.
lv_obj_t* make_action_button(lv_obj_t* row, const char* name, const char* icon, const char* text,
                             bool content_min) {
    const char* attrs[] = {"name",
                           name,
                           "icon",
                           icon,
                           "text",
                           text,
                           "flex_grow",
                           "1",
                           "stacked_subject",
                           "test_ps_row2_stacked",
                           "stacked_if_eq",
                           "1",
                           content_min ? "style_min_width" : nullptr,
                           "content",
                           nullptr};
    return static_cast<lv_obj_t*>(lv_xml_create(row, "ui_button", attrs));
}

bool stacked(lv_obj_t* btn) {
    return lv_obj_get_style_flex_flow(btn, LV_PART_MAIN) == LV_FLEX_FLOW_COLUMN;
}

void settle(lv_obj_t* screen) {
    for (int i = 0; i < 3; ++i) {
        lv_obj_update_layout(screen);
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
    lv_obj_update_layout(screen);
}

/// Row 2 of the print-status grid: Tune and Cancel, with Reprint hidden.
struct ActionRowFixture : XMLTestFixture {
    helix::test::ScopedBreakpoint medium{UiBreakpoint::Medium};
    FitterSubjects subjects;
    lv_obj_t* root = nullptr;
    lv_obj_t* row = nullptr;
    lv_obj_t* tune = nullptr;
    lv_obj_t* cancel = nullptr;
    lv_obj_t* reprint = nullptr;
    PrintStatusLayoutFitter fitter;

    explicit ActionRowFixture(bool content_min = true)
        : fitter("ActionRowTest", state(), subjects.view(),
                 {[this]() { return root; }, []() { return true; }, []() { return false; }}) {
        root = lv_obj_create(test_screen());
        lv_obj_set_size(root, 320, 200);
        lv_obj_set_style_pad_all(root, 0, LV_PART_MAIN);
        row = lv_obj_create(root);
        lv_obj_set_size(row, 300, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_column(row, 7, LV_PART_MAIN);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        tune = make_action_button(row, "btn_tune", "tune", "Tune", content_min);
        cancel = make_action_button(row, "btn_cancel", "stop", "Cancel", content_min);
        reprint = make_action_button(row, "btn_reprint", "refresh", "Reprint this whole job",
                                     content_min);
        lv_obj_add_flag(reprint, LV_OBJ_FLAG_HIDDEN);
        settle(test_screen());
        fitter.recompute_actions_crowded();
        settle(test_screen());
    }
    ~ActionRowFixture() override {
        lv_obj_delete(root);
        settle(test_screen());
    }

    void require_all_stacked(bool want) {
        CHECK(lv_subject_get_int(&subjects.row2_stacked) == (want ? 1 : 0));
        CHECK(lv_subject_get_int(&subjects.row2_tall) == (want ? 1 : 0));
        for (lv_obj_t* btn : {tune, cancel}) {
            CHECK(stacked(btn) == want);
        }
    }
};

/// Buttons with the even share only: a growing label resizes nothing but itself.
struct EvenShareActionRowFixture : ActionRowFixture {
    EvenShareActionRowFixture() : ActionRowFixture(false) {}
};

} // namespace

TEST_CASE_METHOD(ActionRowFixture, "print-status action row stays side by side while it fits",
                 "[print_status][layout_fitter]") {
    require_all_stacked(false);
}

TEST_CASE_METHOD(EvenShareActionRowFixture,
                 "print-status action row stacks when a label outgrows it",
                 "[print_status][layout_fitter]") {
    REQUIRE(lv_subject_get_int(&subjects.row2_stacked) == 0);
    ui_button_set_text(tune, "Tune the print speed and flow now");
    settle(test_screen());
    require_all_stacked(true);

    ui_button_set_text(tune, "Tune");
    settle(test_screen());
    require_all_stacked(false);
}

TEST_CASE_METHOD(ActionRowFixture, "print-status action row stacks when a wide button appears",
                 "[print_status][layout_fitter]") {
    REQUIRE(lv_subject_get_int(&subjects.row2_stacked) == 0);
    lv_obj_remove_flag(reprint, LV_OBJ_FLAG_HIDDEN);
    settle(test_screen());
    require_all_stacked(true);
    CHECK(stacked(reprint));
}

// The fit counts each button's own padding, as its content minimum does: a
// row the fitter leaves side by side never has to squeeze a button below that
// minimum, at any width.
TEST_CASE_METHOD(ActionRowFixture, "print-status action row side by side never overflows",
                 "[print_status][layout_fitter]") {
    int side_by_side = 0;
    int stacked_rows = 0;
    for (int32_t w = 120; w <= 360; w += 2) {
        CAPTURE(w);
        lv_obj_set_width(row, w);
        settle(test_screen());
        fitter.recompute_actions_crowded();
        settle(test_screen());
        if (lv_subject_get_int(&subjects.row2_stacked) == 1) {
            ++stacked_rows;
            continue;
        }
        ++side_by_side;
        const int32_t used = lv_obj_get_width(tune) + lv_obj_get_width(cancel) +
                             lv_obj_get_style_pad_column(row, LV_PART_MAIN);
        CHECK(used <= lv_obj_get_content_width(row));
    }
    // Both outcomes were exercised, so the boundary between them was crossed.
    REQUIRE(side_by_side > 0);
    REQUIRE(stacked_rows > 0);
}
