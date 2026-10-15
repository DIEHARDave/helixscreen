// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_filename_utils.h"

#include "three_mf.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <optional>
#include <vector>

namespace helix::gcode {

namespace {

// The printable-extension list every consumer shares: the Moonraker file list,
// the USB stick scanner and the display-name stripper. A second copy anywhere
// drifts, and each copy reads correct alone.
const std::vector<std::string>& printable_extensions() {
    static const std::vector<std::string> extensions = {".gcode", ".gco", ".g", ".3mf"};
    return extensions;
}

// Case-insensitive suffix match. A name exactly as long as the extension is a
// hidden dotfile (".gcode"), not a printable file.
bool ends_with_ci(const std::string& filename, const std::string& ext) {
    if (filename.size() <= ext.size()) {
        return false;
    }
    size_t pos = filename.size() - ext.size();
    for (size_t i = 0; i < ext.size(); ++i) {
        char c = static_cast<char>(std::tolower(static_cast<unsigned char>(filename[pos + i])));
        if (c != ext[i]) {
            return false;
        }
    }
    return true;
}

} // namespace

bool has_printable_extension(const std::string& filename) {
    for (const auto& ext : printable_extensions()) {
        if (ends_with_ci(filename, ext)) {
            return true;
        }
    }
    return false;
}

std::string join_gcode_path(const std::string& dir, const std::string& filename) {
    return dir.empty() ? filename : dir + "/" + filename;
}

std::string get_filename_basename(const std::string& path) {
    const size_t last_sep = last_path_separator(path);
    return last_sep == std::string::npos ? path : path.substr(last_sep + 1);
}

std::string strip_gcode_extension(const std::string& filename) {
    // A QIDI print file is "Name.gcode.3mf": the whole double extension goes.
    if (filename.size() > 4 && is_3mf(filename)) {
        const std::string inner = filename.substr(0, filename.size() - 4);
        return is_3mf(inner) ? inner : strip_gcode_extension(inner);
    }

    for (const auto& ext : printable_extensions()) {
        if (ends_with_ci(filename, ext)) {
            return filename.substr(0, filename.size() - ext.size());
        }
    }

    return filename;
}

std::string get_display_filename(const std::string& path) {
    return strip_gcode_extension(get_filename_basename(path));
}

// Pattern: .helix_temp/modified_123456789_OriginalName.gcode (Moonraker plugin)
// Also handles: */gcode_mod/mod_XXXXXX_filename.gcode (local temp files)
// Legacy: /tmp/helixscreen_mod_XXXXXX_filename.gcode
// The staging directory on the printer and the prefix inside it. Named once:
// producers build paths through make_rewritten_gcode_path() and consumers
// recognise them through is_uploaded_rewrite_path(), so neither side can spell
// it differently from the other.
static const std::string helix_temp_prefix = ".helix_temp/modified_";
// Where the HelixPrint plugin links a staged copy. Under its `full/` segment
// the remainder IS the original's gcodes-relative path; directly under the
// directory sits only a bare filename.
static const std::string helix_print_prefix = ".helix_print/";
static const std::string helix_print_full_segment = "full/";
static const std::string gcode_mod_prefix = "/gcode_mod/mod_";
static const std::string legacy_prefix = "/tmp/helixscreen_mod_";

// Start of the original's path inside a plugin symlink path, or npos. Klipper
// reports gcodes-relative paths, so the plugin's directory is only ever the
// leading segment; a user folder of that name deeper down is not a link.
static size_t helix_print_original_pos(const std::string& path) {
    if (path.size() <= helix_print_prefix.size() ||
        path.compare(0, helix_print_prefix.size(), helix_print_prefix) != 0) {
        return std::string::npos;
    }
    return helix_print_prefix.size();
}

bool is_rewritten_gcode_path(const std::string& path) {
    return path.find(helix_temp_prefix) != std::string::npos ||
           helix_print_original_pos(path) != std::string::npos ||
           path.find(gcode_mod_prefix) != std::string::npos ||
           path.find(legacy_prefix) != std::string::npos;
}

// A staged copy is one flat file in the staging directory, so its name carries
// the original's whole gcodes-relative path with '/' escaped: '~' -> "~~" and
// '/' -> "~s". The 'p' after the timestamp marks that escaping; a name without
// it holds a bare filename, unescaped.
static const char path_marker = 'p';

static std::string escape_path(const std::string& path) {
    std::string out;
    for (char c : path) {
        if (c == '~') {
            out += "~~";
        } else if (c == '/') {
            out += "~s";
        } else {
            out += c;
        }
    }
    return out;
}

// A copy extracted from one plate of an archive ends "~p<plate>.gcode" after
// the original's name: Klipper prints only G-code names, and the plate goes
// with it. Escaping never yields "~p", so in an escaped name it is unambiguous.
static const std::string plate_marker = "~p";
static const std::string plate_ext = ".gcode";

static std::string plate_suffix(int plate) {
    return plate_marker + std::to_string(plate) + plate_ext;
}

/// The plate @p name ends with, removed from it, or 0 when it carries none.
static int take_plate_suffix(std::string& name) {
    if (name.size() < plate_ext.size() ||
        name.compare(name.size() - plate_ext.size(), plate_ext.size(), plate_ext) != 0) {
        return 0;
    }
    size_t digits = name.size() - plate_ext.size();
    while (digits > 0 && std::isdigit(static_cast<unsigned char>(name[digits - 1]))) {
        --digits;
    }
    if (digits == name.size() - plate_ext.size() || digits < plate_marker.size() ||
        name.compare(digits - plate_marker.size(), plate_marker.size(), plate_marker) != 0) {
        return 0;
    }
    const int plate = std::atoi(name.c_str() + digits);
    name.resize(digits - plate_marker.size());
    return plate;
}

/// Unescapes @p name, ending at a plate suffix, whose plate goes to @p plate.
static std::string unescape_path(const std::string& name, int& plate) {
    std::string out;
    plate = 0;
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '~' && i + 1 < name.size()) {
            if (name[i + 1] == 'p') {
                std::string rest = name.substr(i);
                plate = take_plate_suffix(rest);
                if (plate > 0 && rest.empty()) {
                    break;
                }
                plate = 0;
            }
            out += name[i + 1] == 's' ? '/' : name[i + 1];
            ++i;
        } else {
            out += name[i];
        }
    }
    return out;
}

