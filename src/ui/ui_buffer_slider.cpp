// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_buffer_slider.h"

#include "ui_timer_guard.h"

#include "ams_state.h"
#include "buffer_reading.h"
#include "buffer_slider_geometry.h"
#include "observer_factory.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

namespace {
constexpr uint32_t kTraceRedrawMs = 1000;

lv_point_precise_t point(int32_t x, int32_t y) {
    return {static_cast<lv_value_precise_t>(x), static_cast<lv_value_precise_t>(y)};
}
} // namespace

UiBufferSlider::UiBufferSlider(lv_obj_t* slider_obj, lv_obj_t* trace_obj, int trace_unit)
    : slider_obj_(slider_obj), trace_obj_(trace_obj), trace_unit_(trace_unit) {
    if (!slider_obj_) {
        spdlog::error("[BufferSlider] No object to draw the slider into");
        trace_obj_ = nullptr;
        return;
    }
    // Draw and delete hooks only: XML authors and sizes both objects.
    for (lv_obj_t* obj : {slider_obj_, trace_obj_}) {
        if (obj) {
            lv_obj_add_event_cb(obj, on_draw, LV_EVENT_DRAW_MAIN,
                                this); // DECLARATIVE_OK: draw hook has no XML form
            lv_obj_add_event_cb(obj, on_deleted, LV_EVENT_DELETE,
                                this); // DECLARATIVE_OK: delete hook has no XML form
        }
    }
    if (trace_obj_) {
        trace_timer_.reset(lv_timer_create(on_trace_timer, kTraceRedrawMs, this));
    }
}

void UiBufferSlider::follow_system_reading() {
    auto& ams = AmsState::instance();
    const auto lifetime = ams.get_subjects_lifetime();
    // Immediate: the handlers only store the reading and invalidate.
    bias_observer_ = observe<int>(
        ams.get_buffer_bias_pct_subject(), this,
        [](UiBufferSlider* self, int pct) { self->set_reading(pct / 100.0f, self->status_); },
        lifetime, Dispatch::Immediate);
    status_observer_ = observe<int>(
        ams.get_buffer_status_subject(), this,
        [](UiBufferSlider* self, int status) {
            self->set_reading(self->bias_, static_cast<ClogMeterStatus>(status));
        },
        lifetime, Dispatch::Immediate);
}

UiBufferSlider::~UiBufferSlider() {
    bias_observer_.reset();
    status_observer_.reset();
    trace_timer_.reset();
    for (lv_obj_t* obj : {slider_obj_, trace_obj_}) {
        if (obj) {
            lv_obj_remove_event_cb_with_user_data(obj, on_draw, this);
            lv_obj_remove_event_cb_with_user_data(obj, on_deleted, this);
        }
    }
}

void UiBufferSlider::set_reading(float bias, ClogMeterStatus status) {
    bias_ = bias;
    status_ = status;
    if (slider_obj_) {
        lv_obj_invalidate(slider_obj_);
    }
}

