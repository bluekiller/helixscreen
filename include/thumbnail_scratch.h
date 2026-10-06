// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A thumbnail PNG decodes at full size before it is fitted to its box, and a
// 300x300 RGBA decode needs two ~360KB blocks at once. On a fragmented PSRAM
// heap no such block exists, so the firmware reserves one scratch buffer at
// boot, decodes every thumbnail inside it, and skips any thumbnail that could
// not fit rather than allocating for it.

#include <cstddef>
#include <cstdint>

namespace helix {

/// Largest thumbnail the scratch buffer is sized for: Moonraker's 300x300.
inline constexpr int THUMBNAIL_MAX_SIDE = 300;
/// Largest PNG accepted. stb_image copies the compressed data into the scratch.
inline constexpr size_t THUMBNAIL_MAX_PNG_BYTES = 128 * 1024;

/// Bytes the scratch must hold for the worst decode the caps allow: the PNG's
/// compressed data (stb_image grows that buffer by doubling, so up to twice the
/// file), the inflated scanlines (a filter byte per row), and the decoded
/// image, at 4 bytes per pixel, plus alignment slack. About 0.93MB.
inline constexpr size_t thumbnail_scratch_bytes() {
    constexpr size_t side = THUMBNAIL_MAX_SIDE;
    return 2 * THUMBNAIL_MAX_PNG_BYTES + side * (side * 4 + 1) + side * side * 4 + 4096;
}

struct PngHeader {
    int width = 0;
    int height = 0;
    int bit_depth = 0;
    int color_type = 0;
    int interlace = 0;
};

/// Reads the IHDR chunk. False when @p data is not a PNG.
bool read_png_header(const uint8_t* data, size_t size, PngHeader& out);

/// True when decoding @p data fits the scratch buffer: within the size caps,
/// 8 bits per channel and not interlaced (both would need more buffers).
bool thumbnail_fits_scratch(const uint8_t* data, size_t size);

/// Bump allocator over a caller-owned buffer. A decode's allocations all come
/// from one arena and are dropped together by rewind().
class ScratchArena {
  public:
    void reset(uint8_t* buffer, size_t size);
    void* alloc(size_t size);
    /// Grows the last allocation in place; anything else moves.
    void* realloc(void* ptr, size_t old_size, size_t new_size);
    bool owns(const void* ptr) const;
    void rewind() {
        used_ = 0;
        last_ = nullptr;
    }
    size_t used() const {
        return used_;
    }
    size_t capacity() const {
        return size_;
    }

  private:
    uint8_t* buffer_ = nullptr;
    size_t size_ = 0;
    size_t used_ = 0;
    uint8_t* last_ = nullptr;
};

/// While alive, stb_image allocations made on this thread come from @p arena
/// (through the helix_stbi_* hooks). Rewinds the arena on exit.
class ScratchScope {
  public:
    explicit ScratchScope(ScratchArena& arena);
    ~ScratchScope();
    ScratchScope(const ScratchScope&) = delete;
    ScratchScope& operator=(const ScratchScope&) = delete;

  private:
    ScratchArena* previous_;
};

} // namespace helix

extern "C" {
/// stb_image allocator hooks: the active ScratchScope's arena, else the C heap.
void* helix_stbi_malloc(size_t size);
void* helix_stbi_realloc_sized(void* ptr, size_t old_size, size_t new_size);
void helix_stbi_free(void* ptr);
}
