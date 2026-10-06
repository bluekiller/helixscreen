// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// ESP32-only (Task 11 R2). PSRAM-resident thumbnail for the print-select cards
// and the print status panel. LittleFS is too small for a disk thumbnail cache
// on this platform (Task 10 R6 hard gate — see thumbnail_cache.cpp), so a
// thumbnail fetched via download_file_partial is decoded once, fitted to the
// size it is drawn at, and kept as an RGB565A8 lv_image_dsc_t in PSRAM: the
// form a prescaled .bin takes elsewhere. Drawing the PNG itself would hold a
// full-size ARGB8888 decode per image in LVGL's image cache, which PSRAM
// cannot keep for more than one.
#if defined(HELIX_PLATFORM_ESP32)

#include "async_lifetime_guard.h" // for helix::internal::on_main_thread()
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "miniz.h" // the ROM's streaming inflater
#include "thumbnail_downscale.h"
#include "thumbnail_png_stream.h"

#include <lvgl/src/misc/cache/instance/lv_image_cache.h> // lv_image_cache_drop()

#include <memory>
#include <string>

namespace helix::ui {

/**
 * @brief Owns one PSRAM-allocated decoded image plus the lv_image_dsc_t
 *        wrapping it, for a single print-file thumbnail.
 *
 * Instances are shared_ptr-managed and held both by the PrintFileData entry
 * (source of truth) and by the card widget slot currently displaying it
 * (CardWidgetData::esp_thumbnail) so the buffer stays alive for as long as
 * any widget's `src` still points at its descriptor, independent of when the
 * owning PrintFileData is replaced during a list refresh/sort.
 */
class EspPsramThumbnail {
  public:
    EspPsramThumbnail(const EspPsramThumbnail&) = delete;
    EspPsramThumbnail& operator=(const EspPsramThumbnail&) = delete;

    ~EspPsramThumbnail() {
        if (data_) {
            // LVGL's image cache keys a variable-source (lv_image_dsc_t*) entry
            // on the source pointer itself (&dsc_ here). If this buffer's heap
            // address gets reused for a later EspPsramThumbnail and the old
            // cache entry hasn't LRU-evicted yet, lv_image_set_src(new dsc)
            // could hit the stale decoded bitmap — a card showing a previous
            // file's thumbnail (review Focus 2). Drop it explicitly.
            //
            // lv_image_cache_drop() reaches into the draw units
            // (LV_EVENT_INVALIDATE_AREA broadcast) and is documented unsafe
            // off the UI thread (see Application's memory-pressure responder,
            // application.cpp). Every normal destruction path here (file_list_
            // replaced/sorted, card recycled, panel torn down) runs on the
            // main thread via UpdateQueue::process_pending() — the shared_ptr
            // is only ever unwrapped inside a tok.defer()'d lambda, and
            // UpdateQueue::queue() always stores that lambda into
            // pending_/frozen_buffer_ to run there. The ONE exception:
            // queue()'s shut_down_ branch drops the incoming callback (and
            // whatever it captured) synchronously on the CALLING thread,
            // which could be the EspHttpLane worker if a fetch completes
            // after UpdateQueue::shutdown() has already run. Guard instead of
            // assuming that race can't happen; skipping the drop there is
            // harmless since the process is already tearing down.
            if (helix::internal::on_main_thread()) {
                lv_image_cache_drop(dsc());
            }
            heap_caps_free(data_);
        }
    }

