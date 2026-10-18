// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_edit_overlay.h"

#include "ui_button.h"
#include "ui_callback_helpers.h"
#include "ui_color_picker.h"
#include "ui_component_keypad.h"
#include "ui_error_reporting.h"
#include "ui_filament_catalog_picker.h"
#include "ui_hsv_picker.h"
#include "ui_nav.h"
#include "ui_swatch.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "ams_remap.h"
#include "ams_state.h"
#include "ams_tag_reading.h"
#include "app_globals.h"
#include "color_utils.h"
#include "display_numbering.h"
#include "filament_database.h"
#include "filament_display_name.h"
#include "filament_variants.h"
#include "format_utils.h"
#include "lane_translation.h"
#include "spoolman_manager.h"
#include "static_panel_registry.h"
#include "text_io.h"
#include "ui/ui_widget_helpers.h"
#if HELIX_HAS_LABEL_PRINTER
#include "ipp_print_modal.h"
#include "label_printer_settings.h"
#include "label_printer_utils.h"
#endif
#include "ui_breakpoint.h"
#include "ui_overlay_qr_scanner.h"
#include "ui_toast_manager.h"

#include "i_moonraker_api.h"
#include "printer_state.h"
#include "spoolman_slot_saver.h"
#include "spoolman_types.h"
#include "theme_manager.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

namespace helix::ui {

// Fire-and-forget: notify Moonraker of the active spool so other clients
// (Mainsail, Fluidd) see the change and filament tracking works.
// Pass 0 to clear the active spool (unlink).
static void sync_active_spool(IMoonrakerAPI* api, int spool_id) {
    spdlog::info("[AmsEditOverlay] Syncing active spool to {} on server", spool_id);
    api->spoolman().set_active_spool(
        spool_id,
        [spool_id]() {
            spdlog::debug("[AmsEditOverlay] Active spool synced to {} on server", spool_id);
        },
        [spool_id](const MoonrakerError& err) {
            spdlog::warn("[AmsEditOverlay] Failed to sync active spool {}: {}", spool_id,
                         err.message);
        });
}

namespace {

bool iequals_str(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    return std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
        return std::tolower(x) == std::tolower(y);
    });
}

/// Move every element of @p recents that appears in @p base (case-insensitive)
/// to the front, keeping both lists' own orders otherwise.
std::vector<std::string> hoist_recents(const std::vector<std::string>& base,
                                       const std::vector<std::string>& recents) {
    std::vector<std::string> out;
    out.reserve(base.size());
    std::set<std::string> taken;
    for (const auto& recent : recents) {
        for (const auto& item : base) {
            if (iequals_str(item, recent) && taken.insert(helix::text_io::to_lower(item)).second) {
                out.push_back(item);
            }
        }
    }
    for (const auto& item : base) {
        if (taken.insert(helix::text_io::to_lower(item)).second) {
            out.push_back(item);
        }
    }
    return out;
}

} // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

AmsEditOverlay::AmsEditOverlay()
    : picker_search_debounce_(
          [this](const std::string& query) { handle_picker_search(query.c_str()); }) {}

AmsEditOverlay::~AmsEditOverlay() {
    // Deinitialize subjects first to disconnect observers [L041]
    deinit_subjects();
}

lv_obj_t* AmsEditOverlay::find_widget(const char* name) const {
    return overlay_root_ ? lv_obj_find_by_name(overlay_root_, name) : nullptr;
}

// ============================================================================
// Pure selection decisions
// ============================================================================

AmsEditOverlay::SpoolEditSelection AmsEditOverlay::selection_from_slot(const SlotInfo& info) {
    SpoolEditSelection sel;
    sel.family = filament::display_family(info.material);
    if (sel.family.empty()) {
        sel.family = "PLA"; // a slot with no material at all opens on the common case
    }
    sel.variant = filament::variant_label(info.material, sel.family);
    sel.brand = info.brand.empty() ? "Generic" : info.brand;
    sel.product_id = info.catalog_id;
    return sel;
}

AmsEditOverlay::SpoolEditSelection
AmsEditOverlay::apply_family_change(const SpoolEditSelection& sel, const std::string& new_family,
                                    const printer::FilamentCatalog& catalog) {
    SpoolEditSelection out;
    out.family = new_family;
    out.variant = "Basic";
    // Keep the brand when it makes the new material, else Generic.
    const auto makers = catalog.brands_in_family(new_family);
    bool maker = iequals_str(sel.brand, "Generic");
    for (const auto& brand : makers) {
        if (iequals_str(brand, sel.brand)) {
            maker = true;
            break;
        }
    }
    out.brand = maker ? sel.brand : "Generic";
    // Any previously chosen product described the old material.
    out.product_id = "";
    return out;
}

std::string AmsEditOverlay::selected_type(const SpoolEditSelection& sel,
                                          const printer::FilamentCatalog& catalog) {
    for (const auto& variant : catalog.variants_in_family(sel.family)) {
        if (iequals_str(variant.label, sel.variant)) {
            return variant.sample_type;
        }
    }
    return filament::compose_family_type(sel.family, sel.variant);
}

// ============================================================================
// Public API
// ============================================================================

bool AmsEditOverlay::show_for_slot(lv_obj_t* parent, int slot_index, const SlotInfo& initial_info,
                                   IMoonrakerAPI* api, CompletionCallback on_complete,
                                   bool open_on_picker) {
    slot_index_ = slot_index;
    original_info_ = initial_info;
    working_info_ = initial_info;
    // Picker-entry mode: a selection commits + closes. Cleared when the picker
    // is later re-entered via the header Spoolman button.
    opened_on_picker_ = open_on_picker;
    api_ = api ? api : get_moonraker_api();
    completion_callback_ = std::move(on_complete);
    completion_fired_ = false;
    selection_moved_ = false;
    details_color_set_ = false;
    lv_subject_set_int(&more_open_subject_, 0);
    cached_spools_.clear();
    cached_searchables_.clear();
    picker_search_debounce_.cancel();

    // Always prefer the active screen so the overlay renders above everything
    lv_obj_t* screen = lv_screen_active();
    if (!show(screen ? screen : parent)) {
        spdlog::error("[AmsEditOverlay] Failed to push overlay for slot {}", slot_index);
        return false;
    }

    // Safety net + memory reclaim on close. A dismissal that bypasses our
    // handlers (backdrop tap, external go_back) must still complete as "not
    // saved" — fire_completion is idempotent, so Save/back paths that already
    // fired are unaffected. We ALSO tear the widget tree down here: this
    // overlay is large (chip rows + 30 preset swatches + picker + color view)
    // and is opened only to edit a spool. It has to be ONE combined callback:
    // NavigationManager keeps a single close callback per widget.
    helix::nav::on_close(overlay_root_, []() {
        auto& overlay = get_ams_edit_overlay();
        overlay.fire_completion(false);
        overlay.destroy_overlay_ui();
    });

    // Reset per-session view state HERE (covered-safe — on_deactivate must not
    // touch it, since it also fires when the QR scanner merely covers us).
    if (open_on_picker) {
        set_view(VIEW_SPOOL_PICKER);
        populate_picker();
    } else {
        enter_editor();
    }

    // If linked to Spoolman, fetch authoritative filament data (vendor,
    // material, color) so the editor shows current Spoolman state, not stale
    // backend data. Gate on Spoolman availability: firing the fetch on a
    // Spoolman-less printer is one doomed RPC + warn log per overlay open.
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    bool has_spoolman = spoolman_subj && lv_subject_get_int(spoolman_subj) == 1;
    if (has_spoolman && working_info_.spoolman_id > 0 && api_) {
        const int spool_id = working_info_.spoolman_id;
        auto token = lifetime_.token();
        api_->spoolman().get_spoolman_spool(
            spool_id,
            [this, token, spool_id](const std::optional<SpoolInfo>& spool) {
                // token.defer() gates liveness; no bare expired() check (L081).
                if (!spool)
                    return;
                int fetched_filament_id = spool->filament_id;
                int fetched_vendor_id = spool->vendor_id;
                std::string fetched_vendor = spool->vendor;
                std::string fetched_material = spool->material;
                std::string fetched_color_hex = spool->color_hex;
                token.defer([this, spool_id, fetched_filament_id, fetched_vendor_id,
                             fetched_vendor = std::move(fetched_vendor),
                             fetched_material = std::move(fetched_material),
                             fetched_color_hex = std::move(fetched_color_hex)]() {
                    if (fetched_filament_id > 0) {
                        original_info_.spoolman_filament_id = fetched_filament_id;
                        working_info_.spoolman_filament_id = fetched_filament_id;
                    }
                    if (fetched_vendor_id > 0) {
                        original_info_.spoolman_vendor_id = fetched_vendor_id;
                        working_info_.spoolman_vendor_id = fetched_vendor_id;
                    }
                    if (!fetched_vendor.empty() && working_info_.brand != fetched_vendor) {
                        original_info_.brand = fetched_vendor;
                        working_info_.brand = fetched_vendor;
                    }
                    if (!fetched_material.empty() && working_info_.material != fetched_material) {
                        original_info_.material = fetched_material;
                        working_info_.material = fetched_material;
                    }
                    if (!fetched_color_hex.empty()) {
                        uint32_t rgb = 0;
                        if (helix::parse_hex_color(fetched_color_hex.c_str(), rgb) &&
                            working_info_.color_rgb != rgb) {
                            original_info_.color_rgb = rgb;
                            working_info_.color_rgb = rgb;
                        }
                    }
                    spdlog::debug("[AmsEditOverlay] Synced spool {} from Spoolman: vendor='{}', "
                                  "material='{}'",
                                  spool_id, working_info_.brand, working_info_.material);
                    sel_ = selection_from_slot(working_info_);
                    selection_moved_ = false;
                    rebuild_chip_rows();
                    update_summary();
                });
            },
            [spool_id](const MoonrakerError& err) {
                spdlog::warn("[AmsEditOverlay] Failed to fetch spool {}: {}", spool_id,
                             err.message);
            });
    }

    spdlog::info("[AmsEditOverlay] Shown for slot {} (spoolman_id={}, brand={}, material={})",
                 slot_index, initial_info.spoolman_id, initial_info.brand, initial_info.material);
    return true;
}

