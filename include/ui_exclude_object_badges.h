// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <glm/vec2.hpp>
#include <lvgl.h>
#include <optional>
#include <string>
#include <vector>

namespace helix {
class PrinterExcludedObjectsState;
namespace gcode {
struct ParsedGCodeFile;
}
} // namespace helix

namespace helix::ui {

/**
 * @brief What one object's numbered badge says, and where it belongs.
 *
 * Every surface that identifies an object by number - the side list chips, the
 * thumbnail map, the badges drawn on the 2D/3D render - keys on defined_index,
 * the object's position in PrinterExcludedObjectsState::get_defined_objects().
 * Number and colour are both derived from it, so the same object reads the
 * same everywhere.
 */
struct ObjectBadge {
    int defined_index = -1; ///< Position in the defined list; also the palette index
    std::string name;
    std::string number; ///< lane_number_text(defined_index)
    bool excluded = false;
    bool current = false;
    bool has_anchor = false;
    glm::vec2 anchor{0.0f, 0.0f}; ///< World XY (mm) the badge sits over
    std::optional<float> top_z;   ///< Top of the object's toolpath, when the file was parsed
};

/**
 * @brief One badge per defined object, in defined order.
 *
 * Anchor priority: Klipper CENTER, the parsed file's CENTER, Klipper's bbox
 * centre, the parsed toolpath bbox centre. An object with none of these keeps
 * its number and colour but has_anchor = false. Parsed objects are looked up by
 * name; their map order is alphabetical and never decides a number.
 *
 * Pure: reads both inputs, touches no LVGL state.
 */
std::vector<ObjectBadge> compute_object_badges(const PrinterExcludedObjectsState& state,
                                               const gcode::ParsedGCodeFile* parsed);

// Badge styling, shared by the map view's widgets and the viewer's draw pass.

/// Fill colour for the object at @p defined_index (same as its side-list chip).
lv_color_t object_badge_color(int defined_index);
/// Number colour on a @p fill disc.
lv_color_t object_badge_text_color(lv_color_t fill);
const lv_font_t* object_badge_font();
/// Disc diameter: one line of object_badge_font(), so it scales with the size class.
int32_t object_badge_diameter();
/// Excluded objects fade their badge like the map view fades their rect.
inline lv_opa_t object_badge_opa(bool excluded) {
    return excluded ? LV_OPA_30 : LV_OPA_COVER;
}

/// Draw @p badge as a numbered disc centred on (@p cx, @p cy) in screen coordinates.
void draw_object_badge(lv_layer_t* layer, const ObjectBadge& badge, int32_t cx, int32_t cy);

/// Index into @p centers of the disc containing (@p x, @p y), or -1.
int badge_hit_index(const std::vector<glm::vec2>& centers, float x, float y, float radius);

} // namespace helix::ui
