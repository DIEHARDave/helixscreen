// SPDX-License-Identifier: GPL-3.0-or-later
// include/busy_episode_owner.h
#pragma once

#include <array>
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
 * episode is the app's when the app sent something in the GRACE_WINDOW before
 * it. Only a send made before the episode was observed can have caused it:
 * episode_start is when the main thread saw idle_timeout go high, and a send
 * is stamped before it goes out. A send after the start, even a fraction of a
 * second after, is joining someone else's operation.
 *
 * Asks come late (the first one ~SETTLE into the episode, after more sends
 * have landed), so the recent send times are kept rather than only the last.
 * Ownership is decided once per episode, on the first ask, and then lapses
 * MAX_SELF_BUSY after the latest send, bounding how long an operation that
 * starts mid-burst can pass as the app's. More than HISTORY sends between an
 * episode's start and its first ask reads as not owned.
 *
 * Thread-safe: stamped from the main thread, read at gcode send time.
 */
class BusyEpisodeOwner {
  public:
    using clock = std::chrono::steady_clock;
    static constexpr std::chrono::seconds GRACE_WINDOW{2};
    /// Longest a busy episode can pass as the app's own after its last send.
    static constexpr std::chrono::seconds MAX_SELF_BUSY{30};

    /// Send times kept to answer whether one landed just before an episode.
    static constexpr unsigned HISTORY = 16;

    void note_sent(clock::time_point now = clock::now()) {
        const long long ns = now.time_since_epoch().count();
        const unsigned slot = next_.fetch_add(1, std::memory_order_relaxed) % HISTORY;
        sent_ns_[slot].store(ns, std::memory_order_relaxed);
        last_sent_ns_.store(ns, std::memory_order_relaxed);
    }

    bool owns(clock::time_point episode_start, clock::time_point now = clock::now()) {
        const long long start_ns = episode_start.time_since_epoch().count();
        const auto last_ns = last_sent_ns_.load(std::memory_order_relaxed);
        if (last_ns == 0) {
            return false;
        }
        if (decided_episode_ns_.load(std::memory_order_relaxed) != start_ns) {
            episode_owned_.store(sent_just_before(episode_start), std::memory_order_relaxed);
            decided_episode_ns_.store(start_ns, std::memory_order_relaxed);
        }
        const clock::time_point last{clock::duration{last_ns}};
        return episode_owned_.load(std::memory_order_relaxed) && (now - last) < MAX_SELF_BUSY;
    }

  private:
    bool sent_just_before(clock::time_point episode_start) const {
        const long long lo = (episode_start - GRACE_WINDOW).time_since_epoch().count();
        const long long hi = episode_start.time_since_epoch().count();
        for (const auto& slot : sent_ns_) {
            const long long ns = slot.load(std::memory_order_relaxed);
            if (ns != 0 && ns >= lo && ns <= hi) {
                return true;
            }
        }
        return false;
    }

    std::array<std::atomic<long long>, HISTORY> sent_ns_{};
    std::atomic<unsigned> next_{0};
    std::atomic<long long> last_sent_ns_{0};
    std::atomic<long long> decided_episode_ns_{0};
    std::atomic<bool> episode_owned_{false};
};

} // namespace helix