// ============================================================================
// OverlayBase Hooks
// ============================================================================

lv_obj_t* AmsEditOverlay::create(lv_obj_t* parent) {
    if (!OverlayBase::create(parent)) {
        return nullptr;
    }

    // The header title (the lane name, "Gate 3") is subject-written; bind it
    // here once per tree rather than through overlay_panel's title_subject
    // forward, which the shared header_bar applies at its own build time.
    lv_obj_t* header_title = find_widget("header_title");
    if (header_title) {
        lv_label_bind_text(header_title, &title_subject_, nullptr);
    }

    // header_bar's optional trailing badge: the Spoolman mark + "#19". Wired
    // from C++ like the title, so the shared header component carries no
    // bind_text of its own.
    lv_obj_t* badge_text = find_widget("header_title_badge_text");
    if (badge_text) {
        lv_label_bind_text(badge_text, &spoolman_id_subject_, nullptr);
    }

    // Bind the summary colour dot's painter to the pending colour: the dot is
    // a color_dot, painted imperatively from update_summary().
    lv_obj_t* hsv = helix::ui::find_required(overlay_root_, "ams_color_hsv", get_name());
    if (hsv) {
        ui_hsv_picker_set_callback(
            hsv,
            [](uint32_t rgb, void* /*user_data*/) {
                get_ams_edit_overlay().handle_custom_color_changed(rgb);
            },
            nullptr);
    }

    spdlog::info("[AmsEditOverlay] Overlay created");
    return overlay_root_;
}

void AmsEditOverlay::on_ui_destroyed() {}

void AmsEditOverlay::on_activate() {
    OverlayBase::on_activate();
    update_ui();
}

void AmsEditOverlay::on_deactivating(DeactivateReason) {
    // Fires when POPPED **and** when COVERED (e.g. QR scanner pushed on top).
    // Must NOT fire completion or reset the view subject — session state
    // resets happen in show_for_slot(). A pending debounced search belongs to
    // the session being left.
    picker_search_debounce_.cancel();
    spdlog::debug("[AmsEditOverlay] on_deactivating()");
}

// ============================================================================
// Subject Management
// ============================================================================

void AmsEditOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        lv_subject_init_string(&title_subject_, title_buf_, nullptr, sizeof(title_buf_), "--");
        subjects_.register_subject(&title_subject_);

        // View state (VIEW_EDITOR..VIEW_COLOR) - registered globally
        UI_MANAGED_SUBJECT_INT(view_mode_subject_, 0, "ams_edit_view", subjects_);

        // Picker state (0=loading, 1=empty, 2=content) - registered globally
        UI_MANAGED_SUBJECT_INT(picker_state_subject_, 0, "edit_picker_state", subjects_);

        // Header Save button dirty gate (1=disabled). Starts disabled — nothing
        // is dirty when the editor opens.
        UI_MANAGED_SUBJECT_INT(save_disabled_subject_, 1, "ams_edit_save_disabled", subjects_);

        // Header Save button visibility gate (1=hidden). Save applies to the
        // editor view only; picker and color views hide it. Written
        // exclusively from set_view().
        UI_MANAGED_SUBJECT_INT(save_hidden_subject_, 0, "ams_edit_save_hidden", subjects_);

        // Managed-vs-untracked signal: drives the header Spoolman badge, the
        // More section's Spoolman-owned fields and the Add-link's absence.
        UI_MANAGED_SUBJECT_INT(is_managed_subject_, 0, "ams_edit_is_managed", subjects_);

        // Scan QR reachability: Spoolman availability AND a camera on this
        // build. The header button binds its hidden flag to this.
        UI_MANAGED_SUBJECT_INT(scan_enabled_subject_, 0, "ams_edit_scan_enabled", subjects_);

        // Row visibility for the Generic-gated product dropdown.
        UI_MANAGED_SUBJECT_INT(product_hidden_subject_, 1, "ams_edit_product_hidden", subjects_);
        UI_MANAGED_SUBJECT_INT(color_selected_subject_, -1, "ams_edit_color_selected", subjects_);
        UI_MANAGED_SUBJECT_INT(color_col_count_subject_, 15, "ams_edit_color_col_count", subjects_);
        UI_MANAGED_SUBJECT_INT(color_row_count_subject_, 30, "ams_edit_color_row_count", subjects_);

        // Summary line, temperatures, weight pill, badges, disclosure.
        UI_MANAGED_SUBJECT_STRING(summary_text_subject_, summary_buf_, "", "ams_edit_summary_text",
                                  subjects_);
        UI_MANAGED_SUBJECT_INT(tag_badge_visible_subject_, 0, "ams_edit_tag_badge_visible",
                               subjects_);
        UI_MANAGED_SUBJECT_STRING(temp_text_subject_, temp_buf_, "", "ams_edit_temp_text",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(weight_text_subject_, weight_buf_, "", "ams_edit_weight_text",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(weight_hint_subject_, weight_hint_buf_, "",
                                  "ams_edit_weight_hint", subjects_);
        UI_MANAGED_SUBJECT_INT(reset_tag_visible_subject_, 0, "ams_edit_reset_tag_visible",
                               subjects_);
        UI_MANAGED_SUBJECT_INT(add_spoolman_visible_subject_, 0, "ams_edit_add_spoolman_visible",
                               subjects_);
        UI_MANAGED_SUBJECT_INT(more_open_subject_, 0, "ams_edit_more_open", subjects_);

        // Spoolman spool number for the header badge ("#19").
        UI_MANAGED_SUBJECT_STRING(spoolman_id_subject_, spoolman_id_buf_, "",
                                  "ams_edit_spoolman_id", subjects_);

#if HELIX_HAS_LABEL_PRINTER
        // Expose the label-printer readiness flag to XML so
        // btn_detail_print_label can bind its `hidden` flag declaratively and
        // track pairing/unpairing while the overlay is open.
        helix::LabelPrinterSettingsManager::instance().init_subjects();
        lv_xml_register_subject(
            nullptr, "label_printer_configured",
            helix::LabelPrinterSettingsManager::instance().subject_printer_configured());
#endif
    });
}

void AmsEditOverlay::deinit_subjects() {
    deinit_subjects_base(subjects_);
}

// ============================================================================
// Chip rows
// ============================================================================

void AmsEditOverlay::fill_dropdown(const char* stacked_name, const char* split_name,
                                   const std::vector<Chip>& chips,
                                   const std::string& selected_payload) {
    std::string options;
    int selected = -1;
    for (size_t i = 0; i < chips.size(); ++i) {
        if (!options.empty()) {
            options += '\n';
        }
        options += chips[i].label;
        if (!chips[i].more && selected < 0 && iequals_str(chips[i].payload, selected_payload)) {
            selected = static_cast<int>(i);
        }
    }
    for (const char* name : {stacked_name, split_name}) {
        if (lv_obj_t* dd = name && name[0] ? find_widget(name) : nullptr) {
            lv_dropdown_set_options(dd, options.c_str());
            lv_dropdown_set_selected(dd, selected < 0 ? 0 : selected);
        }
    }
}

void AmsEditOverlay::refresh_dropdowns() {
    fill_dropdown("dropdown_material", "dropdown_material_split", material_chips_, sel_.family);
    fill_dropdown("dropdown_variant", "dropdown_variant_split", variant_chips_, sel_.variant);
    fill_dropdown("dropdown_brand", "dropdown_brand_split", brand_chips_, sel_.brand);
    fill_dropdown("dropdown_product", "dropdown_product_split", product_chips_, sel_.product_id);
}

