// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_queued_job_start.cpp
 * @brief The home tile's one-tap queued start: saved options, every check, no screen.
 *
 * Run with: ./build/bin/helix-tests "[queued_job_start]"
 *
 * QueuedJobStarter starts the queue's job through the same PrintStartController
 * gates and PrintPreparationManager start a file-view Print tap uses, with the
 * file half of the checks from FileStartFacts. A gate that warns shows its
 * dialog and nothing starts until Proceed; the empty-slot pre-flight offers
 * Print Anyway and Remap, and Remap is the one path that opens the file view.
 */

#include "ui_modal.h"
#include "ui_update_queue.h"

#include "../test_helpers/moonraker_client_mock_test_access.h"
#include "../test_helpers/print_start_controller_test_access.h"
#include "../test_helpers/print_start_grace.h"
#include "../test_helpers/queued_job_starter_test_access.h"
#include "../test_helpers/queued_start_fixture.h"
#include "../test_helpers/scoped_mock_ams.h"
#include "queued_job_start.h"
#include "static_panel_registry.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::PlantedGcode;
using helix::PrintJobState;
using helix::queue::QueuedJobOptions;
using helix::queue::QueuedJobStarter;
using helix::test::set_wire_state;
using helix::test::WarningLog;
using Outcome = QueuedJobStarter::Outcome;

TEST_CASE("saved_option_state answers from the saved entry", "[queued_job_start]") {
    const QueuedJobOptions saved{"part.gcode", {{"bed_mesh", false}, {"qgl", true}}};
    CHECK(helix::queue::saved_option_state(saved, "bed_mesh") == 0);
    CHECK(helix::queue::saved_option_state(saved, "qgl") == 1);
    CHECK(helix::queue::saved_option_state(saved, "z_tilt") == -1);
    CHECK(helix::queue::saved_option_state(QueuedJobOptions{}, "bed_mesh") == -1);
}

namespace {

/// One PETG tool, one footer line naming it: the file a PLA lane mismatches.
constexpr const char* kPetgFile = "; filament_type = PETG\n"
                                  "; filament_colour = #FF0000\n"
                                  "G28\n"
                                  "; filament used [g] = 1.50\n";

class StarterFixture : public helix::test::QueuedStartFixture {
  public:
    StarterFixture() : QueuedStartFixture(helix::PrintSelectApi::Mock) {
        helix::test::wait_out_print_start_grace();
        // The pre-start G-code is refused while Klipper reads as halted.
        prior_klippy_ = static_cast<helix::KlippyState>(
            lv_subject_get_int(get_printer_state().network_state().get_klippy_state_subject()));
        get_printer_state().set_klippy_state_sync(helix::KlippyState::READY);
        // A start answered at once, with no simulated print behind it.
        helix::MoonrakerClientMockTestAccess::set_method_handler(
            mock_client_, "printer.print.start",
            [](MoonrakerClientMock*, const json&, std::function<void(const json&)> success_cb,
               std::function<void(const MoonrakerError&)>) -> bool {
                success_cb(json{{"result", "ok"}});
                return true;
            });
        starter = std::make_unique<QueuedJobStarter>(get_printer_state(), api_.get());
    }

    ~StarterFixture() override {
        starter.reset();
        get_printer_state().set_klippy_state_sync(prior_klippy_);
        settle();
    }

    void start(const helix::JobQueueEntry& job) {
        outcome.reset();
        starter->start(job, [this](Outcome o) { outcome = o; });
        settle();
    }

    static void settle() {
        for (int i = 0; i < 6; ++i) {
            drain();
        }
    }

    /// Press @p button on the dialog on top of ModalStack.
    void press(const char* button) {
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        REQUIRE(dialog != nullptr);
        lv_obj_t* btn = lv_obj_find_by_name(dialog, button);
        REQUIRE(btn != nullptr);
        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
        settle();
        lv_timer_handler();
        settle();
    }

    int starts() const {
        return mock_client_.call_count("printer.print.start");
    }

    std::unique_ptr<QueuedJobStarter> starter;
    std::optional<Outcome> outcome;

  private:
    helix::KlippyState prior_klippy_ = helix::KlippyState::SHUTDOWN;
};

} // namespace

TEST_CASE_METHOD(StarterFixture, "a busy printer refuses before anything is sent",
                 "[queued_job_start]") {
    set_wire_state(get_printer_state(), PrintJobState::PRINTING);
    settle();
    const int reads = mock_client_.call_count("server.database.get_item");

    WarningLog warnings;
    start(entry("0001", "3DBenchy.gcode"));

    REQUIRE(outcome.has_value());
    CHECK(*outcome == Outcome::Refused);
    CHECK(warnings.contains("3DBenchy.gcode"));
    CHECK(mock_client_.call_count("server.database.get_item") == reads);
    CHECK_FALSE(starter->in_flight());
    CHECK(queue_has("0001"));
}

