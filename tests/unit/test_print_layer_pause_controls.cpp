// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_layer_pause_controls.cpp
 * @brief The Pause at Layer modal's status line and commands (#1703).
 *
 * Pause at layer is a beta feature: with beta off nothing may open, and no
 * SET_PAUSE_* may reach the printer.
 */

#include "ui_modal.h"
#include "ui_print_layer_pause.h"
#include "ui_update_queue.h"

#include "config.h"
#include "lvgl_test_fixture.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using A = helix::layer_pause::Availability;
using helix::ui::layer_pause_status_text;

namespace {

/// Records every gcode send and answers none of them.
struct RecordingAPI : public MoonrakerAPIMock {
    using MoonrakerAPIMock::MoonrakerAPIMock;

    void execute_gcode(const std::string& gcode, IMoonrakerAPI::SuccessCallback,
                       IMoonrakerAPI::ErrorCallback, uint32_t = 0, bool = false,
                       IMoonrakerAPI::SuccessCallback = nullptr, bool = true,
                       bool = false) override {
        sent.push_back(gcode);
    }

    std::vector<std::string> sent;
};

/// Sets /beta_features for one test and puts the previous value back.
class ScopedBeta {
  public:
    explicit ScopedBeta(bool on) : config_(helix::Config::get_instance()) {
        REQUIRE(config_ != nullptr);
        prev_ = config_->get<bool>("/beta_features", false);
        config_->set<bool>("/beta_features", on);
    }
    ~ScopedBeta() {
        config_->set<bool>("/beta_features", prev_);
    }

  private:
    helix::Config* config_;
    bool prev_{false};
};

class LayerPauseControlsFixture : public LVGLTestFixture {
  public:
    LayerPauseControlsFixture()
        : client(MoonrakerClientMock::PrinterType::VORON_24), api(client, state) {
        state.init_subjects(false);
        controls.init_subjects(state);
        controls.set_api(&api);
        // A running print with slicer layers and a pause already scheduled,
        // so every action has something to act on.
        state.update_from_status({{"print_stats",
                                   {{"filename", "a.gcode"},
                                    {"state", "printing"},
                                    {"info", {{"current_layer", 3}, {"total_layer", 10}}}}}});
        state.print_state().note_layer_pause_at_layer(7);
    }

    ~LayerPauseControlsFixture() override {
        ModalStack::instance().clear();
        controls.deinit_subjects();
        helix::ui::UpdateQueue::instance().drain();
        state.deinit_subjects();
    }

    void every_action() {
        controls.schedule_next_layer();
        controls.schedule_at_layer(8);
        controls.cancel_pending();
        controls.choose_layer();
        controls.open_modal();
    }

    MoonrakerClientMock client;
    helix::PrinterState state;
    RecordingAPI api;
    helix::ui::PrintLayerPauseControls controls;
};

} // namespace

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

TEST_CASE_METHOD(LayerPauseControlsFixture,
                 "Layer pause controls: beta off sends nothing and opens nothing",
                 "[layer_pause][ui][beta]") {
    ScopedBeta beta(false);
    REQUIRE_FALSE(helix::ui::layer_pause_beta_enabled());
    every_action();
    CHECK(api.sent.empty());
    CHECK(Modal::get_top() == nullptr);
}

TEST_CASE_METHOD(LayerPauseControlsFixture, "Layer pause controls: beta on sends each command",
                 "[layer_pause][ui][beta]") {
    ScopedBeta beta(true);
    REQUIRE(helix::ui::layer_pause_beta_enabled());
    controls.schedule_next_layer();
    controls.schedule_at_layer(8);
    controls.cancel_pending();
    REQUIRE(api.sent.size() == 3);
    CHECK(api.sent[0] == "SET_PAUSE_NEXT_LAYER ENABLE=1");
    CHECK(api.sent[1] == "SET_PAUSE_AT_LAYER ENABLE=1 LAYER=8");
    CHECK(api.sent[2] == "SET_PAUSE_AT_LAYER ENABLE=0");
}
