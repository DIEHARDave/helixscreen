// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

namespace helix::demo {

/**
 * @brief Z-Mod's color picker, line for line as its CHANGE_ZCOLOR macro sends it
 *
 * One text line, three groups of eight label-less swatches and a footer Cancel
 * (prestonbrown/helixscreen#1806). Shared by `ctl demo action-prompt-zmod` and
 * the action prompt layout tests, so both show the same prompt.
 */
inline std::vector<std::string> zmod_color_picker_lines() {
    static constexpr const char* kSlotCmd = "CHANGE_ZCOLOR SLOT=1 TYPE=PETG-CF HEX=";
    static constexpr const char* const kGroups[3][8] = {
        {"ffffff", "fff245", "def578", "21cc3d", "167a4b", "156682", "24e4a0", "7bd9f0"},
        {"4caaf8", "2e54dd", "48358c", "a341f7", "f435f6", "d584de", "fa6173", "f82d29"},
        {"805003", "f9903b", "fcebd7", "d5c5a1", "b17c38", "8c8c89", "bebebe", "1b1b1b"},
    };
    std::vector<std::string> lines = {
        "// action:prompt_begin Выберите цвет",
        "// action:prompt_text Катушка 1: PETG-CF/",
    };
    for (const auto& group : kGroups) {
        lines.emplace_back("// action:prompt_button_group_start");
        for (const char* hex : group) {
            lines.push_back(std::string("// action:prompt_button _ |") + kSlotCmd + hex +
                            " HIDE=0|" + hex);
        }
        lines.emplace_back("// action:prompt_button_group_end");
    }
    lines.emplace_back(
        "// action:prompt_footer_button Отмена|RESPOND TYPE=command MSG=action:prompt_end");
    lines.emplace_back("// action:prompt_show");
    return lines;
}

} // namespace helix::demo
