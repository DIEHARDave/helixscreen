// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "src/ui/panel_widgets/size_rung.h"

namespace helix::test {

/// A home tile's box and the rung it is just big enough for.
struct Box {
    int w;
    int h;
    SizeRung rung;
};

/// The medium-tier (800x480) home grid's tiles: one cell and two cells across.
inline constexpr int kTileCellW = 114;
inline constexpr int kTileWideW = 233;

/// The four rungs of that grid's ladder, from a 2x1 cell's Strip to the Full tile.
inline constexpr Box kStrip{kTileCellW, 54, SizeRung::Strip};
inline constexpr Box kCell{kTileCellW, 113, SizeRung::Cell};
inline constexpr Box kWide{kTileWideW, 113, SizeRung::Wide};
inline constexpr Box kFull{kTileWideW, 232, SizeRung::Full};

} // namespace helix::test