void AmsEditOverlay::rebuild_chip_rows() {
    if (!catalog_) {
        return;
    }
    // MATERIAL: recents hoisted over all families, then "More...".
    std::vector<Chip> material;
    std::vector<std::string> families = hoist_recents(catalog_->all_families(), recent_families_);
    if (recent_families_.empty()) {
        // No recents yet: the common families lead so the row opens useful.
        const char* const common[] = {"PLA", "PETG", "ABS", "ASA", "PA", "TPU", "PC"};
        std::vector<std::string> lead;
        for (const char* fam : common) {
            for (const auto& f : families) {
                if (iequals_str(f, fam)) {
                    lead.push_back(f);
                }
            }
        }
        std::set<std::string> taken;
        for (const auto& f : lead) {
            taken.insert(helix::text_io::to_lower(f));
        }
        for (const auto& f : families) {
            if (taken.insert(helix::text_io::to_lower(f)).second) {
                lead.push_back(f);
            }
        }
        families = std::move(lead);
    }
    bool family_known = false;
    for (const auto& fam : families) {
        material.push_back({fam, fam, false});
        if (iequals_str(fam, sel_.family)) {
            family_known = true;
        }
    }
    if (!family_known) {
        material.insert(material.begin(), {sel_.family, sel_.family, false});
    }
    material.push_back({lv_tr("Browse catalog..."), {}, true});
    material_chips_ = std::move(material);

    // VARIANT: the catalog's variants for the family.
    std::vector<Chip> variants;
    for (const auto& v : catalog_->variants_in_family(sel_.family)) {
        variants.push_back({v.label, v.label, false});
    }
    variant_chips_ = std::move(variants);

    // BRAND: Generic, then recents, then the rest, then "All...".
    std::vector<Chip> brands;
    brands.push_back({"Generic", "Generic", false});
    std::vector<std::string> makers = catalog_->brands_in_family(sel_.family);
    std::vector<std::string> ordered = hoist_recents(makers, recent_brands_);
    bool brand_known = iequals_str(sel_.brand, "Generic");
    for (const auto& brand : ordered) {
        brands.push_back({brand, brand, false});
        if (iequals_str(brand, sel_.brand)) {
            brand_known = true;
        }
    }
    if (!brand_known) {
        brands.insert(brands.begin() + 1, {sel_.brand, sel_.brand, false});
    }
    brands.push_back({lv_tr("Browse catalog..."), {}, true});
    brand_chips_ = std::move(brands);

    // PRODUCT: that brand's lines for the material. Hidden under Generic.
    std::vector<Chip> products;
    if (!iequals_str(sel_.brand, "Generic")) {
        for (const auto* ef : catalog_->products_in_family(sel_.brand, sel_.family)) {
            products.push_back({ef->name, ef->id, false});
        }
        // An unresolved saved product still shows (selected) so an untouched
        // open+save keeps it.
        bool product_known = sel_.product_id.empty();
        for (const auto& chip : products) {
            if (chip.payload == sel_.product_id) {
                product_known = true;
                break;
            }
        }
        if (!product_known) {
            const std::string label =
                working_info_.product_name.empty() ? sel_.product_id : working_info_.product_name;
            products.insert(products.begin(), {label, sel_.product_id, false});
        }
    }
    product_chips_ = std::move(products);
    lv_subject_set_int(&product_hidden_subject_, iequals_str(sel_.brand, "Generic") ? 1 : 0);

    refresh_dropdowns();
}

void AmsEditOverlay::handle_option_selected(int row, int index) {
    const std::vector<Chip>* chips = nullptr;
    switch (row) {
    case 0:
        chips = &material_chips_;
        break;
    case 1:
        chips = &variant_chips_;
        break;
    case 2:
        chips = &brand_chips_;
        break;
    case 3:
        chips = &product_chips_;
        break;
    default:
        return;
    }
    if (index < 0 || static_cast<size_t>(index) >= chips->size()) {
        return;
    }
    const Chip& chip = (*chips)[index];
    if (chip.more) {
        // "Browse catalog...": the full catalog picker, with search. A pick
        // there seeds family, variant, brand and product in one step.
        if (!catalog_sheet_) {
            catalog_sheet_ = std::make_unique<FilamentCatalogPickerModal>();
        }
        catalog_sheet_->show(
            lv_screen_active(), sel_.family, [](const helix::printer::EffectiveFilament& ef) {
                auto& editor = get_ams_edit_overlay();
                editor.sel_.family = filament::display_family(ef.type);
                editor.sel_.variant = filament::variant_label(ef.type, editor.sel_.family);
                editor.sel_.brand = ef.brand;
                editor.sel_.product_id = ef.id;
                editor.selection_moved_ = true;
                editor.rebuild_chip_rows();
                editor.update_summary();
                editor.update_sync_button_state();
            });
        return;
    }
    switch (row) {
    case 0:
        if (iequals_str(chip.payload, sel_.family)) {
            return;
        }
        sel_ = apply_family_change(sel_, chip.payload, *catalog_);
        break;
    case 1:
        if (iequals_str(chip.payload, sel_.variant)) {
            return;
        }
        sel_.variant = chip.payload;
        sel_.product_id.clear(); // the product described the old variant
        break;
    case 2:
        if (iequals_str(chip.payload, sel_.brand)) {
            return;
        }
        sel_.brand = chip.payload;
        sel_.product_id.clear(); // lines are per-brand
        break;
    case 3:
        if (chip.payload == sel_.product_id) {
            return;
        }
        sel_.product_id = chip.payload;
        break;
    default:
        return;
    }
    selection_moved_ = true;
    rebuild_chip_rows();
    update_summary();
    update_sync_button_state();
}

// ============================================================================
// View Switching
// ============================================================================

void AmsEditOverlay::set_view(int view) {
    // Leaving the picker with a search still settling: the debounced rebuild
    // would re-render the now-hidden spool list mid-transition.
    if (lv_subject_get_int(&view_mode_subject_) == VIEW_SPOOL_PICKER && view != VIEW_SPOOL_PICKER) {
        picker_search_debounce_.cancel();
    }
    lv_subject_set_int(&view_mode_subject_, view);
    // Header Save is visible on the editor view only; the picker and the color
    // view hide it.
    lv_subject_set_int(&save_hidden_subject_, view == VIEW_EDITOR ? 0 : 1);
    update_sync_button_state();
}

void AmsEditOverlay::switch_to_picker() {
    if (!subjects_initialized_) {
        spdlog::warn("[AmsEditOverlay] switch_to_picker() aborted: subjects not initialized");
        return;
    }
    // Reaching the picker via the header button is the two-step flow — a
    // selection returns to the editor for review.
    opened_on_picker_ = false;
    set_view(VIEW_SPOOL_PICKER);
    populate_picker();
}

void AmsEditOverlay::populate_picker() {
    if (!api_) {
        api_ = get_moonraker_api();
    }
    if (!overlay_root_ || !api_) {
        spdlog::warn("[AmsEditOverlay] populate_picker() aborted: overlay_root_={}, api_={}",
                     static_cast<void*>(overlay_root_), static_cast<void*>(api_));
        lv_subject_set_int(&picker_state_subject_, 1);
        return;
    }

    picker_search_debounce_.cancel();
    cached_spools_.clear();
    cached_searchables_.clear();
    lv_subject_set_int(&picker_state_subject_, 0);

    lv_obj_t* search = helix::ui::find_required(overlay_root_, "picker_search", get_name());
    if (search) {
        lv_textarea_set_text(search, "");
    }

    auto token = lifetime_.token();
    api_->spoolman().get_spoolman_spools(
        [this, token](const std::vector<SpoolInfo>& spools) {
            spdlog::debug("[AmsEditOverlay] Spoolman returned {} spools", spools.size());
            token.defer([this, spools]() {
                if (!overlay_root_ || !subjects_initialized_) {
                    return;
                }
                if (spools.empty()) {
                    lv_subject_set_int(&picker_state_subject_, 1);
                    return;
                }
                // Spools arrive ordered by most recent activity first
                // (sort_spools_by_recency() in the API layer).
                cached_spools_ = spools;
                cached_searchables_.clear();
                cached_searchables_.reserve(cached_spools_.size());
                for (const auto& spool : cached_spools_) {
                    cached_searchables_.push_back(build_searchable_text(spool));
                }
                render_spool_list("");
            });
        },
        [this, token](const MoonrakerError& err) {
            spdlog::warn("[AmsEditOverlay] Spoolman fetch error: {}", err.message);
            token.defer([this, msg = err.message]() {
                if (!overlay_root_ || !subjects_initialized_) {
                    return;
                }
                spdlog::warn("[AmsEditOverlay] Failed to fetch spools: {}", msg);
                lv_subject_set_int(&picker_state_subject_, 1);
            });
        });
}

