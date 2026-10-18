// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pure queries behind the spool editor's chip rows: family grouping, variant
// labels, brand/product scoping, and the Spoolman recency-derived recents.

#include "filament_catalog.h"
#include "filament_variants.h"
#include "helix_test_fixture.h"
#include "spoolman_types.h"

#include <fstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::printer::FilamentCatalog;

namespace {
constexpr const char* FIX = "tests/fixtures/filaments_edit_queries_test.json";

FilamentCatalog load_edit_fixture() {
    return FilamentCatalog::load_from_file(FIX, /*codes_only=*/false, "");
}

std::vector<std::string> labels_of(const std::vector<FilamentCatalog::FamilyVariant>& vs) {
    std::vector<std::string> out;
    out.reserve(vs.size());
    for (const auto& v : vs) {
        out.push_back(v.label);
    }
    return out;
}

SpoolInfo spool_with(const char* vendor, const char* material) {
    SpoolInfo s;
    s.vendor = vendor;
    s.material = material;
    return s;
}

} // namespace

TEST_CASE("variant_label strips the family", "[catalog][edit-spool]") {
    CHECK(filament::variant_label("PLA", "PLA") == "Basic");
    CHECK(filament::variant_label("PLA-CF", "PLA") == "CF");
    CHECK(filament::variant_label("PLA+", "PLA") == "+");
    CHECK(filament::variant_label("Silk PLA", "PLA") == "Silk");
    CHECK(filament::variant_label("Glow PLA", "PLA") == "Glow");
    CHECK(filament::variant_label("PLA-HS", "PLA") == "HS");
    CHECK(filament::variant_label("TPU-85A", "TPU") == "85A");
    // A family the reducer does not collapse (rPLA is a known material of its
    // own) labels as Basic within itself.
    CHECK(filament::variant_label("rPLA", "rPLA") == "Basic");
    CHECK(filament::variant_label("") == "Basic");
}

TEST_CASE("variant_label derives the family when not given", "[catalog][edit-spool]") {
    CHECK(filament::variant_label("PLA-CF") == "CF");
    CHECK(filament::variant_label("ASA") == "Basic");
}

TEST_CASE("compose_family_type mirrors catalog spellings", "[catalog][edit-spool]") {
    CHECK(filament::compose_family_type("PLA", "Basic") == "PLA");
    CHECK(filament::compose_family_type("PLA", "CF") == "PLA-CF");
    CHECK(filament::compose_family_type("PLA", "Silk") == "Silk PLA");
    CHECK(filament::compose_family_type("PLA", "Recycled") == "rPLA");
    CHECK(filament::compose_family_type("PLA", "+") == "PLA+");
    CHECK(filament::compose_family_type("PLA", "") == "PLA");
}

TEST_CASE("compose inverts variant_label for the catalog spellings", "[catalog][edit-spool]") {
    const char* types[] = {"PLA-CF",   "PLA-GF", "Silk PLA", "Matte PLA",
                           "Wood PLA", "PLA+",   "PLA-HS"};
    for (const char* type : types) {
        const std::string family = filament::display_family(type);
        const std::string label = filament::variant_label(type, family);
        INFO(type << " -> " << family << " / " << label);
        CHECK(filament::compose_family_type(family, label) == type);
    }
}

TEST_CASE_METHOD(HelixTestFixture, "all_families groups decorated types", "[catalog][edit-spool]") {
    auto cat = load_edit_fixture();
    const auto families = cat.all_families();
    // The reducer's own model: the decorated PLA spellings collapse to PLA,
    // while rPLA and PETG+HF are known materials of their own.
    REQUIRE(families.size() == 5);
    CHECK(families[0] == "PETG");
    CHECK(families[1] == "PETG+HF");
    CHECK(families[2] == "PLA");
    CHECK(families[3] == "rPLA");
    CHECK(families[4] == "SnapSpeed");
}

TEST_CASE_METHOD(HelixTestFixture, "variants_in_family dedupes and orders",
                 "[catalog][edit-spool]") {
    auto cat = load_edit_fixture();
    const auto vs = cat.variants_in_family("PLA");
    // Basic first, then alphabetical: +, CF, HS, Silk.
    const auto ls = labels_of(vs);
    REQUIRE(ls.size() == 5);
    CHECK(ls[0] == "Basic");
    CHECK(ls[1] == "+");
    CHECK(ls[2] == "CF");
    CHECK(ls[3] == "HS");
    CHECK(ls[4] == "Silk");
    // Every sample type round-trips: its own family reduces it, and the label
    // re-composes to the sample spelling (rPLA via the Recycled spelling).
    for (const auto& v : vs) {
        INFO(v.sample_type << " / " << v.label);
        CHECK(filament::variant_label(v.sample_type, "PLA") == v.label);
        CHECK(filament::compose_family_type("PLA", v.label) == v.sample_type);
    }
}

TEST_CASE_METHOD(HelixTestFixture, "variants_in_family is case-insensitive",
                 "[catalog][edit-spool]") {
    auto cat = load_edit_fixture();
    CHECK(labels_of(cat.variants_in_family("pla")).size() == 5);
}

TEST_CASE_METHOD(HelixTestFixture, "variants_in_family unknown family yields Basic self",
                 "[catalog][edit-spool]") {
    auto cat = load_edit_fixture();
    const auto vs = cat.variants_in_family("PPS");
    REQUIRE(vs.size() == 1);
    CHECK(vs[0].label == "Basic");
    CHECK(vs[0].sample_type == "PPS");
}

