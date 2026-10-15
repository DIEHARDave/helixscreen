// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl.h"

#include <string>

namespace helix::test {

/// A dependency-free component that fills its parent: the stand-in a layout
/// test registers in place of a real widget's XML.
inline constexpr const char* kFillParentComponent =
    "<component><view extends=\"lv_obj\" width=\"100%\" height=\"100%\"/></component>";

/// Registers `xml` under a REAL component name until the scope ends, then puts
/// the real ui_xml/components/<name>.xml back.
///
/// The component registry is process-wide, so a stub left registered under a
/// real name is what every later case in the same shard builds instead of the
/// real widget. When the name was unregistered before, the stub is dropped
/// and the first-use loader registers the real file on its next lookup.
class ScopedXmlComponentStub {
  public:
    explicit ScopedXmlComponentStub(std::string name, const char* xml = kFillParentComponent)
        : name_(std::move(name)),
          was_registered_(lv_xml_component_find_scope(name_.c_str()) != nullptr) {
        lv_xml_register_component_from_data(name_.c_str(), xml);
    }

    ~ScopedXmlComponentStub() {
        lv_xml_component_unregister(name_.c_str());
        if (was_registered_) {
            const std::string path = "A:ui_xml/components/" + name_ + ".xml";
            lv_xml_register_component_from_file(path.c_str());
        }
    }

    ScopedXmlComponentStub(const ScopedXmlComponentStub&) = delete;
    ScopedXmlComponentStub& operator=(const ScopedXmlComponentStub&) = delete;

  private:
    std::string name_;
    bool was_registered_;
};

} // namespace helix::test
