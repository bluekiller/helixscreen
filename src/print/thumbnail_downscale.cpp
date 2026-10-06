// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "thumbnail_downscale.h"

#include <algorithm>

namespace helix {

ThumbnailDims fit_thumbnail(int w, int h, int max_w, int max_h) {
    if (w <= 0 || h <= 0 || max_w <= 0 || max_h <= 0) {
        return {};
    }
    if (w <= max_w && h <= max_h) {
        return {w, h};
    }
    // Width is the binding side when w/h >= max_w/max_h.
    if (static_cast<int64_t>(w) * max_h >= static_cast<int64_t>(h) * max_w) {
        return {max_w, std::max(1, static_cast<int>(static_cast<int64_t>(h) * max_w / w))};
    }
    return {std::max(1, static_cast<int>(static_cast<int64_t>(w) * max_h / h)), max_h};
}

void downscale_to_rgb565a8(const uint8_t* px, int channels, int src_w, int src_h, ThumbnailDims dst,
                           uint8_t* out) {
    const bool grey = channels < 3;
    const bool has_alpha = channels == 2 || channels == 4;
    auto* colour = reinterpret_cast<uint16_t*>(out);
    uint8_t* alpha = out + static_cast<size_t>(dst.w) * dst.h * 2;
    for (int dy = 0; dy < dst.h; ++dy) {
        const int y0 = dy * src_h / dst.h;
        const int y1 = std::max(y0 + 1, (dy + 1) * src_h / dst.h);
        for (int dx = 0; dx < dst.w; ++dx) {
            const int x0 = dx * src_w / dst.w;
            const int x1 = std::max(x0 + 1, (dx + 1) * src_w / dst.w);
            uint32_t r = 0, g = 0, b = 0, a = 0;
            for (int y = y0; y < y1; ++y) {
                const uint8_t* p = px + (static_cast<size_t>(y) * src_w + x0) * channels;
                for (int x = x0; x < x1; ++x, p += channels) {
                    const uint32_t pa = has_alpha ? p[channels - 1] : 255;
                    r += p[0] * pa;
                    g += p[grey ? 0 : 1] * pa;
                    b += p[grey ? 0 : 2] * pa;
                    a += pa;
                }
            }
            const uint32_t n = static_cast<uint32_t>((y1 - y0) * (x1 - x0));
            const size_t i = static_cast<size_t>(dy) * dst.w + dx;
            if (a == 0) {
                colour[i] = 0;
                alpha[i] = 0;
                continue;
            }
            const uint32_t r8 = r / a, g8 = g / a, b8 = b / a;
            colour[i] = static_cast<uint16_t>(((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3));
            alpha[i] = static_cast<uint8_t>(a / n);
        }
    }
}

} // namespace helix
