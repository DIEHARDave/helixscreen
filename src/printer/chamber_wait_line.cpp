// SPDX-License-Identifier: GPL-3.0-or-later
#include "chamber_wait_line.h"

#include "text_io.h"

namespace helix {

namespace {

/// The value of a KEY=VALUE word or a letter-prefixed word (S60) in an
/// upper-cased gcode line; 0 when absent or unreadable.
double gcode_param(const std::string& line, const std::string& key) {
    size_t pos = 0;
    while ((pos = line.find(key, pos)) != std::string::npos) {
        const bool word_start = pos == 0 || line[pos - 1] == ' ' || line[pos - 1] == '\t';
        if (word_start) {
            const auto value = text_io::parse_leading<double>(line.substr(pos + key.size()));
            return value ? *value : 0.0;
        }
        pos += key.size();
    }
    return 0.0;
}

/// The value of a KEY=VALUE word, quoted or bare, in a gcode line; "" when
/// absent.
std::string gcode_word(const std::string& line, const std::string& key) {
    size_t pos = 0;
    while ((pos = line.find(key, pos)) != std::string::npos) {
        const bool word_start = pos == 0 || line[pos - 1] == ' ' || line[pos - 1] == '\t';
        if (word_start) {
            size_t start = pos + key.size();
            if (start < line.size() && line[start] == '"') {
                const size_t end = line.find('"', start + 1);
                return line.substr(start + 1,
                                   end == std::string::npos ? std::string::npos : end - start - 1);
            }
            const size_t end = line.find_first_of(" \t", start);
            return line.substr(start, end == std::string::npos ? std::string::npos : end - start);
        }
        pos += key.size();
    }
    return {};
}

} // namespace

ChamberWaitLine classify_chamber_wait_line(std::string_view chunk) {
    std::string text(chunk);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    const size_t start = text.find_last_of('\n');
    std::string line = start == std::string::npos ? text : text.substr(start + 1);
    if (const size_t comment = line.find(';'); comment != std::string::npos) {
        line.resize(comment);
    }
    line = text_io::to_upper(line);
    const size_t first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos) {
        return {};
    }
    line.erase(0, first);
    if (line.rfind("M191", 0) == 0 && (line.size() == 4 || line[4] == ' ')) {
        const double s = gcode_param(line, "S");
        return {ChamberWaitKind::M191, s > 0 ? s : gcode_param(line, "R")};
    }
    if (line.rfind("TEMPERATURE_WAIT", 0) == 0) {
        return {ChamberWaitKind::TemperatureWait, gcode_param(line, "MINIMUM="),
                text_io::to_lower(gcode_word(line, "SENSOR="))};
    }
    return {};
}

bool wait_names_chamber_heater(const ChamberWaitLine& line, const std::string& chamber_heater,
                               const std::string& chamber_reading) {
    if (line.kind != ChamberWaitKind::TemperatureWait || line.sensor.empty() ||
        chamber_heater.empty()) {
        return false;
    }
    return line.sensor == text_io::to_lower(chamber_heater) ||
           (!chamber_reading.empty() && line.sensor == text_io::to_lower(chamber_reading));
}

} // namespace helix
