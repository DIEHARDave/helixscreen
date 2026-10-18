// SPDX-License-Identifier: GPL-3.0-or-later
// include/busy_episode_owner.h
#pragma once

#include <atomic>
#include <chrono>

namespace helix {

/**
 * Decides whether a busy episode (one continuous run of idle_timeout
 * "Printing") is one the app's own sends began.
 *
 * Any gcode flips idle_timeout to "Printing" while it runs, so a burst of the
 * app's own commands can hold the flag up continuously: a jog, or light taps
 * 0.2 s apart that each read "Printing" for ~0.8 s. A grace window after the
 * last ack cannot tell that apart from an external op for long, because a
 * sustained burst outlasts any window. The episode's start time can: the
 * episode is the app's when the app sent something within GRACE_WINDOW of it.
 *
 * Ownership is decided once per episode, from the send the app made last
 * before first asking about it. A later send never adopts an episode someone
 * else began. It then lapses MAX_SELF_BUSY after the latest send, bounding how
 * long an operation that starts mid-burst can pass as the app's.
 *
 * Thread-safe: stamped from the main thread, read at gcode send time.
 */
class BusyEpisodeOwner {
  public:
    using clock = std::chrono::steady_clock;
    static constexpr std::chrono::seconds GRACE_WINDOW{2};
    /// Longest a busy episode can pass as the app's own after its last send.
    static constexpr std::chrono::seconds MAX_SELF_BUSY{30};

    void note_sent(clock::time_point now = clock::now()) {
        last_sent_ns_.store(now.time_since_epoch().count(), std::memory_order_relaxed);
    }

    bool owns(clock::time_point episode_start, clock::time_point now = clock::now()) {
        const long long start_ns = episode_start.time_since_epoch().count();
        const auto last_ns = last_sent_ns_.load(std::memory_order_relaxed);
        if (last_ns == 0) {
            return false;
        }
        const clock::time_point last{clock::duration{last_ns}};
        if (decided_episode_ns_.load(std::memory_order_relaxed) != start_ns) {
            const bool started =
                last >= episode_start - GRACE_WINDOW && last <= episode_start + GRACE_WINDOW;
            episode_owned_.store(started, std::memory_order_relaxed);
            decided_episode_ns_.store(start_ns, std::memory_order_relaxed);
        }
        return episode_owned_.load(std::memory_order_relaxed) && (now - last) < MAX_SELF_BUSY;
    }

  private:
    std::atomic<long long> last_sent_ns_{0};
    std::atomic<long long> decided_episode_ns_{0};
    std::atomic<bool> episode_owned_{false};
};

} // namespace helix
