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
#include "stb_image.h"
#include "thumbnail_downscale.h"
#include "thumbnail_scratch.h"

#include <lvgl/src/misc/cache/instance/lv_image_cache.h> // lv_image_cache_drop()

#include <memory>
#include <mutex>
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

    /// Reserves the decode scratch (thumbnail_scratch_bytes(), ~0.94MB of PSRAM,
    /// held for the life of the process). Call once at boot, before PSRAM
    /// fragments. False when the reservation failed; thumbnails then show the
    /// placeholder.
    static bool reserve_scratch() {
        helix::ScratchArena& arena = scratch();
        if (arena.capacity() == 0) {
            const size_t size = helix::thumbnail_scratch_bytes();
            auto* buf = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
            arena.reset(buf, size);
        }
        return arena.capacity() != 0;
    }

    /// Decodes png_bytes once and keeps it as an RGB565A8 image fitted inside
    /// max_w x max_h, so drawing it needs no PNG decoder and no image-cache
    /// entry. The full-size decode happens in the boot-time scratch, one at a
    /// time; a PNG the scratch cannot hold is refused, never allocated for.
    /// Only the kept image (3 bytes per pixel) is a new allocation. Touches no
    /// widget state, so it is safe on the HTTP lane worker. Returns nullptr
    /// when nothing was produced, and says why in @p failure.
    static std::shared_ptr<EspPsramThumbnail>
    create_decoded(const std::string& png_bytes, int max_w, int max_h,
                   helix::ThumbnailDecodeFailure& failure) {
        failure = helix::ThumbnailDecodeFailure::None;
        const auto* png = reinterpret_cast<const uint8_t*>(png_bytes.data());
        if (!helix::thumbnail_fits_scratch(png, png_bytes.size())) {
            helix::PngHeader header;
            failure = helix::read_png_header(png, png_bytes.size(), header)
                          ? helix::ThumbnailDecodeFailure::TooLarge
                          : helix::ThumbnailDecodeFailure::BadImage;
            return nullptr;
        }

        std::lock_guard<std::mutex> lock(scratch_mutex());
        helix::ScratchArena& arena = scratch();
        if (arena.capacity() == 0) {
            failure = helix::ThumbnailDecodeFailure::OutOfMemory;
            return nullptr;
        }
        helix::ScratchScope scope(arena);
        int w = 0;
        int h = 0;
        int channels = 0;
        uint8_t* pixels =
            stbi_load_from_memory(png, static_cast<int>(png_bytes.size()), &w, &h, &channels, 0);
        if (!pixels) {
            failure = helix::ThumbnailDecodeFailure::BadImage;
            return nullptr;
        }
        const helix::ThumbnailDims dims = helix::fit_thumbnail(w, h, max_w, max_h);
        const size_t size = helix::rgb565a8_size(dims);
        auto* buf =
            size ? static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM)) : nullptr;
        if (!buf) {
            failure = size ? helix::ThumbnailDecodeFailure::OutOfMemory
                           : helix::ThumbnailDecodeFailure::BadImage;
            return nullptr;
        }
        helix::downscale_to_rgb565a8(pixels, channels, w, h, dims, buf);
        return std::shared_ptr<EspPsramThumbnail>(new EspPsramThumbnail(buf, size, dims));
    }

    /// Pointer suitable for lv_image_set_src().
    const lv_image_dsc_t* dsc() const {
        return &dsc_;
    }

  private:
    static helix::ScratchArena& scratch() {
        static helix::ScratchArena arena;
        return arena;
    }
    static std::mutex& scratch_mutex() {
        static std::mutex m;
        return m;
    }

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
