// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_maze.h"

#include "printer_image_manager.h"

#include <spdlog/spdlog.h>

#include <draw/lv_image_decoder_private.h>
#include <string>

namespace helix::ui {

namespace {

constexpr const char* LOGO_PATH = "A:assets/images/helix-icon-128.png";

/// The pixels of a decoded image as 0xAARRGGBB, for the formats LVGL decodes art into.
bool to_argb(const lv_draw_buf_t* buf, MazeImage& out) {
    const auto w = static_cast<int>(buf->header.w);
    const auto h = static_cast<int>(buf->header.h);
    const uint32_t stride = buf->header.stride;
    const auto cf = static_cast<lv_color_format_t>(buf->header.cf);
    if (w <= 0 || h <= 0) {
        return false;
    }
    out.w = w;
    out.h = h;
    out.argb.assign(static_cast<size_t>(w) * h, 0);
    for (int y = 0; y < h; y++) {
        const uint8_t* row = buf->data + static_cast<size_t>(y) * stride;
        for (int x = 0; x < w; x++) {
            uint32_t a = 0xFF, r = 0, g = 0, b = 0;
            switch (cf) {
            case LV_COLOR_FORMAT_ARGB8888:
            case LV_COLOR_FORMAT_XRGB8888:
                b = row[x * 4];
                g = row[x * 4 + 1];
                r = row[x * 4 + 2];
                a = cf == LV_COLOR_FORMAT_ARGB8888 ? row[x * 4 + 3] : 0xFF;
                break;
            case LV_COLOR_FORMAT_RGB888:
                b = row[x * 3];
                g = row[x * 3 + 1];
                r = row[x * 3 + 2];
                break;
            case LV_COLOR_FORMAT_RGB565:
            case LV_COLOR_FORMAT_RGB565A8: {
                const auto v = static_cast<uint32_t>(row[x * 2] | row[x * 2 + 1] << 8);
                r = (v >> 11 & 0x1F) << 3;
                g = (v >> 5 & 0x3F) << 2;
                b = (v & 0x1F) << 3;
                if (cf == LV_COLOR_FORMAT_RGB565A8) {
                    // The alpha plane follows the colour plane, one byte per pixel.
                    const uint8_t* alpha = buf->data + static_cast<size_t>(stride) * h;
                    a = alpha[static_cast<size_t>(y) * (stride / 2) + x];
                }
                break;
            }
            default:
                return false;
            }
            out.argb[static_cast<size_t>(y) * w + x] = a << 24 | r << 16 | g << 8 | b;
        }
    }
    return true;
}

/// Decodes `path` into `out`; leaves it empty when the image cannot be read.
void load_image(const std::string& path, MazeImage& out, const char* what) {
    out = MazeImage{};
    if (path.empty()) {
        return;
    }
    lv_image_decoder_dsc_t dsc;
    const lv_result_t res = lv_image_decoder_open(&dsc, path.c_str(), nullptr);
    if (res != LV_RESULT_OK || dsc.decoded == nullptr) {
        spdlog::debug("[Screensaver] Maze {} '{}' did not decode", what, path);
        if (res == LV_RESULT_OK) {
            lv_image_decoder_close(&dsc);
        }
        return;
    }
    if (!to_argb(dsc.decoded, out)) {
        spdlog::debug("[Screensaver] Maze {} '{}' has colour format {}, not used", what, path,
                      static_cast<int>(dsc.decoded->header.cf));
        out = MazeImage{};
    }
    lv_image_decoder_close(&dsc);
}

} // namespace

bool MazeScreensaver::on_start() {
    spdlog::info("[Screensaver] Starting 3D maze");
    MazeArt art;
    load_image(LOGO_PATH, art.logo, "logo");
    load_image(helix::PrinterImageManager::instance().get_displayed_image_path(screen_w()),
               art.picture, "picture");
    FrameTarget frame = canvas().frame();
    sim_.init(frame, art, rng(), level());
    spdlog::debug("[Screensaver] Maze started ({}x{}, level {}, logo {}, picture {})", screen_w(),
                  screen_h(), level(), art.logo.empty() ? "drawn" : "decoded",
                  art.picture.empty() ? "none" : "decoded");
    return true;
}

void MazeScreensaver::on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty) {
    FrameTarget frame = canvas().frame();
    sim_.step(dt_ms, frame, rng(), dirty);
}

void MazeScreensaver::on_level_request(size_t level) {
    apply_level(level);
    sim_.set_level(this->level());
}

void MazeScreensaver::on_stop() {
    sim_.clear();
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
