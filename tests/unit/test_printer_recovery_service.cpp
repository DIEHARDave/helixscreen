// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// PrinterRecoveryService's chain: an accepted printer.firmware_restart is
// verified against printer.info before it counts as recovered, and the local
// helix-recover.sh runs at most once when Klippy fails to come back.

#include "../lvgl_test_fixture.h"
#include "app_globals.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_recovery_service.h"
#include "printer_state.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Answers the three calls the chain makes from scripted values, on the
/// calling thread, and counts them.
class ScriptedRecoveryApi : public MoonrakerAPIMock {
  public:
    using MoonrakerAPIMock::MoonrakerAPIMock;

    void restart_firmware(SuccessCallback on_success, ErrorCallback on_error) override {
        ++firmware_restarts;
        if (firmware_error) {
            on_error(*firmware_error);
        } else {
            on_success();
        }
    }

    void restart_service(const std::string& /*service_name*/, SuccessCallback on_success,
                         ErrorCallback /*on_error*/) override {
        ++service_restarts;
        on_success();
    }

    void get_klippy_status(KlippyStatusCallback on_result, ErrorCallback /*on_error*/) override {
        const size_t i = std::min(status_queries, statuses.size() - 1);
        ++status_queries;
        on_result(statuses[i]);
    }

    /// printer.info answers in order; the last one repeats.
    std::vector<KlippyStatus> statuses{{"startup", ""}};
    std::optional<MoonrakerError> firmware_error;
    int firmware_restarts = 0;
    int service_restarts = 0;
    size_t status_queries = 0;
};

constexpr RecoveryTiming kFastTiming{/*poll_interval_ms=*/100, /*verify_window_ms=*/1000};

const KlippyStatus kKey298{"error",
                           "Internal error during connect: !{\"code\": \"key298\", "
                           "\"msg\": \"Can not update MCU rpi config as it is shutdown\"}"};

class RecoveryFixture : public LVGLTestFixture {
  public:
    RecoveryFixture()
        : client_(MoonrakerClientMock::PrinterType::VORON_24),
          dir_(std::filesystem::temp_directory_path() /
               ("helix_recovery_" + std::to_string(::getpid()))) {
        get_printer_state().init_subjects(false);
        api_ = std::make_unique<ScriptedRecoveryApi>(client_, get_printer_state());
        std::filesystem::create_directories(dir_);
        detail::local_recovery_script_override_ref() = (dir_ / "absent.sh").string();
    }

    ~RecoveryFixture() override {
        detail::local_recovery_script_override_ref().clear();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    /// Install a helix-recover.sh that records each run and exits with @p rc.
    void install_script(int rc) {
        const auto script = dir_ / "helix-recover.sh";
        std::ofstream(script) << "#!/bin/sh\necho run >> '" << runs_file().string() << "'\nexit "
                              << rc << "\n";
        ::chmod(script.c_str(), 0755);
        detail::local_recovery_script_override_ref() = script.string();
    }

    int script_runs() const {
        std::ifstream in(runs_file());
        int n = 0;
        for (std::string line; std::getline(in, line);)
            ++n;
        return n;
    }

    void recover() {
        PrinterRecoveryService(api_.get(), kFastTiming)
            .recover([this](RecoveryOutcome o) { outcome_ = o; },
                     [this](const MoonrakerError& e) { error_ = e; });
    }

    bool finished() const {
        return outcome_.has_value() || error_.has_value();
    }

    /// Run the chain to its end on the virtual clock (and real time for the
    /// script's worker thread).
    void run_to_end() {
        REQUIRE(wait_until([this]() { return finished(); }, 10000, 20));
    }

    MoonrakerClientMock client_;
    std::unique_ptr<ScriptedRecoveryApi> api_;
    std::optional<RecoveryOutcome> outcome_;
    std::optional<MoonrakerError> error_;

  private:
    std::filesystem::path runs_file() const {
        return dir_ / "runs";
    }

    std::filesystem::path dir_;
};

} // namespace

