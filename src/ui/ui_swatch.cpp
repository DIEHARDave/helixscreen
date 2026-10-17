// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_swatch.h"

#include "ams_types.h"
#include "color_utils.h"
#include "filament_variants.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_utils.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace helix::ui {

namespace {

constexpr float PI_F = 3.14159265358979f;

uint32_t rgb_of(lv_color_t c) {
    return lv_color_to_u32(c) & 0xFFFFFF;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

float clamped_radius(const lv_area_t& a, int32_t radius) {
    const float w = static_cast<float>(lv_area_get_width(&a));
    const float h = static_cast<float>(lv_area_get_height(&a));
    const float half_short = std::min(w, h) / 2.0f;
    if (radius == LV_RADIUS_CIRCLE) {
        return half_short;
    }
    return std::clamp(static_cast<float>(radius), 0.0f, half_short);
}

// Position along the top-left to bottom-right diagonal, projected onto it: 0
// at the top-left corner, 1 at the bottom-right. Its iso-lines are the band
// boundaries, and it is exactly the parameter of a linear gradient from
// (0, 0) to (100%, 100%), which is how the bands are drawn.
struct DiagField {
    float x0, y0, w, h, inv_len2;
    float operator()(const SwatchPoint& p) const {
        return ((p.x - x0) * w + (p.y - y0) * h) * inv_len2;
    }
};

DiagField diag_field(const lv_area_t& a) {
    // A w-pixel object spans [x1, x2 + 1) in sub-pixel coordinates.
    const float w = static_cast<float>(lv_area_get_width(&a));
    const float h = static_cast<float>(lv_area_get_height(&a));
    return {static_cast<float>(a.x1), static_cast<float>(a.y1), w, h, 1.0f / (w * w + h * h)};
}

// Sutherland-Hodgman clip of a convex polygon by one half-plane of @p f.
SwatchPolygon clip_halfplane(const SwatchPolygon& poly, const DiagField& f, float t, bool keep_ge) {
    SwatchPolygon out;
    if (poly.empty()) {
        return out;
    }
    out.reserve(poly.size() + 2);
    for (size_t i = 0; i < poly.size(); ++i) {
        const SwatchPoint& a = poly[i];
        const SwatchPoint& b = poly[(i + 1) % poly.size()];
        const float fa = f(a);
        const float fb = f(b);
        const bool a_in = keep_ge ? (fa >= t) : (fa <= t);
        const bool b_in = keep_ge ? (fb >= t) : (fb <= t);
        if (a_in) {
            out.push_back(a);
        }
        if (a_in != b_in) {
            const float denom = fb - fa;
            const float alpha = denom != 0.0f ? (t - fa) / denom : 0.0f;
            out.push_back({a.x + alpha * (b.x - a.x), a.y + alpha * (b.y - a.y)});
        }
    }
    return out;
}

// The diagonal-field threshold below which @p fraction of the shape's area lies.
float threshold_for_fraction(const SwatchPolygon& shape, const DiagField& f, float total,
                             float fraction) {
    float lo = 0.0f;
    float hi = 1.0f;
    for (int i = 0; i < 24; ++i) {
        const float mid = 0.5f * (lo + hi);
        const float a = swatch_polygon_area(clip_halfplane(shape, f, mid, false));
        if (a < fraction * total) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return 0.5f * (lo + hi);
}

// Small deterministic generator so a swatch's texture is stable across redraws.
struct Lcg {
    uint32_t state;
    float next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
    }
};

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

lv_opa_t scale_opa(lv_opa_t a, lv_opa_t b) {
    return static_cast<lv_opa_t>((static_cast<uint32_t>(a) * b) / 255);
}

void fill_shape(lv_layer_t* layer, const lv_area_t& area, int32_t radius, lv_color_t color,
                lv_opa_t opa) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = radius;
    dsc.bg_color = color;
    dsc.bg_opa = opa;
    dsc.border_width = 0;
    lv_draw_rect(layer, &dsc, &area);
}

// The bands of a multi-colour swatch as one rounded rect with a hard-stopped
// linear gradient: one draw, an anti-aliased rim, no seams between bands.
// LV_GRADIENT_MAX_STOPS holds two stops per colour for up to four colours;
// past that each colour gets one stop at its band's centre and they blend.
void fill_split(lv_layer_t* layer, const lv_area_t& area, int32_t radius,
                const std::vector<lv_color_t>& colors, lv_opa_t opa) {
    const size_t n = colors.size();
    const std::vector<float> t = swatch_split_thresholds(area, radius, n);
    auto frac = [](float v) {
        return static_cast<uint8_t>(std::clamp(std::lround(v * 255.0f), 0L, 255L));
    };
    lv_color_t stop_colors[LV_GRADIENT_MAX_STOPS];
    uint8_t stop_fracs[LV_GRADIENT_MAX_STOPS];
    int count = 0;
    if (2 * n <= LV_GRADIENT_MAX_STOPS) {
        for (size_t k = 0; k < n; ++k) {
            const uint8_t lo = k == 0 ? 0 : static_cast<uint8_t>(std::min(255, frac(t[k - 1]) + 1));
            const uint8_t hi = k + 1 == n ? 255 : frac(t[k]);
            stop_colors[count] = colors[k];
            stop_fracs[count++] = lo;
            stop_colors[count] = colors[k];
            stop_fracs[count++] = std::max(lo, hi);
        }
    } else {
        const size_t used = std::min<size_t>(n, LV_GRADIENT_MAX_STOPS);
        for (size_t k = 0; k < used; ++k) {
            const float lo = k == 0 ? 0.0f : t[k - 1];
            const float hi = k + 1 == n ? 1.0f : t[k];
            stop_colors[count] = colors[k];
            stop_fracs[count++] = frac(0.5f * (lo + hi));
        }
    }

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = radius;
    dsc.bg_opa = opa;
    dsc.border_width = 0;
    dsc.bg_color = colors[0];
    lv_grad_init_stops(&dsc.bg_grad, stop_colors, nullptr, stop_fracs, count);
    lv_grad_linear_init(&dsc.bg_grad, lv_pct(0), lv_pct(0), lv_pct(100), lv_pct(100),
                        LV_GRAD_EXTEND_PAD);
    lv_draw_rect(layer, &dsc, &area);
}

// Silk: a diagonal specular band across the shape, brightest just above the
// centre line, fading to nothing at both sides.
void draw_silk(lv_layer_t* layer, const lv_area_t& area, int32_t radius, lv_opa_t opa) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = radius;
    dsc.bg_opa = opa;
    dsc.border_width = 0;
    const lv_color_t white = lv_color_white();
    const lv_color_t colors[5] = {white, white, white, white, white};
    const lv_opa_t opas[5] = {0, 70, 170, 50, 0};
    const uint8_t fracs[5] = {60, 100, 120, 145, 190};
    lv_grad_init_stops(&dsc.bg_grad, colors, opas, fracs, 5);
    lv_grad_linear_init(&dsc.bg_grad, lv_pct(0), lv_pct(0), lv_pct(100), lv_pct(100),
                        LV_GRAD_EXTEND_PAD);
    lv_draw_rect(layer, &dsc, &area);
}

