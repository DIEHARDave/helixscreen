// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * Which shutdown dialog opens, and which scope its split buttons start on.
 *
 * A screen on the printer's own host gets one row of buttons that go through
 * Moonraker (or local power when Moonraker is down). A screen on a different
 * host gets the Both / Printer / Screen split buttons, starting on Screen: the
 * user is standing at the screen, and taking a printer down from another
 * machine is the choice that has to be made on purpose.
 */

#include "ui_split_button.h"

#include "../../src/ui/panel_widgets/shutdown_widget.h"
#include "../test_fixtures.h"
#include "async_lifetime_guard.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("Shutdown dialog plan: remote screen defaults to Screen", "[shutdown][power]") {
    SECTION("connected, live verdict remote") {
        const auto plan = plan_shutdown_dialog(/*api_connected=*/true,
                                               /*live_moonraker_remote=*/true,
                                               /*configured_host_same=*/false);
        CHECK(plan.mode == ShutdownDialogMode::Dual);
        CHECK(plan.dual_default == ShutdownScope::Screen);
    }

    SECTION("connected: the live verdict outranks a stale configured host") {
        const auto plan = plan_shutdown_dialog(/*api_connected=*/true,
                                               /*live_moonraker_remote=*/true,
                                               /*configured_host_same=*/true);
        CHECK(plan.mode == ShutdownDialogMode::Dual);
        CHECK(plan.dual_default == ShutdownScope::Screen);
    }

    SECTION("disconnected: the configured host decides") {
        const auto plan = plan_shutdown_dialog(/*api_connected=*/false,
                                               /*live_moonraker_remote=*/false,
                                               /*configured_host_same=*/false);
        CHECK(plan.mode == ShutdownDialogMode::Dual);
        CHECK(plan.dual_default == ShutdownScope::Screen);
    }
}

TEST_CASE("Shutdown dialog plan: same host stays single-scope", "[shutdown][power]") {
    SECTION("connected goes through Moonraker") {
        const auto plan = plan_shutdown_dialog(/*api_connected=*/true,
                                               /*live_moonraker_remote=*/false,
                                               /*configured_host_same=*/false);
        CHECK(plan.mode == ShutdownDialogMode::SingleViaMoonraker);
    }

    SECTION("disconnected falls back to local power") {
        const auto plan = plan_shutdown_dialog(/*api_connected=*/false,
                                               /*live_moonraker_remote=*/true,
                                               /*configured_host_same=*/true);
        CHECK(plan.mode == ShutdownDialogMode::SingleViaLocalPower);
    }
}

TEST_CASE_METHOD(XMLTestFixture, "Dual-scope shutdown modal opens on the requested scope",
                 "[shutdown][power]") {
    REQUIRE(register_component("shutdown_modal"));
    ui_split_button_init();

    ShutdownModal modal;
    auto noop = []() {};
    modal.set_dual_callbacks(noop, noop, noop, noop, noop, noop, ShutdownScope::Screen);
    REQUIRE(modal.show(test_screen()));
    process_lvgl(10);

    lv_obj_t* restart = lv_obj_find_by_name(modal.dialog(), "split_restart");
    lv_obj_t* shutdown = lv_obj_find_by_name(modal.dialog(), "split_shutdown");
    REQUIRE(restart != nullptr);
    REQUIRE(shutdown != nullptr);
    CHECK(ui_split_button_get_selected(restart) == static_cast<uint32_t>(ShutdownScope::Screen));
    CHECK(ui_split_button_get_selected(shutdown) == static_cast<uint32_t>(ShutdownScope::Screen));

    SECTION("a narrowed selection does not survive a reopen") {
        ui_split_button_set_selected(restart, static_cast<uint32_t>(ShutdownScope::Printer));
        ui_split_button_set_selected(shutdown, static_cast<uint32_t>(ShutdownScope::Both));
        modal.hide();
        process_lvgl(10);

        modal.set_dual_callbacks(noop, noop, noop, noop, noop, noop, ShutdownScope::Screen);
        REQUIRE(modal.show(test_screen()));
        process_lvgl(10);
        restart = lv_obj_find_by_name(modal.dialog(), "split_restart");
        shutdown = lv_obj_find_by_name(modal.dialog(), "split_shutdown");
        REQUIRE(restart != nullptr);
        REQUIRE(shutdown != nullptr);
        CHECK(ui_split_button_get_selected(restart) ==
              static_cast<uint32_t>(ShutdownScope::Screen));
        CHECK(ui_split_button_get_selected(shutdown) ==
              static_cast<uint32_t>(ShutdownScope::Screen));
    }

    modal.hide();
    process_lvgl(10);
}
