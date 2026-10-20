// tests/test_helpers/print_select_detail_view_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_print_select_detail_view.h"

#include <string>

#include "../catch_amalgamated.hpp"

// Test seam into PrintSelectDetailView, befriended by it.
class PrintSelectDetailViewTestAccess {
  public:
    static void begin_viewer_load(helix::ui::PrintSelectDetailView& view, const std::string& path) {
        view.begin_viewer_load(path);
    }
    static lv_obj_t* gcode_viewer(const helix::ui::PrintSelectDetailView& view) {
        return view.gcode_viewer_;
    }
    /// The metadata fetch's answer for the file whose current_file_key() is @p key.
    static void apply_file_metadata(helix::ui::PrintSelectDetailView& view, const std::string& key,
                                    const FileMetadata& metadata) {
        view.apply_file_metadata(key, metadata);
    }
    /// What the plugin card's Set up tap and the remap help's Install button run.
    static void request_plugin_setup(helix::ui::PrintSelectDetailView& view) {
        REQUIRE(view.on_plugin_setup_requested_);
        view.on_plugin_setup_requested_();
    }
};