namespace {
/// What a rewritten path says about its original: the name it carries, and
/// whether that name is the original's whole gcodes-relative path or only a
/// bare filename that may belong to a file in any folder.
struct ParsedRewrite {
    std::string original;
    bool full_path = false;
    int plate = 0; ///< the archive plate a copy was extracted from, or 0
};
} // namespace

// The one reading of every rewrite shape this app or its plugin produces.
static std::optional<ParsedRewrite> parse_rewrite(const std::string& path) {
    if (const size_t start = helix_print_original_pos(path); start != std::string::npos) {
        // .helix_print/full/parts/benchy.gcode -> parts/benchy.gcode (whole path)
        // .helix_print/benchy.gcode            -> benchy.gcode (bare name)
        std::string rest = path.substr(start);
        if (rest.size() > helix_print_full_segment.size() &&
            rest.compare(0, helix_print_full_segment.size(), helix_print_full_segment) == 0) {
            return ParsedRewrite{rest.substr(helix_print_full_segment.size()), true};
        }
        return ParsedRewrite{std::move(rest), false};
    }

    size_t prefix_end = std::string::npos;
    if (const size_t pos = path.find(helix_temp_prefix); pos != std::string::npos) {
        // .helix_temp/modified_<ts>p_parts~sbenchy.gcode -> parts/benchy.gcode
        // .helix_temp/modified_<ts>_benchy.gcode         -> benchy.gcode
        prefix_end = pos + helix_temp_prefix.size();
        const size_t underscore = path.find('_', prefix_end);
        if (underscore != std::string::npos && underscore > prefix_end &&
            path[underscore - 1] == path_marker && underscore + 1 < path.size()) {
            int plate = 0;
            std::string original = unescape_path(path.substr(underscore + 1), plate);
            return ParsedRewrite{std::move(original), true, plate};
        }
    } else if (const size_t mod = path.find(gcode_mod_prefix); mod != std::string::npos) {
        // */gcode_mod/mod_123456_OriginalName.gcode -> OriginalName.gcode
        prefix_end = mod + gcode_mod_prefix.size();
    } else if (const size_t legacy = path.find(legacy_prefix); legacy != std::string::npos) {
        // /tmp/helixscreen_mod_123456_OriginalName.gcode -> OriginalName.gcode
        prefix_end = legacy + legacy_prefix.size();
    }
    if (prefix_end == std::string::npos) {
        return std::nullopt;
    }
    const size_t underscore = path.find('_', prefix_end);
    if (underscore == std::string::npos || underscore + 1 >= path.size()) {
        return std::nullopt;
    }
    std::string original = path.substr(underscore + 1);
    const int plate = take_plate_suffix(original);
    return ParsedRewrite{std::move(original), false, plate};
}

