// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_layer_pause_controls.cpp
 * @brief The Pause at Layer modal's status line and commands (#1703).
 */

#include "ui_print_layer_pause.h"

#include "../catch_amalgamated.hpp"

using A = helix::layer_pause::Availability;
using helix::ui::layer_pause_status_text;

TEST_CASE("Layer pause status text: one line for where, one for what is pending",
          "[layer_pause][ui]") {
    CHECK(layer_pause_status_text(A::Hidden, 3, 10, 0).empty());
    CHECK(layer_pause_status_text(A::Ready, 3, 10, 0) == "Layer 3 of 10.\nNo pause scheduled.");
    CHECK(layer_pause_status_text(A::Ready, 3, 10, 7) == "Layer 3 of 10.\nPausing at layer 7.");
    CHECK(layer_pause_status_text(A::LastLayer, 10, 10, 0) ==
          "The print is on its last layer.\nNo pause scheduled.");
    CHECK(layer_pause_status_text(A::NoLayerData, 0, 0, 0) ==
          "This print does not report layers, so no layer pause can be scheduled.\n"
          "No pause scheduled.");
}
