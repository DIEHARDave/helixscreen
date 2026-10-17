// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mount_feed_sequence.h"

#include "macro_param_cache.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <utility>

namespace helix::mount_feed {

namespace {

constexpr std::string_view kContinueMarker = "HELIX_MOUNT_FEED_CONTINUE";
constexpr std::string_view kCancelMarker = "HELIX_MOUNT_FEED_CANCEL";

/// Text safe inside a RESPOND MSG="..." and an action-prompt field: a double
/// quote ends the MSG, `;` starts a gcode comment, `|` splits a button's fields.
std::string respond_safe(std::string text) {
    for (char& c : text) {
        if (c == '"') {
            c = '\'';
        } else if (c == ';') {
            c = ',';
        } else if (c == '|') {
            c = '/';
        } else if (c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    return text;
}

std::string respond_command(std::string_view message) {
    return fmt::format("RESPOND TYPE=command MSG=\"{}\"", message);
}

std::string heat_gcode(int temp_c) {
    return fmt::format("M109 S{}", temp_c);
}

std::string with_heater_off(std::string gcode) {
    return gcode + "\nM104 S0";
}

} // namespace

std::vector<Step> steps(bool load) {
    if (load) {
        return {Step::Mount, Step::Heat, Step::Feed, Step::Purge, Step::Restore};
    }
    return {Step::Mount, Step::Heat, Step::Retract, Step::Release, Step::Restore};
}

const char* step_label(Step step) {
    switch (step) {
    case Step::Mount:
        return "Mount tool";
    case Step::Heat:
        return "Heat nozzle";
    case Step::Feed:
        return "Feed filament";
    case Step::Purge:
        return "Purge";
    case Step::Retract:
        return "Retract filament";
    case Step::Release:
        return "Release filament";
    case Step::Restore:
        return "Restore tool";
    }
    return "";
}

AmsAction step_action(Step step) {
    switch (step) {
    case Step::Mount:
    case Step::Restore:
        return AmsAction::SELECTING;
    case Step::Heat:
        return AmsAction::HEATING;
    case Step::Feed:
        return AmsAction::LOADING;
    case Step::Purge:
        return AmsAction::PURGING;
    case Step::Retract:
    case Step::Release:
        return AmsAction::UNLOADING;
    }
    return AmsAction::LOADING;
}

std::string prompt_gcode(const Request& r) {
    std::string out;
    out += respond_command("action:prompt_begin " + respond_safe(r.prompt_title)) + "\n";
    out += respond_command("action:prompt_text " + respond_safe(r.prompt_text)) + "\n";
    out += respond_command(fmt::format("action:prompt_footer_button {}|RESPOND TYPE=command "
                                       "MSG={}|error",
                                       respond_safe(r.cancel_label), kCancelMarker)) +
           "\n";
    out += respond_command(fmt::format("action:prompt_footer_button {}|RESPOND TYPE=command "
                                       "MSG={}|primary",
                                       respond_safe(r.continue_label), kContinueMarker)) +
           "\n";
    out += respond_command("action:prompt_show");
    return out;
}

std::string prompt_end_gcode() {
    return respond_command("action:prompt_end");
}

std::vector<Stage> plan(const Request& r) {
    std::vector<Stage> stages;
    if (!r.mount_gcode.empty()) {
        stages.push_back({Step::Mount, r.mount_gcode});
    }
    stages.push_back({Step::Heat, heat_gcode(r.temp_c)});
    if (r.load) {
        stages.push_back({Step::Feed, prompt_gcode(r), /*awaits_operator=*/true});
        // Draw the strand in slowly, purge the old color, retract off the melt.
        const std::string feed = r.macro.empty()
                                     ? "M83\nG1 E5 F60\nG1 E50 F300\nG1 E50 F300\nG1 E-10 F1800"
                                     : fmt::format("{} T={} TEMP={}", r.macro, r.head, r.temp_c);
        stages.push_back({Step::Purge, prompt_end_gcode() + "\n" + with_heater_off(feed)});
    } else {
        const std::string retract = r.macro.empty() ? "M83\nG1 E-25 F1800\nG1 E-150 F1200"
                                                    : fmt::format("{} T={}", r.macro, r.head);
        stages.push_back({Step::Retract, with_heater_off(retract)});
        if (!r.release_gcode.empty()) {
            stages.push_back({Step::Release, r.release_gcode});
        }
    }
    if (!r.restore_gcode.empty()) {
        stages.push_back({Step::Restore, r.restore_gcode});
    }
    return stages;
}

bool may_be_reply(std::string_view line) {
    return line.find("HELIX_MOUNT_FEED_") != std::string_view::npos;
}

Reply parse_reply(std::string_view line) {
    // The console echo of `RESPOND TYPE=command MSG=<marker>` is "// <marker>".
    // Exact match only: the prompt's own button lines carry the marker text too.
    auto trim = [](std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
            s.remove_prefix(1);
        }
        while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n')) {
            s.remove_suffix(1);
        }
        return s;
    };
    line = trim(line);
    if (line.substr(0, 2) == "//") {
        line = trim(line.substr(2));
    }
    if (line == kContinueMarker) {
        return Reply::Continue;
    }
    if (line == kCancelMarker) {
        return Reply::Cancel;
    }
    return Reply::None;
}

bool macro_takes_tool(const CachedMacroInfo& macro) {
    if (macro.knowledge != MacroParamKnowledge::KNOWN_PARAMS) {
        return false;
    }
    return std::any_of(macro.params.begin(), macro.params.end(),
                       [](const MacroParam& p) { return p.name == "T"; });
}

Run::Run(Request request) : request_(std::move(request)), stages_(plan(request_)) {}

std::optional<std::string> Run::current_gcode() const {
    if (finished() || awaiting_operator_) {
        return std::nullopt;
    }
    return stages_[index_].gcode;
}

bool Run::finished() const {
    return index_ >= stages_.size();
}

Step Run::current_step() const {
    return finished() ? Step::Restore : stages_[index_].step;
}

int Run::step_index() const {
    const auto all = steps(request_.load);
    const auto it = std::find(all.begin(), all.end(), current_step());
    return it == all.end() ? -1 : static_cast<int>(it - all.begin());
}

void Run::stage_done() {
    if (finished() || awaiting_operator_) {
        return;
    }
    if (stages_[index_].awaits_operator && !cleaning_up_) {
        awaiting_operator_ = true;
        return;
    }
    ++index_;
}

void Run::operator_reply(Reply reply) {
    if (!awaiting_operator_ || reply == Reply::None) {
        return;
    }
    awaiting_operator_ = false;
    if (reply == Reply::Continue) {
        ++index_;
        return;
    }
    abort();
}

void Run::abort() {
    if (cleaning_up_ || finished()) {
        index_ = stages_.size();
        return;
    }
    // Heater off and the carriage back where it was. A prompt still open is
    // closed with it.
    std::string cleanup;
    if (awaiting_operator_ || current_step() == Step::Feed) {
        cleanup = prompt_end_gcode() + "\n";
    }
    cleanup += "M104 S0";
    if (!request_.restore_gcode.empty()) {
        cleanup += "\n" + request_.restore_gcode;
    }
    awaiting_operator_ = false;
    cleaning_up_ = true;
    stages_ = {Stage{Step::Restore, std::move(cleanup)}};
    index_ = 0;
}

} // namespace helix::mount_feed