std::string resolve_gcode_filename(const std::string& path) {
    auto parsed = parse_rewrite(path);
    if (!parsed) {
        return path;
    }
    spdlog::debug("[resolve_gcode_filename] '{}' -> '{}'", path, parsed->original);
    return std::move(parsed->original);
}

std::optional<std::string> trusted_original_path(const std::string& path) {
    if (!is_rewritten_gcode_path(path)) {
        return path;
    }
    auto parsed = parse_rewrite(path);
    if (!parsed || !parsed->full_path) {
        return std::nullopt;
    }
    return std::move(parsed->original);
}

bool thumbnail_source_describes(const std::string& raw, const std::string& source) {
    if (raw == source) {
        return true;
    }
    // A rewritten temp path is the whole reason an override exists, and only
    // this app produces one - so it always belongs to a print we started, whose
    // preparing epoch set the override being held. Keep it even when the
    // original cannot be recovered from the string.
    if (is_rewritten_gcode_path(raw)) {
        return true;
    }
    const std::string resolved = resolve_gcode_filename(raw);
    if (resolved == source) {
        return true;
    }
    return get_filename_basename(resolved) == get_filename_basename(source);
}

bool is_native_3mf_shadow(const std::string& name) {
    static const std::string prefix = "shadow_native_plate_";
    static const std::string suffix = ".gcode";

    // Require at least one character between the prefix and suffix (the plate id).
    if (name.size() <= prefix.size() + suffix.size()) {
        return false;
    }
    if (name.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    return name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string qidi_3mf_extract_name(const std::string& print_filename) {
    return get_filename_basename(three_mf::extracted_gcode_name(print_filename, 0, false));
}

bool is_qidi_3mf_extract(const std::string& entry, const std::string& print_filename) {
    const std::string name = qidi_3mf_extract_name(print_filename);
    return entry.size() == name.size() && std::equal(entry.begin(), entry.end(), name.begin(),
                                                     [](unsigned char a, unsigned char b) {
                                                         return std::tolower(a) == std::tolower(b);
                                                     });
}

// The staged name for @p original_path, ending in @p suffix (empty, or a plate suffix).
static std::string staged_path(const std::string& original_path, const std::string& suffix) {
    // One path component on the printer's filesystem, so it must fit NAME_MAX.
    constexpr size_t name_max = 255;
    static const std::string staging_dir = ".helix_temp/";
    const std::string stamp = std::to_string(static_cast<long long>(std::time(nullptr)));

    std::string staged =
        helix_temp_prefix + stamp + path_marker + "_" + escape_path(original_path) + suffix;
    if (staged.size() - staging_dir.size() <= name_max) {
        return staged;
    }
    // Too long to carry the whole path: a bare filename, which
    // trusted_original_path() refuses to read as the original's location.
    staged = helix_temp_prefix + stamp + "_";
    std::string name = get_filename_basename(original_path);
    const size_t room = name_max - (staged.size() - staging_dir.size()) - suffix.size();
    if (name.size() > room) {
        // Keep the extension, and start on a UTF-8 lead byte: a name cut inside
        // a multibyte character is not a valid filename to upload.
        size_t start = name.size() - room;
        while (start < name.size() && (static_cast<unsigned char>(name[start]) & 0xC0) == 0x80) {
            ++start;
        }
        name = name.substr(start);
    }
    return staged + name + suffix;
}

std::string make_rewritten_gcode_path(const std::string& original_path) {
    return staged_path(original_path, "");
}

std::string make_extracted_plate_path(const std::string& archive_path, int plate) {
    return staged_path(archive_path, plate_suffix(plate));
}

int rewritten_archive_plate(const std::string& path) {
    const auto parsed = parse_rewrite(path);
    return parsed ? parsed->plate : 0;
}

bool is_uploaded_rewrite_path(const std::string& path) {
    return path.find(helix_temp_prefix) != std::string::npos;
}

bool is_3mf(const std::string& name) {
    return ends_with_ci(name, ".3mf");
}

} // namespace helix::gcode
