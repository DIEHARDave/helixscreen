// SPDX-License-Identifier: GPL-3.0-or-later

#include "capability_chip_text.h"

#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/fmt/fmt.h>

#include <cmath>

namespace helix {

namespace {

constexpr const char* SEP = " · ";

std::string with_detail(const char* label, const std::string& detail) {
    std::string out = lv_tr(label);
    if (!detail.empty()) {
        out += SEP;
        out += detail;
    }
    return out;
}

std::string count_detail(int count, int shown_from) {
    return count >= shown_from ? std::to_string(count) : std::string();
}

std::string build_volume_text(const BuildVolume& v) {
    const long x = std::lround(v.x_max - v.x_min);
    const long y = std::lround(v.y_max - v.y_min);
    const long z = std::lround(v.z_max);
    if (x <= 0 || y <= 0 || z <= 0) {
        return {};
    }
    return fmt::format("{}×{}×{}", x, y, z);
}

std::string filament_system_text(const std::vector<FilamentSystemFact>& systems) {
    std::string out;
    for (const auto& sys : systems) {
        if (sys.is_tool_changer) {
            continue;
        }
        if (!out.empty()) {
            out += ", ";
        }
        out += sys.name;
        if (sys.slots > 0) {
            out += SEP;
            out += std::to_string(sys.slots);
        }
    }
    return out;
}

std::string base_text(CapChip chip, const CapabilityChipFacts& f) {
    switch (chip) {
    case CapChip::Kinematics:
        return kinematics_display_name(f.kinematics);
    case CapChip::BuildVolume:
        return build_volume_text(f.build_volume);
    case CapChip::Probe:
        return with_detail("Probe", f.probe_type);
    case CapChip::BedMesh:
        return lv_tr("Bed Mesh");
    case CapChip::Qgl:
        return lv_tr("QGL");
    case CapChip::ZTilt:
        return lv_tr("Z-Tilt");
    case CapChip::ScrewsTilt:
        return lv_tr("Screws Tilt");
    case CapChip::Accelerometer:
        return with_detail("Accelerometer", count_detail(f.accel_count, 2));
    case CapChip::BeltTension:
        return lv_tr("Belt Tension");
    case CapChip::ShakeTune:
        return lv_tr("Shake&Tune");
    case CapChip::HeatedBed:
        return lv_tr("Heated Bed");
    case CapChip::ChamberSensor:
        return lv_tr("Chamber Sensor");
    case CapChip::ChamberHeater:
        return lv_tr("Chamber Heater");
    case CapChip::ChamberDryer:
        return lv_tr("Chamber Dryer");
    case CapChip::FilterFan:
        return lv_tr("Filter Fan");
    case CapChip::BedDrying:
        return lv_tr("Bed Drying");
    case CapChip::Fans:
        return with_detail("Fans", count_detail(f.fan_count, 1));
    case CapChip::FanTach:
        return lv_tr("Fan Tach");
    case CapChip::FilamentSystem:
        return filament_system_text(f.filament_systems);
    case CapChip::ToolChanger:
        return with_detail("Tool Changer",
                           f.tool_count > 0
                               ? fmt::format(fmt::runtime(lv_tr("{} Tools")), f.tool_count)
                               : std::string());
    case CapChip::FilamentSensors:
        return with_detail("Filament Sensors", count_detail(f.filament_sensor_count, 1));
    case CapChip::WidthSensors:
        return with_detail("Width Sensors", count_detail(f.width_sensor_count, 1));
    case CapChip::Retraction:
        return lv_tr("Retraction");
    case CapChip::PaCal:
        return with_detail("PA Calibration", f.pa_provider);
    case CapChip::ToolOffsets:
        return lv_tr("Tool Offsets");
    case CapChip::NozzleClean:
        return lv_tr("Nozzle Clean");
    case CapChip::PurgeLine:
        return lv_tr("Purge Line");
    case CapChip::HeatSoak:
        return lv_tr("Heat Soak");
    case CapChip::ExcludeObject:
        return lv_tr("Exclude Object");
    case CapChip::LayerPause:
        return lv_tr("Layer Pause");
    case CapChip::JobQueue:
        return lv_tr("Job Queue");
    case CapChip::Mcus:
        if (f.mcus.size() == 1) {
            return f.mcus.front();
        }
        return f.mcus.empty() ? std::string()
                              : fmt::format(fmt::runtime(lv_tr("{} MCUs")), f.mcus.size());
    case CapChip::Power:
        return with_detail("Power", count_detail(f.power_device_count, 1));
    case CapChip::Leds:
        return with_detail("LEDs", count_detail(f.led_count, 1));
    case CapChip::LedEffects:
        return with_detail("LED Effects", count_detail(f.led_effect_count, 1));
    case CapChip::Speaker:
        return lv_tr("Speaker");
    case CapChip::Webcam:
        return with_detail("Webcam", count_detail(f.webcam_count, 2));
    case CapChip::Spoolman:
        return lv_tr("Spoolman");
    case CapChip::Timelapse:
        return lv_tr("Timelapse");
    case CapChip::HelixMacros:
        return lv_tr("HelixScreen Macros");
    case CapChip::HelixPlugin:
        return lv_tr("HelixPrint Plugin");
    case CapChip::Count:
        break;
    }
    return {};
}

} // namespace

std::string capability_chip_text(CapChip chip, const CapabilityChipFacts& facts) {
    std::string text = base_text(chip, facts);
    if (!text.empty() && facts.forced_on.count(chip) != 0) {
        text += SEP;
        text += lv_tr("set manually");
    }
    return text;
}

std::string kinematics_display_name(const std::string& kinematics) {
    struct Name {
        const char* klipper;
        const char* display;
    };
    static constexpr Name NAMES[] = {
        {"corexy", "CoreXY"}, {"limited_corexy", "CoreXY"},     {"hybrid_corexy", "Hybrid CoreXY"},
        {"corexz", "CoreXZ"}, {"cartesian", "Cartesian"},       {"limited_cartesian", "Cartesian"},
        {"delta", "Delta"},   {"rotary_delta", "Rotary Delta"}, {"polar", "Polar"},
        {"winch", "Winch"},
    };
    for (const auto& n : NAMES) {
        if (kinematics == n.klipper) {
            return n.display;
        }
    }
    return kinematics;
}

} // namespace helix
