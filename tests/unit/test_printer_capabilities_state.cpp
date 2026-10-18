// SPDX-License-Identifier: GPL-3.0-or-later
//
// PrinterCapabilitiesState::set_hardware: the capability subjects the Printer
// Manager's chips bind, each set from discovery and cleared by a printer
// without it.

#include "../lvgl_test_fixture.h"
#include "capability_overrides.h"
#include "printer_capabilities_state.h"
#include "printer_discovery.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::Capability;
using helix::PrinterCapabilitiesState;
using helix::PrinterDiscovery;
using json = nlohmann::json;

namespace {

int cap(const PrinterCapabilitiesState& caps, Capability c) {
    return lv_subject_get_int(caps.subject(c));
}

} // namespace

TEST_CASE("set_hardware sets the Shake&Tune, fan tach, LED effect and extra fan subjects",
          "[printer_capabilities]") {
    LVGLTestFixture fixture;
    PrinterCapabilitiesState caps;
    caps.init_subjects(false);

    PrinterDiscovery full;
    full.parse_objects(json{"fan", "heater_fan hotend_fan", "fan_feedback", "led_effect rainbow",
                            "gcode_macro AXES_SHAPER_CALIBRATION"});
    CapabilityOverrides full_overrides;
    full_overrides.set_hardware(full);

    caps.set_hardware(full, full_overrides);
    CHECK(cap(caps, Capability::HasShakeTune) == 1);
    CHECK(cap(caps, Capability::HasFanFeedback) == 1);
    CHECK(cap(caps, Capability::HasLedEffects) == 1);
    CHECK(cap(caps, Capability::HasExtraFans) == 1);
    CHECK(cap(caps, Capability::HasHeatSoak) == 0);

    SECTION("a printer without them clears every one") {
        PrinterDiscovery bare;
        bare.parse_objects(json{"fan"});
        CapabilityOverrides bare_overrides;
        bare_overrides.set_hardware(bare);

        caps.set_hardware(bare, bare_overrides);
        CHECK(cap(caps, Capability::HasShakeTune) == 0);
        CHECK(cap(caps, Capability::HasFanFeedback) == 0);
        CHECK(cap(caps, Capability::HasLedEffects) == 0);
        CHECK(cap(caps, Capability::HasExtraFans) == 0);
    }

    caps.deinit_subjects();
}

TEST_CASE("set_hardware: the part cooling fan alone is not an extra fan",
          "[printer_capabilities]") {
    LVGLTestFixture fixture;
    PrinterCapabilitiesState caps;
    caps.init_subjects(false);

    PrinterDiscovery hw;
    hw.parse_objects(json{"fan"});
    CapabilityOverrides overrides;
    overrides.set_hardware(hw);
    caps.set_hardware(hw, overrides);
    CHECK(cap(caps, Capability::HasExtraFans) == 0);

    PrinterDiscovery generic;
    generic.parse_objects(json{"fan_generic exhaust"});
    CapabilityOverrides generic_overrides;
    generic_overrides.set_hardware(generic);
    caps.set_hardware(generic, generic_overrides);
    CHECK(cap(caps, Capability::HasExtraFans) == 1);

    caps.deinit_subjects();
}

TEST_CASE("set_hardware: heat soak follows its override", "[printer_capabilities]") {
    LVGLTestFixture fixture;
    PrinterCapabilitiesState caps;
    caps.init_subjects(false);

    PrinterDiscovery hw; // no heat soak macro
    hw.parse_objects(json{"fan"});
    CapabilityOverrides overrides;
    overrides.set_hardware(hw);

    caps.set_hardware(hw, overrides);
    CHECK(cap(caps, Capability::HasHeatSoak) == 0);

    overrides.set_override(capability::HEAT_SOAK, OverrideState::ENABLE);
    caps.set_hardware(hw, overrides);
    CHECK(cap(caps, Capability::HasHeatSoak) == 1);

    caps.deinit_subjects();
}

TEST_CASE("new capability subjects register under their XML names", "[printer_capabilities]") {
    LVGLTestFixture fixture;
    PrinterCapabilitiesState caps;
    caps.init_subjects(true);
    for (const char* name : {"printer_has_shaketune", "printer_has_heat_soak",
                             "printer_has_fan_feedback", "printer_has_led_effects"}) {
        CAPTURE(name);
        CHECK(lv_xml_get_subject(nullptr, name) != nullptr);
    }
    caps.deinit_subjects();
}
