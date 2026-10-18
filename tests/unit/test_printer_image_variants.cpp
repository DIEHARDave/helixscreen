// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_image_variants.cpp
 * @brief The image_variants matcher: which database image a detected printer shows
 */

#include "printer_image_variants.h"

#include "../catch_amalgamated.hpp"

using helix::AmsType;
using helix::printer_images::HardwareFacts;
using helix::printer_images::is_known_variant_condition;
using helix::printer_images::is_valid_variant_condition_value;
using helix::printer_images::select_image;
using helix::printer_images::variant_matches;
using nlohmann::json;

namespace {

const HardwareFacts kNoHardware{};
const HardwareFacts kCfs{{AmsType::CFS}};

json entry_with(json variants) {
    return json{{"image", "plain.png"}, {"image_variants", std::move(variants)}};
}

} // namespace

TEST_CASE("select_image: an entry without variants shows its image", "[printer_image_variants]") {
    const json entry{{"image", "plain.png"}};
    CHECK(select_image(entry, kNoHardware) == "plain.png");
    CHECK(select_image(entry, kCfs) == "plain.png");
    CHECK(select_image(json::object(), kCfs).empty());
}

TEST_CASE("select_image: a matching variant replaces the image", "[printer_image_variants]") {
    const json entry =
        entry_with({{{"when", {{"filament_system", "cfs"}}}, {"image", "with-cfs.png"}}});
    CHECK(select_image(entry, kCfs) == "with-cfs.png");
}

TEST_CASE("select_image: no matching variant falls back to the image", "[printer_image_variants]") {
    const json entry =
        entry_with({{{"when", {{"filament_system", "cfs"}}}, {"image", "with-cfs.png"}}});
    CHECK(select_image(entry, kNoHardware) == "plain.png");
    CHECK(select_image(entry, HardwareFacts{{AmsType::AFC}}) == "plain.png");
}

TEST_CASE("select_image: the first matching variant wins", "[printer_image_variants]") {
    const json entry = entry_with({
        {{"when", {{"filament_system", "afc"}}}, {"image", "with-afc.png"}},
        {{"when", {{"filament_system", "cfs"}}}, {"image", "first-cfs.png"}},
        {{"when", {{"filament_system", "cfs"}}}, {"image", "second-cfs.png"}},
    });
    CHECK(select_image(entry, kCfs) == "first-cfs.png");
    // Any detected backend of the kind counts, not only the first.
    CHECK(select_image(entry, HardwareFacts{{AmsType::HAPPY_HARE, AmsType::CFS}}) ==
          "first-cfs.png");
}

TEST_CASE("select_image: a variant without an image is skipped", "[printer_image_variants]") {
    const json entry = entry_with({
        {{"when", {{"filament_system", "cfs"}}}},
        {{"when", {{"filament_system", "cfs"}}}, {"image", "with-cfs.png"}},
    });
    CHECK(select_image(entry, kCfs) == "with-cfs.png");
}

TEST_CASE("variant_matches: an unknown condition key never matches", "[printer_image_variants]") {
    // An entry naming a condition this build cannot evaluate shows its plain image.
    CHECK_FALSE(variant_matches({{"enclosure", true}}, kCfs));
    CHECK_FALSE(variant_matches({{"filament_system", "cfs"}, {"unit_count", 2}}, kCfs));
    const json entry = entry_with({{{"when", {{"unit_count", 2}}}, {"image", "two-units.png"}}});
    CHECK(select_image(entry, kCfs) == "plain.png");
}

TEST_CASE("variant_matches: malformed conditions never match", "[printer_image_variants]") {
    CHECK_FALSE(variant_matches(json("cfs"), kCfs));
    CHECK_FALSE(variant_matches(json::array(), kCfs));
    CHECK_FALSE(variant_matches({{"filament_system", 6}}, kCfs));
    CHECK_FALSE(variant_matches({{"filament_system", "not-a-system"}}, kNoHardware));
    // "none" names no filament system, so it cannot be used to match a bare printer.
    CHECK_FALSE(variant_matches({{"filament_system", "none"}}, kNoHardware));
}

TEST_CASE("variant_matches: an empty condition set matches", "[printer_image_variants]") {
    CHECK(variant_matches(json::object(), kNoHardware));
}

TEST_CASE("variant condition vocabulary", "[printer_image_variants]") {
    CHECK(is_known_variant_condition("filament_system"));
    CHECK_FALSE(is_known_variant_condition("enclosure"));
    CHECK(is_valid_variant_condition_value("filament_system", "cfs"));
    CHECK(is_valid_variant_condition_value("filament_system", "happy_hare"));
    CHECK_FALSE(is_valid_variant_condition_value("filament_system", "none"));
    CHECK_FALSE(is_valid_variant_condition_value("filament_system", 6));
    CHECK_FALSE(is_valid_variant_condition_value("enclosure", true));
}