void draw_dot(lv_layer_t* layer, float x, float y, int32_t size, lv_color_t color, lv_opa_t opa) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = opa;
    dsc.radius = size > 1 ? LV_RADIUS_CIRCLE : 0;
    lv_area_t a;
    a.x1 = static_cast<int32_t>(std::lround(x)) - size / 2;
    a.y1 = static_cast<int32_t>(std::lround(y)) - size / 2;
    a.x2 = a.x1 + size - 1;
    a.y2 = a.y1 + size - 1;
    lv_draw_rect(layer, &dsc, &a);
}

int32_t short_side(const lv_area_t& a) {
    return std::min(lv_area_get_width(&a), lv_area_get_height(&a));
}

uint32_t area_seed(const lv_area_t& a) {
    return static_cast<uint32_t>(lv_area_get_width(&a)) * 7919u +
           static_cast<uint32_t>(lv_area_get_height(&a)) * 104729u;
}

// Marble: a fine speckle of darker and lighter flecks.
void draw_marble(lv_layer_t* layer, const lv_area_t& area, int32_t radius, lv_opa_t opa) {
    const int32_t s = short_side(area);
    const size_t count = static_cast<size_t>(std::clamp<int32_t>(s * s / 28, 8, 220));
    const int32_t fleck = s >= 48 ? 2 : 1;
    const auto points = swatch_texture_points(area, radius, count, area_seed(area));
    for (size_t i = 0; i < points.size(); ++i) {
        const bool dark = (i % 3) != 0;
        draw_dot(layer, points[i].x, points[i].y, fleck, dark ? lv_color_black() : lv_color_white(),
                 scale_opa(dark ? 90 : 120, opa));
    }
}

