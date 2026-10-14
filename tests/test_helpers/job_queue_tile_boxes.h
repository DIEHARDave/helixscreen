// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace helix::test {

/// The medium-tier pixel widths the job queue tile's rungs need, with the fixture's
/// fonts: the Cell rung holds "Start next" in a button, the Wide rung the longer text.
inline constexpr int kJobQueueCellW = 124;
inline constexpr int kJobQueueWideW = 244;

} // namespace helix::test
