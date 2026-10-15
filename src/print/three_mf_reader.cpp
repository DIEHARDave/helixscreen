// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "three_mf_reader.h"

#include "tinfl_inflate.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <mutex>
#include <tuple>

#if !defined(HELIX_PLATFORM_ESP32)
#include "hv/md5.h"
#endif

namespace helix::three_mf {

namespace {

constexpr const char* ROOT = "gcodes";

/// Summaries kept at once; a folder of archives scrolls through a few dozen.
constexpr size_t CACHE_ENTRIES = 64;

using Key = std::tuple<std::string, uint64_t, double>;

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
    for (const auto& p : sliced_plates(job->entries)) {
        PlateSummary plate;
        plate.index = p.index;
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
    const zip::ZipEntry entry = *info;
    job->transfers->download_file_range(
        ROOT, job->path, entry.lh_offset, static_cast<size_t>(zip::entry_read_span(entry)),
        [job, entry](const std::string& bytes) {
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

void read_tail(const std::shared_ptr<SummaryJob>& job, uint64_t len, bool may_grow) {
    job->tail_offset = job->size - len;
    job->transfers->download_file_range(
        ROOT, job->path, job->tail_offset, static_cast<size_t>(len),
        [job, len, may_grow](const std::string& bytes) {
            if (bytes.size() != len) {
                job->on_error(error_text(job->path, zip::ZipError::Truncated));
                return;
            }
            job->tail = bytes;
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
        },
        [job](const MoonrakerError& error) { job->on_error(job->path + ": " + error.message); },
        job->cancelled);
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
    std::map<Key, ArchiveSummary> summaries;
};

const zip::ZipEntry* plate_thumbnail(const PlateSummary& plate) {
    if (plate.thumb) {
        return &*plate.thumb;
    }
    return plate.thumb_small ? &*plate.thumb_small : nullptr;
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
    Key key{path, size, modified};
    {
        std::lock_guard<std::mutex> lock(cache_->mutex);
        if (auto it = cache_->summaries.find(key); it != cache_->summaries.end()) {
            const ArchiveSummary summary = it->second;
            on_done(summary);
            return;
        }
    }
    if (size == 0) {
        on_error(error_text(path, zip::ZipError::NotZip));
        return;
    }

    auto job = std::make_shared<SummaryJob>();
    job->transfers = &transfers_;
    job->path = path;
    job->size = size;
    job->on_done = std::move(on_done);
    job->on_error = std::move(on_error);
    job->cancelled = std::move(cancelled);
    job->remember = [cache = cache_, key](const ArchiveSummary& summary) {
        std::lock_guard<std::mutex> lock(cache->mutex);
        if (cache->summaries.size() >= CACHE_ENTRIES) {
            cache->summaries.erase(cache->summaries.begin());
        }
        cache->summaries[key] = summary;
    };
    read_tail(job, std::min<uint64_t>(size, zip::ZIP_TAIL_READ_BYTES), true);
}

void ThreeMfReader::read_entry(const std::string& path, const zip::ZipEntry& entry,
                               BytesCallback on_done, ErrorCallback on_error,
                               ITransfersAPI::CancelFlag cancelled) {
    if (entry.usize > MAX_ENTRY_BYTES) {
        on_error(error_text(path + "/" + entry.name, zip::ZipError::Truncated));
        return;
    }
    transfers_.download_file_range(
        ROOT, path, entry.lh_offset, static_cast<size_t>(zip::entry_read_span(entry)),
        [path, entry, on_done, on_error](const std::string& bytes) {
            zip::ZipError err = zip::ZipError::None;
            const std::string_view data =
                zip::entry_bytes(bytes_of(bytes), bytes.size(), entry, err);
            std::string out;
            if (err == zip::ZipError::None) {
                zip::inflate_entry_to_string<RawInflate>(entry, data, MAX_ENTRY_BYTES, out, err);
            }
            if (err != zip::ZipError::None) {
                on_error(error_text(path + "/" + entry.name, err));
                return;
            }
            on_done(out);
        },
        [path, on_error](const MoonrakerError& error) { on_error(path + ": " + error.message); },
        std::move(cancelled));
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