// Sparkle: a few small four-pointed glints.
void draw_sparkle(lv_layer_t* layer, const lv_area_t& area, int32_t radius, lv_opa_t opa) {
    const int32_t s = short_side(area);
    const size_t count = static_cast<size_t>(std::clamp<int32_t>(s * s / 160, 3, 24));
    const int32_t arm = std::max<int32_t>(1, s / 14);
    const auto points = swatch_texture_points(area, radius, count, area_seed(area) ^ 0x5A5Au);
    const lv_color_t white = lv_color_white();
    for (size_t i = 0; i < points.size(); ++i) {
        const float x = points[i].x;
        const float y = points[i].y;
        const bool big = (i % 3) == 0;
        draw_dot(layer, x, y, big ? 2 : 1, white, scale_opa(240, opa));
        if (big) {
            for (int32_t d = 1; d <= arm; ++d) {
                const lv_opa_t o = scale_opa(static_cast<lv_opa_t>(170 / d), opa);
                const float fd = static_cast<float>(d);
                draw_dot(layer, x + fd, y, 1, white, o);
                draw_dot(layer, x - fd, y, 1, white, o);
                draw_dot(layer, x, y + fd, 1, white, o);
                draw_dot(layer, x, y - fd, 1, white, o);
            }
        }
    }
}

void draw_edge(lv_layer_t* layer, const lv_area_t& area, int32_t radius, lv_opa_t opa) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = radius;
    dsc.bg_opa = LV_OPA_TRANSP;
    dsc.border_color = theme_manager_get_color("border");
    dsc.border_width = std::max<int32_t>(1, theme_manager_get_spacing("border_width"));
    dsc.border_opa = opa;
    lv_draw_rect(layer, &dsc, &area);
}

// ---------------------------------------------------------------------------
// Object paint
// ---------------------------------------------------------------------------

// Retained per-object paint, keyed by object. Main (UI) thread only: the
// setters, the draw hook and the DELETE hook all run there.
struct ObjectPaint {
    std::vector<lv_color_t> colors; ///< empty: paint the object's bg_color / bg_grad
    bool style_color = false;       ///< a colour style was given, so bg_color is meant
    SwatchFinish finish = SwatchFinish::Plain;
    bool edge = true;
};

std::unordered_map<const lv_obj_t*, ObjectPaint>& paint_registry() {
    static std::unordered_map<const lv_obj_t*, ObjectPaint> registry;
    return registry;
}

