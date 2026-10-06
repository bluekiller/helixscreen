// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_buffer_meter.h"

#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace helix::ui {

constexpr float RECT_WIDTH_RATIO = 0.25f;
constexpr float RECT_HEIGHT_RATIO = 0.45f;
constexpr int32_t MIN_RECT_W = 20;
constexpr int32_t MIN_RECT_H = 30;
constexpr int32_t RECT_RADIUS = 3;
constexpr float FILAMENT_EXTEND = 0.1f;

UiBufferMeter::UiBufferMeter(lv_obj_t* parent) {
    if (!parent) {
        spdlog::error("[BufferMeter] NULL parent");
        return;
    }

    root_ = parent;

    // Create drawing object
    canvas_obj_ = lv_obj_create(root_);
    lv_obj_remove_style_all(canvas_obj_);
    lv_obj_set_size(canvas_obj_, LV_PCT(40), LV_PCT(80));
    lv_obj_align(canvas_obj_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(canvas_obj_, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(canvas_obj_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(canvas_obj_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(canvas_obj_, on_draw, LV_EVENT_DRAW_MAIN, this);

    // Direction + percentage label (below drawing)
    direction_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(direction_label_, theme_manager_get_font("font_small"), 0);
    lv_obj_set_style_text_color(direction_label_, theme_manager_get_color("text_muted"), 0);
    lv_obj_set_style_text_align(direction_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(direction_label_, LV_PCT(100));
    lv_obj_align(direction_label_, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_label_set_text(direction_label_, "N 0%");

    // No neutral label — the dashed line in the drawing is sufficient

    // SIZE_CHANGED callback for responsive layout
    lv_obj_add_event_cb(root_, on_size_changed, LV_EVENT_SIZE_CHANGED, this);

    update_labels();
}

UiBufferMeter::~UiBufferMeter() {
    // Remove callbacks to prevent dangling this pointer after C++ object destruction.
    // The LVGL objects (canvas_obj_, labels) are children of root_ and may outlive
    // this C++ object if root_ isn't deleted at the same time (e.g. detach patterns).
    if (canvas_obj_) {
        lv_obj_remove_event_cb_with_user_data(canvas_obj_, on_draw, this);
    }
    if (root_) {
        lv_obj_remove_event_cb_with_user_data(root_, on_size_changed, this);
    }
}

void UiBufferMeter::set_bias(float bias) {
    bias_ = std::clamp(bias, -1.0f, 1.0f);
    update_labels();
    if (canvas_obj_)
        lv_obj_invalidate(canvas_obj_);
}

void UiBufferMeter::resize() {
    if (canvas_obj_)
        lv_obj_invalidate(canvas_obj_);
}

void UiBufferMeter::on_draw(lv_event_t* e) {
    auto* self = static_cast<UiBufferMeter*>(lv_event_get_user_data(e));
    if (!self)
        return;
    auto* layer = lv_event_get_layer(e);
    if (!layer)
        return;
    self->draw(layer);
}

void UiBufferMeter::on_size_changed(lv_event_t* e) {
    auto* self = static_cast<UiBufferMeter*>(lv_event_get_user_data(e));
    if (self)
        self->resize();
}

void UiBufferMeter::update_labels() {
    if (!direction_label_)
        return;

    int pct = static_cast<int>(bias_ * 100.0f);

    char buf[16];
    if (std::fabs(bias_) < 0.02f) {
        lv_snprintf(buf, sizeof(buf), "0%%");
    } else {
        lv_snprintf(buf, sizeof(buf), "%+d%%", pct);
    }
    lv_label_set_text(direction_label_, buf);
}

void UiBufferMeter::draw(lv_layer_t* layer) {
    if (!canvas_obj_)
        return;

    lv_area_t area;
    lv_obj_get_coords(canvas_obj_, &area);
    int32_t w = lv_area_get_width(&area);
    int32_t h = lv_area_get_height(&area);
    int32_t cx = area.x1 + w / 2;
    int32_t cy = area.y1 + h / 2;

    // Rectangle dimensions
    int32_t rect_w = std::max(MIN_RECT_W, static_cast<int32_t>(w * RECT_WIDTH_RATIO));
    int32_t rect_h = std::max(MIN_RECT_H, static_cast<int32_t>(h * RECT_HEIGHT_RATIO));

    // Bias maps to vertical offset of inner rect relative to outer:
    //   bias= 0: 50% overlap (neutral — plunger halfway in housing)
    //   bias=-1: 0% overlap  (tension — plunger pulled fully out, upward)
    //   bias=+1: 100% overlap (compression — plunger fully pushed in)
    // neutral_gap is the distance between centers at bias=0 (half a rect height apart)
    float neutral_gap = rect_h * 0.5f;
    // bias shifts from that neutral position: +bias pushes inner down (more overlap),
    // -bias pulls inner up (less overlap)
    int32_t inner_offset = static_cast<int32_t>(-neutral_gap + bias_ * neutral_gap);

    // Outer rectangle (stationary, above center)
    int32_t outer_top = cy - rect_h;
    int32_t outer_bot = cy;

    // Inner rectangle (slides vertically, below center at neutral)
    int32_t inner_cy = cy + inner_offset;
    int32_t inner_top = inner_cy;
    int32_t inner_bot = inner_cy + rect_h;

    // Color based on abs(bias): green -> orange -> red
    float abs_bias = std::fabs(bias_);
    lv_color_t rect_color;
    if (abs_bias < 0.3f) {
        rect_color = lv_color_hex(0x22C55E);
    } else if (abs_bias < 0.7f) {
        float t = (abs_bias - 0.3f) / 0.4f;
        uint8_t r = static_cast<uint8_t>(0x22 + t * (0xF5 - 0x22));
        uint8_t g = static_cast<uint8_t>(0xC5 + t * (0x9E - 0xC5));
        uint8_t b = static_cast<uint8_t>(0x5E + t * (0x0B - 0x5E));
        rect_color = lv_color_make(r, g, b);
    } else {
        float t = std::min((abs_bias - 0.7f) / 0.3f, 1.0f);
        uint8_t r = static_cast<uint8_t>(0xF5 + t * (0xEF - 0xF5));
        uint8_t g = static_cast<uint8_t>(0x9E + t * (0x44 - 0x9E));
        uint8_t b = static_cast<uint8_t>(0x0B + t * (0x44 - 0x0B));
        rect_color = lv_color_make(r, g, b);
    }

    // Draw filament line (vertical, extending beyond both rectangles)
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = lv_color_hex(0x4ADE80);
    line_dsc.width = 3;
    line_dsc.round_start = true;
    line_dsc.round_end = true;
    int32_t fil_top = std::min(outer_top, inner_top) - static_cast<int32_t>(h * FILAMENT_EXTEND);
    int32_t fil_bot = std::max(outer_bot, inner_bot) + static_cast<int32_t>(h * FILAMENT_EXTEND);
    line_dsc.p1.x = static_cast<lv_value_precise_t>(cx);
    line_dsc.p1.y = static_cast<lv_value_precise_t>(fil_top);
    line_dsc.p2.x = static_cast<lv_value_precise_t>(cx);
    line_dsc.p2.y = static_cast<lv_value_precise_t>(fil_bot);
    lv_draw_line(layer, &line_dsc);

    // Draw outer rectangle (housing)
    lv_draw_border_dsc_t border_dsc;
    lv_draw_border_dsc_init(&border_dsc);
    border_dsc.color = rect_color;
    border_dsc.opa = LV_OPA_70;
    border_dsc.width = 2;
    border_dsc.radius = RECT_RADIUS;
    lv_area_t outer_area = {cx - rect_w / 2, outer_top, cx + rect_w / 2, outer_bot};
    lv_draw_border(layer, &border_dsc, &outer_area);

    lv_draw_fill_dsc_t fill_dsc;
    lv_draw_fill_dsc_init(&fill_dsc);
    fill_dsc.color = rect_color;
    fill_dsc.opa = LV_OPA_10;
    fill_dsc.radius = RECT_RADIUS;
    lv_draw_fill(layer, &fill_dsc, &outer_area);

    // Draw inner rectangle (plunger)
    lv_area_t inner_area = {cx - rect_w / 2 + 3, inner_top, cx + rect_w / 2 - 3, inner_bot};
    fill_dsc.color = rect_color;
    fill_dsc.opa = LV_OPA_40;
    lv_draw_fill(layer, &fill_dsc, &inner_area);

    border_dsc.color = rect_color;
    border_dsc.opa = LV_OPA_COVER;
    border_dsc.width = 1;
    lv_draw_border(layer, &border_dsc, &inner_area);

    // Neutral reference line (dashed)
    lv_draw_line_dsc_t neutral_dsc;
    lv_draw_line_dsc_init(&neutral_dsc);
    neutral_dsc.color = theme_manager_get_color("text_muted");
    neutral_dsc.width = 1;
    neutral_dsc.dash_gap = 3;
    neutral_dsc.dash_width = 3;
    neutral_dsc.p1.x = static_cast<lv_value_precise_t>(cx - rect_w / 2 - 6);
    neutral_dsc.p1.y = static_cast<lv_value_precise_t>(cy);
    neutral_dsc.p2.x = static_cast<lv_value_precise_t>(cx + rect_w / 2 + 6);
    neutral_dsc.p2.y = static_cast<lv_value_precise_t>(cy);
    lv_draw_line(layer, &neutral_dsc);
}

} // namespace helix::ui
