// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file queued_job_starter_test_access.h
 * @brief Friend shim for QueuedJobStarter's private members.
 *
 * Declared `friend class ::QueuedJobStarterTestAccess` in queued_job_start.h,
 * so it lives at global scope and is defined once for the whole test binary.
 */

#include "ui_print_preparation_manager.h"
#include "ui_print_start_controller.h"

#include "queued_job_start.h"

class QueuedJobStarterTestAccess {
  public:
    static helix::ui::PrintPreparationManager& prep(helix::queue::QueuedJobStarter& s) {
        return *s.prep_;
    }

    static helix::ui::PrintStartController& controller(helix::queue::QueuedJobStarter& s) {
        return *s.controller_;
    }
};
