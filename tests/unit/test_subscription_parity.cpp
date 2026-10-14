// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pins the whole subscription map MoonrakerDiscoverySequence::build_subscription_objects
// produces for each filament system, keys and field lists, against files under
// tests/fixtures/subscription_parity/. A narrowed field list that silently becomes
// `null` makes Klipper serialize every internal field on every tick, so values are
// compared, not just keys.
//
// HELIX_WRITE_GOLDENS=1 rewrites the files from the current builder output.

#include "moonraker_discovery_sequence.h"
#include "printer_discovery.h"

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::MoonrakerDiscoverySequence;
using helix::PrinterDiscovery;
using nlohmann::json;

namespace {

std::string golden_path(const std::string& name) {
    std::string src = __FILE__;
    auto pos = src.rfind("/tests/unit/");
    std::string root = pos != std::string::npos ? src.substr(0, pos) + "/" : std::string{};
    return root + "tests/fixtures/subscription_parity/" + name + ".json";
}

/// One printer's object list, bucketed the way parse_objects() buckets it.
struct Printer {
    std::vector<std::string> heaters{"extruder", "heater_bed"};
    std::vector<std::string> fans{"fan"};
    std::vector<std::string> afc_objects;
    std::vector<std::string> filament_sensors;
    json all_objects = json::array({"extruder", "heater_bed", "fan"});
    json status_claim;

    Printer& obj(const std::string& name) {
        all_objects.push_back(name);
        return *this;
    }
    Printer& afc(const std::string& name) {
        all_objects.push_back(name);
        afc_objects.push_back(name);
        return *this;
    }
    Printer& sensor(const std::string& name) {
        all_objects.push_back(name);
        filament_sensors.push_back(name);
        return *this;
    }

    json build() const {
        PrinterDiscovery hw;
        hw.parse_objects(all_objects);
        if (!status_claim.is_null()) {
            hw.settle_status_claims(status_claim);
        }
        return MoonrakerDiscoverySequence::build_subscription_objects(
            hw, heaters, {}, {}, fans, {}, afc_objects, filament_sensors, {});
    }
};

void check_against_golden(const std::string& name, const json& actual) {
    const std::string path = golden_path(name);
    const char* write = std::getenv("HELIX_WRITE_GOLDENS");
    if (write != nullptr && std::string(write) == "1") {
        std::ofstream out(path);
        REQUIRE(out.good());
        out << actual.dump(2) << "\n";
        WARN("wrote golden " << path);
        return;
    }
    std::ifstream in(path);
    INFO("golden missing or unreadable: " << path);
    REQUIRE(in.good());
    json expected = json::parse(in, nullptr, false);
    REQUIRE_FALSE(expected.is_discarded());
    INFO("actual:\n" << actual.dump(2));
    INFO("expected:\n" << expected.dump(2));
    CHECK(actual == expected);
}

} // namespace

TEST_CASE("subscription objects are unchanged for every backend fixture", "[subscription_parity]") {
    SECTION("plain printer") {
        Printer p;
        check_against_golden("plain", p.build());
    }

    SECTION("Happy Hare") {
        Printer p;
        p.obj("mmu");
        check_against_golden("happy_hare", p.build());
    }

    SECTION("AFC BoxTurtle") {
        Printer p;
        p.afc("AFC")
            .afc("AFC_BoxTurtle Turtle_1")
            .afc("AFC_stepper lane1")
            .afc("AFC_stepper lane2")
            .afc("AFC_hub Turtle_1")
            .afc("AFC_extruder extruder")
            .afc("AFC_buffer Turtle_1");
        check_against_golden("afc_boxturtle", p.build());
    }

    SECTION("ACE with named instances") {
        Printer p;
        p.obj("ace_instance0").obj("ace_instance1");
        check_against_golden("ace_named", p.build());
    }

    SECTION("ACE published as the plain ace object") {
        Printer p;
        p.obj("ace");
        check_against_golden("ace_plain", p.build());
    }

    SECTION("ACE published as filament_hub") {
        Printer p;
        p.obj("filament_hub");
        check_against_golden("ace_filament_hub", p.build());
    }

    SECTION("CFS") {
        Printer p;
        p.obj("box");
        check_against_golden("cfs", p.build());
    }

    SECTION("Snapmaker with the batch macro") {
        Printer p;
        p.obj("filament_detect").obj("gcode_macro Auto_Feeding_Batch");
        check_against_golden("snapmaker_batch", p.build());
    }

    SECTION("Snapmaker without the batch macro") {
        Printer p;
        p.obj("filament_detect");
        check_against_golden("snapmaker_no_batch", p.build());
    }

    SECTION("QIDI Box") {
        Printer p;
        p.obj("box_stepper slot0").obj("box_stepper slot1").obj("box_extras");
        check_against_golden("qidi_box", p.build());
    }

#if HELIX_HAS_IFS
    SECTION("AD5X IFS on the module firmware") {
        Printer p;
        p.obj("ifs")
            .obj("ifs_materials")
            .obj("save_variables")
            .sensor("filament_switch_sensor lane1");
        check_against_golden("ad5x_ifs_module", p.build());
    }

    SECTION("AD5X IFS on native Z-Mod") {
        Printer p;
        p.sensor("filament_motion_sensor ifs_motion_sensor")
            .sensor("zmod_ifs_switch_sensor head_switch_sensor")
            .sensor("zmod_ifs_motion_sensor ifs_motion_sensor")
            .obj("zmod_ifs")
            .obj("zmod_color")
            .obj("gcode_macro END_CHANGE_FILAMENT")
            .obj("save_variables");
        check_against_golden("ad5x_ifs_zmod", p.build());
    }
#endif // HELIX_HAS_IFS

    SECTION("OpenAMS") {
        Printer p;
        p.obj("oams_manager");
        p.status_claim =
            json{{"oams_manager", {{"api_version", 1}, {"schema", "openams.manager"}}}};
        check_against_golden("openams", p.build());
    }

    SECTION("klipper-toolchanger") {
        Printer p;
        p.obj("toolchanger").obj("tool T0").obj("tool T1").obj("extruder1");
        p.heaters.push_back("extruder1");
        check_against_golden("toolchanger", p.build());
    }
}
