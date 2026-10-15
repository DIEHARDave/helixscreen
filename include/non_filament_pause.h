// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <utility>

namespace helix::non_filament_pause {

/// Answers "is the current pause explained by something other than filament?"
/// A subsystem that recognises its own pauses (print-defect detection)
/// installs one; the runout surfaces ask before they read the sensors, so one
/// pause gets one explanation. Header-only so every build that carries a
/// runout surface links, whether or not it carries a subsystem that installs
/// an explainer. Main thread only.
using Explainer = std::function<bool()>;

inline Explainer& explainer_slot() {
    static Explainer explainer;
    return explainer;
}

/// Install (or, with an empty function, remove) the explainer.
inline void set_explainer(Explainer explainer) {
    explainer_slot() = std::move(explainer);
}

/// True when the installed explainer claims the current pause. With none
/// installed every pause is open to the runout surfaces.
inline bool explained() {
    const Explainer& explainer = explainer_slot();
    return explainer && explainer();
}

} // namespace helix::non_filament_pause
