// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include "print_lifecycle_state.h"
#include "screensaver_canvas.h"
#include "screensaver_clock_reading.h"
#include "screensaver_frame.h"

#include <cstdint>
#include <lvgl.h>
#include <string>
#include <vector>

/**
 * @file screensaver_clock.h
 * @brief The app side of the clock savers: the local time, the print, and their text
 *
 * The pure decision is helix::ui::ClockState; this file reads the wall clock and the Time
 * Format setting, maps the print lifecycle, turns a reading into translated text, and draws
 * text onto a saver canvas.
 */

namespace helix::ui {

/// The local time of day, the date and the 12/24-hour setting, with no print.
ClockInput saver_clock_now();

/// A clock's reading of the app's print lifecycle: cancelled and failed prints read as no print.
ClockPrintPhase clock_print_phase(PrintState state);

/// The print as two short lines: "ETA 9:41 PM" and "1h 12m left" while counting down,
/// "PAUSED" and the time left while paused, "DONE" and nothing when done, both empty for the
/// time of day.
struct ClockPrintText {
    std::string eta;
    std::string left;
};
ClockPrintText clock_print_text(const ClockReading& reading);

/// The date as a long date and a weekday, through locale_formats: "October 21, 2019" and
/// "Monday" in English.
struct ClockDateText {
    std::string date;
    std::string weekday;
};
ClockDateText clock_date_text(const ClockDate& date);

/// The C library's AM/PM for `meridiem`, empty for NONE.
std::string clock_meridiem_text(ClockMeridiem meridiem);

/// Largest breakpoint text font no taller than `height` px, or the smallest when none fits.
const lv_font_t* clock_font_fitting(int32_t height);

/**
 * @brief One line of text a saver keeps on its canvas over pixels it redraws itself
 *
 * draw() puts the text back whenever it changed, its box moved, or a box in `under` (the
 * pixels the frame just rewrote) crosses it, so text survives a scene repainting beneath it.
 * Removing old text is the scene's job: the caller repaints the old box before draw() when
 * the text changes or moves.
 */
class SaverLabel {
  public:
    /// Draws `text` into `box` if needed and adds `box` to `dirty` when it does. Returns true
    /// when it drew.
    bool draw(SaverCanvas& canvas, const std::string& text, const DirtyRect& box, Rgb color,
              lv_opa_t opa, lv_text_align_t align, const std::vector<DirtyRect>& under,
              std::vector<DirtyRect>& dirty);

    /// The box last drawn into, empty when nothing is drawn.
    const DirtyRect& box() const {
        return box_;
    }
    const std::string& text() const {
        return text_;
    }
    /// Forgets what was drawn, so the next draw() draws.
    void reset();

  private:
    std::string text_;
    DirtyRect box_;
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
