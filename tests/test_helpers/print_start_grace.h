// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app_constants.h"

#include <thread>

namespace helix::test {

/// PrintStartController::initiate() refuses every start in the first moments
/// of the process. A test that drives initiate() waits that window out first,
/// or a shard that reaches it early sees a refusal no code under test made.
inline void wait_out_print_start_grace() {
    std::this_thread::sleep_until(app_constants::startup::PROCESS_START_TIME +
                                  app_constants::startup::PRINT_START_GRACE_PERIOD);
}

} // namespace helix::test
