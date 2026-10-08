// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_path_canvas_buffer.cpp
 * @brief The path canvas buffer: one allocation per open, never two live at once,
 *        and never a stale buffer drawn at the wrong scale.
 *
 * The canvas holds an ARGB8888 buffer the size of the widget plus its top
 * overhang, which is 646 KB on the AMS panel. On the ESP32 that block comes out
 * of a fragmented PSRAM heap, so the number of allocations per open, and how many
 * are live at once, decide whether the open succeeds.
 */

#include "ui_filament_path_canvas.h"

#include "../lvgl_test_fixture.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/draw/lv_draw_buf_private.h" // handler fields: no public setters
#include "src/ui/ui_filament_path_internal.h"

#include <algorithm>
#include <cstddef>
#include <map>

#include "../catch_amalgamated.hpp"

namespace {

/// Counts the large draw-buffer allocations made through LVGL's default handlers,
/// and can refuse them to stand in for an exhausted heap.
class BufAllocSpy {
  public:
    static constexpr size_t BIG = 200 * 1024;

    BufAllocSpy() : handlers_(lv_draw_buf_get_handlers()) {
        orig_malloc_ = handlers_->buf_malloc_cb;
        orig_free_ = handlers_->buf_free_cb;
        handlers_->buf_malloc_cb = &BufAllocSpy::malloc_cb;
        handlers_->buf_free_cb = &BufAllocSpy::free_cb;
        self_ = this;
    }
    ~BufAllocSpy() {
        handlers_->buf_malloc_cb = orig_malloc_;
        handlers_->buf_free_cb = orig_free_;
        self_ = nullptr;
    }

    int allocations = 0;   ///< big allocations that succeeded
    int attempts = 0;      ///< big allocations requested, refused or not
    size_t live_bytes = 0; ///< big buffers currently allocated
    size_t peak_bytes = 0; ///< most big-buffer bytes ever live at once
    bool refuse_big = false;

  private:
    static void* malloc_cb(size_t size, lv_color_format_t cf) {
        BufAllocSpy& s = *self_;
        if (size < BIG)
            return s.orig_malloc_(size, cf);
        ++s.attempts;
        if (s.refuse_big)
            return nullptr;
        void* p = s.orig_malloc_(size, cf);
        if (p) {
            ++s.allocations;
            s.live_[p] = size;
            s.live_bytes += size;
            s.peak_bytes = std::max(s.peak_bytes, s.live_bytes);
        }
        return p;
    }
    static void free_cb(void* p) {
        BufAllocSpy& s = *self_;
        auto it = s.live_.find(p);
        if (it != s.live_.end()) {
            s.live_bytes -= it->second;
            s.live_.erase(it);
        }
        s.orig_free_(p);
    }

    static inline BufAllocSpy* self_ = nullptr;
    lv_draw_buf_handlers_t* handlers_;
    lv_draw_buf_malloc_cb_t orig_malloc_;
    lv_draw_buf_free_cb_t orig_free_;
    std::map<void*, size_t> live_;
};

size_t argb_bytes(int32_t w, int32_t h) {
    return lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_ARGB8888) * h;
}

} // namespace

