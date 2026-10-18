// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// After notify_klippy_disconnected the client polls printer.info until Klippy
// settles, because Moonraker announces a successful start but never a failed
// one: the poll is how a startup error reaches the klippy state.

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/moonraker_client_test_access.h"
#include "app_globals.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Answers printer.info from a scripted result and counts the requests; every
/// other method goes to the mock as usual.
class PrinterInfoClient : public MoonrakerClientMock {
  public:
    using MoonrakerClientMock::MoonrakerClientMock;

    RequestId
    send_jsonrpc(const std::string& method, const json& params,
                 std::function<void(const json&)> success_cb,
                 std::function<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms = 0,
                 bool silent = false,
                 std::optional<rpc_error_policy::CallerIntent> intent = std::nullopt) override {
        if (method != "printer.info") {
            return MoonrakerClientMock::send_jsonrpc(method, params, std::move(success_cb),
                                                     std::move(error_cb), timeout_ms, silent,
                                                     intent);
        }
        ++info_requests;
        if (success_cb) {
            success_cb(json{{"jsonrpc", "2.0"}, {"result", info}});
        }
        return 1;
    }

    json info = {{"state", "startup"}, {"state_message", "Printer is starting up"}};
    int info_requests = 0;
};

std::string klippy_notification(const char* method) {
    return json{{"jsonrpc", "2.0"}, {"method", method}, {"params", json::array()}}.dump();
}

const char* const kKey298 = "Internal error during connect: !{\"code\": \"key298\", \"msg\": "
                            "\"Can not update MCU rpi config as it is shutdown\"}";

class KlippyPollFixture : public LVGLTestFixture {
  public:
    KlippyPollFixture() : client_(MoonrakerClientMock::PrinterType::VORON_24) {
        get_printer_state().init_subjects(false);
    }

    ~KlippyPollFixture() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    KlippyState klippy_state() {
        return static_cast<KlippyState>(
            lv_subject_get_int(get_printer_state().network_state().get_klippy_state_subject()));
    }

    void disconnect_klippy() {
        client_.on_ws_message(klippy_notification("notify_klippy_disconnected"));
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(klippy_state() == KlippyState::SHUTDOWN);
    }

    void tick() {
        MoonrakerClientTestAccess::poll_klippy_state(client_);
        helix::ui::UpdateQueue::instance().drain();
    }

    PrinterInfoClient client_;
};

} // namespace

TEST_CASE_METHOD(KlippyPollFixture, "Klippy poll: a startup error after disconnect reaches ERROR",
                 "[klippy][klippy_poll]") {
    disconnect_klippy();
    REQUIRE(MoonrakerClientTestAccess::klippy_state_polling(client_));

    // Still on its way: nothing applied, the poll carries on.
    client_.info = {{"state", "disconnected"}, {"state_message", ""}};
    tick();
    client_.info = {{"state", "startup"}, {"state_message", "Printer is starting up"}};
    tick();
    REQUIRE(klippy_state() == KlippyState::SHUTDOWN);
    REQUIRE(MoonrakerClientTestAccess::klippy_state_polling(client_));

    client_.info = {{"state", "error"}, {"state_message", kKey298}};
    tick();

    REQUIRE(klippy_state() == KlippyState::ERROR);
    REQUIRE(get_printer_state().network_state().get_klippy_fault_message() == kKey298);
    REQUIRE(client_.info_requests == 3);

    // Settled: the next tick asks nothing.
    REQUIRE_FALSE(MoonrakerClientTestAccess::klippy_state_polling(client_));
    tick();
    REQUIRE(client_.info_requests == 3);
}

TEST_CASE_METHOD(KlippyPollFixture, "Klippy poll: ready after disconnect reaches READY and stops",
                 "[klippy][klippy_poll]") {
    disconnect_klippy();

    client_.info = {{"state", "ready"}, {"state_message", "Printer is ready"}};
    tick();

    REQUIRE(klippy_state() == KlippyState::READY);
    REQUIRE_FALSE(MoonrakerClientTestAccess::klippy_state_polling(client_));
    tick();
    REQUIRE(client_.info_requests == 1);
}

TEST_CASE_METHOD(KlippyPollFixture, "Klippy poll: notify_klippy_ready ends the poll",
                 "[klippy][klippy_poll]") {
    disconnect_klippy();
    client_.on_ws_message(klippy_notification("notify_klippy_ready"));
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE_FALSE(MoonrakerClientTestAccess::klippy_state_polling(client_));
    tick();
    REQUIRE(client_.info_requests == 0);
}

TEST_CASE_METHOD(KlippyPollFixture, "Klippy poll: a closed socket ends the poll",
                 "[klippy][klippy_poll]") {
    disconnect_klippy();
    MoonrakerClientTestAccess::fire_ws_close(client_);
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE_FALSE(MoonrakerClientTestAccess::klippy_state_polling(client_));
    client_.info = {{"state", "error"}, {"state_message", kKey298}};
    tick();
    REQUIRE(client_.info_requests == 0);
    REQUIRE(klippy_state() != KlippyState::ERROR);
}

TEST_CASE_METHOD(KlippyPollFixture, "Klippy poll: nothing polls while Klippy is connected",
                 "[klippy][klippy_poll]") {
    REQUIRE_FALSE(MoonrakerClientTestAccess::klippy_state_polling(client_));
    tick();
    REQUIRE(client_.info_requests == 0);
}
