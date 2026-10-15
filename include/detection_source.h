// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <optional>
#include <string>

namespace helix::detection {

enum class DetectionKind { Spaghetti, DirtyBed, Residue, DirtyNozzle, Unknown };

/// Map a Snapmaker U1 `print_stats.exception.code` to a kind.
/// Stock defect_detection.py: 1=dirty-bed, 2=noodle/spaghetti, 3=residue,
/// 4=dirty-nozzle. 0 / -1 / anything else = not a visual defect we surface.
inline DetectionKind kind_from_u1_code(int code) {
    switch (code) {
    case 1:
        return DetectionKind::DirtyBed;
    case 2:
        return DetectionKind::Spaghetti;
    case 3:
        return DetectionKind::Residue;
    case 4:
        return DetectionKind::DirtyNozzle;
    default:
        return DetectionKind::Unknown;
    }
}

/// The U1 defect_detection module raises exception id 532. A code under any
/// other id belongs to a different module; a frame that carried no id
/// (-1) is judged by its code alone.
inline constexpr int U1_DEFECT_EXCEPTION_ID = 532;
inline DetectionKind kind_from_u1_exception(int exception_id, int code) {
    if (exception_id >= 0 && exception_id != U1_DEFECT_EXCEPTION_ID) {
        return DetectionKind::Unknown;
    }
    return kind_from_u1_code(code);
}

/// Normalized "a detector reported a defect" event.
struct DetectionEvent {
    std::string source_id;
    DetectionKind kind = DetectionKind::Unknown;
    bool attributable = false;
    std::optional<float> confidence;
    bool already_paused = false;
    std::string message;
};

/// The printer's own stored on/off + pause choice, as the vendor stack keeps
/// it. Seeded into HelixScreen settings once, on the first start where a
/// capable source exists; after that the settings own both values.
struct DetectionPreference {
    bool enabled = true;
    bool pause = true;
};

/// A backend that can report print-failure detections.
class DetectionSource {
  public:
    using Callback = std::function<void(const DetectionEvent&)>;

    virtual ~DetectionSource() = default;
    virtual std::string id() const = 0;
    virtual bool available() const = 0;
    virtual bool can_tune() const {
        return false;
    }
    /// Whether the source's own firmware pauses the print on a detection.
    /// The default (false) is the common shape: HelixScreen sends the pause,
    /// so the pause-on-detect setting governs the source and its settings
    /// row shows. Sources paused in firmware override to true; the setting
    /// is inert for them, their row hides, and a warn-only response still
    /// escalates to the modal.
    virtual bool self_pauses() const {
        return false;
    }
    /// Lower the source's sensitivity (the response modal's Tune button).
    /// Called only while can_tune() is true, on the main thread.
    virtual void tune() {}
    /// Re-run the local capability probe. Called on every connect: answers
    /// that depend on state saved after boot (e.g. the wizard's printer
    /// type) can change without a restart. Sources whose capability arrives
    /// over the wire keep the base no-op.
    virtual void refresh_capability() {}
    /// The printer's stored preference, or nullopt when this source has none
    /// (settings then keep their defaults: on, pause on detect).
    virtual std::optional<DetectionPreference> printer_preference() const {
        return std::nullopt;
    }
    /// Whether the print is paused right now because this source detected
    /// something. Read by other pause consumers (the runout guidance) so one
    /// pause gets one explanation, whichever observer runs first.
    virtual bool explains_current_pause() const {
        return false;
    }

    /// Callback for the bed check's state: the printer's own value, or nullopt
    /// when the printer could not be asked or did not answer. Called on any
    /// thread; the caller marshals.
    using BedCheckCallback = std::function<void(std::optional<bool>)>;
    /// Whether the printer runs a pre-print check for objects left on the bed
    /// that the user can switch on and off.
    virtual bool has_bed_check() const {
        return false;
    }
    /// Ask the printer whether its bed check is on. Called only while
    /// has_bed_check() is true.
    virtual void read_bed_check(BedCheckCallback done) {
        done(std::nullopt);
    }
    /// Switch the printer's bed check. @p done receives the value the printer
    /// accepted, or nullopt when it refused. Called only while has_bed_check().
    virtual void set_bed_check(bool enabled, BedCheckCallback done) {
        (void)enabled;
        done(std::nullopt);
    }
    /// Callback fires on the MAIN thread (already marshaled).
    virtual void set_callback(Callback cb) = 0;
};

} // namespace helix::detection