void AmsEditOverlay::render_spool_list(const std::string& filter) {
    lv_obj_t* spool_list = helix::ui::find_required(overlay_root_, "picker_spool_list", get_name());
    if (!spool_list) {
        return;
    }

    // Invoked from a token.defer() callback (UpdateQueue batch). Sync
    // lv_obj_clean in that context corrupts LVGL's event linked list (#776).
    helix::ui::safe_clean_children(spool_list);

    auto filtered = filter_spools(cached_spools_, filter, cached_searchables_);
    auto in_use = ToolState::instance().assigned_spool_ids(slot_index_);

    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    UiBreakpoint bp = bp_subj ? as_breakpoint(lv_subject_get_int(bp_subj)) : UiBreakpoint::Medium;
    const bool is_compact = bp <= UiBreakpoint::Medium;
    const char* flow = is_compact ? "row" : "column";
    const char* name_grow = is_compact ? "1" : "0";
    const char* attrs_plain[] = {"detail_flow", flow, "name_grow", name_grow, nullptr, nullptr};
    const char* attrs_current[] = {"detail_flow",      flow,    "name_grow", name_grow,
                                   "hide_edit_pencil", "false", nullptr,     nullptr};

    bool have_preselection = false;

    for (const auto& spool : filtered) {
        const bool is_current_spool =
            (working_info_.spoolman_id > 0 && spool.id == working_info_.spoolman_id);
        lv_obj_t* item = static_cast<lv_obj_t*>(lv_xml_create(
            spool_list, "spoolman_spool_item", is_current_spool ? attrs_current : attrs_plain));
        if (!item) {
            continue;
        }

        lv_obj_set_user_data(item, reinterpret_cast<void*>(static_cast<intptr_t>(spool.id)));

        lv_obj_t* name_label = helix::ui::find_required(item, "spool_name", get_name());
        if (name_label) {
            const std::string name = "#" + std::to_string(spool.id) + " " + spool.display_name();
            lv_label_set_text(name_label, name.c_str());
        }

        lv_obj_t* weight_label = helix::ui::find_required(item, "spool_weight", get_name());
        if (weight_label && spool.remaining_weight_g > 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.0fg", spool.remaining_weight_g);
            lv_label_set_text(weight_label, buf);
        }

        lv_obj_t* swatch = helix::ui::find_required(item, "spool_swatch", get_name());
        if (swatch) {
            uint32_t rgb = helix::parse_hex_color(spool.color_hex).value_or(0x808080);
            helix::ui::apply_swatch_color(swatch, rgb, spool.multi_color_hexes,
                                          helix::ui::swatch_finish(spool.finish()));
        }

        const bool disabled = in_use.count(spool.id) > 0;
        if (disabled) {
            lv_obj_add_state(item, LV_STATE_DISABLED);
            lv_obj_remove_flag(item, LV_OBJ_FLAG_CLICKABLE);
        }

        bool is_selected = false;
        if (working_info_.spoolman_id > 0) {
            is_selected = (spool.id == working_info_.spoolman_id);
        } else if (!have_preselection && !disabled) {
            is_selected = true;
        }
        if (is_selected) {
            have_preselection = true;
            lv_obj_set_state(item, LV_STATE_CHECKED, true);
            lv_obj_t* check_icon = helix::ui::find_required(item, "selected_icon", get_name());
            if (check_icon) {
                lv_obj_remove_flag(check_icon, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_set_state(item, LV_STATE_CHECKED, false);
        }
    }

    lv_subject_set_int(&picker_state_subject_, filtered.empty() ? 3 : 2);
}

void AmsEditOverlay::handle_spool_selected(int spool_id) {
    spdlog::info("[AmsEditOverlay] Spool {} selected for slot {}", spool_id, slot_index_);

    for (const auto& spool : cached_spools_) {
        if (spool.id == spool_id) {
            apply_spool_to_slot(working_info_, spool);
            break;
        }
    }

    if (opened_on_picker_) {
        spdlog::debug("[AmsEditOverlay] Picker-entry selection - committing and closing");
        commit_and_close();
        return;
    }

    // Two-step flow: return to the editor with the picked spool loaded.
    sel_ = selection_from_slot(working_info_);
    selection_moved_ = false;
    details_color_ = working_info_.color_rgb;
    details_color_set_ = false;
    set_view(VIEW_EDITOR);
    update_ui();
    rebuild_chip_rows();
    update_summary();
}

void AmsEditOverlay::fetch_recents() {
    // Recents come from Spoolman history: the recency-sorted spool list. One
    // fetch on open; without Spoolman the rows stay in catalog order.
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    if (!spoolman_subj || lv_subject_get_int(spoolman_subj) != 1) {
        return;
    }
    if (!api_) {
        api_ = get_moonraker_api();
    }
    if (!api_) {
        return;
    }
    auto token = lifetime_.token();
    api_->spoolman().get_spoolman_spools(
        [this, token](const std::vector<SpoolInfo>& spools) {
            token.defer([this, spools]() {
#if !defined(HELIX_PLATFORM_ESP32)
                // spoolman_types.cpp is scoped out of the ESP32 image; the
                // rows there stay in catalog order.
                recent_families_ = recent_material_families(spools);
                recent_brands_ = recent_brands(spools);
#endif
                // cached_spools_ doubles as the picker's warm cache; a later
                // populate_picker() refetches anyway.
                if (lv_subject_get_int(&view_mode_subject_) == VIEW_EDITOR) {
                    rebuild_chip_rows();
                }
            });
        },
        [](const MoonrakerError& err) {
            spdlog::debug("[AmsEditOverlay] recents fetch skipped: {}", err.message);
        });
}

void AmsEditOverlay::refresh_tag_state() {
    tag_.reset();
    auto* backend = AmsState::instance().get_backend();
    if (backend && slot_index_ >= 0) {
        tag_ = backend->tag_reading(slot_index_);
    }
    lv_subject_set_int(&reset_tag_visible_subject_, (tag_ && !tag_->empty()) ? 1 : 0);
    bool from_tag = false;
    if (tag_) {
        const ams::TagProvenance prov = ams::tag_provenance(
            *tag_, backend ? backend->get_slot_info(slot_index_) : working_info_);
        from_tag = prov.any();
    }
    lv_subject_set_int(&tag_badge_visible_subject_, from_tag ? 1 : 0);
}

void AmsEditOverlay::enter_editor() {
    catalog_ = printer::FilamentCatalog::load_full();
    // The colour areas are subject-counted (a literal <repeat> inside <if> is
    // refused by the engine): the narrow split column always shows 15, the
    // stacked grid 18 where height is scarce and 30 where it is not.
    lv_subject_set_int(&color_col_count_subject_, 15);
    lv_subject_t* bp = theme_manager_get_breakpoint_subject();
    const int row_count =
        (bp && lv_subject_get_int(bp) <= static_cast<int>(UiBreakpoint::Small)) ? 18 : 30;
    lv_subject_set_int(&color_row_count_subject_, row_count);
    sel_ = selection_from_slot(working_info_);
    selection_moved_ = false;
    details_color_ = working_info_.color_rgb;
    details_color_set_ = false;

    if (!api_) {
        api_ = get_moonraker_api();
    }

    // Seed the logistics fields from what we know (meaningful for managed
    // slots; the section hides otherwise, but seeding is harmless).
    detail_original_ = SpoolInfo{};
    detail_original_.id = working_info_.spoolman_id;
    detail_original_.filament_id = working_info_.spoolman_filament_id;
    detail_original_.vendor = working_info_.brand;
    detail_original_.material = working_info_.material;
    detail_original_.filament_name = working_info_.spool_name;
    detail_original_.remaining_weight_g = working_info_.remaining_weight_g;
    detail_original_.initial_weight_g = working_info_.total_weight_g;
    detail_original_.spool_weight_g = working_info_.total_weight_g;
    detail_working_ = detail_original_;

    set_view(VIEW_EDITOR);
    populate_editor_view();
    refresh_tag_state();
    fetch_recents();

    // Managed slots: refresh logistics from Spoolman (fresh data on entry).
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    bool has_spoolman = spoolman_subj && lv_subject_get_int(spoolman_subj) == 1;
    if (has_spoolman && working_info_.spoolman_id > 0 && api_) {
        const int spool_id = working_info_.spoolman_id;
        auto token = lifetime_.token();
        api_->spoolman().get_spoolman_spool(
            spool_id,
            [this, token, spool_id](const std::optional<SpoolInfo>& spool) {
                if (!spool) {
                    return;
                }
                token.defer([this, spool = *spool]() {
                    detail_original_ = spool;
                    detail_working_ = spool;
                    populate_detail_fields();
                });
            },
            [spool_id](const MoonrakerError& err) {
                spdlog::warn("[AmsEditOverlay] Spool-details fetch for {} failed: {}", spool_id,
                             err.message);
            });
    }

    spdlog::debug("[AmsEditOverlay] Entered editor view");
}

void AmsEditOverlay::populate_editor_view() {
    if (!catalog_) {
        catalog_ = printer::FilamentCatalog::load_full();
    }
    rebuild_chip_rows();
    update_summary();
    update_temp_display();
    populate_detail_fields();
}

void AmsEditOverlay::repopulate() {
    // The view subject is C++-owned, so it keeps its value across the rebuild
    // and the fresh widgets bind to the right branch on their own. What does
    // not survive is the content each view populates on entry.
    const int view = lv_subject_get_int(&view_mode_subject_);
    switch (view) {
    case VIEW_SPOOL_PICKER:
        populate_picker();
        break;
    case VIEW_COLOR:
        populate_color_view();
        break;
    case VIEW_EDITOR:
    default:
        populate_editor_view();
        refresh_tag_state();
        break;
    }
    spdlog::debug("[AmsEditOverlay] Repopulated view {}", view);
}

// ============================================================================
// Summary / temps / weight
// ============================================================================

void AmsEditOverlay::update_summary() {
    // Identity: "eSUN Silk PETG, Blue" — brand + the selected type + colour
    // name. The type comes from the SELECTION (live per tap), not the slot.
    const std::string type = catalog_ ? selected_type(sel_, *catalog_) : working_info_.material;
    const std::string color = details_color_set_
                                  ? helix::get_color_name_from_hex(details_color_)
                                  : helix::get_color_name_from_hex(working_info_.color_rgb);
    std::string label =
        helix::compose_filament_label(sel_.brand, working_info_.spool_name, type, " ");
    if (!color.empty()) {
        label += ", " + color;
    }
    snprintf(summary_buf_, sizeof(summary_buf_), "%s", label.c_str());
    lv_subject_copy_string(&summary_text_subject_, summary_buf_);

    // Colour dot: multi-colour spools paint the split; a picked colour paints
    // plain. Multi-colour shows read-only in the grid (the ring has no
    // multi-cell); picking replaces.
    lv_obj_t* dot = find_widget("summary_color_swatch");
    if (dot) {
        apply_swatch_color(dot, details_color_set_ ? details_color_ : working_info_.color_rgb,
                           details_color_set_ ? std::string() : working_info_.multi_color_hexes);
    }

    // Ring the preset the pending colour matches, if any.
    const uint32_t pending = details_color_set_ ? details_color_ : working_info_.color_rgb;
    int ring = -1;
    const auto& palette = swatch_palette(ColorPicker::Palette::General);
    for (size_t i = 0; i < palette.size(); ++i) {
        if (palette[i] == pending) {
            ring = static_cast<int>(i);
            break;
        }
    }
    if (!working_info_.multi_color_hexes.empty() && !details_color_set_) {
        ring = -1; // a split swatch is not a preset cell
    }
    lv_subject_set_int(&color_selected_subject_, ring);

    // Weight pill: "750 g" + "of 1000 g".
    if (working_info_.remaining_weight_g >= 0) {
        snprintf(weight_buf_, sizeof(weight_buf_), "%.0f g", working_info_.remaining_weight_g);
    } else {
        snprintf(weight_buf_, sizeof(weight_buf_), "—");
    }
    lv_subject_copy_string(&weight_text_subject_, weight_buf_);
    if (working_info_.total_weight_g > 0) {
        snprintf(weight_hint_buf_, sizeof(weight_hint_buf_), "%s %.0f g", lv_tr("of"),
                 working_info_.total_weight_g);
    } else {
        weight_hint_buf_[0] = '\0';
    }
    lv_subject_copy_string(&weight_hint_subject_, weight_hint_buf_);
}

void AmsEditOverlay::update_temp_display() {
    if (!overlay_root_) {
        return;
    }

    // Live per tap: the product's temperatures when a product is selected,
    // else the material database's range for the selected type, else the
    // slot's stored values.
    int nozzle_min = working_info_.nozzle_temp_min;
    int nozzle_max = working_info_.nozzle_temp_max;
    int bed_temp = working_info_.bed_temp;
    const std::string type = catalog_ ? selected_type(sel_, *catalog_) : working_info_.material;
    bool from_catalog = false;
    if (catalog_ && !sel_.product_id.empty()) {
        if (const auto* ef = catalog_->resolve_id(sel_.product_id)) {
            nozzle_min = ef->nozzle_min;
            nozzle_max = ef->nozzle_max;
            bed_temp = ef->bed_temp;
            from_catalog = true;
        }
    }
    if (!from_catalog && nozzle_min == 0 && nozzle_max == 0 && bed_temp == 0 && !type.empty()) {
        if (auto mat = filament::find_material(type)) {
            nozzle_min = mat->nozzle_min;
            nozzle_max = mat->nozzle_max;
            bed_temp = mat->bed_temp;
        }
    }

    // One read-only line under the summary: "Nozzle 230–250 °C · Bed 80 °C".
    if (nozzle_min > 0 || nozzle_max > 0 || bed_temp > 0) {
        snprintf(temp_buf_, sizeof(temp_buf_),
                 "%s %d\xE2\x80\x93%d\xC2\xB0"
                 "C \xC2\xB7 %s %d\xC2\xB0"
                 "C",
                 lv_tr("Nozzle"), nozzle_min, nozzle_max, lv_tr("Bed"), bed_temp);
    } else {
        temp_buf_[0] = '\0';
    }
    lv_subject_copy_string(&temp_text_subject_, temp_buf_);
}

// ============================================================================
// Internal Methods
// ============================================================================

void AmsEditOverlay::update_ui() {
    if (!overlay_root_) {
        return;
    }

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    bool has_spoolman = spoolman_subj && lv_subject_get_int(spoolman_subj) == 1;
    const bool managed = has_spoolman && working_info_.spoolman_id > 0;
    lv_subject_set_int(&is_managed_subject_, managed ? 1 : 0);

#if defined(HELIX_PLATFORM_ESP32)
    const bool scan_ok = false; // no camera on the v1 Core+AMS cut
#else
    const bool scan_ok = has_spoolman;
#endif
    lv_subject_set_int(&scan_enabled_subject_, scan_ok ? 1 : 0);

    // The quiet Add link: untracked slots reach Spoolman only through it.
    lv_subject_set_int(&add_spoolman_visible_subject_,
                       (has_spoolman && working_info_.spoolman_id == 0) ? 1 : 0);

    // Title: the slot's own name in the backend's lane noun ("Gate 3").
    if (slot_index_ == -2) {
        snprintf(title_buf_, sizeof(title_buf_), "%s", lv_tr("External Filament"));
    } else {
        snprintf(title_buf_, sizeof(title_buf_), "%s",
                 helix::ui::lane_label(helix::ui::active_lane_noun(), slot_index_).c_str());
    }
    lv_subject_copy_string(&title_subject_, title_buf_);

    // Spool number beside the tracked mark.
    if (managed) {
        snprintf(spoolman_id_buf_, sizeof(spoolman_id_buf_), "#%d", working_info_.spoolman_id);
    } else {
        spoolman_id_buf_[0] = '\0';
    }
    lv_subject_copy_string(&spoolman_id_subject_, spoolman_id_buf_);

    update_summary();
    update_temp_display();
}

bool AmsEditOverlay::selection_dirty() const {
    return selection_moved_ || details_color_set_;
}

bool AmsEditOverlay::is_dirty() const {
    // The same field set the old editor compared, minus the tool mapping
    // (Assigned Tool moved to the context menu).
    return working_info_.color_rgb != original_info_.color_rgb ||
           working_info_.material != original_info_.material ||
           working_info_.brand != original_info_.brand ||
           working_info_.spoolman_id != original_info_.spoolman_id ||
           std::abs(working_info_.remaining_weight_g - original_info_.remaining_weight_g) > 0.1f ||
           std::abs(working_info_.total_weight_g - original_info_.total_weight_g) > 0.1f ||
           selection_dirty();
}

void AmsEditOverlay::update_sync_button_state() {
    if (!subjects_initialized_) {
        return;
    }
    // The editor view's Save is dirty-gated; a save in flight holds it shut on
    // every view.
    const int view = lv_subject_get_int(&view_mode_subject_);
    lv_subject_set_int(&save_disabled_subject_,
                       save_is_disabled(view, save_in_flight_, is_dirty()) ? 1 : 0);
}

bool AmsEditOverlay::identity_locked(const SlotInfo& original, const SlotInfo& working,
                                     bool spoolman_available) {
    if (spoolman_available) {
        return false;
    }
    // Only the spool the editor opened on.
    return original.spoolman_id > 0 && working.spoolman_id == original.spoolman_id;
}

bool AmsEditOverlay::save_is_disabled(int view, bool save_in_flight, bool dirty) {
    if (save_in_flight) {
        return true;
    }
    return (view == VIEW_EDITOR) ? !dirty : false;
}

// ============================================================================
// Colour staging (shared by the grid, the HSV view and Reset to tag)
// ============================================================================

void AmsEditOverlay::open_color_view() {
    // Seed custom sub-state from the pending colour. Black seeds as-is — it is
    // a real colour (#1608).
    custom_color_ = details_color_;
    populate_color_view();
    set_view(VIEW_COLOR);
}

void AmsEditOverlay::populate_color_view() {
    lv_obj_t* hsv = helix::ui::find_required(overlay_root_, "ams_color_hsv", get_name());
    if (hsv) {
        ui_hsv_picker_set_color_rgb(hsv, custom_color_);
    }
    lv_obj_t* preview = helix::ui::find_required(overlay_root_, "ams_color_preview", get_name());
    if (preview) {
        helix::ui::apply_swatch_color(preview, custom_color_, {});
    }
    lv_obj_t* hex_input =
        helix::ui::find_required(overlay_root_, "ams_color_hex_input", get_name());
    if (hex_input) {
        char buf[10];
        snprintf(buf, sizeof(buf), "#%06X", custom_color_);
        lv_textarea_set_text(hex_input, buf);
    }
}

void AmsEditOverlay::apply_color(uint32_t rgb) {
    details_color_ = helix::ams::picked_color(rgb);
    details_color_set_ = true;
    set_view(VIEW_EDITOR);
    update_summary();
    update_sync_button_state();
}

void AmsEditOverlay::handle_picker_search(const char* text) {
    if (cached_spools_.empty()) {
        return;
    }
    render_spool_list(text ? text : "");
}

void AmsEditOverlay::handle_quick_swatch(lv_obj_t* swatch) {
    if (!swatch) {
        return;
    }
    details_color_ = lv_color_to_u32(helix::ui::swatch_color_of(swatch)) & 0xFFFFFF;
    details_color_set_ = true;
    update_summary();
    update_sync_button_state();
}

void AmsEditOverlay::handle_reset_to_tag() {
    // Every field the tag states goes back to the tag's values - material,
    // brand, product line, colour, temperatures, total weight - through the
    // shared ams::reset_to_tag(). The editor's staging (selection, pending
    // colour) re-derives from the restored slot, and dirty state follows the
    // working/original diff: a slot that never left the tag's values resets
    // clean, one edited past them resets dirty.
    if (!tag_) {
        return;
    }
    working_info_ = ams::reset_to_tag(working_info_, *tag_);
    sel_ = selection_from_slot(working_info_);
    selection_moved_ = false;
    details_color_ = working_info_.color_rgb;
    details_color_set_ = false;
    rebuild_chip_rows();
    update_summary();
    update_temp_display();
    refresh_tag_state();
    update_sync_button_state();
}

void AmsEditOverlay::handle_custom_color_changed(uint32_t rgb) {
    custom_color_ = rgb;
    lv_obj_t* preview = helix::ui::find_required(overlay_root_, "ams_color_preview", get_name());
    if (preview) {
        helix::ui::apply_swatch_color(preview, rgb, {});
    }
    lv_obj_t* hex_input =
        helix::ui::find_required(overlay_root_, "ams_color_hex_input", get_name());
    if (hex_input) {
        char buf[10];
        snprintf(buf, sizeof(buf), "#%06X", rgb);
        lv_textarea_set_text(hex_input, buf);
    }
}

void AmsEditOverlay::handle_color_swatch(lv_obj_t* swatch) {
    if (!swatch) {
        return;
    }
    const uint32_t rgb = lv_color_to_u32(helix::ui::swatch_color_of(swatch)) & 0xFFFFFF;
    apply_color(rgb); // preset tap applies immediately and returns
}

void AmsEditOverlay::handle_color_hex_changed() {
    lv_obj_t* hex_input =
        helix::ui::find_required(overlay_root_, "ams_color_hex_input", get_name());
    if (!hex_input) {
        return;
    }
    const char* text = lv_textarea_get_text(hex_input);
    uint32_t rgb = 0;
    if (text && helix::parse_hex_color(text, rgb)) {
        custom_color_ = rgb;
        lv_obj_t* hsv = helix::ui::find_required(overlay_root_, "ams_color_hsv", get_name());
        if (hsv) {
            ui_hsv_picker_set_color_rgb(hsv, rgb);
        }
        lv_obj_t* preview =
            helix::ui::find_required(overlay_root_, "ams_color_preview", get_name());
        if (preview) {
            helix::ui::apply_swatch_color(preview, rgb, {});
        }
    }
}

void AmsEditOverlay::handle_color_apply() {
    apply_color(custom_color_);
}

// ============================================================================
// More section / weight / logistics
// ============================================================================

void AmsEditOverlay::handle_more_toggle() {
    const int open = lv_subject_get_int(&more_open_subject_) ? 0 : 1;
    lv_subject_set_int(&more_open_subject_, open);
}

void AmsEditOverlay::handle_weight_tap() {
    ui_keypad_config_t config = {
        .initial_value =
            working_info_.remaining_weight_g > 0 ? working_info_.remaining_weight_g : 0,
        .min_value = 0,
        .max_value = 100000,
        .title_label = lv_tr("Remaining weight"),
        .unit_label = "g",
        .allow_decimal = false,
        .allow_negative = false,
        .callback =
            [](float value, void* user_data) {
                static_cast<AmsEditOverlay*>(user_data)->apply_weight(value);
            },
        .user_data = this};
    ui_keypad_show(&config);
}

void AmsEditOverlay::apply_weight(float grams) {
    working_info_.remaining_weight_g = grams;
    // A spool weighed for the first time: the same number is the honest total
    // until something better is known.
    if (working_info_.total_weight_g <= 0 && grams > 0) {
        working_info_.total_weight_g = grams;
    }
    update_summary();
    update_sync_button_state();
}

void AmsEditOverlay::populate_detail_fields() {
    char buf[16];
    // Negative values are the "unknown" sentinel — render blank rather than a
    // literal "-1" (and a blank field can't trip the negative-value Save
    // validation on an untouched, weight-unknown untracked slot).
    std::string price;
    if (detail_working_.price > 0) {
        snprintf(buf, sizeof(buf), "%.2f", detail_working_.price);
        price = buf;
    }
    const struct {
        const char* widget;
        std::string value;
    } fields[] = {
        {"detail_field_price", price},
        {"detail_field_location", detail_working_.location},
        {"detail_field_lot_nr", detail_working_.lot_nr},
        {"detail_field_comment", detail_working_.comment},
    };
    for (const auto& f : fields) {
        lv_obj_t* w = find_widget(f.widget);
        if (w) {
            lv_textarea_set_text(w, f.value.c_str());
        }
    }
}

void AmsEditOverlay::read_detail_fields() {
    auto text_of = [this](const char* name) -> std::string {
        lv_obj_t* w = find_widget(name);
        const char* t = w ? lv_textarea_get_text(w) : nullptr;
        return t ? t : "";
    };
    std::string s = text_of("detail_field_price");
    detail_working_.price = s.empty() ? 0 : std::atof(s.c_str());
    detail_working_.location = text_of("detail_field_location");
    detail_working_.lot_nr = text_of("detail_field_lot_nr");
    detail_working_.comment = text_of("detail_field_comment");
}

// ============================================================================
// Scan QR / label printing
// ============================================================================

void AmsEditOverlay::handle_scan_qr() {
#if !defined(HELIX_PLATFORM_ESP32)
    spdlog::info("[AmsEditOverlay] Scan QR requested for slot {}", slot_index_);

    // The scanner overlay pushes ON TOP of the editor. Our on_deactivate
    // treats that as "covered": session state and the view subject survive.
    auto& scanner = helix::ui::get_qr_scanner_overlay();
    scanner.show(
        lv_screen_active(), slot_index_,
        [](const SpoolInfo& spool) {
            // Scanner result arrives outside our activation — marshal via the
            // queue and re-validate on the singleton, which lives for the
            // process. No direct backend write: the editor repopulates and the
            // user confirms with Save.
            SlotInfo scanned;
            apply_spool_to_slot(scanned, spool);
            helix::ui::queue_update(
                "AmsEditOverlay::handle_scan_qr", [scanned = std::move(scanned)]() {
                    auto& editor = get_ams_edit_overlay();
                    if (!editor.get_root()) {
                        return;
                    }
                    int keep_slot = editor.working_info_.slot_index;
                    int keep_global = editor.working_info_.global_index;
                    editor.working_info_ = scanned;
                    editor.working_info_.slot_index = keep_slot;
                    editor.working_info_.global_index = keep_global;
                    editor.sel_ = editor.selection_from_slot(editor.working_info_);
                    editor.selection_moved_ = false;
                    editor.set_view(VIEW_EDITOR);
                    editor.update_ui();
                    editor.rebuild_chip_rows();
                    editor.update_summary();
                    NOTIFY_INFO("{} {} scanned — review and Save", scanned.brand, scanned.material);
                });
        },
        []() { spdlog::debug("[AmsEditOverlay] QR scan cancelled - editor resumes"); });
#endif
}

#if HELIX_HAS_LABEL_PRINTER
void AmsEditOverlay::handle_print_label() {
    auto& settings = helix::LabelPrinterSettingsManager::instance();

    if (!settings.is_configured()) {
        ToastManager::instance().show(ToastSeverity::INFO,
                                      lv_tr("Set up your label printer in Settings"), 3000);
        return;
    }

    SpoolInfo spool_info;
    bool found = false;
    for (const auto& spool : cached_spools_) {
        if (spool.id == working_info_.spoolman_id) {
            spool_info = spool;
            found = true;
            break;
        }
    }

    if (!found) {
        spool_info.id = working_info_.spoolman_id;
        spool_info.vendor = working_info_.brand;
        spool_info.material = working_info_.material;
        spool_info.filament_name = working_info_.spool_name;
        spool_info.remaining_weight_g = working_info_.remaining_weight_g;
        spool_info.initial_weight_g = working_info_.total_weight_g;
    }

    auto print_cb = [](bool success, const std::string& error) {
        if (success) {
            ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("Label printed"), 2000);
        } else {
            spdlog::error("[AmsEditOverlay] Print failed: {}", error);
            ToastManager::instance().show(ToastSeverity::ERROR,
                                          helix::friendly_label_printer_error(error).c_str(), 5000);
        }
    };

    if (!helix::maybe_show_ipp_print_modal(spool_info, print_cb)) {
        ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Printing label..."), 2000);
        helix::print_spool_label(spool_info, print_cb);
    }
}
#endif

// ============================================================================
// Save Orchestration
// ============================================================================

void AmsEditOverlay::stage_selection_into_working() {
    if (!selection_moved_ || !catalog_) {
        return;
    }
    const std::string type = selected_type(sel_, *catalog_);
    working_info_.material = type;
    working_info_.brand = iequals_str(sel_.brand, "Generic") ? std::string("Generic") : sel_.brand;
    if (!sel_.product_id.empty()) {
        if (const auto* ef = catalog_->resolve_id(sel_.product_id)) {
            working_info_.nozzle_temp_min = ef->nozzle_min;
            working_info_.nozzle_temp_max = ef->nozzle_max;
            working_info_.bed_temp = ef->bed_temp;
            working_info_.catalog_id = ef->id;
            working_info_.product_name = ef->name;
            // A Spoolman name describes the spool the old identity named.
            helix::ams::drop_stale_spool_name(original_info_, working_info_);
            return;
        }
    }
    // No product: temperatures follow the material's database range.
    working_info_.catalog_id.clear();
    working_info_.product_name.clear();
    if (auto mat = filament::find_material(type)) {
        working_info_.nozzle_temp_min = mat->nozzle_min;
        working_info_.nozzle_temp_max = mat->nozzle_max;
        working_info_.bed_temp = mat->bed_temp;
    }
    helix::ams::drop_stale_spool_name(original_info_, working_info_);
}

void AmsEditOverlay::fire_completion(bool saved) {
    if (completion_fired_) {
        return;
    }
    completion_fired_ = true;
    spdlog::info("[AmsEditOverlay] fire_completion saved={} slot={} spoolman_id={} material={}",
                 saved, slot_index_, working_info_.spoolman_id, working_info_.material);
    if (completion_callback_) {
        EditResult result;
        result.saved = saved;
        result.slot_index = slot_index_;
        result.slot_info = working_info_;
        completion_callback_(result);
    }
}

void AmsEditOverlay::close_editor(bool saved) {
    fire_completion(saved);
    helix::nav::go_back();
}

// ============================================================================
// Event Handlers
// ============================================================================

void AmsEditOverlay::handle_back() {
    int view = lv_subject_get_int(&view_mode_subject_);
    switch (view) {
    case VIEW_SPOOL_PICKER:
        set_view(VIEW_EDITOR);
        break;
    case VIEW_COLOR:
        set_view(VIEW_EDITOR);
        break;
    case VIEW_EDITOR:
    default:
        if (is_dirty()) {
            prompt_unsaved_changes();
            return;
        }
        spdlog::debug("[AmsEditOverlay] Back on editor - cancelling");
        working_info_ = original_info_;
        close_editor(false);
        break;
    }
}

void AmsEditOverlay::prompt_unsaved_changes() {
    // Back with unsaved edits: Save / Discard. A dismissal (backdrop tap,
    // ESC) keeps the editor open — the user has not answered.
    ConfirmOptions opts;
    opts.cancel_text = lv_tr("Discard");
    opts.on_cancel = [] {
        auto& editor = get_ams_edit_overlay();
        editor.working_info_ = editor.original_info_;
        editor.close_editor(false);
    };
    opts.on_dismiss = [] { /* stay open */ };
    opts.owner_token = lifetime_.token();
    modal_confirm(
        lv_tr("Unsaved changes"), lv_tr("Save the changes to this spool?"), ModalSeverity::Info,
        lv_tr("Save"), [] { get_ams_edit_overlay().handle_save(); }, opts);
}

bool AmsEditOverlay::should_create_new_spool(const SlotInfo& working_info, bool add_requested) {
    return add_requested && working_info.spoolman_id == 0 &&
           helix::SpoolmanSlotSaver::is_filament_complete(working_info);
}

bool AmsEditOverlay::needs_identity_confirmation(const SlotInfo& original, const SlotInfo& edited) {
    if (edited.spoolman_id <= 0) {
        return false; // nothing linked to clobber
    }
    if (edited.spoolman_id != original.spoolman_id) {
        return false; // a relink is not an edit of the linked spool
    }
    auto changes = helix::SpoolmanSlotSaver::detect_changes(original, edited);
    if (!changes.any()) {
        return false;
    }
    return is_material_identity_change(original, edited);
}

void AmsEditOverlay::handle_add_to_spoolman() {
    // The quiet link: create a Spoolman spool from what the slot holds (then
    // print its label). Stages the selection first so the created spool
    // carries the chips' identity, not the pre-edit one.
    stage_selection_into_working();
    if (details_color_set_) {
        working_info_.color_rgb = details_color_;
        working_info_.color_name = helix::get_color_name_from_hex(details_color_);
        working_info_.multi_color_hexes.clear();
    }
    const helix::MissingFilamentFields missing =
        helix::SpoolmanSlotSaver::missing_filament_fields(working_info_);
    if (missing.any()) {
        show_missing_filament_toast(missing);
        return;
    }
    do_spoolman_save(helix::SpoolmanSlotSaver::LinkIntent::CreateAndRebind);
}

void AmsEditOverlay::handle_save() {
    if (lv_subject_get_int(&view_mode_subject_) == VIEW_COLOR) {
        apply_color(custom_color_); // Select on the colour view stages + returns
        return;
    }

    spdlog::info("[AmsEditOverlay] Saving edits for slot {}", slot_index_);

    read_detail_fields();
    if (detail_working_.price < 0) {
        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Values must not be negative"),
                                      3000);
        return;
    }

    stage_selection_into_working();
    if (details_color_set_) {
        working_info_.color_rgb = details_color_;
        working_info_.color_name = helix::get_color_name_from_hex(details_color_);
        working_info_.multi_color_hexes.clear();
    }

    commit_and_close();
}

