// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "home_layout_sync.h"

#include "app_globals.h"
#include "config.h"
#include "helix_version.h"
#include "i_moonraker_api.h"
#include "json_utils.h"
#include "moonraker_error.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include <ctime>
#include <utility>

namespace helix {

namespace {

constexpr const char* kHomePanelId = "home";

std::string active_printer_id() {
    Config* config = Config::get_instance();
    return config ? config->get_active_printer_id() : std::string();
}

/// Identity of a payload for "did this change": everything but the save time.
std::size_t payload_hash(nlohmann::json payload) {
    payload.erase("saved_at");
    return std::hash<std::string>{}(json_util::safe_dump(payload));
}

} // namespace

HomeLayoutSync& HomeLayoutSync::instance() {
    static HomeLayoutSync sync;
    return sync;
}

void HomeLayoutSync::init(IMoonrakerAPI& api) {
    api_ = &api;
    PanelWidgetConfig::set_save_listener(
        [](const PanelWidgetConfig& config) { HomeLayoutSync::instance().publish(config); });
}

void HomeLayoutSync::shutdown() {
    PanelWidgetConfig::set_save_listener(nullptr);
    lifetime_.invalidate();
    api_ = nullptr;
    held_.reset();
    last_posted_hash_.reset();
    ++connection_generation_;
}

void HomeLayoutSync::on_connected() {
    switch (get_printer_state().moonraker_locality()) {
    case MoonrakerLocality::Local:
        publish(PanelWidgetManager::instance().get_widget_config(kHomePanelId));
        break;
    case MoonrakerLocality::Remote: {
        const std::string printer_id = active_printer_id();
        request([this,
                 printer_id](std::optional<Layout> layout) { hold(printer_id, std::move(layout)); },
                nullptr);
        break;
    }
    case MoonrakerLocality::Unknown:
        break;
    }
}

void HomeLayoutSync::on_disconnected() {
    held_.reset();
    last_posted_hash_.reset();
    ++connection_generation_;
}

std::optional<HomeLayoutSync::Layout> HomeLayoutSync::take_seed(const std::string& printer_id) {
    if (!held_ || held_->printer_id != printer_id) {
        return std::nullopt;
    }
    std::optional<Layout> out = std::move(held_->layout);
    held_.reset();
    return out;
}

void HomeLayoutSync::fetch_now(std::function<void(std::optional<Layout>)> done) {
    if (!api_) {
        if (done) {
            done(std::nullopt);
        }
        return;
    }
    request(
        [done](std::optional<Layout> layout) {
            if (done) {
                done(std::move(layout));
            }
        },
        [done]() {
            if (done) {
                done(std::nullopt);
            }
        });
}

void HomeLayoutSync::set_seed_listener(std::function<void(const std::string&)> fn) {
    seed_listener_ = std::move(fn);
}

void HomeLayoutSync::publish(const PanelWidgetConfig& config) {
    if (!api_ || config.panel_id() != kHomePanelId ||
        get_printer_state().moonraker_locality() != MoonrakerLocality::Local) {
        return;
    }
    if (config.grid_signature().empty()) {
        spdlog::debug("[HomeLayoutSync] Home layout has no grid yet; not publishing");
        return;
    }

    nlohmann::json payload =
        home_layout::build_payload(config.published_pages(), config.grid_signature(),
                                   helix_version(), static_cast<int64_t>(std::time(nullptr)));
    const std::size_t hash = payload_hash(payload);
    if (last_posted_hash_ == hash) {
        return;
    }
    last_posted_hash_ = hash;

    spdlog::debug("[HomeLayoutSync] Publishing home layout ({})", config.grid_signature());
    api_->database_post_item(
        home_layout::kNamespace, home_layout::kKey, payload,
        []() { spdlog::debug("[HomeLayoutSync] Home layout published"); },
        lifetime_.bg_cb("HomeLayoutSync::publish_failed", [this, hash](const MoonrakerError& err) {
            spdlog::warn("[HomeLayoutSync] Publishing the home layout failed: {}", err.message);
            // The next save retries instead of matching a payload that never landed.
            if (last_posted_hash_ == hash) {
                last_posted_hash_.reset();
            }
        }));
}

void HomeLayoutSync::request(ReplyHandler on_reply, std::function<void()> on_stale) {
    if (!api_) {
        return;
    }
    const std::string printer_id = active_printer_id();
    const uint64_t generation = connection_generation_;

    // Main thread: the reply is for this connection and this printer, or it is
    // stale and only on_stale hears about it.
    auto deliver = [this, printer_id, generation, on_reply = std::move(on_reply),
                    on_stale = std::move(on_stale)](std::optional<Layout> layout) {
        if (generation != connection_generation_ || printer_id != active_printer_id()) {
            spdlog::debug("[HomeLayoutSync] Dropping a home layout reply for '{}': the "
                          "connection or active printer changed",
                          printer_id);
            if (on_stale) {
                on_stale();
            }
            return;
        }
        if (on_reply) {
            on_reply(std::move(layout));
        }
    };

    auto tok = lifetime_.token();
    api_->database_get_item(
        home_layout::kNamespace, home_layout::kKey,
        [tok, deliver](const nlohmann::json& value) mutable {
            // Callback thread: parse into a local, then hand it to the main thread.
            std::optional<Layout> layout = home_layout::parse_payload(value);
            if (!layout) {
                spdlog::debug("[HomeLayoutSync] Published home layout is unreadable; ignoring");
            }
            tok.defer("HomeLayoutSync::reply", [deliver, layout = std::move(layout)]() mutable {
                deliver(std::move(layout));
            });
        },
        [tok, deliver](const MoonrakerError& err) mutable {
            // A missing key is the normal "nothing published" answer.
            spdlog::debug("[HomeLayoutSync] No published home layout ({})", err.message);
            tok.defer("HomeLayoutSync::reply_error",
                      [deliver]() mutable { deliver(std::nullopt); });
        });
}

void HomeLayoutSync::hold(const std::string& printer_id, std::optional<Layout> layout) {
    if (!layout) {
        return;
    }
    spdlog::info("[HomeLayoutSync] Holding the printer's home layout ({}x{}) for '{}'",
                 layout->cols, layout->rows, printer_id);
    held_ = HeldSeed{printer_id, std::move(*layout)};
    if (seed_listener_) {
        seed_listener_(printer_id);
    }
}

} // namespace helix
