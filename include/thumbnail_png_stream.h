// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A PNG decoded a row at a time. The inflated scanlines arrive in pieces from a
// streaming inflater, each finished row is unfiltered against the one above it
// and handed on as RGBA, so the decode never holds more than two rows of the
// image: what lets a thumbnail decode on a heap with no large free block.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace helix {

/// Largest thumbnail side decoded; anything larger keeps the placeholder.
inline constexpr int THUMBNAIL_MAX_SIDE = 640;

struct PngHeader {
    int width = 0;
    int height = 0;
    int bit_depth = 0;
    int color_type = 0;
    int interlace = 0;
};

/// Reads the IHDR chunk. False when @p data is not a PNG.
bool read_png_header(const uint8_t* data, size_t size, PngHeader& out);

/// True for what PngRowDecoder handles: 8 bits per channel, not interlaced,
/// grey, RGB, palette, grey+alpha or RGBA, and at most THUMBNAIL_MAX_SIDE a side.
bool png_thumbnail_supported(const PngHeader& header);

/// Palette and transparency chunks, which colour conversion needs.
struct PngPalette {
    uint8_t rgb[256 * 3] = {};
    uint8_t alpha[256] = {};
    int entries = 0;
    bool has_key = false; ///< a tRNS colour key for grey or RGB
    uint8_t key[3] = {};
};

/// Walks the chunks after IHDR: fills @p palette from PLTE and tRNS and passes
/// each IDAT payload to @p on_idat in order. False on a malformed file or when
/// @p on_idat returns false.
bool for_each_png_idat(const uint8_t* data, size_t size, PngPalette& palette,
                       const std::function<bool(const uint8_t*, size_t)>& on_idat);

/// Turns inflated PNG scanline bytes, fed in pieces of any size, into RGBA rows.
class PngRowDecoder {
  public:
    using RowSink = std::function<void(const uint8_t* rgba)>;

    PngRowDecoder(const PngHeader& header, const PngPalette& palette, RowSink on_row);

    /// False once the data is invalid (an unknown filter type) or overruns the image.
    bool feed(const uint8_t* data, size_t size);

    bool complete() const {
        return row_ == height_;
    }

  private:
    void finish_row();

    const PngPalette& palette_;
    RowSink on_row_;
    int width_, height_, color_type_, bpp_;
    size_t stride_;
    std::vector<uint8_t> cur_, prev_, rgba_;
    int filter_ = -1; ///< -1 while the next byte is a row's filter type
    size_t pos_ = 0;
    int row_ = 0;
    bool failed_ = false;
};

} // namespace helix