void AmsEditOverlay::commit_and_close() {
    if (!api_) {
        api_ = get_moonraker_api();
    }

    // Spoolman can go away between staging a brand or material and saving it.
    // The change has nowhere to go, and committing it locally would leave the
    // slot claiming an identity the server never heard.
    if (identity_locked(original_info_, working_info_,
                        get_printer_state().is_spoolman_available()) &&
        (working_info_.brand != original_info_.brand ||
         working_info_.material != original_info_.material)) {
        ToastManager::instance().show(
            ToastSeverity::ERROR,
            lv_tr("Spoolman is offline, so brand and material can't be saved."), 3000);
        stay_open_after_failed_save();
        return;
    }

    // Switching the linked spool is a pure RELINK, not an edit of the linked
    // spool's record.
    const bool relinked_to_existing_spool =
        working_info_.spoolman_id > 0 && working_info_.spoolman_id != original_info_.spoolman_id;
    if (relinked_to_existing_spool) {
        spdlog::info("[AmsEditOverlay] Relink slot {} to spool {} (was {})", slot_index_,
                     working_info_.spoolman_id, original_info_.spoolman_id);
        close_editor(true);
        return;
    }

    if (api_ && get_printer_state().is_spoolman_available()) {
        const bool has_linked_spool = working_info_.spoolman_id > 0;
        auto changes = helix::SpoolmanSlotSaver::detect_changes(original_info_, working_info_);

        if (needs_identity_confirmation(original_info_, working_info_)) {
            prompt_identity_change_then_save();
            return;
        }

        if ((has_linked_spool && changes.any())) {
            do_spoolman_save();
            return; // Async path - close_editor called from callback
        }
    }

    // No Spoolman changes (or no Spoolman) - save locally immediately
    close_editor(true);
}

