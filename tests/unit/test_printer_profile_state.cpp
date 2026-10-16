// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "printer_profile_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("PrinterProfileState resolves a type once and honours external persistence",
          "[printer_state][profile][1401]") {
    LVGLTestFixture fixture;
    PrinterProfileState profile;
    profile.init_subjects(false);

    // No database entry: the strategy follows the probe.
    REQUIRE(profile.set_printer_type("Unknown Printer", true, false, false));
    CHECK(profile.printer_type() == "Unknown Printer");
    CHECK(profile.z_offset_calibration_strategy() == ZOffsetCalibrationStrategy::PROBE_CALIBRATE);
    CHECK(std::string(lv_subject_get_string(profile.get_printer_type_subject())) ==
          "Unknown Printer");
    CHECK(lv_subject_get_int(profile.get_z_offset_can_save_subject()) == 1);

    // Same type, same strategy: nothing to do.
    CHECK_FALSE(profile.set_printer_type("Unknown Printer", true, false, false));

    // A module that persists the offset forces FIRMWARE_MANAGED on re-resolution.
    REQUIRE(profile.set_external_persistence(true));
    CHECK_FALSE(profile.set_external_persistence(true));
    REQUIRE(profile.set_printer_type("Unknown Printer", true, false, false));
    CHECK(profile.z_offset_calibration_strategy() == ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
    CHECK(lv_subject_get_int(profile.get_z_offset_can_save_subject()) == 0);

    // Refuted: the type-derived strategy comes back.
    REQUIRE(profile.set_external_persistence(false));
    REQUIRE(profile.set_printer_type("Unknown Printer", false, false, false));
    CHECK(profile.z_offset_calibration_strategy() == ZOffsetCalibrationStrategy::ENDSTOP);
    CHECK(lv_subject_get_int(profile.get_z_offset_can_save_subject()) == 1);

    profile.deinit_subjects();
}

TEST_CASE("PrinterProfileState merges firmware option defaults as deltas",
          "[printer_state][profile]") {
    PrinterProfileState profile;
    CHECK(profile.merge_firmware_option_defaults({{"bed_mesh", true}}));
    CHECK_FALSE(profile.merge_firmware_option_defaults({{"bed_mesh", true}}));
    CHECK(profile.merge_firmware_option_defaults({{"priming", false}}));
    CHECK(profile.merge_firmware_option_defaults({{"bed_mesh", false}}));
}

TEST_CASE("PrinterProfileState synthesizes the timelapse option only while available",
          "[printer_state][profile]") {
    PrinterProfileState profile;
    profile.set_timelapse_default_enabled(true);

    profile.apply_dynamic_options(false, true);
    const auto* tl = profile.pre_print_option_set().find("timelapse");
    REQUIRE(tl != nullptr);
    CHECK(tl->default_enabled);

    profile.apply_dynamic_options(false, true);
    CHECK(profile.pre_print_option_set().options.size() == 1);

    profile.apply_dynamic_options(false, false);
    CHECK(profile.pre_print_option_set().find("timelapse") == nullptr);
}

TEST_CASE("PrinterProfileState knows which printers cannot print from a dot-directory",
          "[printer_state][profile][staging]") {
    LVGLTestFixture fixture;
    PrinterProfileState profile;
    profile.init_subjects(false);

    REQUIRE(profile.set_printer_type("FlashForge Adventurer 5M (ForgeX)", true, false, false));
    CHECK(profile.klipper_hides_dot_dirs());
    REQUIRE(profile.set_printer_type("FlashForge Adventurer 5M Pro (ForgeX)", true, false, false));
    CHECK(profile.klipper_hides_dot_dirs());
    REQUIRE(profile.set_printer_type("FlashForge Adventurer 5X", true, false, false));
    CHECK(profile.klipper_hides_dot_dirs());
    // Stock firmware and everything else print from .helix_temp as before.
    REQUIRE(profile.set_printer_type("FlashForge Adventurer 5M", true, false, false));
    CHECK_FALSE(profile.klipper_hides_dot_dirs());
    REQUIRE(profile.set_printer_type("Unknown Printer", true, false, false));
    CHECK_FALSE(profile.klipper_hides_dot_dirs());
}

TEST_CASE("Z-offset tune hint claims auto-save only where something persists it",
          "[printer_state][profile][zoffset]") {
    using S = ZOffsetCalibrationStrategy;
    using H = ZOffsetTuneHint;

    // A strategy with its own Save step: the hint points at it, whatever else is known.
    CHECK(resolve_z_offset_tune_hint(S::PROBE_CALIBRATE, false, true, false) ==
          H::SAVE_ON_CONTROLS);
    CHECK(resolve_z_offset_tune_hint(S::ENDSTOP, false, false, true) == H::SAVE_ON_CONTROLS);

    // Firmware-managed and the firmware persists the offset.
    CHECK(resolve_z_offset_tune_hint(S::FIRMWARE_MANAGED, false, true, false) == H::AUTO_SAVED);
    // An installed persistence provider is proof on its own.
    CHECK(resolve_z_offset_tune_hint(S::FIRMWARE_MANAGED, true, false, false) == H::AUTO_SAVED);
    CHECK(resolve_z_offset_tune_hint(S::FIRMWARE_MANAGED, true, false, true) == H::AUTO_SAVED);

    // No Save step and nothing persists: the adjustment is temporary.
    CHECK(resolve_z_offset_tune_hint(S::FIRMWARE_MANAGED, false, false, false) == H::TEMPORARY);
    CHECK(resolve_z_offset_tune_hint(S::FIRMWARE_MANAGED, false, false, true) ==
          H::TEMPORARY_TOOL_CHANGE);
}

TEST_CASE("PrinterProfileState publishes the z-offset tune hint from the database",
          "[printer_state][profile][zoffset]") {
    LVGLTestFixture fixture;
    PrinterProfileState profile;
    profile.init_subjects(false);
    auto hint = [&] {
        return static_cast<ZOffsetTuneHint>(
            lv_subject_get_int(profile.get_z_offset_tune_hint_subject()));
    };

    SECTION("Snapmaker U1: no save path, and a tool change re-applies the head's offset") {
        REQUIRE(profile.set_printer_type("Snapmaker U1", true, false, false));
        CHECK(profile.z_offset_calibration_strategy() ==
              ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
        CHECK(lv_subject_get_int(profile.get_z_offset_can_save_subject()) == 0);
        CHECK(hint() == ZOffsetTuneHint::TEMPORARY_TOOL_CHANGE);

        // A provider latching later is a change even though the strategy is not.
        REQUIRE(profile.set_external_persistence(true));
        REQUIRE(profile.set_printer_type("Snapmaker U1", true, false, false));
        CHECK(hint() == ZOffsetTuneHint::AUTO_SAVED);
        REQUIRE(profile.set_external_persistence(false));
        REQUIRE(profile.set_printer_type("Snapmaker U1", true, false, false));
        CHECK(hint() == ZOffsetTuneHint::TEMPORARY_TOOL_CHANGE);
    }

    SECTION("the other firmware-managed printers keep the auto-saved claim") {
        for (const char* type :
             {"FlashForge Adventurer 5M (ForgeX)", "FlashForge Adventurer 5M Pro (ForgeX)",
              "FlashForge Adventurer 5X", "FlashForge Creator 5", "FlashForge Creator 5 Pro",
              "Artillery M1 Pro"}) {
            INFO(type);
            REQUIRE(profile.set_printer_type(type, true, false, false));
            CHECK(profile.z_offset_calibration_strategy() ==
                  ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
            CHECK(hint() == ZOffsetTuneHint::AUTO_SAVED);
        }
    }

    SECTION("a printer with a Save step points at it") {
        REQUIRE(profile.set_printer_type("Unknown Printer", true, false, false));
        CHECK(lv_subject_get_int(profile.get_z_offset_can_save_subject()) == 1);
        CHECK(hint() == ZOffsetTuneHint::SAVE_ON_CONTROLS);
    }

    profile.deinit_subjects();
}
