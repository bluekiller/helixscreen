// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace helix {

struct ThumbnailDims {
    int w = 0;
    int h = 0;
};

/// The size an image of @p w x @p h takes inside a @p max_w x @p max_h box,
/// aspect kept. Never upscales; each side is at least 1. {0,0} for a degenerate
/// input.
ThumbnailDims fit_thumbnail(int w, int h, int max_w, int max_h);

/// Why a thumbnail decode produced nothing.
enum class ThumbnailDecodeFailure {
    None,
    OutOfMemory, ///< Worth retrying once memory frees up
    BadImage,    ///< Corrupt, truncated or unsupported: retrying cannot help
    TooLarge,    ///< Beyond what the decode scratch holds: retrying cannot help
};

/// Bytes an RGB565A8 image of @p dims needs: a 16-bit colour plane followed by
/// an 8-bit alpha plane.
inline size_t rgb565a8_size(ThumbnailDims dims) {
    return static_cast<size_t>(dims.w) * static_cast<size_t>(dims.h) * 3;
}

/// Box-filters an 8-bit image of @p channels per pixel (1 grey, 2 grey+alpha,
/// 3 RGB, 4 RGBA; rows packed) down to @p dst and writes it to @p out in LVGL's
/// RGB565A8 layout: dst.w * dst.h native-endian RGB565 pixels, then dst.w *
/// dst.h alpha bytes. Colour is averaged weighted by alpha, so transparent
/// pixels do not darken edges. @p out must hold rgb565a8_size(dst). @p dst must
/// not exceed the source.
void downscale_to_rgb565a8(const uint8_t* px, int channels, int src_w, int src_h, ThumbnailDims dst,
                           uint8_t* out);

} // namespace helix
