// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_LABEL_PRINTER

#include "pwg_raster.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

namespace helix::pwg {

static constexpr size_t kPwgHeaderSize = 1796;
static constexpr uint8_t kSyncWord[4] = {0x52, 0x61, 0x53, 0x32}; // "RaS2"

// Write a big-endian uint32 at the given byte offset
static void put_be32(uint8_t* buf, size_t offset, uint32_t value) {
    buf[offset] = static_cast<uint8_t>((value >> 24) & 0xFF);
    buf[offset + 1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    buf[offset + 2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    buf[offset + 3] = static_cast<uint8_t>(value & 0xFF);
}

std::vector<uint8_t> build_page_header(const PageParams& params) {
    std::vector<uint8_t> hdr(kPwgHeaderSize, 0x00);

    // MediaType at offset 128 (64-byte field, zero-padded)
    size_t mt_len = std::min(params.media_type.size(), size_t{63});
    std::memcpy(hdr.data() + 128, params.media_type.data(), mt_len);

    // HWResolution[2] at offset 276
    put_be32(hdr.data(), 276, params.hw_resolution_x);
    put_be32(hdr.data(), 280, params.hw_resolution_y);

    // Width at offset 368, Height at offset 372
    put_be32(hdr.data(), 368, params.width_px);
    put_be32(hdr.data(), 372, params.height_px);

    // BitsPerColor at offset 380
    put_be32(hdr.data(), 380, params.bits_per_color);

    // BitsPerPixel at offset 384
    put_be32(hdr.data(), 384, params.bits_per_pixel);

    // BytesPerLine at offset 388
    uint32_t bytes_per_line = params.width_px * (params.bits_per_pixel / 8);
    put_be32(hdr.data(), 388, bytes_per_line);

    // ColorOrder at offset 392 (0 = chunky)
    put_be32(hdr.data(), 392, 0);

    // ColorSpace at offset 396
    put_be32(hdr.data(), 396, static_cast<uint32_t>(params.color_space));

    // NumColors at offset 404
    put_be32(hdr.data(), 404, params.num_colors);

    return hdr;
}

void packbits_encode(const uint8_t* line, size_t len, std::vector<uint8_t>& out) {
    size_t pos = 0;

    while (pos < len) {
        // Check for a run of identical bytes (need at least 2)
        if (pos + 1 < len && line[pos] == line[pos + 1]) {
            uint8_t val = line[pos];
            size_t run = 1;
            while (pos + run < len && line[pos + run] == val && run < 128) {
                run++;
            }
            // Emit run: count-1 as unsigned byte, then the repeated byte
            out.push_back(static_cast<uint8_t>(run - 1));
            out.push_back(val);
            pos += run;
        } else {
            // Collect literal bytes (stop when we see a run of 2+ identical)
            size_t lit_start = pos;
            size_t lit_count = 0;

            while (pos < len && lit_count < 128) {
                // If next two bytes are a run, stop literals here
                if (pos + 1 < len && line[pos] == line[pos + 1]) {
                    break;
                }
                pos++;
                lit_count++;
            }

            if (lit_count > 0) {
                // Emit literal: (256 - count) as unsigned byte, then the bytes
                out.push_back(static_cast<uint8_t>(256 - lit_count));
                out.insert(out.end(), line + lit_start, line + lit_start + lit_count);
            }
        }
    }
}

// Expand a 1bpp row to 8bpp grayscale (bit=1 → 0x00 black, bit=0 → 0xFF white)
static void expand_1bpp_to_8bpp(const uint8_t* src, int src_row_bytes, int width_px, uint8_t* dst) {
    for (int x = 0; x < width_px; x++) {
        int byte_idx = x / 8;
        int bit_idx = 7 - (x % 8);
        bool black = (byte_idx < src_row_bytes) ? ((src[byte_idx] >> bit_idx) & 1) != 0 : false;
        dst[x] = black ? 0x00 : 0xFF;
    }
}

// Encode page data: line repeat counts + PackBits-compressed scanlines
static void encode_page(const LabelBitmap& bitmap, const PageParams& params,
                        std::vector<uint8_t>& out) {
    int width = static_cast<int>(params.width_px);
    int height = static_cast<int>(params.height_px);
    int bmp_w = bitmap.width();
    int bmp_h = bitmap.height();
    uint32_t bytes_per_pixel = params.bits_per_pixel / 8;
    size_t line_bytes = static_cast<size_t>(width) * bytes_per_pixel;

    // Current and previous scanline buffers for repeat detection
    std::vector<uint8_t> cur_line(line_bytes, 0xFF);
    std::vector<uint8_t> prev_line(line_bytes, 0xFF);

    // Temporary buffer for PackBits output of a single line
    std::vector<uint8_t> packed;
    packed.reserve(line_bytes + line_bytes / 128 + 2);

    int y = 0;
    while (y < height) {
        // Expand current row from 1bpp to 8bpp
        if (y < bmp_h) {
            if (bytes_per_pixel == 1) {
                expand_1bpp_to_8bpp(bitmap.row_data(y), bitmap.row_byte_width(),
                                    std::min(width, bmp_w), cur_line.data());
                // If bitmap is narrower than page, fill remainder with white
                if (bmp_w < width) {
                    std::memset(cur_line.data() + bmp_w, 0xFF, width - bmp_w);
                }
            } else {
                // SRGB: expand each 1bpp pixel to 3 bytes (R=G=B)
                for (int x = 0; x < width; x++) {
                    bool black = (x < bmp_w) ? bitmap.get_pixel(x, y) : false;
                    uint8_t val = black ? 0x00 : 0xFF;
                    size_t off = static_cast<size_t>(x) * 3;
                    cur_line[off] = val;
                    cur_line[off + 1] = val;
                    cur_line[off + 2] = val;
                }
            }
        } else {
            // Beyond bitmap height: white scanline
            std::memset(cur_line.data(), 0xFF, line_bytes);
        }

        // Count how many consecutive identical lines follow
        uint8_t repeat_count = 0;
        int next_y = y + 1;
        while (next_y < height && repeat_count < 255) {
            // Expand candidate line
            bool same = false;
            if (next_y < bmp_h) {
                if (bytes_per_pixel == 1) {
                    expand_1bpp_to_8bpp(bitmap.row_data(next_y), bitmap.row_byte_width(),
                                        std::min(width, bmp_w), prev_line.data());
                    if (bmp_w < width) {
                        std::memset(prev_line.data() + bmp_w, 0xFF, width - bmp_w);
                    }
                } else {
                    for (int x = 0; x < width; x++) {
                        bool black = (x < bmp_w) ? bitmap.get_pixel(x, next_y) : false;
                        uint8_t val = black ? 0x00 : 0xFF;
                        size_t off = static_cast<size_t>(x) * 3;
                        prev_line[off] = val;
                        prev_line[off + 1] = val;
                        prev_line[off + 2] = val;
                    }
                }
                same = (cur_line == prev_line);
            } else {
                // Beyond bitmap: all white. Check if current is all white too.
                bool cur_all_white = true;
                for (size_t i = 0; i < line_bytes; i++) {
                    if (cur_line[i] != 0xFF) {
                        cur_all_white = false;
                        break;
                    }
                }
                same = cur_all_white;
            }

            if (!same)
                break;
            repeat_count++;
            next_y++;
        }

        // Emit: repeat count byte + PackBits-encoded line
        out.push_back(repeat_count);

        packed.clear();
        packbits_encode(cur_line.data(), line_bytes, packed);
        out.insert(out.end(), packed.begin(), packed.end());

        y = next_y;
    }
}

std::vector<uint8_t> generate(const LabelBitmap& page_bitmap, const PageParams& params) {
    PageParams p = params;
    if (p.width_px == 0)
        p.width_px = static_cast<uint32_t>(page_bitmap.width());
    if (p.height_px == 0)
        p.height_px = static_cast<uint32_t>(page_bitmap.height());

    uint32_t bytes_per_pixel = p.bits_per_pixel / 8;
    size_t line_bytes = static_cast<size_t>(p.width_px) * bytes_per_pixel;
    size_t uncompressed_estimate = line_bytes * p.height_px;

    spdlog::debug("pwg: generating {}x{} page, {}bpp, ~{} bytes uncompressed", p.width_px,
                  p.height_px, p.bits_per_pixel, uncompressed_estimate);

    // Pre-allocate: sync + header + estimated compressed data
    // PackBits worst case is input_size + input_size/128 + 1 per line,
    // plus 1 byte repeat count per unique line. Estimate 50% compression.
    size_t estimated = 4 + kPwgHeaderSize + uncompressed_estimate / 2;
    std::vector<uint8_t> out;
    out.reserve(estimated);

    // Sync word
    out.insert(out.end(), kSyncWord, kSyncWord + 4);

    // Page header
    auto hdr = build_page_header(p);
    out.insert(out.end(), hdr.begin(), hdr.end());

    // Page data
    encode_page(page_bitmap, p, out);

    spdlog::debug("pwg: output size {} bytes ({:.0f}% of uncompressed)", out.size(),
                  uncompressed_estimate > 0 ? (100.0 * out.size() / uncompressed_estimate) : 0.0);

    return out;
}

std::vector<uint8_t> generate_multi(const std::vector<const LabelBitmap*>& pages,
                                    const PageParams& params) {
    if (pages.empty()) {
        spdlog::warn("pwg: generate_multi called with no pages");
        return {};
    }

    spdlog::debug("pwg: generating {} page(s)", pages.size());

    // Sync word (once, at start of document)
    std::vector<uint8_t> out;
    out.reserve(4 + pages.size() * (kPwgHeaderSize + 64 * 1024));
    out.insert(out.end(), kSyncWord, kSyncWord + 4);

    for (size_t i = 0; i < pages.size(); i++) {
        if (!pages[i]) {
            spdlog::warn("pwg: skipping null page {}", i);
            continue;
        }

        PageParams p = params;
        if (p.width_px == 0)
            p.width_px = static_cast<uint32_t>(pages[i]->width());
        if (p.height_px == 0)
            p.height_px = static_cast<uint32_t>(pages[i]->height());

        auto hdr = build_page_header(p);
        out.insert(out.end(), hdr.begin(), hdr.end());

        encode_page(*pages[i], p, out);
    }

    spdlog::debug("pwg: multi-page output {} bytes", out.size());
    return out;
}

} // namespace helix::pwg

#endif // HELIX_HAS_LABEL_PRINTER
