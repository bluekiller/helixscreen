// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file thumbnail_rules.h
 * @brief The thumbnail decisions every backend shares
 *
 * The desktop disk cache, the firmware card grid and the firmware active-print
 * path each deliver thumbnails their own way; they ask these functions for the
 * decision. Exception-free and pool-free, so the firmware compiles them.
 */

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace helix {

/// What a thumbnail byte stream actually is, read from its magic bytes.
enum class ImageFormat : uint8_t { Unknown, Png, Jpeg, Qoi };

[[nodiscard]] ImageFormat sniff_image_format(const uint8_t* data, size_t size);

[[nodiscard]] inline ImageFormat sniff_image_format(std::string_view bytes) {
    return sniff_image_format(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

[[nodiscard]] inline ImageFormat sniff_image_format(const std::vector<uint8_t>& bytes) {
    return sniff_image_format(bytes.data(), bytes.size());
}

/// A PNG that ends in IEND, or a JPEG that ends in EOI. Decoders are fragile on
/// a stream cut mid-way (a partial download, a 100 KB gcode-header boundary),
/// so nothing is decoded that fails this.
[[nodiscard]] bool is_complete_image(const std::vector<uint8_t>& bytes);

/**
 * @brief The bytes as a PNG, which is what every file the cache names `.png` must be
 *
 * A PNG passes through; a JPEG is re-encoded (desktop only, up to 512px a side).
 * Empty for anything else, including QOI, and for a JPEG that cannot be
 * re-encoded: LVGL picks its decoder by extension, so a JPEG saved as `.png`
 * renders blank.
 */
[[nodiscard]] std::vector<uint8_t> ensure_png(std::vector<uint8_t> bytes);

} // namespace helix
