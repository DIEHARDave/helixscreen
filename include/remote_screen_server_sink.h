// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "remote_screen_sink.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace helix {

/// How long after the last request a viewer counts as present.
constexpr int64_t kViewerIdleMs = 10000;
/// How long the first request after idle waits for a full repaint to land.
constexpr int64_t kFirstFrameWaitMs = 500;

/// Encode rate ceilings. Host work during toolhead motion is what trips "timer too
/// close" on the small boards, so printing caps lower, and lower still on them.
struct SnapshotRateCap {
    int idle_fps;
    int printing_fps;
};

SnapshotRateCap snapshot_rate_cap(bool constrained_host);
/// Little memory or a single core: the class of board where printing caps at 1 fps.
bool remote_screen_host_is_constrained();
std::chrono::milliseconds min_encode_interval(SnapshotRateCap cap, bool printing);

struct EncodedSnapshot {
    std::shared_ptr<const std::vector<uint8_t>> jpeg;
    uint64_t generation = 0;
    int w = 0;
    int h = 0;
};

/**
 * @brief The latest rendered screen, shared between the flush hook and the HTTP server.
 *
 * The flush hook (main thread) copies dirty rects into a shadow frame; the HTTP
 * thread turns the shadow into a JPEG on request. Nothing is allocated until a
 * viewer asks, and everything is freed once the viewer has been gone for
 * kViewerIdleMs. A static screen costs one encode in total: the JPEG is cached
 * with the frame generation it was made from.
 *
 * Thread-safe. The mutex is held for one rect's copy and for the RGB888
 * conversion, never across the JPEG encode.
 */
class ScreenFrameStore {
  public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    /// An empty @p clock means `steady_clock::now`.
    explicit ScreenFrameStore(Clock clock = {});

    /// True within kViewerIdleMs of the last request. Cheap: called on every flush.
    bool wants_frames() const;

    /// Copy one dirty rect in, (re)allocating the shadow at the frame's display size
    /// and pixel format, and advance the generation.
    void on_frame(const RemoteScreenFrame& f);

    enum class Need { Ready, FullRedraw };
    /// Mark a viewer as present. FullRedraw means the shadow cannot be trusted to
    /// hold the whole screen, and the caller should invalidate the display: always
    /// while nothing has been captured, and once after a resize.
    Need note_request();

    /// Wait until the generation passes @p gen. False on timeout or abort_waits().
    bool wait_for_generation_after(uint64_t gen, std::chrono::milliseconds timeout);
    /// Wake every waiter now; they return false.
    void abort_waits();

    uint64_t generation() const;

    /// The captured screen's size, {0, 0} when nothing is captured.
    std::pair<int, int> frame_size() const;

    /// The current screen as a JPEG, or nullopt when nothing has been captured. Reuses
    /// the cached JPEG when the generation is unchanged or the last encode is newer
    /// than min_encode_interval(cap, printing).
    std::optional<EncodedSnapshot> snapshot(bool printing, SnapshotRateCap cap);

    /// Free every buffer if no viewer is present. True if it freed anything.
    bool release_if_idle();
    /// Free every buffer unconditionally.
    void release();

    size_t allocated_bytes() const;

  private:
    std::chrono::steady_clock::time_point now() const;
    void release_locked();

    Clock clock_;

    /// steady_clock ticks of the last request; min() = never.
    std::atomic<int64_t> last_request_{INT64_MIN};

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    uint64_t abort_epoch_ = 0;

    std::vector<uint8_t> shadow_;
    int shadow_w_ = 0;
    int shadow_h_ = 0;
    int shadow_bpp_ = 0; ///< bytes per pixel: 2 or 4
    bool has_frame_ = false;
    bool full_redraw_needed_ = true;
    uint64_t generation_ = 0;

    std::vector<uint8_t> rgb_;
    std::shared_ptr<const std::vector<uint8_t>> cached_jpeg_;
    uint64_t cached_generation_ = 0;
    int cached_w_ = 0;
    int cached_h_ = 0;
    std::chrono::steady_clock::time_point last_encode_{};

    /// Serializes encodes, which read rgb_ outside mutex_.
    std::mutex encode_mutex_;
};

/// The RemoteScreenSink that feeds a ScreenFrameStore.
class ScreenServerSink : public RemoteScreenSink {
  public:
    explicit ScreenServerSink(std::shared_ptr<ScreenFrameStore> store);

    bool start() override;
    void stop() override;
    bool wants_frames() const override;
    void on_frame(const RemoteScreenFrame& frame) override;
    const char* name() const override;

  private:
    std::shared_ptr<ScreenFrameStore> store_;
};

} // namespace helix