void draw_object_paint(lv_event_t* e) {
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto& registry = paint_registry();
    auto it = registry.find(obj);
    if (it == registry.end()) {
        return;
    }
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    if (lv_area_get_width(&coords) <= 0 || lv_area_get_height(&coords) <= 0) {
        return;
    }

    SwatchPaint paint;
    paint.radius = lv_obj_get_style_radius(obj, LV_PART_MAIN);
    paint.finish = it->second.finish;
    paint.edge = it->second.edge;
    paint.opa = lv_obj_get_style_opa_recursive(obj, LV_PART_MAIN);
    if (!it->second.colors.empty()) {
        paint.colors = it->second.colors;
    } else {
        const lv_grad_dsc_t* grad = lv_obj_get_style_bg_grad(obj, LV_PART_MAIN);
        if (grad && grad->stops_count > 0) {
            paint.gradient = grad;
        } else if (it->second.style_color) {
            paint.colors = {lv_obj_get_style_bg_color(obj, LV_PART_MAIN)};
        } else {
            return; // no colour yet: an empty slot, or one C++ has not painted
        }
    }
    draw_swatch(lv_event_get_layer(e), coords, paint);
}

// The edge is judged against the live theme at draw time, so a theme change
// only needs the swatch redrawn.
void theme_changed_cb(lv_observer_t* observer, lv_subject_t* /*subject*/) {
    lv_obj_invalidate(static_cast<lv_obj_t*>(lv_observer_get_target(observer)));
}

void on_paint_deleted(lv_event_t* e) {
    paint_registry().erase(static_cast<lv_obj_t*>(lv_event_get_current_target(e)));
}

ObjectPaint& ensure_paint(lv_obj_t* obj) {
    auto& registry = paint_registry();
    auto it = registry.find(obj);
    if (it != registry.end()) {
        return it->second;
    }
    // DECLARATIVE_OK: the swatch painter's own draw and teardown hooks. Painting
    // before the class draw lets a style border (a warning or selection mark)
    // land on top of the fill.
    lv_obj_add_event_cb(obj, draw_object_paint, LV_EVENT_DRAW_MAIN_BEGIN, nullptr);
    lv_obj_add_event_cb(obj, on_paint_deleted, LV_EVENT_DELETE, nullptr);
    // The painter draws the fill; the style's own background would cover it.
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    if (lv_subject_t* theme = theme_manager_get_changed_subject()) {
        lv_subject_add_observer_obj(theme, theme_changed_cb, obj, nullptr);
    }
    return registry[obj];
}

// ---------------------------------------------------------------------------
// <swatch_paint> XML widget
// ---------------------------------------------------------------------------

void colors_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    auto* obj = static_cast<lv_obj_t*>(lv_observer_get_target(observer));
    const char* csv = lv_subject_get_string(subject);
    std::vector<lv_color_t> colors = parse_multi_colors(csv ? csv : "");
    ObjectPaint& p = ensure_paint(obj);
    // One colour is the bg_color the color binding already paints.
    p.colors = colors.size() >= 2 ? std::move(colors) : std::vector<lv_color_t>{};
    lv_obj_invalidate(obj);
}

void finish_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    auto* obj = static_cast<lv_obj_t*>(lv_observer_get_target(observer));
    const char* name = lv_subject_get_string(subject);
    ensure_paint(obj).finish = swatch_finish_from_variant(name ? name : "");
    lv_obj_invalidate(obj);
}

// A <color_swatch> holder paints through its disc; anything else paints itself.
lv_obj_t* paint_target(lv_obj_t* obj) {
    if (paint_registry().count(obj) > 0) {
        return obj;
    }
    lv_obj_t* disc = lv_obj_find_by_name(obj, "color_swatch_disc");
    return disc ? disc : obj;
}

bool non_empty(const char* v) {
    return v && v[0] != '\0';
}

void* swatch_paint_xml_create(lv_xml_parser_state_t* state, const char** /*attrs*/) {
    auto* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    ensure_paint(obj);
    return obj;
}

void bind_string_subject(lv_xml_parser_state_t* state, lv_obj_t* obj, const char* name,
                         lv_observer_cb_t cb, const char* attr) {
    lv_subject_t* subject = lv_xml_get_subject(&state->scope, name);
    if (!subject) {
        spdlog::warn("[Swatch] {} subject '{}' does not exist", attr, name);
        return;
    }
    lv_subject_add_observer_obj(subject, cb, obj, nullptr);
}

