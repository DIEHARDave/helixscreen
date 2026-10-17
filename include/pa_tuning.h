// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Pressure-advance tuning by eye, during a test print.
//
// HelixScreen prints a small hollow tower whose corners show pressure-advance
// artefacts, and drives K itself: on each layer change it sends
// SET_PRESSURE_ADVANCE for that layer's band, so the file carries no PA
// commands and the user can take over at any moment. What the user finally
// picks from is the K the printer REPORTED for each layer, not the K that was
// planned, because the printer is the only witness to what was in effect.
//
// Everything here is pure: no LVGL, no network. The overlay feeds it events
// and sends the commands it asks for.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "hv/json.hpp"

namespace helix::pa {

/// Klipper accepts any non-negative K; this ceiling only stops a runaway
/// stepper from asking for a value no extruder can use.
inline constexpr float kMinK = 0.0f;
inline constexpr float kMaxK = 2.0f;

float clamp_k(float k);

/// K formatted the way tuning guides and slicers quote it: four decimals.
std::string format_k(float k);

/// The command that sets K on one extruder. smooth_time is left alone.
std::string set_pa_gcode(const std::string& extruder, float k);

/// Step sizes offered for a live nudge, smallest first. A bowden K is an
/// order of magnitude above a direct-drive one, so the large steps exist for it.
inline constexpr float kStepSizes[] = {0.001f, 0.005f, 0.01f, 0.05f};
inline constexpr int kStepCount = 4;
inline constexpr int kDefaultStepIndex = 1;

enum class ExtruderKind : int { DirectDrive = 0, Bowden = 1 };

/// The planned K sweep: band 0 prints at start_k, the last band at end_k.
struct Ramp {
    float start_k = 0.0f;
    float end_k = 0.1f;
    int bands = 10;
};

/// The sweep every tuning guide starts from for this extruder kind.
Ramp default_ramp(ExtruderKind kind);

/// The tower. A hollow square: corners are where pressure advance shows, and a
/// thin wall keeps every layer short enough that a band is a few minutes.
struct TowerGeometry {
    float first_layer_mm = 0.25f;
    float layer_mm = 0.2f;
    /// Layers below the first band, at start_k, so the first band does not sit
    /// on the squashed first layer.
    float base_mm = 1.0f;
    float band_mm = 3.0f;
    float size_mm = 40.0f;
    int perimeters = 2;
};

/// Total layers for a tower with `bands` bands.
int layer_count(const TowerGeometry& g, int bands);

/// Top-of-layer Z for 1-based `layer`.
float layer_z(const TowerGeometry& g, int layer);

/// The band 1-based `layer` belongs to: -1 for the base, else 0..bands-1.
int band_for_layer(const TowerGeometry& g, int bands, int layer);

/// The ramp's planned K for 1-based `layer`.
float ramp_k_for_layer(const TowerGeometry& g, const Ramp& r, int layer);

/// Everything the gcode generator needs. Coordinates are gcode space.
struct TestPrintParams {
    TowerGeometry geometry;
    int bands = 10;
    float bed_x_min = 0.0f;
    float bed_x_max = 220.0f;
    float bed_y_min = 0.0f;
    float bed_y_max = 220.0f;
    int nozzle_temp = 210;
    int bed_temp = 60;
    /// Tool to print with; -1 prints with whatever is mounted.
    int tool = -1;
    float nozzle_diameter_mm = 0.4f;
    float filament_diameter_mm = 1.75f;
    /// Wall speed. Fast enough that the corners show the artefact.
    float wall_speed_mm_s = 100.0f;
    float first_layer_speed_mm_s = 25.0f;
    /// Part-cooling fan from the third layer on, 0-100.
    int fan_percent = 100;
    std::string material;
};

/// Generate the test print. Self-contained: heats, homes, primes, prints, and
/// cools down; reports layers with SET_PRINT_STATS_INFO; never sets K.
std::string generate_test_gcode(const TestPrintParams& p);

/// Rough wall-clock for the print itself, heat-up excluded.
int estimate_print_seconds(const TestPrintParams& p);

/// One height range the printer reported a single K for.
struct Band {
    int first_layer = 0;
    int last_layer = 0;
    float z_low = 0.0f;  ///< bottom of first_layer
    float z_high = 0.0f; ///< top of last_layer
    float k = 0.0f;
};

/// K in effect per layer, as reported by the printer.
class BandLog {
  public:
    /// Record that `k` was in effect while 1-based `layer` (top at `z_top`,
    /// bottom at `z_bottom`) printed. The latest report for a layer wins.
    void record(int layer, float z_bottom, float z_top, float k);
    void clear();
    bool empty() const {
        return layers_.empty();
    }
    /// Consecutive layers with the same K merged, lowest first.
    std::vector<Band> bands() const;

