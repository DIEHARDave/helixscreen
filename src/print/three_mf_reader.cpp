// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "three_mf_reader.h"

#include "tinfl_inflate.h"
#include "versioned_lru.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>

#if !defined(HELIX_PLATFORM_ESP32)
#include "hv/md5.h"
#endif

namespace helix::three_mf {

namespace {

constexpr const char* ROOT = "gcodes";

/// Summaries kept at once; a folder of archives scrolls through a few dozen.
constexpr size_t CACHE_ENTRIES = 64;

std::string error_text(const std::string& path, zip::ZipError error) {
    return path + ": " + zip::zip_error_name(error);
}

const uint8_t* bytes_of(const std::string& s) {
    return reinterpret_cast<const uint8_t*>(s.data());
}

// One read_summary() in flight: the tail it read and what it has found.
struct SummaryJob {
    ITransfersAPI* transfers;
    std::string path;
    uint64_t size;
    ThreeMfReader::SummaryCallback on_done;
    ThreeMfReader::ErrorCallback on_error;
    ITransfersAPI::CancelFlag cancelled;
    std::function<void(const ArchiveSummary&)> remember;

    std::string tail;
    uint64_t tail_offset = 0;
    std::vector<zip::ZipEntry> entries;
    ArchiveSummary summary;
};

void read_tail(const std::shared_ptr<SummaryJob>& job, uint64_t len, bool may_grow);

/// Whether the reader of @p job has stopped wanting it: nothing more is asked
/// for, and no callback runs.
bool abandoned(const std::shared_ptr<SummaryJob>& job) {
    return job->cancelled && job->cancelled->load();
}

void finish(const std::shared_ptr<SummaryJob>& job) {
    job->remember(job->summary);
    job->on_done(job->summary);
}

void apply_slice_info(const std::shared_ptr<SummaryJob>& job, const zip::ZipEntry& entry,
                      const uint8_t* lh, size_t n) {
    zip::ZipError err = zip::ZipError::None;
    const std::string_view data = zip::entry_bytes(lh, n, entry, err);
    std::string xml;
    if (err == zip::ZipError::None) {
        zip::inflate_entry_to_string<RawInflate>(entry, data, SLICE_INFO_MAX_BYTES, xml, err);
    }
    if (err != zip::ZipError::None) {
        // The plates still print; only their time and filament go unknown.
        spdlog::warn("[ThreeMfReader] slice_info unreadable in {}: {}", job->path,
                     zip::zip_error_name(err));
        return;
    }
    for (auto& plate : job->summary.plates) {
        plate.has_info = parse_slice_info(xml, plate.index, plate.info);
    }
}

void read_plates(const std::shared_ptr<SummaryJob>& job) {
    if (abandoned(job)) {
        return;
    }
    for (const auto& p : sliced_plates(job->entries)) {
        PlateSummary plate;
        plate.index = p.index;
        plate.gcode = *p.gcode;
        if (p.thumb) {
            plate.thumb = *p.thumb;
        }
        if (p.thumb_small) {
            plate.thumb_small = *p.thumb_small;
        }
        job->summary.plates.push_back(std::move(plate));
    }
    const zip::ZipEntry* info = find_archive_entry(job->entries, SLICE_INFO_ENTRY);
    if (job->summary.plates.empty() || !info) {
        finish(job);
        return;
    }
    // slice_info.config sits near the end of a slicer's archive, so the tail
    // read usually holds it already.
    if (info->lh_offset >= job->tail_offset) {
        const size_t at = static_cast<size_t>(info->lh_offset - job->tail_offset);
        zip::ZipError err = zip::ZipError::None;
        zip::entry_bytes(bytes_of(job->tail) + at, job->tail.size() - at, *info, err);
        if (err == zip::ZipError::None) {
            apply_slice_info(job, *info, bytes_of(job->tail) + at, job->tail.size() - at);
            finish(job);
            return;
        }
    }
    if (!zip::entry_fits(*info, SLICE_INFO_MAX_BYTES)) {
        spdlog::warn("[ThreeMfReader] slice_info in {} is too large to read", job->path);
        finish(job);
        return;
    }
    const zip::ZipEntry entry = *info;
    job->transfers->download_file_range(
        ROOT, job->path, entry.lh_offset, static_cast<size_t>(zip::entry_read_span(entry)),
        [job, entry](const std::string& bytes) {
            if (abandoned(job)) {
                return;
            }
            apply_slice_info(job, entry, bytes_of(bytes), bytes.size());
            finish(job);
        },
        [job](const MoonrakerError& error) {
            spdlog::warn("[ThreeMfReader] slice_info read failed for {}: {}", job->path,
                         error.message);
            finish(job);
        },
        job->cancelled);
}

void parse_tail(const std::shared_ptr<SummaryJob>& job, uint64_t len, bool may_grow);

// A tail read for an archive whose size is not known: a suffix range, sized
// afterwards from the end record.
void read_tail_unsized(const std::shared_ptr<SummaryJob>& job) {
    if (abandoned(job)) {
        return;
    }
    job->transfers->download_file_tail(
        ROOT, job->path, zip::ZIP_TAIL_READ_BYTES,
        [job](const std::string& bytes) {
            if (abandoned(job)) {
                return;
            }
            const auto size = zip::archive_size_from_tail(bytes_of(bytes), bytes.size());
            if (!size) {
                job->on_error(error_text(job->path, zip::ZipError::NotZip));
                return;
            }
            job->size = *size;
            job->tail = bytes;
            job->tail_offset = *size - bytes.size();
            parse_tail(job, bytes.size(), true);
        },
        [job](const MoonrakerError& error) { job->on_error(job->path + ": " + error.message); });
}

void read_tail(const std::shared_ptr<SummaryJob>& job, uint64_t len, bool may_grow) {
    if (abandoned(job)) {
        return;
    }
    job->tail_offset = job->size - len;
    job->transfers->download_file_range(
        ROOT, job->path, job->tail_offset, static_cast<size_t>(len),
        [job, len, may_grow](const std::string& bytes) {
            if (abandoned(job)) {
                return;
            }
            if (bytes.size() != len) {
                job->on_error(error_text(job->path, zip::ZipError::Truncated));
                return;
            }
            job->tail = bytes;
            parse_tail(job, len, may_grow);
        },
        [job](const MoonrakerError& error) { job->on_error(job->path + ": " + error.message); },
        job->cancelled);
}

void parse_tail(const std::shared_ptr<SummaryJob>& job, uint64_t len, bool may_grow) {
    zip::ZipError err = zip::ZipError::None;
    uint64_t needed = 0;
    if (!zip::parse_central_directory(bytes_of(job->tail), job->tail.size(), job->size,
                                      job->entries, err, &needed)) {
        if (err == zip::ZipError::Truncated && may_grow && needed > len &&
            needed <= MAX_CENTRAL_DIRECTORY_BYTES) {
            read_tail(job, needed, false);
            return;
        }
        job->on_error(error_text(job->path, err));
        return;
    }
    read_plates(job);
}

#if !defined(HELIX_PLATFORM_ESP32)
struct File {
    FILE* f = nullptr;
    explicit File(FILE* file) : f(file) {}
    ~File() {
        if (f) {
            std::fclose(f);
        }
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
};

bool read_at(FILE* f, uint64_t offset, size_t len, std::string& out) {
    out.resize(len);
    if (std::fseek(f, static_cast<long>(offset), SEEK_SET) != 0) {
        return false;
    }
    out.resize(std::fread(out.data(), 1, len, f));
    return out.size() == len;
}

std::string lower_hex(std::string_view s) {
    std::string out;
    for (char c : s) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return out;
}

zip::ZipError extract_from(FILE* in, uint64_t size, int plate_index, FILE* out) {
    std::string tail;
    const uint64_t tail_len = std::min<uint64_t>(size, zip::ZIP_TAIL_READ_BYTES);
    if (!read_at(in, size - tail_len, static_cast<size_t>(tail_len), tail)) {
        return zip::ZipError::Io;
    }
    std::vector<zip::ZipEntry> entries;
    zip::ZipError err = zip::ZipError::None;
    uint64_t needed = 0;
    if (!zip::parse_central_directory(bytes_of(tail), tail.size(), size, entries, err, &needed)) {
        if (err != zip::ZipError::Truncated || needed <= tail_len ||
            needed > MAX_CENTRAL_DIRECTORY_BYTES ||
            !read_at(in, size - needed, static_cast<size_t>(needed), tail) ||
            !zip::parse_central_directory(bytes_of(tail), tail.size(), size, entries, err)) {
            return err == zip::ZipError::None ? zip::ZipError::Io : err;
        }
    }

    const auto plates = sliced_plates(entries);
    const auto plate = std::find_if(plates.begin(), plates.end(), [plate_index](const auto& p) {
        return p.index == plate_index;
    });
    if (plate == plates.end()) {
        return zip::ZipError::Missing;
    }

    // The archive's own checksum of the plate, when it carries one.
    std::string want_md5;
    if (plate->md5 && !zip::entry_fits(*plate->md5, 256)) {
        return zip::ZipError::Md5; // no digest is that long
    }
    if (plate->md5) {
        std::string lh;
        read_at(in, plate->md5->lh_offset,
                static_cast<size_t>(std::min<uint64_t>(zip::entry_read_span(*plate->md5),
                                                       size - plate->md5->lh_offset)),
                lh);
        const std::string_view data = zip::entry_bytes(bytes_of(lh), lh.size(), *plate->md5, err);
        std::string text;
        if (err == zip::ZipError::None) {
            zip::inflate_entry_to_string<RawInflate>(*plate->md5, data, 256, text, err);
        }
        if (err != zip::ZipError::None) {
            return err;
        }
        want_md5 = lower_hex(text);
    }

    const zip::ZipEntry& gcode = *plate->gcode;
    std::string header;
    const size_t header_len = static_cast<size_t>(std::min<uint64_t>(
        zip::ZIP_LOCAL_HEADER_BYTES + gcode.name.size() + zip::ZIP_LOCAL_EXTRA_SLACK,
        size - std::min(size, gcode.lh_offset)));
    read_at(in, gcode.lh_offset, header_len, header);
    const auto data_offset = zip::entry_data_offset(bytes_of(header), header.size(), gcode);
    if (!data_offset) {
        return zip::ZipError::BadLocalHeader;
    }
    if (*data_offset + gcode.csize > size) {
        return zip::ZipError::Truncated;
    }
    if (std::fseek(in, static_cast<long>(*data_offset), SEEK_SET) != 0) {
        return zip::ZipError::Io;
    }

    HV_MD5_CTX md5;
    HV_MD5Init(&md5);
    auto sink = [out, &md5](const uint8_t* p, size_t n) {
        HV_MD5Update(&md5, const_cast<unsigned char*>(p), static_cast<unsigned int>(n));
        return std::fwrite(p, 1, n, out) == n;
    };
    zip::ZipEntryInflater<RawInflate> inflater(gcode);
    std::string buf(64 * 1024, '\0');
    uint64_t left = gcode.csize;
    while (left > 0) {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(left, buf.size()));
        const size_t got = std::fread(buf.data(), 1, want, in);
        if (got != want) {
            return zip::ZipError::Io;
        }
        left -= got;
        if (!inflater.feed(bytes_of(buf), got, sink)) {
            break;
        }
    }
    if (!inflater.finish(err)) {
        return err;
    }
    if (std::fflush(out) != 0) {
        return zip::ZipError::Io;
    }
    if (!want_md5.empty()) {
        unsigned char digest[16];
        HV_MD5Final(&md5, digest);
        static const char* const HEX = "0123456789abcdef";
        std::string got;
        for (unsigned char b : digest) {
            got += HEX[b >> 4];
            got += HEX[b & 0xF];
        }
        if (got != want_md5) {
            return zip::ZipError::Md5;
        }
    }
    return zip::ZipError::None;
}
#endif

} // namespace

struct ThreeMfReader::Cache {
    std::mutex mutex;
    VersionedLru<ArchiveSummary> summaries; ///< by path, for one version of the file
};

const zip::ZipEntry* plate_thumbnail(const PlateSummary& plate) {
    if (plate.thumb) {
        return &*plate.thumb;
    }
    return plate.thumb_small ? &*plate.thumb_small : nullptr;
}

bool entry_payload(const std::string& read, const zip::ZipEntry& entry, size_t max_bytes,
                   std::string& out, zip::ZipError& error) {
    const std::string_view data = zip::entry_bytes(bytes_of(read), read.size(), entry, error);
    return error == zip::ZipError::None &&
           zip::inflate_entry_to_string<RawInflate>(entry, data, max_bytes, out, error);
}

FileMetadata plate_file_metadata(const PlateSummary& plate) {
    FileMetadata meta;
    meta.estimated_time = plate.info.prediction_s;
    meta.filament_weight_total = plate.info.weight_g;
    size_t tools = 0;
    for (const auto& f : plate.info.filaments) {
        tools = std::max(tools, static_cast<size_t>(std::max(f.id, 1)));
        meta.filament_total += f.used_m * 1000.0;
    }
    std::vector<std::string> types(tools);
    meta.filament_colors.assign(tools, "");
    meta.filament_weights.assign(tools, 0.0);
    for (const auto& f : plate.info.filaments) {
        const size_t t = static_cast<size_t>(std::max(f.id, 1)) - 1;
        types[t] = f.type;
        meta.filament_colors[t] = f.color;
        meta.filament_weights[t] = f.used_g;
    }
    for (size_t t = 0; t < types.size(); ++t) {
        meta.filament_type += (t ? ";" : "") + types[t];
    }
    return meta;
}

FileMetadata archive_file_metadata(const ArchiveSummary& summary, const PlateSummary& shown) {
    FileMetadata meta = plate_file_metadata(shown);
    meta.estimated_time = 0;
    meta.filament_weight_total = 0;
    for (const auto& p : summary.plates) {
        meta.estimated_time += p.info.prediction_s;
        meta.filament_weight_total += p.info.weight_g;
    }
    return meta;
}

const PlateSummary* find_plate(const ArchiveSummary& summary, int index) {
    for (const auto& p : summary.plates) {
        if (p.index == index) {
            return &p;
        }
    }
    return summary.plates.empty() ? nullptr : &summary.plates.front();
}

ThreeMfReader::ThreeMfReader(ITransfersAPI& transfers)
    : transfers_(transfers), cache_(std::make_shared<Cache>()) {}

void ThreeMfReader::clear_cache() {
    std::lock_guard<std::mutex> lock(cache_->mutex);
    cache_->summaries.clear();
}

void ThreeMfReader::read_summary(const std::string& path, uint64_t size, double modified,
                                 SummaryCallback on_done, ErrorCallback on_error,
                                 ITransfersAPI::CancelFlag cancelled) {
    const FileVersion version{size, static_cast<time_t>(modified)};
    {
        std::lock_guard<std::mutex> lock(cache_->mutex);
        if (const ArchiveSummary* kept = cache_->summaries.lookup(path, version)) {
            const ArchiveSummary summary = *kept;
            on_done(summary);
            return;
        }
    }

    auto job = std::make_shared<SummaryJob>();
    job->transfers = &transfers_;
    job->path = path;
    job->size = size;
    job->on_done = std::move(on_done);
    job->on_error = std::move(on_error);
    job->cancelled = std::move(cancelled);
    job->remember = [cache = cache_, path, version](const ArchiveSummary& summary) {
        if (version.size_bytes == 0) {
            return; // an unsized read names no version of the file to keep it for
        }
        std::lock_guard<std::mutex> lock(cache->mutex);
        cache->summaries.put(path, version, summary);
        cache->summaries.evict(CACHE_ENTRIES); // the least recently used go first
    };
    if (size == 0) {
        read_tail_unsized(job);
        return;
    }
    read_tail(job, std::min<uint64_t>(size, zip::ZIP_TAIL_READ_BYTES), true);
}

void ThreeMfReader::read_entry(ITransfersAPI& transfers, const std::string& path,
                               const zip::ZipEntry& entry, BytesCallback on_done,
                               ErrorCallback on_error, ITransfersAPI::CancelFlag cancelled) {
    if (!zip::entry_fits(entry, MAX_ENTRY_BYTES)) {
        on_error(error_text(path + "/" + entry.name, zip::ZipError::Truncated));
        return;
    }
    transfers.download_file_range(
        ROOT, path, entry.lh_offset, static_cast<size_t>(zip::entry_read_span(entry)),
        [path, entry, on_done, on_error](const std::string& bytes) {
            zip::ZipError err = zip::ZipError::None;
            std::string out;
            if (!entry_payload(bytes, entry, MAX_ENTRY_BYTES, out, err)) {
                on_error(error_text(path + "/" + entry.name, err));
                return;
            }
            on_done(out);
        },
        [path, on_error](const MoonrakerError& error) { on_error(path + ": " + error.message); },
        std::move(cancelled));
}

void ThreeMfReader::read_entry_head(ITransfersAPI& transfers, const std::string& path,
                                    const zip::ZipEntry& entry, size_t max_bytes,
                                    BytesCallback on_done, ErrorCallback on_error) {
    // Deflate rarely shrinks G-code below its own length per byte read, so
    // max_bytes of the compressed stream inflate to at least max_bytes.
    const uint64_t take = std::min<uint64_t>(entry.csize, max_bytes);
    const uint64_t span =
        zip::ZIP_LOCAL_HEADER_BYTES + entry.name.size() + zip::ZIP_LOCAL_EXTRA_SLACK + take;
    transfers.download_file_range(
        ROOT, path, entry.lh_offset, static_cast<size_t>(span),
        [path, entry, max_bytes, take, on_done, on_error](const std::string& bytes) {
            const auto data = zip::entry_data_offset(bytes_of(bytes), bytes.size(), entry);
            if (!data) {
                on_error(error_text(path + "/" + entry.name, zip::ZipError::BadLocalHeader));
                return;
            }
            const size_t start = static_cast<size_t>(*data - entry.lh_offset);
            const size_t have =
                start < bytes.size() ? std::min<size_t>(bytes.size() - start, take) : 0;
            std::string out;
            auto sink = [&out, max_bytes](const uint8_t* p, size_t n) {
                out.append(reinterpret_cast<const char*>(p), std::min(n, max_bytes - out.size()));
                return out.size() < max_bytes;
            };
            zip::ZipEntryInflater<RawInflate> inflater(entry);
            inflater.feed(bytes_of(bytes) + start, have, sink);
            // Stopping at max_bytes is the point; anything else is a failure.
            if (out.empty() && inflater.error() != zip::ZipError::None) {
                on_error(error_text(path + "/" + entry.name, inflater.error()));
                return;
            }
            on_done(out);
        },
        [path, on_error](const MoonrakerError& error) { on_error(path + ": " + error.message); });
}

zip::ZipError ThreeMfReader::extract_plate(const std::string& local_zip, int plate,
                                           const std::string& local_gcode) {
#if defined(HELIX_PLATFORM_ESP32)
    (void)local_zip;
    (void)plate;
    (void)local_gcode;
    return zip::ZipError::Io;
#else
    File in(std::fopen(local_zip.c_str(), "rb"));
    if (!in.f || std::fseek(in.f, 0, SEEK_END) != 0) {
        return zip::ZipError::Io;
    }
    const long end = std::ftell(in.f);
    if (end <= 0) {
        return zip::ZipError::NotZip;
    }
    zip::ZipError err = zip::ZipError::Io;
    {
        File out(std::fopen(local_gcode.c_str(), "wb"));
        if (out.f) {
            err = extract_from(in.f, static_cast<uint64_t>(end), plate, out.f);
            if (std::fclose(out.f) != 0 && err == zip::ZipError::None) {
                err = zip::ZipError::Io;
            }
            out.f = nullptr;
        }
    }
    if (err != zip::ZipError::None) {
        std::remove(local_gcode.c_str());
        spdlog::warn("[ThreeMfReader] Plate {} of {} not extracted: {}", plate, local_zip,
                     zip::zip_error_name(err));
    }
    return err;
#endif
}

} // namespace helix::three_mf
