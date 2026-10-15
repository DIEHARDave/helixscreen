// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Reading a sliced .3mf that sits on the printer, a few small ranged reads at a
// time, and extracting one plate's G-code from a local copy of it.
//
// read_summary() costs one read of the archive's tail (its central directory)
// and, when slice_info.config is not already inside that tail, one more for it.
// read_entry() costs one read per entry, which is how a card's thumbnail is
// fetched. Every callback runs on the transfer's worker thread.

#include "i_moonraker_sub_apis.h"
#include "three_mf.h"
#include "zip_reader.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace helix::three_mf {

/// What a card or the detail view shows for one sliced plate.
struct PlateSummary {
    int index = 0;
    bool has_info = false; ///< slice_info.config carried a block for this plate
    SliceInfo info;
    std::optional<zip::ZipEntry> thumb;       ///< Metadata/plate_<N>.png
    std::optional<zip::ZipEntry> thumb_small; ///< Metadata/plate_<N>_small.png
};

/// A sliced 3MF's plates, by plate number. Empty: a project nothing was sliced in.
struct ArchiveSummary {
    std::vector<PlateSummary> plates;
};

/// The thumbnail entry to draw for @p plate: the full render, else the small one.
const zip::ZipEntry* plate_thumbnail(const PlateSummary& plate);

/// Largest central directory read_summary() fetches when the tail read missed part of it.
inline constexpr size_t MAX_CENTRAL_DIRECTORY_BYTES = 1024 * 1024;

/// Largest entry read_entry() inflates into memory.
inline constexpr size_t MAX_ENTRY_BYTES = 2 * 1024 * 1024;

class ThreeMfReader {
  public:
    using SummaryCallback = std::function<void(const ArchiveSummary&)>;
    using BytesCallback = std::function<void(const std::string&)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    /// @p transfers must outlive every read started through this reader.
    explicit ThreeMfReader(ITransfersAPI& transfers);

    /**
     * @brief Reads the plates of the archive at @p path (gcodes root) on the printer.
     *
     * @p size is the archive's size from the file listing: the tail read is a
     * plain byte range, which every transport can make. A summary is kept per
     * (path, modified, size) and answered from memory, synchronously, until
     * the file changes.
     */
    void read_summary(const std::string& path, uint64_t size, double modified,
                      SummaryCallback on_done, ErrorCallback on_error,
                      ITransfersAPI::CancelFlag cancelled = nullptr);

    /// Reads and, when it is deflated, inflates one entry of the archive at @p path.
    void read_entry(const std::string& path, const zip::ZipEntry& entry, BytesCallback on_done,
                    ErrorCallback on_error, ITransfersAPI::CancelFlag cancelled = nullptr);

    /**
     * @brief Writes plate @p plate's G-code from the local archive @p local_zip to @p local_gcode.
     *
     * Streams the entry through the inflater a buffer at a time, so the memory
     * it takes does not grow with the plate. Checks the entry's CRC and, when
     * the archive carries one, its .md5. A failure removes @p local_gcode.
     * Unavailable on a transport with no local copies (the firmware): Io.
     */
    static zip::ZipError extract_plate(const std::string& local_zip, int plate,
                                       const std::string& local_gcode);

    /// Drops every kept summary.
    void clear_cache();

  private:
    struct Cache;
    ITransfersAPI& transfers_;
    std::shared_ptr<Cache> cache_;
};

} // namespace helix::three_mf