  private:
    struct Entry {
        int layer;
        float z_bottom;
        float z_top;
        float k;
    };
    std::vector<Entry> layers_;
};

/// The tuning run, as a state machine. The caller owns the printer; this owns
/// what K should be and what was observed.
class TuningSession {
  public:
    enum class State : int {
        Idle = 0,     ///< nothing running
        Starting = 1, ///< file uploaded / print requested, not printing yet
        Printing = 2, ///< the test print is running
        Picking = 3,  ///< print over; the user chooses a band
        Done = 4,     ///< a result exists
    };
    enum class Mode : int {
        Ramp = 0,   ///< K follows the ramp on every layer change
        Manual = 1, ///< the user nudged; K stays where they put it
    };

    /// Asks the caller to set K on the printer.
    using SendK = std::function<void(float)>;

    explicit TuningSession(SendK send) : send_(std::move(send)) {}

    void start(const TowerGeometry& g, const Ramp& r, float original_k);
    /// The print reached "printing".
    void on_print_started();
    /// 1-based layer now printing. 0 means not started yet.
    void on_layer(int layer);
    /// The printer reported K for the extruder being tuned.
    void on_k_reported(float k);
    /// The print ended, by any route. With nothing observed the run is void.
    void on_print_ended();
    /// Nudge K by `delta`. Stops the ramp for the rest of the print.
    void nudge(float delta);
    /// Pick one of bands(). Returns false for an index out of range.
    bool pick_band(int index);
    /// Keep whatever K is in effect now.
    void keep_current();
    /// Abandon the run. Asks for the original K back when the run changed it
    /// and produced no result.
    void abandon();

    State state() const {
        return state_;
    }
    Mode mode() const {
        return mode_;
    }
    int layer() const {
        return layer_;
    }
    /// Last K the printer reported, or the original before any report.
    float current_k() const {
        return current_k_;
    }
    float original_k() const {
        return original_k_;
    }
    std::optional<float> result() const {
        return result_;
    }
    const TowerGeometry& geometry() const {
        return geometry_;
    }
    const Ramp& ramp() const {
        return ramp_;
    }
    std::vector<Band> bands() const {
        return log_.bands();
    }

  private:
    void send(float k);

    SendK send_;
    State state_ = State::Idle;
    Mode mode_ = Mode::Ramp;
    TowerGeometry geometry_;
    Ramp ramp_;
    BandLog log_;
    int layer_ = 0;
    float current_k_ = 0.0f;
    float original_k_ = 0.0f;
    std::optional<float> last_sent_;
    bool changed_k_ = false;
    std::optional<float> result_;
};

/// Where a tuned K is published in Moonraker's database, for other clients
/// (a slicer plugin) to read. Schema: docs/devel/PRESSURE_ADVANCE_TUNING.md.
inline constexpr const char* kMoonrakerNamespace = "helix-screen";
inline constexpr const char* kMoonrakerKey = "pressure_advance";

/// One tuned result, as published.
struct SavedK {
    float k = 0.0f;
    std::string extruder;
    std::string material;
    int nozzle_temp = 0;
    /// Spoolman ids, 0 when no spool was active.
    int spool_id = 0;
    int filament_id = 0;
    std::string filament_name;
    /// ISO 8601 UTC.
    std::string updated;
};

/// `existing` (the key's current value, or null) with `saved` recorded as the
/// latest result, as the extruder's latest, and as its filament's when it has
/// one. Other entries are kept.
nlohmann::json merge_moonraker_record(const nlohmann::json& existing, const SavedK& saved);

/// Whether a spool activation should apply its saved K. Never mid-print: a
/// slicer that sets PA in its start gcode has already spoken, and an AMS swap
/// during a print must not override it.
bool should_auto_apply_on_activation(bool printing, std::optional<float> saved_k);

} // namespace helix::pa