void AmsEditOverlay::stay_open_after_failed_save() {
    update_sync_button_state();
}

void AmsEditOverlay::show_missing_filament_toast(const helix::MissingFilamentFields& missing) {
    const char* message = nullptr;
    if (missing.brand) {
        message = lv_tr("Spoolman needs a brand for this filament.");
    } else if (missing.material) {
        message = lv_tr("Spoolman needs a material for this filament.");
    } else if (missing.color) {
        message = lv_tr("Spoolman needs a color for this filament.");
    }
    if (message) {
        ToastManager::instance().show(ToastSeverity::ERROR, message, 3000);
    }
}

void AmsEditOverlay::do_spoolman_save(helix::SpoolmanSlotSaver::LinkIntent intent) {
    auto token = lifetime_.token();
    auto saver = std::make_shared<helix::SpoolmanSlotSaver>(
        api_, get_printer_state().get_discovery().filament_diameter_mm());
    // One save at a time: the editor stays open while this runs.
    save_in_flight_ = true;
    update_sync_button_state();
    saver->save(
        original_info_, working_info_, intent,
        [this, token, saver](const helix::SaveResult& result) {
            // Spoolman callback arrives on a background thread — defer
            // to the UI thread before touching LVGL.
            token.defer([this, result]() {
                save_in_flight_ = false;
                if (result.missing.any()) {
                    spdlog::info("[AmsEditOverlay] Spoolman save stopped: the filament is "
                                 "missing a field it is identified by");
                    show_missing_filament_toast(result.missing);
                    stay_open_after_failed_save();
                    return;
                }
                if (!result.success) {
                    spdlog::error("[AmsEditOverlay] Spoolman save failed; nothing written");
                    ToastManager::instance().show(
                        ToastSeverity::ERROR,
                        lv_tr("Couldn't save to Spoolman. Nothing changed on this "
                              "printer."),
                        3000);
                    stay_open_after_failed_save();
                    return;
                }
                if (result.repointed_filament) {
                    // The spool now carries the identity this save
                    // wrote. File it as the lane's Spoolman record
                    // BEFORE closing: the consumer commit keeps a
                    // linked spool's own identity, and the record it
                    // would otherwise read is the pre-repoint one the
                    // next fetch has not replaced yet.
                    SpoolInfo repointed;
                    repointed.id = working_info_.spoolman_id;
                    repointed.filament_id = result.new_filament_id;
                    repointed.vendor_id = result.new_vendor_id;
                    repointed.vendor = working_info_.brand;
                    repointed.material = working_info_.material;
                    repointed.filament_name = working_info_.spool_name;
                    repointed.color_hex = helix::color_to_hex_string(working_info_.color_rgb);
                    if (auto* owner = AmsState::instance().get_backend()) {
                        SpoolmanManager::apply_fetched_spool(*owner, 0, slot_index_, repointed);
                    }
                }
                if (result.created_new_spool || result.repointed_filament) {
                    if (result.new_spool_id != 0) {
                        working_info_.spoolman_id = result.new_spool_id;
                    }
                    if (result.new_filament_id != 0) {
                        working_info_.spoolman_filament_id = result.new_filament_id;
                    }
                    if (result.new_vendor_id != 0) {
                        working_info_.spoolman_vendor_id = result.new_vendor_id;
                    }
                    // The commit owns the active-spool sync EXCEPT for
                    // a newly created spool, whose id only exists here.
                    if (result.created_new_spool && result.new_spool_id != 0 && api_) {
                        sync_active_spool(api_, result.new_spool_id);
                    }
                    if (result.created_new_spool) {
                        ToastManager::instance().show(ToastSeverity::INFO,
                                                      lv_tr("Added to Spoolman"), 2500);
                    }
                }
                const int saved_spool_id = working_info_.spoolman_id;
                close_editor(true);
                SpoolmanManager::refresh_spool(saved_spool_id);
            });
        });
}

