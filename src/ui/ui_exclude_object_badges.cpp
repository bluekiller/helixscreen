// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_exclude_object_badges.h"

#include "display_numbering.h"
#include "gcode_parser.h"
#include "printer_excluded_objects_state.h"
#include "theme_manager.h"

#include <glm/geometric.hpp>

namespace helix::ui {

std::vector<ObjectBadge> compute_object_badges(const PrinterExcludedObjectsState& state,
                                               const gcode::ParsedGCodeFile* parsed) {
    const auto& defined = state.get_defined_objects();
    const auto& excluded = state.get_excluded_objects();
    const auto& current = state.get_current_object();

    std::vector<ObjectBadge> badges;
    badges.reserve(defined.size());
    for (int i = 0; i < static_cast<int>(defined.size()); ++i) {
        ObjectBadge b;
        b.defined_index = i;
        b.name = defined[static_cast<size_t>(i)];
        b.number = lane_number_text(i);
        b.excluded = excluded.count(b.name) > 0;
        b.current = !current.empty() && b.name == current;

        const auto info = state.get_object_geometry(b.name);
        const gcode::GCodeObject* pobj = nullptr;
        if (parsed) {
            auto it = parsed->objects.find(b.name);
            if (it != parsed->objects.end()) {
                pobj = &it->second;
            }
        }
        const bool parsed_bbox = pobj && !pobj->bounding_box.is_empty();
        if (parsed_bbox) {
            b.top_z = pobj->bounding_box.max.z;
        }

        // The parser has no "CENTER present" flag; an unset CENTER reads (0,0),
        // so the origin falls through to the bbox (which sits there too if real).
        const bool parsed_center = pobj && pobj->center != glm::vec2(0.0f);

        if (info && info->has_center) {
            b.anchor = info->center;
        } else if (parsed_center) {
            b.anchor = pobj->center;
        } else if (info && info->has_bbox) {
            b.anchor = (info->bbox_min + info->bbox_max) * 0.5f;
        } else if (parsed_bbox) {
            const glm::vec3 c = pobj->bounding_box.center();
            b.anchor = {c.x, c.y};
        } else {
            badges.push_back(std::move(b));
            continue;
        }
        b.has_anchor = true;
        badges.push_back(std::move(b));
    }
    return badges;
}

lv_color_t object_badge_color(int defined_index) {
    return theme_manager_get_object_palette_color(defined_index);
}

lv_color_t object_badge_text_color(lv_color_t fill) {
    return theme_manager_get_contrast_adjusted_text(theme_manager_get_color("text"), fill);
}

const lv_font_t* object_badge_font() {
    return theme_manager_get_font("font_small");
}

int32_t object_badge_diameter() {
    const lv_font_t* font = object_badge_font();
    return font ? lv_font_get_line_height(font) : 0;
}

void draw_object_badge(lv_layer_t* layer, const ObjectBadge& badge, int32_t cx, int32_t cy) {
    const int32_t d = object_badge_diameter();
    const int32_t r = d / 2;
    const lv_area_t area = {cx - r, cy - r, cx - r + d - 1, cy - r + d - 1};
    const lv_color_t fill = object_badge_color(badge.defined_index);
    const lv_opa_t opa = object_badge_opa(badge.excluded);

    lv_draw_rect_dsc_t disc;
    lv_draw_rect_dsc_init(&disc);
    disc.bg_color = fill;
    disc.bg_opa = opa;
    disc.radius = LV_RADIUS_CIRCLE;
    lv_draw_rect(layer, &disc, &area);

    lv_draw_label_dsc_t label;
    lv_draw_label_dsc_init(&label);
    label.font = object_badge_font();
    label.color = object_badge_text_color(fill);
    label.opa = opa;
    label.align = LV_TEXT_ALIGN_CENTER;
    label.text = badge.number.c_str();
    label.text_local = 1;
    // Vertically centre one line inside the disc.
    lv_area_t text_area = area;
    const int32_t line_h = label.font ? lv_font_get_line_height(label.font) : d;
    text_area.y1 = cy - line_h / 2;
    text_area.y2 = text_area.y1 + line_h - 1;
    lv_draw_label(layer, &label, &text_area);
}

int badge_hit_index(const std::vector<glm::vec2>& centers, float x, float y, float radius) {
    for (int i = 0; i < static_cast<int>(centers.size()); ++i) {
        if (glm::distance(centers[static_cast<size_t>(i)], glm::vec2(x, y)) <= radius) {
            return i;
        }
    }
    return -1;
}

} // namespace helix::ui
