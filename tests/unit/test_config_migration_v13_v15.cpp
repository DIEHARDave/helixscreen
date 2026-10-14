// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The v12 -> v13 and v14 -> v15 migrations repair the same stale AD5X display pair
// (sleep_backlight_off=false with hardware_blank=0). Both are driven through the public
// Config::init() path, starting one version below the migration under test.

#include "config.h"
#include "test_helpers/unique_temp_dir.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;
using namespace helix;

namespace {

class SleepMigrationFixture {
  protected:
    Config config;
    std::string temp_dir;
    std::string config_path;
    std::string saved_config_dir_;
    bool had_config_dir_ = false;

  public:
    SleepMigrationFixture() {
        temp_dir = helix::test::unique_temp_dir("helix_migration_sleep_test");
        fs::remove_all(temp_dir);
        fs::create_directories(temp_dir);
        if (const char* prev = std::getenv("HELIX_CONFIG_DIR")) {
            saved_config_dir_ = prev;
            had_config_dir_ = true;
        }
        setenv("HELIX_CONFIG_DIR", temp_dir.c_str(), 1);
        config_path = temp_dir + "/settings.json";
    }
    ~SleepMigrationFixture() {
        fs::remove_all(temp_dir);
        if (had_config_dir_) {
            setenv("HELIX_CONFIG_DIR", saved_config_dir_.c_str(), 1);
        } else {
            unsetenv("HELIX_CONFIG_DIR");
        }
        config.clear_path();
    }

    void write_and_init(const json& contents) {
        std::ofstream f(config_path);
        f << contents.dump(2);
        f.close();
        config.init(config_path);
    }
};

json stale_display() {
    return {{"sleep_backlight_off", false}, {"hardware_blank", 0}};
}

} // namespace

TEST_CASE_METHOD(SleepMigrationFixture, "both AD5X sleep migrations repair the same stale pair",
                 "[config][migration]") {
    // The version below each migration, so only that migration repairs the config.
    const int from_version = GENERATE(12, 14);
    CAPTURE(from_version);

    SECTION("preset ad5x") {
        write_and_init(
            {{"config_version", from_version}, {"preset", "ad5x"}, {"display", stale_display()}});
        CHECK(config.get<bool>("/display/sleep_backlight_off", false) == true);
        CHECK(config.get<int>("/display/hardware_blank", -1) == 1);
    }

    SECTION("saved printer of type FlashForge Adventurer 5X") {
        write_and_init({{"config_version", from_version},
                        {"printers", {{"p1", {{"type", "FlashForge Adventurer 5X"}}}}},
                        {"display", stale_display()}});
        CHECK(config.get<bool>("/display/sleep_backlight_off", false) == true);
        CHECK(config.get<int>("/display/hardware_blank", -1) == 1);
    }

    SECTION("preset ad5m is untouched") {
        write_and_init(
            {{"config_version", from_version}, {"preset", "ad5m"}, {"display", stale_display()}});
        CHECK(config.get<bool>("/display/sleep_backlight_off", true) == false);
        CHECK(config.get<int>("/display/hardware_blank", -1) == 0);
    }

    SECTION("an AD5X config already repaired is untouched") {
        write_and_init({{"config_version", from_version},
                        {"preset", "ad5x"},
                        {"display", {{"sleep_backlight_off", true}, {"hardware_blank", 2}}}});
        CHECK(config.get<bool>("/display/sleep_backlight_off", false) == true);
        CHECK(config.get<int>("/display/hardware_blank", -1) == 2);
    }
}
