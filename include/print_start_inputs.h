// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/**
 * @file print_start_inputs.h
 * @brief The file half of the pre-start checks, computed without a file view.
 *
 * The print-select file view derives a print's tool info, lane mapping, remap
 * and empty-slot pre-flight from the file's palette, its used tools and the live
 * filament state. A start with no screen (a queued job started from the home
 * tile) needs the same answers, so the derivation and the file read behind it
 * live here, and the file view calls them too.
 */

#include "async_lifetime_guard.h"
#include "filament_mapper.h"
#include "gcode_footer_summary.h"
#include "moonraker_types.h"
#include "preflight_validator.h"
#include "print_start_checks.h"

#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace helix {

class IMoonrakerAPI;

/// The slicer palette Moonraker's metadata carries: per-tool colors as given,
/// per-tool materials split from the ';'-separated filament_type.
struct FilePalette {
    std::vector<std::string> colors;
    std::vector<std::string> materials;
};

[[nodiscard]] FilePalette file_palette_from_metadata(const FileMetadata& metadata);

/// The file half of the pre-start checks' input.
struct FileStartFacts {
    std::string filename; ///< gcodes-relative
    std::optional<FileMetadata> metadata;
    FilePalette palette;
    std::set<int> tools_used;       ///< empty when the scan could not answer
    std::vector<double> tool_grams; ///< empty means unknown, never zero
};

/// The mapping a print uses: @p card_mappings when the file view's mapping card
/// holds any (its seed, or the user's edit), else the automatic seed for
/// @p used_tool_info against the live slots.
[[nodiscard]] std::vector<ToolMapping>
effective_file_mappings(std::vector<ToolMapping> card_mappings,
                        const std::vector<GcodeToolInfo>& used_tool_info);

/// The tool-to-slot remap @p mappings send: only genuine remaps, judged against
/// the backend's firmware routing.
[[nodiscard]] std::map<int, int> effective_file_remap(const std::vector<ToolMapping>& mappings);

/// The empty-slot pre-flight for @p used_tool_info printed through @p mappings.
[[nodiscard]] PreflightResult file_preflight(const std::vector<GcodeToolInfo>& used_tool_info,
                                             const std::vector<ToolMapping>& mappings);

/// Fill the file fields of @p ctx from @p facts and the live filament state,
/// the derivation the file view uses when its mapping card holds no user edit:
/// FilamentMappingCard::build_used_tool_info(), AmsState::seed_tool_mappings(),
/// AmsState::collect_available_slots(), FilamentMapper::identity_filtered_remap().
/// Sets ctx.has_file_facts.
void fill_file_context(PrintStartContext& ctx, const FileStartFacts& facts);

/// The empty-slot pre-flight for @p facts, computed with the same inputs.
[[nodiscard]] PreflightResult preflight_for(const FileStartFacts& facts);

/// Gathers FileStartFacts for one file: metadata, then the tools/palette/grams
/// footer read, then a whole-file tools scan when the footer cannot answer.
/// The file view runs its own scan through the same steps, handing in its
/// shared download so the viewer preview and the scan use one transfer.
class FileStartFactsLoader {
  public:
    /// Downloads the file and answers ok plus a local path the scan may read.
    using DownloadFn =
        std::function<void(std::function<void(bool ok, const std::string& local_path)>)>;
    using ReadyFn = std::function<void(FileStartFacts facts)>;

    explicit FileStartFactsLoader(IMoonrakerAPI* api);
    ~FileStartFactsLoader();
    FileStartFactsLoader(const FileStartFactsLoader&) = delete;
    FileStartFactsLoader& operator=(const FileStartFactsLoader&) = delete;

    void set_api(IMoonrakerAPI* api);

    /// Gather facts for @p filename and answer once, on the main thread. A
    /// failed metadata read, footer read or download still answers, with what
    /// it has. A later load() supersedes an unanswered one.
    void load(const std::string& filename, ReadyFn on_ready, DownloadFn download = nullptr);

    /// One file's tools/palette/grams read: the half of load() after the metadata.
    struct ScanRequest {
        std::string file_key; ///< ToolsUsedCache key; the Moonraker path unless a USB file
        uint64_t size_bytes = 0;
        time_t modified = 0;
        uint64_t gcode_end_byte = 0;
        gcode::FooterReadNeed need;
        size_t palette_size = 0; ///< the whole-file scan stops once every palette tool is seen
        /// A readable copy on this host to read the tail from, or empty. Asked
        /// only when the footer read is issued.
        std::function<std::string()> local_source;
        bool on_moonraker = true; ///< false for a USB file Moonraker has no copy of
        DownloadFn download;      ///< null: download to a temp file, deleted after the scan
    };

    /// What a scan reports. Each runs on the main thread, and none runs once a
    /// later scan(), load() or cancel() has superseded the scan.
    struct ScanHooks {
        /// The footer answered what it was read for. Its tools answer, when
        /// it gave one, is already in the ToolsUsedCache.
        std::function<void(const gcode::GcodeFooterSummary& summary)> on_footer;
        /// The footer did not answer. @p full_scan_follows is need.full_scan_justified().
        std::function<void(bool full_scan_follows)> on_footer_unanswered;
        /// The whole-file scan finished. @p read_file is false when the file
        /// could not be had, in which case @p tools is empty and says nothing.
        std::function<void(std::set<int> tools, bool read_file)> on_tools_scanned;
    };

    enum class ScanStart {
        NothingToRead, ///< need asks nothing: no hook will run
        Unreadable,    ///< no API, or a .3mf with no G-code lines: no hook will run
        Started,
    };

    /// Read @p req's file; supersedes any scan or load still unanswered.
    [[nodiscard]] ScanStart scan(ScanRequest req, ScanHooks hooks);

    /// Drop every unanswered scan or load.
    void cancel();

  private:
    void start_tail_read(uint32_t generation, ScanRequest req, ScanHooks hooks);
    void start_full_scan(uint32_t generation, ScanRequest req, ScanHooks hooks);
    void download_to_temp(const std::string& file_key,
                          std::function<void(bool ok, const std::string& local_path)> done);

    IMoonrakerAPI* api_ = nullptr;
    uint32_t generation_ = 0;
    AsyncLifetimeGuard lifetime_;
};

} // namespace helix