void swatch_paint_xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    auto* obj = static_cast<lv_obj_t*>(lv_xml_state_get_item(state));
    lv_xml_obj_apply(state, attrs);
    // A style the attributes set must not bring the background back.
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    ObjectPaint& p = ensure_paint(obj);

    for (int i = 0; attrs[i]; i += 2) {
        const char* name = attrs[i];
        const char* value = attrs[i + 1];
        if (!non_empty(value)) {
            continue;
        }
        if (std::strcmp(name, "color") == 0) {
            lv_obj_set_style_bg_color(obj, lv_xml_to_color(value), LV_PART_MAIN);
            p.style_color = true;
        } else if (std::strcmp(name, "bind_color") == 0) {
            lv_subject_t* subject = lv_xml_get_subject(&state->scope, value);
            if (subject) {
                lv_obj_bind_style_prop(obj, LV_STYLE_BG_COLOR, LV_PART_MAIN, subject);
                p.style_color = true;
            } else {
                spdlog::warn("[Swatch] bind_color subject '{}' does not exist", value);
            }
        } else if (std::strcmp(name, "colors") == 0) {
            std::vector<lv_color_t> colors = parse_multi_colors(value);
            if (colors.size() >= 2) {
                p.colors = std::move(colors);
            } else if (colors.size() == 1) {
                lv_obj_set_style_bg_color(obj, colors[0], LV_PART_MAIN);
                p.style_color = true;
            }
        } else if (std::strcmp(name, "bind_colors") == 0) {
            bind_string_subject(state, obj, value, colors_observer_cb, "bind_colors");
        } else if (std::strcmp(name, "finish") == 0) {
            p.finish = swatch_finish_from_variant(value);
        } else if (std::strcmp(name, "bind_finish") == 0) {
            bind_string_subject(state, obj, value, finish_observer_cb, "bind_finish");
        } else if (std::strcmp(name, "edge") == 0) {
            p.edge = std::strcmp(value, "off") != 0 && std::strcmp(value, "false") != 0;
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Pure decisions
// ---------------------------------------------------------------------------

bool swatch_needs_edge(uint32_t rgb, uint32_t surface) {
    return helix::contrast_ratio(lv_color_hex(rgb), lv_color_hex(surface)) <
           SWATCH_EDGE_CONTRAST_RATIO;
}

bool swatch_needs_edge_here(uint32_t rgb) {
    // Swatches sit on panels (screen_bg) and in dialogs (elevated_bg).
    return swatch_needs_edge(rgb, rgb_of(theme_manager_get_color("screen_bg"))) ||
           swatch_needs_edge(rgb, rgb_of(theme_manager_get_color("elevated_bg")));
}

bool swatch_colors_need_edge(const std::vector<lv_color_t>& colors,
                             const std::vector<uint32_t>& surfaces) {
    for (lv_color_t c : colors) {
        for (uint32_t s : surfaces) {
            if (swatch_needs_edge(rgb_of(c), s)) {
                return true;
            }
        }
    }
    return false;
}

std::vector<lv_color_t> parse_multi_colors(const std::string& csv) {
    std::vector<lv_color_t> colors;
    size_t start = 0;
    while (start <= csv.size()) {
        const size_t comma = csv.find(',', start);
        const std::string token =
            csv.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        const size_t b = token.find_first_not_of(" \t");
        const size_t e = token.find_last_not_of(" \t");
        if (b != std::string::npos) {
            if (auto rgb = helix::parse_hex_color(token.substr(b, e - b + 1))) {
                colors.push_back(lv_color_hex(*rgb));
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return colors;
}

SwatchFinish swatch_finish(helix::FilamentFinish finish) {
    switch (finish) {
    case helix::FilamentFinish::Silk:
        return SwatchFinish::Silk;
    case helix::FilamentFinish::Matte:
        return SwatchFinish::Matte;
    case helix::FilamentFinish::Marble:
        return SwatchFinish::Marble;
    case helix::FilamentFinish::Sparkle:
        return SwatchFinish::Sparkle;
    case helix::FilamentFinish::None:
        break;
    }
    return SwatchFinish::Plain;
}

SwatchFinish swatch_finish_from_variant(std::string_view name) {
    return swatch_finish(helix::filament_finish_of(name));
}

SwatchFinish swatch_finish_of(const helix::SlotInfo& slot) {
    return swatch_finish(slot.finish());
}

SwatchFinish swatch_effective_finish(SwatchFinish finish, int32_t w, int32_t h) {
    // By area, so a long thin band or a spool's wound body keeps its finish
    // while a dot of the same short side does not.
    return w * h < SWATCH_FINISH_MIN_PX * SWATCH_FINISH_MIN_PX ? SwatchFinish::Plain : finish;
}

lv_color_t swatch_matte_color(lv_color_t c) {
    // Rec. 601 luma; a fifth of the way to grey keeps the hue readable.
    const float grey = 0.299f * c.red + 0.587f * c.green + 0.114f * c.blue;
    auto mix = [grey](uint8_t ch) {
        return static_cast<uint8_t>(std::lround(ch + (grey - ch) * 0.2f));
    };
    return lv_color_make(mix(c.red), mix(c.green), mix(c.blue));
}

SwatchPolygon swatch_shape_polygon(const lv_area_t& area, int32_t radius) {
    const float x0 = static_cast<float>(area.x1);
    const float y0 = static_cast<float>(area.y1);
    const float x1 = static_cast<float>(area.x2 + 1);
    const float y1 = static_cast<float>(area.y2 + 1);
    const float r = clamped_radius(area, radius);
    if (r < 0.5f) {
        return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    }
    // About one vertex per 1.5px of arc keeps the outline round at swatch sizes.
    const int seg = std::clamp(static_cast<int>(r * PI_F / 2.0f / 1.5f), 3, 24);
    SwatchPolygon poly;
    poly.reserve(static_cast<size_t>(4 * (seg + 1)));
    const SwatchPoint centres[4] = {
        {x1 - r, y0 + r}, // top-right: -90..0 degrees
        {x1 - r, y1 - r}, // bottom-right: 0..90
        {x0 + r, y1 - r}, // bottom-left: 90..180
        {x0 + r, y0 + r}, // top-left: 180..270
    };
    for (int c = 0; c < 4; ++c) {
        const float start = -PI_F / 2.0f + static_cast<float>(c) * PI_F / 2.0f;
        for (int i = 0; i <= seg; ++i) {
            const float t = start + (PI_F / 2.0f) * static_cast<float>(i) / static_cast<float>(seg);
            poly.push_back({centres[c].x + r * std::cos(t), centres[c].y + r * std::sin(t)});
        }
    }
    return poly;
}

bool swatch_shape_contains(const lv_area_t& area, int32_t radius, float x, float y) {
    const float x0 = static_cast<float>(area.x1);
    const float y0 = static_cast<float>(area.y1);
    const float x1 = static_cast<float>(area.x2 + 1);
    const float y1 = static_cast<float>(area.y2 + 1);
    if (x < x0 || x > x1 || y < y0 || y > y1) {
        return false;
    }
    const float r = clamped_radius(area, radius);
    const float cx = std::clamp(x, x0 + r, x1 - r);
    const float cy = std::clamp(y, y0 + r, y1 - r);
    const float dx = x - cx;
    const float dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

float swatch_polygon_area(const SwatchPolygon& poly) {
    float a = 0.0f;
    for (size_t i = 0; i < poly.size(); ++i) {
        const SwatchPoint& p = poly[i];
        const SwatchPoint& q = poly[(i + 1) % poly.size()];
        a += p.x * q.y - q.x * p.y;
    }
    return std::fabs(a) * 0.5f;
}

std::vector<float> swatch_split_thresholds(const lv_area_t& area, int32_t radius, size_t n) {
    std::vector<float> thresholds;
    if (n < 2 || lv_area_get_width(&area) <= 0 || lv_area_get_height(&area) <= 0) {
        return thresholds;
    }
    const SwatchPolygon shape = swatch_shape_polygon(area, radius);
    const DiagField f = diag_field(area);
    const float total = swatch_polygon_area(shape);
    thresholds.reserve(n - 1);
    for (size_t k = 1; k < n; ++k) {
        thresholds.push_back(
            threshold_for_fraction(shape, f, total, static_cast<float>(k) / static_cast<float>(n)));
    }
    return thresholds;
}

std::vector<SwatchPolygon> swatch_split_polygons(const lv_area_t& area, int32_t radius, size_t n) {
    const SwatchPolygon shape = swatch_shape_polygon(area, radius);
    if (n < 2) {
        return {shape};
    }
    const DiagField f = diag_field(area);
    const std::vector<float> thresholds = swatch_split_thresholds(area, radius, n);
    std::vector<SwatchPolygon> bands;
    bands.reserve(n);
    for (size_t k = 0; k < n; ++k) {
        SwatchPolygon band = shape;
        if (k > 0) {
            band = clip_halfplane(band, f, thresholds[k - 1], true);
        }
        if (k + 1 < n) {
            band = clip_halfplane(band, f, thresholds[k], false);
        }
        bands.push_back(std::move(band));
    }
    return bands;
}

std::vector<SwatchPoint> swatch_texture_points(const lv_area_t& area, int32_t radius, size_t count,
                                               uint32_t seed) {
    std::vector<SwatchPoint> points;
    points.reserve(count);
    const float x0 = static_cast<float>(area.x1);
    const float y0 = static_cast<float>(area.y1);
    const float w = static_cast<float>(lv_area_get_width(&area));
    const float h = static_cast<float>(lv_area_get_height(&area));
    if (w <= 0.0f || h <= 0.0f) {
        return points;
    }
    // Keep texture a pixel inside the rim, where the edge would cover it.
    lv_area_t inner = area;
    lv_area_increase(&inner, -1, -1);
    Lcg rng{seed ? seed : 1u};
    for (size_t tries = 0; points.size() < count && tries < count * 8; ++tries) {
        const float x = x0 + rng.next() * w;
        const float y = y0 + rng.next() * h;
        if (swatch_shape_contains(inner, radius, x, y)) {
            points.push_back({x, y});
        }
    }
    return points;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void draw_swatch(lv_layer_t* layer, const lv_area_t& area, const SwatchPaint& paint) {
    if (!layer || paint.opa <= LV_OPA_MIN || lv_area_get_width(&area) <= 0 ||
        lv_area_get_height(&area) <= 0) {
        return;
    }
    if (!paint.gradient && paint.colors.empty()) {
        return;
    }
    const SwatchFinish finish =
        swatch_effective_finish(paint.finish, lv_area_get_width(&area), lv_area_get_height(&area));
    std::vector<lv_color_t> colors = paint.colors;
    if (finish == SwatchFinish::Matte) {
        for (auto& c : colors) {
            c = swatch_matte_color(c);
        }
    }

    if (paint.gradient) {
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.radius = paint.radius;
        dsc.bg_opa = paint.opa;
        dsc.bg_grad = *paint.gradient;
        dsc.bg_color = paint.gradient->stops[0].color;
        lv_draw_rect(layer, &dsc, &area);
    } else {
        if (colors.size() < 2) {
            fill_shape(layer, area, paint.radius, colors[0], paint.opa);
        } else {
            fill_split(layer, area, paint.radius, colors, paint.opa);
        }
    }

    draw_swatch_finish(layer, area, paint.radius, finish, paint.opa);

    if (paint.edge && !paint.gradient &&
        swatch_colors_need_edge(colors, {rgb_of(theme_manager_get_color("screen_bg")),
                                         rgb_of(theme_manager_get_color("elevated_bg"))})) {
        draw_edge(layer, area, paint.radius, paint.opa);
    }
}

void draw_swatch_finish(lv_layer_t* layer, const lv_area_t& area, int32_t radius,
                        SwatchFinish finish, lv_opa_t opa) {
    switch (swatch_effective_finish(finish, lv_area_get_width(&area), lv_area_get_height(&area))) {
    case SwatchFinish::Silk:
        draw_silk(layer, area, radius, opa);
        break;
    case SwatchFinish::Marble:
        draw_marble(layer, area, radius, opa);
        break;
    case SwatchFinish::Sparkle:
        draw_sparkle(layer, area, radius, opa);
        break;
    case SwatchFinish::Matte:
    case SwatchFinish::Plain:
        break;
    }
}

SwatchPaint swatch_paint_for(uint32_t rgb, const std::string& multi_color_hexes,
                             SwatchFinish finish) {
    SwatchPaint paint;
    paint.colors = parse_multi_colors(multi_color_hexes);
    if (paint.colors.size() < 2) {
        paint.colors = {lv_color_hex(rgb)};
    }
    paint.finish = finish;
    return paint;
}

SwatchPaint swatch_paint_for(const helix::SlotInfo& slot) {
    return swatch_paint_for(slot.color_rgb, slot.multi_color_hexes, swatch_finish_of(slot));
}

// ---------------------------------------------------------------------------
// Object paint
// ---------------------------------------------------------------------------

void apply_swatch_color(lv_obj_t* swatch, uint32_t primary_rgb,
                        const std::string& multi_color_hexes, SwatchFinish finish) {
    if (!swatch) {
        return;
    }
    swatch = paint_target(swatch);
    ObjectPaint& p = ensure_paint(swatch);
    std::vector<lv_color_t> colors = parse_multi_colors(multi_color_hexes);
    p.colors =
        colors.size() >= 2 ? std::move(colors) : std::vector<lv_color_t>{lv_color_hex(primary_rgb)};
    p.finish = finish;
    // Click handlers read a swatch's colour back from its bg_color.
    lv_obj_set_style_bg_color(swatch, lv_color_hex(primary_rgb), LV_PART_MAIN);
    lv_obj_invalidate(swatch);
}

void apply_swatch_color(lv_obj_t* swatch, const helix::SlotInfo& slot) {
    apply_swatch_color(swatch, slot.color_rgb, slot.multi_color_hexes, swatch_finish_of(slot));
}

void attach_swatch_paint(lv_obj_t* obj) {
    if (obj) {
        ensure_paint(obj).style_color = true;
    }
}

void clear_swatch_color(lv_obj_t* swatch) {
    if (!swatch) {
        return;
    }
    swatch = paint_target(swatch);
    ObjectPaint& p = ensure_paint(swatch);
    p.colors.clear();
    p.style_color = false;
    lv_obj_invalidate(swatch);
}

bool has_swatch_paint(const lv_obj_t* obj) {
    return paint_registry().count(obj) > 0;
}

std::vector<lv_color_t> swatch_paint_colors(const lv_obj_t* obj) {
    auto& registry = paint_registry();
    auto it = registry.find(obj);
    return it == registry.end() ? std::vector<lv_color_t>{} : it->second.colors;
}

SwatchFinish swatch_paint_finish(const lv_obj_t* obj) {
    auto& registry = paint_registry();
    auto it = registry.find(obj);
    return it == registry.end() ? SwatchFinish::Plain : it->second.finish;
}

lv_color_t swatch_color_of(lv_obj_t* obj) {
    if (!obj) {
        return lv_color_black();
    }
    lv_obj_t* target = paint_target(obj);
    const std::vector<lv_color_t> colors = swatch_paint_colors(target);
    if (colors.size() == 1) {
        return colors[0];
    }
    return lv_obj_get_style_bg_color(target, LV_PART_MAIN);
}

void register_swatch_paint_widget() {
    lv_xml_register_widget("swatch_paint", swatch_paint_xml_create, swatch_paint_xml_apply);
}

void reset_swatch_state() {
    paint_registry().clear();
}

} // namespace helix::ui