TEST_CASE_METHOD(HelixTestFixture, "brands_in_family scopes to the material",
                 "[catalog][edit-spool]") {
    auto cat = load_edit_fixture();
    const auto pla_brands = cat.brands_in_family("PLA");
    REQUIRE(pla_brands.size() == 3);
    CHECK(pla_brands[0] == "Creality");
    CHECK(pla_brands[1] == "Generic");
    CHECK(pla_brands[2] == "Polymaker");
    // eSUN's only product is PETG+HF, which the reducer keeps as its own
    // family, so it does not reach the PETG row.
    const auto petg_brands = cat.brands_in_family("PETG");
    REQUIRE(petg_brands.size() == 1);
    CHECK(petg_brands[0] == "Polymaker");
    CHECK(cat.brands_in_family("PA").empty());
}

TEST_CASE_METHOD(HelixTestFixture, "products_in_family scopes to brand and material",
                 "[catalog][edit-spool]") {
    auto cat = load_edit_fixture();
    const auto generic_pla = cat.products_in_family("Generic", "PLA");
    // Generic's PLA family products: PLA, PLA-CF, PLA+, Silk PLA (rPLA is its
    // own family).
    REQUIRE(generic_pla.size() == 4);
    for (const auto* p : generic_pla) {
        CHECK(p->brand == "Generic");
        CHECK(filament::display_family(p->type) == "PLA");
    }
    // Case-insensitive on both arguments.
    CHECK(cat.products_in_family("generic", "pla").size() == 4);
    CHECK(cat.products_in_family("eSUN", "PLA").empty());
    CHECK(cat.products_in_family("eSUN", "PETG+HF").size() == 1);
}

TEST_CASE("recent_material_families keeps first-seen order", "[catalog][edit-spool]") {
    std::vector<SpoolInfo> spools{
        spool_with("eSUN", "PLA-CF"), spool_with("Polymaker", "Silk PLA"),
        spool_with("eSUN", "PETG"),   spool_with("eSUN", "PLA"), // dup family, skipped
        spool_with("", "ABS"),        spool_with("NoBrand", ""), // empty sides skipped
    };
    const auto families = recent_material_families(spools);
    REQUIRE(families.size() == 3);
    CHECK(families[0] == "PLA");
    CHECK(families[1] == "PETG");
    CHECK(families[2] == "ABS");
}

TEST_CASE("recent_brands keeps first-seen order", "[catalog][edit-spool]") {
    std::vector<SpoolInfo> spools{
        spool_with("eSUN", "PLA"),
        spool_with("esun", "PLA"),
        spool_with("Polymaker", "PETG"),
    };
    const auto brands = recent_brands(spools);
    REQUIRE(brands.size() == 2);
    CHECK(brands[0] == "eSUN");
    CHECK(brands[1] == "Polymaker");
}

TEST_CASE("editor swatch touch targets meet the 44px floor at every class",
          "[catalog][edit-spool]") {
    // The touch box is disc + 2*hit (color_swatch's ext_click_area), and the
    // grid gap must be >= 2*hit so adjacent targets tile at their pitch
    // without overlapping. Read the ladders straight out of the XML so a
    // regression in the consts fails here, not on a device.
    std::ifstream in("ui_xml/ams_edit_overlay.xml");
    REQUIRE(in.is_open());
    std::string xml((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto read_px = [&](const std::string& name) -> int {
        const std::string needle = "name=\"" + name + "\" value=\"";
        auto at = xml.find(needle);
        REQUIRE(at != std::string::npos);
        return std::atoi(xml.c_str() + at + needle.size());
    };
    // The disc ladders are literals in the editor's consts; read them so a
    // change to either side of the 44px math fails here.
    const int disc_micro = read_px("ams_edit_grid_swatch_micro");
    // medium+ discs resolve through #swatch_grid_size; read the shared
    // literal from globals.xml.
    std::ifstream globals_in("ui_xml/globals.xml");
    REQUIRE(globals_in.is_open());
    std::string globals_xml((std::istreambuf_iterator<char>(globals_in)),
                            std::istreambuf_iterator<char>());
    auto read_global_px = [&](const std::string& name) -> int {
        const std::string needle = "name=\"" + name + "\" value=\"";
        auto at = globals_xml.find(needle);
        REQUIRE(at != std::string::npos);
        return std::atoi(globals_xml.c_str() + at + needle.size());
    };
    const int disc_medium = read_global_px("swatch_grid_size_medium");
    for (const char* cls : {"micro", "tiny", "small"}) {
        const std::string p = std::string("ams_edit_grid_");
        const int disc = read_px(p + "swatch_" + cls);
        const int hit = read_px(p + "hit_" + cls);
        const int gap = read_px(p + "gap_" + cls);
        CHECK(disc + 2 * hit >= 44);
        CHECK(gap >= 2 * hit);
    }
    for (const char* cls : {"medium", "large", "xlarge", "xxlarge"}) {
        const std::string p = std::string("ams_edit_grid_");
        const int hit = read_px(p + "hit_" + cls);
        const int gap = read_px(p + "gap_" + cls);
        CHECK(disc_medium + 2 * hit >= 44);
        CHECK(gap >= 2 * hit);
    }
    (void)disc_micro;
}
