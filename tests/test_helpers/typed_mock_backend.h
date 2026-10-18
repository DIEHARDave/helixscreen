// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_backend_mock.h"

namespace helix::test {

/// An AmsBackendMock that reports the AmsType the test names, for code that keys
/// on which kind of filament system is registered rather than on its behaviour.
class TypedMockBackend : public AmsBackendMock {
  public:
    explicit TypedMockBackend(AmsType type) : AmsBackendMock(4), type_(type) {}

    [[nodiscard]] AmsType get_type() const override {
        return type_;
    }

  private:
    AmsType type_;
};

} // namespace helix::test