void UiBufferSlider::on_draw(lv_event_t* e) {
    auto* self = static_cast<UiBufferSlider*>(lv_event_get_user_data(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!self || !layer) {
        return;
    }
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (target == self->slider_obj_) {
        self->draw_slider(layer);
    } else if (target == self->trace_obj_) {
        self->draw_trace(layer);
    }
}

void UiBufferSlider::on_deleted(lv_event_t* e) {
    auto* self = static_cast<UiBufferSlider*>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (target == self->slider_obj_) {
        self->slider_obj_ = nullptr;
    }
    if (target == self->trace_obj_) {
        self->trace_obj_ = nullptr;
        self->trace_timer_.reset();
    }
}

void UiBufferSlider::on_trace_timer(lv_timer_t* timer) {
    auto* self = static_cast<UiBufferSlider*>(lv_timer_get_user_data(timer));
    if (self && self->trace_obj_) {
        ++self->trace_ticks_;
        lv_obj_invalidate(self->trace_obj_);
    }
}

void UiBufferSlider::draw_slider(lv_layer_t* layer) const {
    lv_area_t a;
    lv_obj_get_content_coords(slider_obj_, &a);
    const int32_t w = lv_area_get_width(&a);
    const int32_t h = lv_area_get_height(&a);
    if (w <= 0 || h <= 0) {
        return;
    }
    const BufferSliderGeometry g = buffer_slider_geometry(bias_, h);
    const lv_color_t muted = theme_manager_get_color("text_muted");
    const int32_t radius = theme_manager_get_spacing("space_xxs");
    const int32_t cx = a.x1 + w / 2;

    lv_draw_fill_dsc_t fill;
    lv_draw_fill_dsc_init(&fill);
    fill.color = theme_manager_get_color("danger");
    fill.opa = LV_OPA_20;
    const lv_area_t loose_stop = {a.x1, a.y1, a.x2, a.y1 + g.danger_top_h - 1};
    const lv_area_t tight_stop = {a.x1, a.y1 + g.danger_bottom_y, a.x2, a.y2};
    lv_draw_fill(layer, &fill, &loose_stop);
    lv_draw_fill(layer, &fill, &tight_stop);

    lv_draw_border_dsc_t housing;
    lv_draw_border_dsc_init(&housing);
    housing.color = muted;
    housing.width = 1;
    housing.radius = radius;
    lv_draw_border(layer, &housing, &a);

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = muted;
    line.width = 2;
    line.p1 = point(cx, a.y1);
    line.p2 = point(cx, a.y2);
    lv_draw_line(layer, &line);

    line.width = 1;
    line.dash_width = 3;
    line.dash_gap = 3;
    for (int32_t y : {a.y1 + g.target_y, a.y1 + g.target_y + g.target_h}) {
        line.p1 = point(a.x1, y);
        line.p2 = point(a.x2, y);
        lv_draw_line(layer, &line);
    }

    fill.color = theme_manager_get_color(buffer_status_token(status_));
    fill.opa = LV_OPA_COVER;
    fill.radius = radius;
    const lv_area_t block = {a.x1 + 2, a.y1 + g.block_y, a.x2 - 2,
                             a.y1 + g.block_y + g.block_h - 1};
    lv_draw_fill(layer, &fill, &block);
}

void UiBufferSlider::draw_trace(lv_layer_t* layer) const {
    lv_area_t a;
    lv_obj_get_content_coords(trace_obj_, &a);
    const int32_t w = lv_area_get_width(&a);
    const int32_t h = lv_area_get_height(&a);
    if (w <= 0 || h <= 0) {
        return;
    }
    const lv_color_t muted = theme_manager_get_color("text_muted");

    // The target window carries on across the trace, so a reading reads
    // against it the way the block does.
    const BufferSliderGeometry g = buffer_slider_geometry(0.0f, h);
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = muted;
    line.width = 1;
    line.dash_width = 3;
    line.dash_gap = 3;
    for (int32_t y : {a.y1 + g.target_y, a.y1 + g.target_y + g.target_h}) {
        line.p1 = point(a.x1, y);
        line.p2 = point(a.x2, y);
        lv_draw_line(layer, &line);
    }

    const int64_t now = buffer_clock_ms();
    const auto window = AmsState::instance().buffer_trace(trace_unit_).window(now);

    // The part of the minute with no history yet is a faint dotted baseline at
    // the target level, so the area always spans the full window.
    const int32_t unrecorded_x = buffer_trace_unrecorded_x(window, now, w);
    if (unrecorded_x < w) {
        line.opa = LV_OPA_50;
        const int32_t y = a.y1 + buffer_slider_y(0.0f, h);
        line.p1 = point(a.x1 + unrecorded_x, y);
        line.p2 = point(a.x2, y);
        lv_draw_line(layer, &line);
    }

    const auto lines = buffer_trace_polylines(window, now, w, h);
    lv_draw_line_dsc_t trace;
    lv_draw_line_dsc_init(&trace);
    trace.color = muted;
    trace.width = 2;
    trace.round_start = 1;
    trace.round_end = 1;
    for (const auto& run : lines) {
        for (std::size_t i = 1; i < run.size(); ++i) {
            trace.p1 = point(a.x1 + run[i - 1].x, a.y1 + run[i - 1].y);
            trace.p2 = point(a.x1 + run[i].x, a.y1 + run[i].y);
            lv_draw_line(layer, &trace);
        }
    }
}

} // namespace helix::ui