bool AmsEditOverlay::may_write_spoolman_now(const SlotInfo& original, const SlotInfo& edited) {
    return !needs_identity_confirmation(original, edited);
}

bool AmsEditOverlay::is_material_identity_change(const SlotInfo& original, const SlotInfo& edited) {
    if (!helix::FilamentMapper::materials_match(original.material, edited.material)) {
        return true;
    }
    return !helix::FilamentMapper::colors_match(original.color_rgb, edited.color_rgb);
}

void AmsEditOverlay::prompt_identity_change_then_save() {
    // A Spoolman-linked slot whose material/brand/colour changed. The two real
    // outcomes: move the spool to the matching filament (creating one if
    // needed — the saver repoints, it never edits the shared filament record),
    // or unlink and keep the slot local. Cancel/dismiss aborts the save
    // entirely: nothing written, edits stay staged.
    auto true_abort = [] { get_ams_edit_overlay().stay_open_after_failed_save(); };

    ConfirmOptions opts;
    opts.cancel_text = lv_tr("Unlink, keep on this slot");
    opts.on_cancel = [] {
        // Keep the slot's edited values lane-local; nothing reaches Spoolman.
        auto& editor = get_ams_edit_overlay();
        editor.working_info_.clear_spoolman_link();
        editor.close_editor(true);
    };
    opts.on_dismiss = true_abort;
    opts.owner_token = lifetime_.token();
    lv_obj_t* dlg = modal_confirm(
        lv_tr("Different filament?"),
        lv_tr("This doesn't match the linked Spoolman spool. Update it in Spoolman, or unlink and "
              "keep it on this slot?"),
        ModalSeverity::Warning, lv_tr("Update in Spoolman"),
        [] {
            get_ams_edit_overlay().do_spoolman_save(
                helix::SpoolmanSlotSaver::LinkIntent::UpdateLinked);
        },
        opts);
    if (!dlg) {
        spdlog::warn("[AmsEditOverlay] identity confirmation failed to show; aborting save");
        close_editor(false);
    }
}