// An AMS open sizes the path canvas twice: once to the column it has before the
// slots exist, then to the column the slots leave it.
TEST_CASE_METHOD(LVGLTestFixture,
                 "FilamentPath: an open's resizes cost one buffer, never two live at once",
                 "[filament_path][canvas_buffer]") {
    BufAllocSpy spy;
    lv_obj_t* path = ui_filament_path_canvas_create(test_screen());
    REQUIRE(path != nullptr);

    lv_obj_set_size(path, 454, 318);
    lv_obj_update_layout(path);
    process_lvgl(60);
    lv_obj_set_size(path, 470, 294);
    lv_obj_update_layout(path);
    process_lvgl(60);

    auto* data = helix::ui::fpath::get_data(path);
    REQUIRE(data != nullptr);
    REQUIRE(data->layers.overlay_buf != nullptr);
    CHECK(data->layers.overlay_buf->header.w == 470);
    CHECK(data->layers.overlay_buf->header.h == 344);

    CHECK(spy.allocations == 1);
    CHECK(spy.peak_bytes <= argb_bytes(454, 368));

    lv_obj_delete(path);
    process_lvgl(30);
    CHECK(spy.live_bytes == 0);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "FilamentPath: a growth past the buffer frees the old one before allocating",
                 "[filament_path][canvas_buffer]") {
    BufAllocSpy spy;
    lv_obj_t* path = ui_filament_path_canvas_create(test_screen());
    REQUIRE(path != nullptr);
    lv_obj_set_size(path, 300, 200);
    lv_obj_update_layout(path);
    process_lvgl(60);
    REQUIRE(spy.allocations == 1);

    lv_obj_set_size(path, 470, 294);
    lv_obj_update_layout(path);
    process_lvgl(60);

    CHECK(spy.allocations == 2);
    CHECK(spy.peak_bytes <= argb_bytes(470, 344));

    lv_obj_delete(path);
    process_lvgl(30);
}

TEST_CASE_METHOD(LVGLTestFixture, "FilamentPath: a growth the heap cannot fit draws nothing",
                 "[filament_path][canvas_buffer]") {
    BufAllocSpy spy;
    lv_obj_t* path = ui_filament_path_canvas_create(test_screen());
    REQUIRE(path != nullptr);
    lv_obj_set_size(path, 300, 200);
    lv_obj_update_layout(path);
    process_lvgl(60);

    auto* data = helix::ui::fpath::get_data(path);
    REQUIRE(data != nullptr);
    REQUIRE(data->layers.overlay_buf != nullptr);
    REQUIRE(data->layers.overlay_buf->header.w == 300);

    // Too big to reshape into, and the heap refuses it.
    spy.refuse_big = true;
    lv_obj_set_size(path, 470, 294);
    lv_obj_update_layout(path);
    process_lvgl(60);

    // The 300-wide buffer is gone, not left drawing a topology scaled for 300.
    CHECK(data->layers.overlay_buf == nullptr);
    lv_draw_buf_t* shown = lv_canvas_get_draw_buf(data->layers.overlay_canvas);
    REQUIRE(shown != nullptr);
    CHECK(shown->header.w <= 1);

    // Retries are bounded: a heap that never frees stops being asked.
    const int after_first = spy.attempts;
    process_lvgl(30000);
    CHECK(spy.attempts > after_first);
    CHECK(spy.attempts - after_first <= 5);

    // Memory comes back and a new layout pass asks again: the canvas recovers.
    spy.refuse_big = false;
    ui_filament_path_canvas_set_slot_count(path, 5);
    process_lvgl(60);
    REQUIRE(data->layers.overlay_buf != nullptr);
    CHECK(data->layers.overlay_buf->header.w == 470);
    CHECK(data->layers.overlay_buf->header.h == 344);

    lv_obj_delete(path);
    process_lvgl(30);
}

TEST_CASE_METHOD(LVGLTestFixture, "FilamentPath: deleting the widget drops a pending retry",
                 "[filament_path][canvas_buffer]") {
    BufAllocSpy spy;
    lv_obj_t* path = ui_filament_path_canvas_create(test_screen());
    REQUIRE(path != nullptr);

    spy.refuse_big = true;
    lv_obj_set_size(path, 470, 294);
    lv_obj_update_layout(path);
    process_lvgl(60);
    auto* data = helix::ui::fpath::get_data(path);
    REQUIRE(data != nullptr);
    REQUIRE(data->layers.alloc_retry_timer.pending());

    const int attempts = spy.attempts;
    lv_obj_delete(path);
    process_lvgl(3000);

    CHECK(spy.attempts == attempts);
}
