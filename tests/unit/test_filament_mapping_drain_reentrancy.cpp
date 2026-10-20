// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_mapping_drain_reentrancy.cpp
 * @brief rebuild_compact_view() runs no queued work under itself (#1221).
 *
 * NavigationManager::go_back() is fully deferred, so a pop of the print-detail
 * overlay can sit in the queue when set_used_tools() reaches
 * rebuild_compact_view(). The pop deletes the container and calls
 * on_ui_destroyed(). The rebuild must finish against the live container, the
 * pop must run on the next tick, and the card must stay usable after it.
 */

#include "ui_filament_mapping_card.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "../lvgl_test_fixture.h"
#include "../mapping_card_render_fixture.h"

#include <set>

#include "../catch_amalgamated.hpp"

using helix::ui::FilamentMappingCard;

namespace {

struct MappingCardFixture : public LVGLTestFixture {
    FilamentMappingCard card;
    lv_obj_t* card_widget = nullptr;
    lv_obj_t* rows = nullptr;
    lv_obj_t* warning = nullptr;

    MappingCardFixture() {
        card_widget = lv_obj_create(test_screen());
        rows = lv_obj_create(card_widget);
        warning = lv_obj_create(card_widget);
        card.create(card_widget, rows, warning);
    }
};

} // namespace

TEST_CASE_METHOD(MappingCardFixture, "rebuild_compact_view: a pop queued ahead of it runs after it",
                 "[filament][mapping][filament_mapping][crash][1221][drain_reentrancy]") {
    // Stand in for the deferred go_back() that pops the print-detail overlay:
    // the same two steps destroy_overlay_ui() takes, in the same order.
    bool ran = false;
    helix::ui::queue_update([this, &ran]() {
        ran = true;
        helix::ui::safe_delete_deferred(rows); // nulls `rows`
        card.on_ui_destroyed();                // nulls rows_container_ / card_ / warning_container_
    });

    // set_used_tools() is the entry point the crash came through.
    REQUIRE_NOTHROW(card.set_used_tools(std::set<int>{0}));
    CHECK_FALSE(ran);
    CHECK(rows != nullptr);

    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(ran);
    REQUIRE(rows == nullptr);

    // The card outlives its widgets: a later call finds no container and does nothing.
    REQUIRE_NOTHROW(card.set_used_tools(std::set<int>{1}));
}

TEST_CASE_METHOD(MappingCardRenderFixture,
                 "rebuild_compact_view: still rebuilds normally when nothing is queued",
                 "[filament][mapping][filament_mapping][1221]") {
    // A bare early-return in rebuild_compact_view() would satisfy the case
    // above while silently disabling the card. With an empty queue the
    // container must survive AND be re-rendered.
    //
    // MappingCardRenderFixture, NOT the MappingCardFixture the crash test uses.
    // rebuild_compact_view() styles and populates the container only for a card
    // it is going to show, and "show" needs a started AMS backend and a palette
    // - neither of which that fixture has. On it, the function returns before
    // lv_obj_set_flex_flow() ever runs, so every assertion here read an
    // untouched container, and LV_FLEX_FLOW_ROW is 0x00, the LVGL style default
    // for a property with no case in lv_style_prop_get_default(). The flow check
    // passed whether or not the production call existed.
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PETG"});
    REQUIRE(card.should_show());
    REQUIRE(lv_obj_get_child_count(rows) == 2);

    // ONE tool out of a two-tool palette, deliberately: it changes the render
    // fingerprint, so this goes through a real rebuild rather than the
    // idempotent-render early-out that an identical set would hit. Without that
    // the chip count below would be describing update()'s render, not this one,
    // and the early-return this case exists to catch would sail through.
    REQUIRE_NOTHROW(card.set_used_tools(std::set<int>{0}));

    REQUIRE(rows != nullptr);
    REQUIRE(lv_obj_is_valid(rows));
    // The rebuild genuinely ran: 2 chips became 1. This is what makes the flow
    // assertion below a statement about code that executed.
    REQUIRE(lv_obj_get_child_count(rows) == 1);
    // ROW, not ROW_WRAP: the chips live in one fixed-height row, so a wrapped
    // second row would be clipped rather than shown - rebuild_compact_view caps
    // them at what fits and renders a "+N" pill for the rest instead.
    CHECK(lv_obj_get_style_flex_flow(rows, LV_PART_MAIN) == LV_FLEX_FLOW_ROW);
}