    /// Decodes png_bytes once and keeps it as an RGB565A8 image fitted inside
    /// max_w x max_h, so drawing it needs no PNG decoder and no image-cache
    /// entry. The PNG is inflated and unfiltered a row at a time and each row
    /// goes straight into the downscale, so the full-size image never exists:
    /// the decode needs the ROM inflater's state and 32KB window, two rows and
    /// the kept image (3 bytes per kept pixel), and no block larger than that.
    /// Touches no widget state, so it is safe on the HTTP lane worker. Returns
    /// nullptr when nothing was produced, and says why in @p failure.
    static std::shared_ptr<EspPsramThumbnail>
    create_decoded(const std::string& png_bytes, int max_w, int max_h,
                   helix::ThumbnailDecodeFailure& failure) {
        failure = helix::ThumbnailDecodeFailure::BadImage;
        const auto* png = reinterpret_cast<const uint8_t*>(png_bytes.data());
        helix::PngHeader header;
        if (!helix::read_png_header(png, png_bytes.size(), header)) {
            return nullptr;
        }
        if (!helix::png_thumbnail_supported(header)) {
            failure = helix::ThumbnailDecodeFailure::TooLarge;
            return nullptr;
        }
        const helix::ThumbnailDims dims =
            helix::fit_thumbnail(header.width, header.height, max_w, max_h);
        const size_t size = helix::rgb565a8_size(dims);

        struct Buffers {
            uint8_t* out = nullptr;
            tinfl_decompressor* inflater = nullptr;
            uint8_t* window = nullptr;
            ~Buffers() {
                heap_caps_free(out);
                heap_caps_free(inflater);
                heap_caps_free(window);
            }
        } b;
        b.out = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
        b.inflater = static_cast<tinfl_decompressor*>(
            heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM));
        b.window = static_cast<uint8_t*>(heap_caps_malloc(TINFL_LZ_DICT_SIZE, MALLOC_CAP_SPIRAM));
        if (!b.out || !b.inflater || !b.window) {
            failure = helix::ThumbnailDecodeFailure::OutOfMemory;
            return nullptr;
        }

        helix::RowDownscaler scaler(header.width, header.height, dims, b.out);
        helix::PngPalette palette;
        helix::PngRowDecoder rows(header, palette,
                                  [&scaler](const uint8_t* rgba) { scaler.add_row(rgba); });
        tinfl_init(b.inflater);
        size_t window_at = 0;
        tinfl_status status = TINFL_STATUS_NEEDS_MORE_INPUT;
        // Inflates one IDAT payload (or, with more_input false, flushes the end of
        // the stream) through the circular window into the row decoder.
        auto inflate = [&](const uint8_t* in, size_t in_left, bool more_input) {
            const mz_uint32 flags =
                TINFL_FLAG_PARSE_ZLIB_HEADER | (more_input ? TINFL_FLAG_HAS_MORE_INPUT : 0);
            for (;;) {
                size_t in_size = in_left;
                size_t out_size = TINFL_LZ_DICT_SIZE - window_at;
                status = tinfl_decompress(b.inflater, in, &in_size, b.window, b.window + window_at,
                                          &out_size, flags);
                in += in_size;
                in_left -= in_size;
                if (out_size && !rows.feed(b.window + window_at, out_size)) {
                    return false;
                }
                window_at = (window_at + out_size) & (TINFL_LZ_DICT_SIZE - 1);
                if (status < TINFL_STATUS_DONE) {
                    return false;
                }
                if (status == TINFL_STATUS_DONE ||
                    (status == TINFL_STATUS_NEEDS_MORE_INPUT && in_left == 0)) {
                    return true;
                }
            }
        };
        const bool walked = helix::for_each_png_idat(
            png, png_bytes.size(), palette, [&](const uint8_t* data, size_t len) {
                return status == TINFL_STATUS_DONE || inflate(data, len, true);
            });
        if (!walked || (status != TINFL_STATUS_DONE && !inflate(nullptr, 0, false)) ||
            !rows.complete()) {
            return nullptr;
        }

        failure = helix::ThumbnailDecodeFailure::None;
        uint8_t* out = b.out;
        b.out = nullptr;
        return std::shared_ptr<EspPsramThumbnail>(new EspPsramThumbnail(out, size, dims));
    }

    /// Pointer suitable for lv_image_set_src().
    const lv_image_dsc_t* dsc() const {
        return &dsc_;
    }

  private:
    EspPsramThumbnail(uint8_t* data, size_t size, helix::ThumbnailDims dims) : data_(data) {
        dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc_.header.cf = LV_COLOR_FORMAT_RGB565A8;
        dsc_.header.w = static_cast<uint32_t>(dims.w);
        dsc_.header.h = static_cast<uint32_t>(dims.h);
        dsc_.header.stride = static_cast<uint32_t>(dims.w * 2); // the RGB565 plane's
        dsc_.data_size = static_cast<uint32_t>(size);
        dsc_.data = data_;
    }

    uint8_t* data_ = nullptr;
    lv_image_dsc_t dsc_{};
};

} // namespace helix::ui

#endif // HELIX_PLATFORM_ESP32
