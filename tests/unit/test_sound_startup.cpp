// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "../test_helpers/sound_manager_test_access.h"
#include "audio_settings_manager.h"
#include "config.h"
#include "runtime_config.h"
#include "sound_backend.h"
#include "sound_manager.h"

#include <atomic>
#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Counts the tones the sequencer drives, from the sequencer thread.
class ToneCountingBackend : public SoundBackend {
  public:
    std::atomic<int> tones{0};

    void set_tone(float, float, float) override {
        tones++;
    }
    void silence() override {}
};

class StartupSoundFixture : public LVGLTestFixture {
  public:
    StartupSoundFixture() {
        Config::get_instance();
        AudioSettingsManager::instance().init_subjects();
        AudioSettingsManager::instance().set_sounds_enabled(true);
        SoundManager::instance().shutdown();
        auto& sm = SoundManager::instance();
        SoundManagerTestAccess::install_backend(sm, backend);
        SoundManagerTestAccess::finalize(sm);
    }
    ~StartupSoundFixture() override {
        SoundManager::instance().shutdown();
        AudioSettingsManager::instance().set_sounds_enabled(false);
        AudioSettingsManager::instance().deinit_subjects();
    }

    bool tones_within(uint32_t timeout_ms) {
        return wait_until([this] { return backend->tones > 0; }, timeout_ms);
    }

    std::shared_ptr<ToneCountingBackend> backend = std::make_shared<ToneCountingBackend>();
};

} // namespace

TEST_CASE("The startup chime plays on a normal run and not under --test", "[sound][startup]") {
    RuntimeConfig config;

    config.test_mode = false;
    CHECK(config.should_play_startup_sound());

    config.test_mode = true;
    CHECK_FALSE(config.should_play_startup_sound());
}

TEST_CASE_METHOD(StartupSoundFixture, "Boot plays the startup chime only outside --test",
                 "[sound][startup]") {
    RuntimeConfig config;

    config.test_mode = true;
    SoundManager::instance().play_startup(config);
    CHECK_FALSE(tones_within(200));

    // The same backend does sound once the run is not a test run, so the silence
    // above came from the decision and not from a dead sequencer.
    config.test_mode = false;
    SoundManager::instance().play_startup(config);
    CHECK(tones_within(2000));
}
