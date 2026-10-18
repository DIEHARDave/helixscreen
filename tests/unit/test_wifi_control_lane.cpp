// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// WiFiManager's control lane and breaker: a backend that stops answering must
// never hold the LVGL thread, must not be polled while it is wedged, and must
// not be asked twice for a scan it is already running.

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/wifi_manager_test_access.h"
#include "async_lifetime_guard.h"
#include "http_executor.h"
#include "wifi_backend_mock.h"
#include "wifi_control_breaker.h"
#include "wifi_manager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::WiFiManager;
using helix::WiFiManagerTestAccess;
using helix::http::HttpExecutor;

namespace {

// Outlives any one backend, so a test can ask after the manager is gone.
std::atomic<int> g_forget_calls{0};

/// Every control call parks while `stall()` is in force, standing in for a
/// wpa_supplicant whose driver wedged: wpa_ctrl waits out its reply deadline.
/// The park is bounded so a call made on the test's own thread fails the
/// timing checks instead of hanging the run. get_status() can also be made
/// merely slow, which is what the breaker measures.
class StallingBackend : public WifiBackendMock {
  public:
    static constexpr auto MAX_STALL = std::chrono::seconds(3);

    WiFiError trigger_scan() override {
        enter(triggers_);
        return WiFiError(WiFiResult::SUCCESS);
    }
    WiFiError connect_network(const std::string&, const std::string&, bool) override {
        enter(connects_);
        return WiFiError(WiFiResult::SUCCESS);
    }
    WiFiError disconnect_network() override {
        enter(disconnects_);
        return WiFiError(WiFiResult::SUCCESS);
    }
    WiFiError forget_network(const std::string&) override {
        g_forget_calls++;
        enter(forgets_);
        return WiFiError(WiFiResult::SUCCESS);
    }
    WiFiError set_radio_enabled(bool on) override {
        enter(radios_);
        return WifiBackendMock::set_radio_enabled(on);
    }
    ConnectionStatus get_status() override {
        enter(statuses_);
        if (const int ms = status_delay_ms_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
        return WifiBackendMock::get_status();
    }

    void stall() {
        std::lock_guard<std::mutex> lock(mutex_);
        stalled_ = true;
    }
    void release() {
        // Notify under the lock: a parked call returns as soon as stalled_
        // flips, and this backend dies with its manager.
        std::lock_guard<std::mutex> lock(mutex_);
        stalled_ = false;
        cv_.notify_all();
    }
    void set_status_delay_ms(int ms) {
        status_delay_ms_ = ms;
    }

    int triggers() const {
        return triggers_.load();
    }
    int connects() const {
        return connects_.load();
    }
    int disconnects() const {
        return disconnects_.load();
    }
    int forgets() const {
        return forgets_.load();
    }
    int radios() const {
        return radios_.load();
    }
    int statuses() const {
        return statuses_.load();
    }
    int calls() const {
        return triggers() + connects() + disconnects() + forgets() + radios() + statuses();
    }
    std::vector<std::thread::id> callers() {
        std::lock_guard<std::mutex> lock(mutex_);
        return callers_;
    }

  private:
    void enter(std::atomic<int>& counter) {
        std::unique_lock<std::mutex> lock(mutex_);
        callers_.push_back(std::this_thread::get_id());
        counter++;
        cv_.wait_for(lock, MAX_STALL, [this] { return !stalled_; });
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    bool stalled_ = false;
    std::vector<std::thread::id> callers_;
    std::atomic<int> triggers_{0}, connects_{0}, disconnects_{0}, forgets_{0}, radios_{0},
        statuses_{0};
    std::atomic<int> status_delay_ms_{0};
};

struct ControlLaneFixture : LVGLTestFixture {
    StallingBackend* backend = nullptr;
    std::shared_ptr<WiFiManager> wm;
    helix::AsyncLifetimeGuard guard;
    const std::thread::id main_thread = std::this_thread::get_id();

    ControlLaneFixture() {
        HttpExecutor::fast().start();
        auto b = std::make_unique<StallingBackend>();
        backend = b.get();
        wm = std::make_shared<WiFiManager>(std::move(b));
        wm->init_self_reference(wm);
        REQUIRE(WiFiManagerTestAccess::settle(*wm));
    }

    ~ControlLaneFixture() override {
        if (wm) {
            backend->release();
            WiFiManagerTestAccess::settle(*wm);
            wm.reset();
        }
        helix::ui::UpdateQueue::instance().drain();
    }

    /// One status read, answered on this (the LVGL) thread.
    /// The answer is held by the callback, not this frame, so a REQUIRE that
    /// gives up leaves nothing for a late answer to write through.
    WifiBackend::ConnectionStatus read_status() {
        auto got = std::make_shared<std::optional<WifiBackend::ConnectionStatus>>();
        wm->get_status_async(guard.token(),
                             [got](const WifiBackend::ConnectionStatus& s) { *got = s; });
        REQUIRE(wait_until([&] { return got->has_value(); }));
        return **got;
    }
};

} // namespace

TEST_CASE_METHOD(ControlLaneFixture,
                 "WiFi UI entry points return at once while the backend is stalled",
                 "[wifi][wifi_control_lane]") {
    backend->stall();

    const auto started = std::chrono::steady_clock::now();
    wm->start_scan([](const std::vector<WiFiNetwork>&) {});
    wm->connect("Net", "password", [](bool, const std::string&, WiFiResult) {});
    wm->disconnect();
    wm->forget("Net", [](bool, const std::string&) {});
    wm->set_enabled_async(true, guard.token(), [](bool, bool) {});
    wm->get_status_async(guard.token(), [](const WifiBackend::ConnectionStatus&) {});
    const auto elapsed = std::chrono::steady_clock::now() - started;

    // One stalled call on this thread would cost StallingBackend::MAX_STALL.
    CHECK(elapsed < std::chrono::milliseconds(500));

    // The calls happen, one at a time and in order, on a worker.
    REQUIRE(wait_until([&] { return backend->triggers() == 1; }));
    CHECK(backend->calls() == 1);
    backend->release();
    REQUIRE(wait_until([&] {
        return backend->connects() == 1 && backend->disconnects() == 1 && backend->forgets() == 1 &&
               backend->radios() == 1 && backend->statuses() == 1;
    }));
    for (const auto& id : backend->callers()) {
        CHECK(id != main_thread);
    }
}

TEST_CASE_METHOD(ControlLaneFixture,
                 "a scan result reads the cached connection state, not the backend",
                 "[wifi][wifi_control_lane]") {
    backend->set_connected_state(true, "Home", "192.168.1.10", 80);
    CHECK(read_status().ssid == "Home");

    int delivered = 0;
    wm->start_scan([&](const std::vector<WiFiNetwork>&) { delivered++; });
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    backend->clear_status_callers();

    WiFiManagerTestAccess::fire_scan_complete(*wm);
    REQUIRE(wait_until([&] { return delivered == 1; }));

    const auto readers = backend->status_callers();
    CHECK(std::find(readers.begin(), readers.end(), main_thread) == readers.end());
}

TEST_CASE_METHOD(ControlLaneFixture, "a scan requested while one is in flight is not sent again",
                 "[wifi][wifi_control_lane]") {
    backend->stall();
    wm->start_scan([](const std::vector<WiFiNetwork>&) {});
    REQUIRE(wait_until([&] { return backend->triggers() == 1; }));

    // The user reopens the list while the first trigger is still out.
    int second = 0;
    wm->start_scan([&](const std::vector<WiFiNetwork>&) { second++; });
    backend->release();
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    CHECK(backend->triggers() == 1);

    // The one trigger's results reach the callback that asked second.
    WiFiManagerTestAccess::fire_scan_complete(*wm);
    REQUIRE(wait_until([&] { return second == 1; }));
}

TEST_CASE_METHOD(ControlLaneFixture,
                 "the control breaker trips on slow status reads, answers from cache, and "
                 "recovers on a probe",
                 "[wifi][wifi_control_lane][wifi_breaker]") {
    WiFiManagerTestAccess::set_breaker_config(*wm, {50, 3, 400, 2000});
    int notified = 0;
    WiFiManagerTestAccess::add_observer(*wm, guard.token(), [&] { notified++; });

    backend->set_connected_state(true, "Home", "192.168.1.10", 80);
    REQUIRE(read_status().ssid == "Home");

    // The driver wedges: every read now runs past the threshold.
    backend->set_status_delay_ms(80);
    for (int i = 0; i < 3; ++i) {
        read_status();
    }
    REQUIRE(WiFiManagerTestAccess::breaker_open(*wm));
    REQUIRE(wait_until([&] { return notified == 1; }));

    // While open, a poll is answered from the last good read without asking.
    const int asked = backend->statuses();
    const auto cached = read_status();
    CHECK(cached.unresponsive);
    CHECK(cached.connected);
    CHECK(cached.ssid == "Home");
    CHECK(backend->statuses() == asked);

    // So is a scan: nothing is sent, and the caller still hears back.
    bool scan_answered = false;
    wm->start_scan([&](const std::vector<WiFiNetwork>& n) { scan_answered = n.empty(); });
    REQUIRE(wait_until([&] { return scan_answered; }));
    CHECK(backend->triggers() == 0);

    // The driver recovers; once the backoff passes, one poll probes and closes it.
    backend->set_status_delay_ms(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const auto fresh = read_status();
    CHECK_FALSE(fresh.unresponsive);
    CHECK(backend->statuses() == asked + 1);
    CHECK_FALSE(WiFiManagerTestAccess::breaker_open(*wm));
    REQUIRE(wait_until([&] { return notified == 2; }));
}

TEST_CASE_METHOD(ControlLaneFixture, "a status answer for a caller that died is dropped",
                 "[wifi][wifi_control_lane]") {
    backend->stall();
    auto owner = std::make_unique<helix::AsyncLifetimeGuard>();
    bool answered = false;
    wm->get_status_async(owner->token(),
                         [&](const WifiBackend::ConnectionStatus&) { answered = true; });
    REQUIRE(wait_until([&] { return backend->statuses() == 1; }));

    owner.reset(); // the overlay closes while the read is parked
    backend->release();
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    CHECK_FALSE(answered);
}

TEST_CASE_METHOD(ControlLaneFixture,
                 "destroying the manager waits for the call in progress and drops the queue",
                 "[wifi][wifi_control_lane]") {
    backend->stall();
    wm->start_scan([](const std::vector<WiFiNetwork>&) {});
    REQUIRE(wait_until([&] { return backend->triggers() == 1; }));
    g_forget_calls = 0;
    bool forgot = false;
    wm->forget("Net", [&](bool, const std::string&) { forgot = true; });

    std::thread releaser([b = backend] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        b->release();
    });
    const auto started = std::chrono::steady_clock::now();
    wm.reset();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    releaser.join();
    helix::ui::UpdateQueue::instance().drain();

    CHECK(elapsed >= std::chrono::milliseconds(100));
    CHECK(g_forget_calls == 0);
    CHECK_FALSE(forgot);
    backend = nullptr;
}

TEST_CASE_METHOD(ControlLaneFixture,
                 "polls and scans behind a call that never returns are answered without it",
                 "[wifi][wifi_control_lane][wifi_breaker]") {
    // A send blocked on a daemon that stopped reading never finishes, so the
    // breaker never hears of it; the lane's own clock has to.
    WiFiManagerTestAccess::set_breaker_config(*wm, {50, 3, 400, 2000});
    backend->set_connected_state(true, "Home", "192.168.1.10", 80);
    REQUIRE(read_status().ssid == "Home");

    backend->stall();
    bool first_answered = false;
    wm->get_status_async(guard.token(),
                         [&](const WifiBackend::ConnectionStatus&) { first_answered = true; });
    REQUIRE(wait_until([&] { return backend->statuses() == 2; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Still parked, and a poll arriving now is answered from cache at once.
    const auto cached = read_status();
    CHECK(cached.unresponsive);
    CHECK(cached.ssid == "Home");
    CHECK_FALSE(first_answered);

    bool scan_answered = false;
    wm->start_scan([&](const std::vector<WiFiNetwork>& n) { scan_answered = n.empty(); });
    REQUIRE(wait_until([&] { return scan_answered; }, 1000));
    CHECK(backend->triggers() == 0);

    backend->release();
    REQUIRE(wait_until([&] { return first_answered; }));
}
