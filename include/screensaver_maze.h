// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include "print_history_data.h"
#include "screensaver_base.h"
#include "screensaver_canvas.h"
#include "screensaver_maze_sim.h"

#include <cstdint>
#include <functional>
#include <lvgl.h>
#include <optional>
#include <string>
#include <vector>

namespace helix::ui {

/// Wall art: at most this many recent prints, other files on the printer, and lookups spent
/// finding the files' (most files on a printer were never shown, so their thumbnails are not
/// in the cache).
inline constexpr size_t MAZE_ART_RECENT = 5;
inline constexpr size_t MAZE_ART_FILES = 6;
inline constexpr size_t MAZE_ART_FILE_PROBES = 48;
/// A thumbnail larger than this a side is not decoded; the printer picture is exempt.
inline constexpr int MAZE_ART_MAX_SOURCE_SIDE = 512;
/// Each picture is kept at most this size a side until its texture is built.
inline constexpr int MAZE_ART_PICTURE_SIDE = 64;
/// No new decode starts once start-up has spent this long on art.
inline constexpr uint32_t MAZE_ART_DECODE_BUDGET_MS = 120;

/// Where wall art comes from: LVGL paths of cached thumbnails, and the printer picture.
struct MazeArtSources {
    std::vector<std::string> recent;
    std::vector<std::string> files;
    std::string printer;
};

/// One file's thumbnail keys (paths from the gcodes root), best first.
using MazeArtCandidate = std::vector<std::string>;
/// The LVGL path of a key's cached thumbnail, or empty when it is not cached.
using MazeArtLookup = std::function<std::string(const std::string& key)>;

/// Up to `max_jobs` files from print history, newest first and each once, with their
/// thumbnails resolved from the gcodes root: the largest that fit
/// MAZE_ART_MAX_SOURCE_SIDE first, then the larger ones, smallest first.
std::vector<MazeArtCandidate> history_art_candidates(const std::vector<PrintHistoryJob>& jobs,
                                                     size_t max_jobs);

/**
 * @brief The first cached thumbnail of each candidate, in order
 *
 * Stops at `max` paths or after `max_probes` lookups; a path already in the result or in
 * `taken` is passed over.
 */
std::vector<std::string> cached_art_paths(const std::vector<MazeArtCandidate>& candidates,
                                          const MazeArtLookup& lookup, size_t max,
                                          size_t max_probes,
                                          const std::vector<std::string>& taken = {});

/// Recent, file and printer art interleaved one of each in turn, without repeats or empty
/// paths, up to `max`.
std::vector<std::string> mix_maze_art(const MazeArtSources& sources, size_t max);

/**
 * @brief Windows 95-style 3D Maze screensaver
 *
 * MazeSim walks a random maze and raycasts it Wolfenstein-style straight into the
 * canvas at the display's own depth; every frame repaints the whole canvas. The Helix icon
 * stands where the original placed OpenGL logos, and picture walls show the printer's image.
 */
class MazeScreensaver : public SaverBase {
  public:
    ScreensaverType type() const override {
        return ScreensaverType::MAZE_3D;
    }

  protected:
    std::optional<lv_color_format_t> canvas_format() const override {
        return SAVER_BUILD_CANVAS_FORMAT;
    }
    bool on_start() override;
    void on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty) override;
    void on_stop() override;
    size_t ladder_size() const override {
        return MAZE_LEVEL_COUNT;
    }
    uint32_t ladder_period_ms(size_t level) const override {
        return MAZE_LEVELS[level].period_ms;
    }
    void on_level_request(size_t level) override;

  private:
    MazeSim sim_;
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