TEST_CASE_METHOD(StarterFixture, "a second start while one is in flight is refused",
                 "[queued_job_start]") {
    mock_client_.defer_next("server.database.get_item");
    starter->start(entry("0001", "3DBenchy.gcode"));
    REQUIRE(starter->in_flight());

    std::optional<Outcome> second;
    starter->start(entry("0002", "xyz-10mm-calibration-cube.gcode"),
                   [&second](Outcome o) { second = o; });
    REQUIRE(second.has_value());
    CHECK(*second == Outcome::Refused);
    CHECK(starter->in_flight());

    // End the first start without a print.
    set_wire_state(get_printer_state(), PrintJobState::PRINTING);
    mock_client_.fire_deferred("server.database.get_item");
    settle();
    CHECK_FALSE(starter->in_flight());
}

TEST_CASE_METHOD(StarterFixture, "a controller refusal ends the start", "[queued_job_start]") {
    mock_client_.defer_next("server.database.get_item");
    start(entry("0001", "3DBenchy.gcode"));
    REQUIRE(starter->in_flight());

    // The printer took another job while the saved options were read.
    set_wire_state(get_printer_state(), PrintJobState::PRINTING);
    mock_client_.fire_deferred("server.database.get_item");
    settle();

    REQUIRE(outcome.has_value());
    CHECK(*outcome == Outcome::NotStarted);
    CHECK(starts() == 0);
    CHECK(queue_has("0001"));
}

TEST_CASE_METHOD(StarterFixture,
                 "a material mismatch from the tile warns and does not start until confirmed",
                 "[queued_job_start]") {
    helix::test::ScopedMockAms ams({{0xFF0000, "PLA"}, {0x00FF00, "PLA"}});
    PlantedGcode file("tile_mismatch.gcode", "", kPetgFile);
    seed_store("0001", file.name(), {{"bed_mesh", false}});

    start(entry("0001", file.name()));

    auto& controller = QueuedJobStarterTestAccess::controller(*starter);
    REQUIRE(PrintStartControllerTestAccess::print_gate_modal(controller) != nullptr);
    CHECK(PrintStartControllerTestAccess::open_gate_name(controller) == "material_compatibility");
    CHECK(starts() == 0);
    CHECK(starter->in_flight());
    CHECK_FALSE(outcome.has_value());

    SECTION("cancel ends the start and keeps the job") {
        press("btn_secondary");
        REQUIRE(outcome.has_value());
        CHECK(*outcome == Outcome::NotStarted);
        CHECK(starts() == 0);
        CHECK(queue_has("0001"));
        CHECK(read_store().count("0001") == 1);
    }
    SECTION("proceed starts it and the job leaves the queue") {
        press("btn_primary");
        REQUIRE(outcome.has_value());
        CHECK(*outcome == Outcome::Started);
        CHECK(starts() == 1);
        CHECK_FALSE(queue_has("0001"));
        CHECK(read_store().count("0001") == 0);
    }
}

TEST_CASE_METHOD(StarterFixture,
                 "an empty-slot block offers Force and Remap; Remap opens the file view",
                 "[queued_job_start]") {
    // One lane, empty: tool 0 maps to it.
    helix::test::ScopedMockAms ams({{0xFF0000, "PETG"}});
    ams.load(0, 0xFF0000, "PETG", helix::SlotStatus::EMPTY);
    PlantedGcode file("tile_empty.gcode", "", kPetgFile);
    StaticPanelRegistry::instance().destroy_all();

    start(entry("0001", file.name()));

    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    REQUIRE(lv_obj_find_by_name(dialog, "btn_tertiary") != nullptr); // the pre-flight modal
    CHECK(starts() == 0);
    CHECK_FALSE(outcome.has_value());

    SECTION("Remap hands the job to the file view") {
        press("btn_tertiary");
        REQUIRE(outcome.has_value());
        CHECK(*outcome == Outcome::OpenedFileView);
        CHECK(starts() == 0);
        const std::string* pending = ::PrintSelectPanelTestAccess::pending_queued_job_id(
            *get_print_select_panel(get_printer_state(), api_.get()));
        REQUIRE(pending != nullptr);
        CHECK(*pending == "0001");
        CHECK(queue_has("0001"));
    }
    SECTION("Force runs the gates and starts") {
        press("btn_primary");
        // Any later gate that warns is confirmed the same way.
        for (int i = 0; i < 4 && !outcome; ++i) {
            if (PrintStartControllerTestAccess::print_gate_modal(
                    QueuedJobStarterTestAccess::controller(*starter)) == nullptr) {
                break;
            }
            press("btn_primary");
        }
        REQUIRE(outcome.has_value());
        CHECK(*outcome == Outcome::Started);
        CHECK(starts() == 1);
    }
    SECTION("Cancel ends the start") {
        press("btn_secondary");
        REQUIRE(outcome.has_value());
        CHECK(*outcome == Outcome::NotStarted);
        CHECK(starts() == 0);
    }

    StaticPanelRegistry::instance().destroy_all();
}
