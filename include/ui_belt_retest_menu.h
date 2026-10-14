// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_context_menu.h"

namespace helix::ui {

/**
 * @brief Belt Tension's "Re-test" menu: which path to measure again
 *
 * On the smallest screens RESULTS has room for three buttons, not four, so
 * Re-test A and Re-test B fold into one button that raises this menu. The
 * action id is the path index (0 = A, 1 = B), or ACTION_CANCELLED.
 */
class BeltRetestMenu : public ContextMenu {
    HELIX_CONTEXT_MENU_KIND(BeltRetestMenu)

  public:
    static constexpr int ACTION_PATH_A = 0;
    static constexpr int ACTION_PATH_B = 1;

    /// Register the menu's row callbacks. Idempotent.
    static void register_callbacks();

  protected:
    const char* xml_component_name() const override {
        return "belt_retest_menu";
    }

  private:
    static void pick(int action);
};

} // namespace helix::ui
