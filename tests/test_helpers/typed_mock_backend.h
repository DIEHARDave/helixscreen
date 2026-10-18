// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_backend_mock.h"

namespace helix::test {

/// An AmsBackendMock that reports the AmsType the test names, for code that keys
/// on which kind of filament system is registered rather than on its behaviour.
class TypedMockBackend : public AmsBackendMock {
  public:
    explicit TypedMockBackend(AmsType type, bool with_units = true)
        : AmsBackendMock(4), type_(type), with_units_(with_units) {}

    /// With @p with_units false the backend reads like one registered from a
    /// firmware object before any unit has reported.
    [[nodiscard]] AmsSystemInfo get_system_info() const override {
        AmsSystemInfo info = AmsBackendMock::get_system_info();
        if (!with_units_) {
            info.units.clear();
            info.total_slots = 0;
        }
        return info;
    }

    [[nodiscard]] AmsType get_type() const override {
        return type_;
    }

  private:
    AmsType type_;
    bool with_units_;
};

} // namespace helix::test