// ============================================================================
// Callback Registration
// ============================================================================

void AmsEditOverlay::register_callbacks() {
    register_xml_callbacks({
        {"ams_edit_back_cb", [](lv_event_t*) { get_ams_edit_overlay().handle_back(); }},
        {"ams_edit_choose_spoolman_cb",
         [](lv_event_t*) { get_ams_edit_overlay().switch_to_picker(); }},
        {"ams_edit_setup_entry_cb", [](lv_event_t*) { get_ams_edit_overlay().enter_editor(); }},
        {"ams_edit_quick_swatch_cb",
         [](lv_event_t* e) {
             get_ams_edit_overlay().handle_quick_swatch(lv_event_get_target_obj(e));
         }},
        {"ams_edit_custom_color_cb", [](lv_event_t*) { get_ams_edit_overlay().open_color_view(); }},
        {"ams_edit_reset_color_cb",
         [](lv_event_t*) { get_ams_edit_overlay().handle_reset_to_tag(); }},
        {"ams_edit_color_swatch_cb",
         [](lv_event_t* e) {
             get_ams_edit_overlay().handle_color_swatch(lv_event_get_target_obj(e));
         }},
        {"ams_edit_color_apply_cb",
         [](lv_event_t*) { get_ams_edit_overlay().handle_color_apply(); }},
        {"ams_edit_color_hex_changed_cb",
         [](lv_event_t*) { get_ams_edit_overlay().handle_color_hex_changed(); }},
        {"ams_edit_detail_field_changed_cb",
         [](lv_event_t*) { get_ams_edit_overlay().read_detail_fields(); }},
        {"ams_edit_save_cb", [](lv_event_t*) { get_ams_edit_overlay().handle_save(); }},
        {"ams_edit_more_toggle_cb",
         [](lv_event_t*) { get_ams_edit_overlay().handle_more_toggle(); }},
        {"ams_edit_weight_cb", [](lv_event_t*) { get_ams_edit_overlay().handle_weight_tap(); }},
        {"ams_edit_add_to_spoolman_cb",
         [](lv_event_t*) { get_ams_edit_overlay().handle_add_to_spoolman(); }},
        {"ams_edit_material_dd_cb",
         [](lv_event_t* e) {
             get_ams_edit_overlay().handle_option_selected(
                 0, helix::ui::event_user_int(e).value_or(-1));
         }},
        {"ams_edit_variant_dd_cb",
         [](lv_event_t* e) {
             get_ams_edit_overlay().handle_option_selected(
                 1, helix::ui::event_user_int(e).value_or(-1));
         }},
        {"ams_edit_brand_dd_cb",
         [](lv_event_t* e) {
             get_ams_edit_overlay().handle_option_selected(
                 2, helix::ui::event_user_int(e).value_or(-1));
         }},
        {"ams_edit_product_dd_cb",
         [](lv_event_t* e) {
             get_ams_edit_overlay().handle_option_selected(
                 3, helix::ui::event_user_int(e).value_or(-1));
         }},
        {"ams_edit_print_label_cb",
         [](lv_event_t*) {
#if HELIX_HAS_LABEL_PRINTER
             get_ams_edit_overlay().handle_print_label();
#endif
         }},
        {"ams_edit_scan_qr_cb", [](lv_event_t*) { get_ams_edit_overlay().handle_scan_qr(); }},
        {"ams_edit_picker_search_cb",
         [](lv_event_t* e) {
             const char* text = lv_textarea_get_text(lv_event_get_target_obj(e));
             get_ams_edit_overlay().picker_search_debounce_.schedule(text ? text : "");
         }},
        {"ams_edit_picker_retry_cb",
         [](lv_event_t*) {
             spdlog::info("[AmsEditOverlay] Picker retry requested by user");
             get_ams_edit_overlay().populate_picker();
         }},
        // Shared spool_item component callbacks.
        {"spoolman_spool_item_clicked_cb",
         [](lv_event_t* e) {
             lv_obj_t* item = lv_event_get_current_target_obj(e);
             auto spool_id =
                 static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(item)));
             if (spool_id <= 0) {
                 spdlog::warn("[AmsEditOverlay] Spool item clicked with invalid spool_id={}",
                              spool_id);
                 return;
             }
             get_ams_edit_overlay().handle_spool_selected(spool_id);
         }},
        {"spoolman_spool_item_edit_cb", [](lv_event_t*) { get_ams_edit_overlay().enter_editor(); }},
    });
}

} // namespace helix::ui
