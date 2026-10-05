// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "thumbnail_downscale.h"

#include <algorithm>

namespace helix {

ThumbnailDims fit_thumbnail(int w, int h, int max_px) {
    if (w <= 0 || h <= 0 || max_px <= 0) {
        return {};
    }
    if (w <= max_px && h <= max_px) {
        return {w, h};
    }
    if (w >= h) {
        return {max_px, std::max(1, static_cast<int>(static_cast<int64_t>(h) * max_px / w))};
    }
    return {std::max(1, static_cast<int>(static_cast<int64_t>(w) * max_px / h)), max_px};
}

void downscale_rgba_to_rgb565a8(const uint8_t* rgba, int src_w, int src_h, ThumbnailDims dst,
                                uint8_t* out) {
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
                const uint8_t* p = rgba + (static_cast<size_t>(y) * src_w + x0) * 4;
                for (int x = x0; x < x1; ++x, p += 4) {
                    r += p[0] * p[3];
                    g += p[1] * p[3];
                    b += p[2] * p[3];
                    a += p[3];
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
