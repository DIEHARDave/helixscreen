// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file mapping_card_render_fixture.h
 * @brief Shared fixture for tests that render FilamentMappingCard for real.
 *
 * The pure-seam mapping tests need no LVGL; the render tests do, so they need
 * LVGLUITestFixture (XML component registration — lv_xml_create must build
 * filament_swatch) plus a started AmsBackendMock so update() passes its
 * availability/editable-mapping gates. Widget wiring mirrors MappingCardFixture
 * (test_filament_mapping_drain_reentrancy.cpp); the AMS is a ScopedMockAms.
 *
 * Lives in its own header because more than one test file renders the card.
 */

#include "ui_filament_mapping_card.h"
#include "ui_update_queue.h"

#include "ams_backend_mock.h"
#include "lvgl_ui_test_fixture.h"
#include "test_helpers/scoped_mock_ams.h"

struct MappingCardRenderFixture : LVGLUITestFixture {
    helix::ui::FilamentMappingCard card;
    /// Declared after the card, so the AMS is removed before the card goes.
    helix::test::ScopedMockAms ams;
    helix::AmsBackendMock* mock = nullptr;
    lv_obj_t* card_widget = nullptr;
    lv_obj_t* rows = nullptr;
    lv_obj_t* warning = nullptr;

    /// @param slot_count AMS backend slot count. Defaults to 4, matching every
    ///        existing caller, so this stays default-constructible; a derived
    ///        fixture can pass 1 to force the single-slot case the
    ///        multi-tool-vs-single-extruder visibility rule branches on.
    explicit MappingCardRenderFixture(int slot_count = 4) : ams(slot_count), mock(ams.backend) {
        card_widget = lv_obj_create(test_screen());
        rows = lv_obj_create(card_widget);
        warning = lv_obj_create(card_widget);
        card.create(card_widget, rows, warning);
    }
};
