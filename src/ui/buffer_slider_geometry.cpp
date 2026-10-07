// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "buffer_slider_geometry.h"

#include "clog_meter_geometry.h"

#include <algorithm>
#include <cmath>

namespace helix::ui {

namespace {

int block_height(int height) {
    return std::max(4, height / 8);
}

} // namespace

int buffer_slider_y(float bias, int height) {
    if (height <= 0) {
        return 0;
    }
    const float b = std::isnan(bias) ? 0.0f : std::clamp(bias, -1.0f, 1.0f);
    const int block_h = block_height(height);
    const int travel = height - block_h;
    return block_h / 2 + static_cast<int>(std::lround((1.0f - b) * travel / 2.0f));
}

BufferSliderGeometry buffer_slider_geometry(float bias, int height) {
    BufferSliderGeometry g;
    if (height <= 0) {
        return g;
    }
    g.block_h = block_height(height);
    g.block_y = buffer_slider_y(bias, height) - g.block_h / 2;
    const float warning = kPressureWarningPct / 100.0f;
    const float fault = kPressureFaultPct / 100.0f;
    g.target_y = buffer_slider_y(warning, height);
    g.target_h = buffer_slider_y(-warning, height) - g.target_y;
    g.danger_top_h = buffer_slider_y(fault, height);
    g.danger_bottom_y = buffer_slider_y(-fault, height);
    g.danger_bottom_h = height - g.danger_bottom_y;
    return g;
}

std::vector<std::vector<BufferTraceXY>>
buffer_trace_polylines(const std::vector<BufferTracePoint>& window, int64_t now_ms, int width,
                       int height) {
    std::vector<std::vector<BufferTraceXY>> lines;
    if (width <= 0 || height <= 0) {
        return lines;
    }
    auto x_of = [&](int64_t t_ms) {
        return static_cast<int>(
            std::clamp<int64_t>((now_ms - t_ms) * width / BufferTrace::kWindowMs, 0, width));
    };
    std::vector<BufferTraceXY> run;
    int newer_x = 0; // where the next newer reading began; the newest holds from now
    for (auto it = window.rbegin(); it != window.rend(); ++it) {
        const int older_x = x_of(it->t_ms);
        if (it->valid) {
            const int y = buffer_slider_y(it->bias, height);
            run.push_back({newer_x, y});
            run.push_back({older_x, y});
        } else if (!run.empty()) {
            lines.push_back(std::move(run));
            run.clear();
        }
        newer_x = older_x;
    }
    if (!run.empty()) {
        lines.push_back(std::move(run));
    }
    return lines;
}

int buffer_trace_unrecorded_x(const std::vector<BufferTracePoint>& window, int64_t now_ms,
                              int width) {
    if (width <= 0 || window.empty()) {
        return 0;
    }
    return static_cast<int>(std::clamp<int64_t>(
        (now_ms - window.front().t_ms) * width / BufferTrace::kWindowMs, 0, width));
}

} // namespace helix::ui
