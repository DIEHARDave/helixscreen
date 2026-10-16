// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

namespace helix {

/// Which command holds the queue. They end differently: M191 returns on a
/// target of 0 or a print cancel, while TEMPERATURE_WAIT on a sensor's
/// MINIMUM ends only at that temperature or at a shutdown.
enum class ChamberWaitKind { Unknown, M191, TemperatureWait };

struct ChamberWaitLine {
    ChamberWaitKind kind = ChamberWaitKind::Unknown;
    double target_c = 0; ///< M191's S/R or TEMPERATURE_WAIT's MINIMUM; 0 = none
    std::string sensor;  ///< TEMPERATURE_WAIT's SENSOR, lower-cased; "" = none
};

/// Read the command the print is executing from the file bytes that end at
/// virtual_sdcard.file_position (the last complete line). Anything else,
/// such as a macro whose body holds the wait, is Unknown.
ChamberWaitLine classify_chamber_wait_line(std::string_view chunk_ending_at_position);

/// The line is a TEMPERATURE_WAIT on the chamber heater: its SENSOR names
/// @p chamber_heater, or @p chamber_reading (the object the chamber reading
/// comes from) while a chamber heater exists. A sensor-only soak or a probe
/// coil's wait names no chamber heater. Names compare case-insensitively.
bool wait_names_chamber_heater(const ChamberWaitLine& line, const std::string& chamber_heater,
                               const std::string& chamber_reading);

} // namespace helix
