// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nozzle_renderer_common.h"

namespace helix {

void nr_draw_tinted_tip(lv_layer_t* layer, int32_t cx, int32_t top_y, int32_t top_width,
                        int32_t bottom_width, int32_t height, lv_color_t left, lv_color_t right,
                        std::optional<lv_color_t> filament, lv_opa_t opa, int32_t glint_right) {
    if (filament) {
        lv_color_t tint = nr_dim(*filament, opa);
        left = nr_blend(left, tint, 0.4f);
        right = nr_blend(right, tint, 0.4f);
    }
    nr_draw_nozzle_tip(layer, cx, top_y, top_width, bottom_width, height, left, right);

    lv_draw_fill_dsc_t fill_dsc;
    lv_draw_fill_dsc_init(&fill_dsc);
    fill_dsc.color = lv_color_hex(0xFFFFFF);
    fill_dsc.opa = LV_OPA_70;
    int32_t bottom = top_y + height;
    lv_area_t glint = {cx - 1, bottom - 1, cx + glint_right, bottom};
    lv_draw_fill(layer, &fill_dsc, &glint);
}

void nr_draw_filament_tip(lv_layer_t* layer, int32_t cx, int32_t top_y, int32_t top_width,
                          int32_t bottom_width, int32_t height, std::optional<lv_color_t> filament,
                          lv_opa_t opa) {
    lv_color_t tip_left, tip_right;
    if (filament) {
        lv_color_t tip = nr_dim(*filament, opa);
        tip_left = nr_lighten(tip, 30);
        tip_right = nr_darken(tip, 20);
    } else {
        lv_color_t metal = nr_dim(lv_color_hex(0x3A3A3A), opa);
        tip_left = nr_lighten(metal, 30);
        tip_right = nr_darken(metal, 10);
    }
    nr_draw_nozzle_tip(layer, cx, top_y, top_width, bottom_width, height, tip_left, tip_right);

    lv_draw_fill_dsc_t glint_dsc;
    lv_draw_fill_dsc_init(&glint_dsc);
    glint_dsc.color = nr_dim(lv_color_hex(0xFFFFFF), opa);
    glint_dsc.opa = LV_OPA_70;
    int32_t glint_y = top_y + height - 1;
    lv_area_t glint = {cx - 1, glint_y, cx + 1, glint_y + 1};
    lv_draw_fill(layer, &glint_dsc, &glint);
}

} // namespace helix