TEST_CASE_METHOD(RecoveryFixture,
                 "Recovery: accepted restart that fails to start runs the local script once",
                 "[recovery]") {
    install_script(0);
    api_->statuses = {{"disconnected", ""}, {"startup", ""}, kKey298};

    recover();
    run_to_end();

    REQUIRE(script_runs() == 1);
    REQUIRE(outcome_ == RecoveryOutcome::RESTART_ISSUED);
    REQUIRE_FALSE(error_.has_value());
    REQUIRE(api_->status_queries == 3);
    REQUIRE(api_->service_restarts == 0);

    // Nothing polls on after the script: the chain is over.
    process_lvgl(2000);
    REQUIRE(api_->status_queries == 3);
    REQUIRE(script_runs() == 1);
}

TEST_CASE_METHOD(RecoveryFixture,
                 "Recovery: a failing local script after a startup error is an error",
                 "[recovery]") {
    install_script(3);
    api_->statuses = {kKey298};

    recover();
    run_to_end();

    REQUIRE(script_runs() == 1);
    REQUIRE_FALSE(outcome_.has_value());
    REQUIRE(error_.has_value());
    REQUIRE(error_->code == 3);
    REQUIRE(api_->service_restarts == 0);
}

TEST_CASE_METHOD(RecoveryFixture, "Recovery: accepted restart that comes back ready is verified",
                 "[recovery]") {
    install_script(0);
    api_->statuses = {{"startup", ""}, {"ready", "Printer is ready"}};

    recover();

    // Accepting the restart is not success on its own.
    process_lvgl(20);
    REQUIRE_FALSE(finished());

    run_to_end();

    REQUIRE(outcome_ == RecoveryOutcome::KLIPPY_READY);
    REQUIRE(script_runs() == 0);
    REQUIRE(api_->service_restarts == 0);
}

TEST_CASE_METHOD(RecoveryFixture,
                 "Recovery: accepted restart never ready within the window runs the script",
                 "[recovery]") {
    install_script(0);
    api_->statuses = {{"startup", ""}};

    recover();
    run_to_end();

    REQUIRE(script_runs() == 1);
    REQUIRE(outcome_ == RecoveryOutcome::RESTART_ISSUED);
    // One poll per interval across the 1000ms window, not a tight loop.
    REQUIRE(api_->status_queries >= 8);
    REQUIRE(api_->status_queries <= 11);
}

TEST_CASE_METHOD(RecoveryFixture,
                 "Recovery: startup error without a local script ends the chain with no service "
                 "restart",
                 "[recovery]") {
    api_->statuses = {kKey298};

    recover();
    run_to_end();

    REQUIRE(outcome_ == RecoveryOutcome::RESTART_ISSUED);
    REQUIRE(api_->service_restarts == 0);
    REQUIRE(script_runs() == 0);
}

TEST_CASE_METHOD(RecoveryFixture, "Recovery: 503 from firmware_restart goes straight to the script",
                 "[recovery]") {
    install_script(0);
    api_->firmware_error = MoonrakerError{MoonrakerErrorType::UNKNOWN,
                                          503,
                                          "Klippy Host not connected",
                                          "printer.firmware_restart",
                                          {}};

    recover();
    run_to_end();

    REQUIRE(script_runs() == 1);
    REQUIRE(outcome_ == RecoveryOutcome::RESTART_ISSUED);
    REQUIRE(api_->status_queries == 0);
    REQUIRE(api_->service_restarts == 0);
}

TEST_CASE_METHOD(RecoveryFixture,
                 "Recovery: 503 with no local script falls back to a service restart",
                 "[recovery]") {
    api_->firmware_error = MoonrakerError{MoonrakerErrorType::UNKNOWN,
                                          503,
                                          "Klippy Host not connected",
                                          "printer.firmware_restart",
                                          {}};

    recover();
    run_to_end();

    REQUIRE(api_->service_restarts == 1);
    REQUIRE(outcome_ == RecoveryOutcome::RESTART_ISSUED);
}
