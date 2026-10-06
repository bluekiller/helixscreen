// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "thumbnail_scratch.h"

#include <cstdlib>
#include <cstring>

namespace helix {

namespace {

thread_local ScratchArena* t_arena = nullptr;

constexpr size_t ALIGN = 16;

uint32_t be32_at(const uint8_t* p) {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
}

} // namespace

bool read_png_header(const uint8_t* data, size_t size, PngHeader& out) {
    static constexpr uint8_t SIGNATURE[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    // Signature, then the IHDR chunk: length, "IHDR", 13 bytes of fields.
    if (!data || size < 8 + 8 + 13 || std::memcmp(data, SIGNATURE, 8) != 0 ||
        std::memcmp(data + 12, "IHDR", 4) != 0) {
        return false;
    }
    const uint8_t* f = data + 16;
    out.width = static_cast<int>(be32_at(f));
    out.height = static_cast<int>(be32_at(f + 4));
    out.bit_depth = f[8];
    out.color_type = f[9];
    out.interlace = f[12];
    return true;
}

bool thumbnail_fits_scratch(const uint8_t* data, size_t size) {
    PngHeader h;
    return size <= THUMBNAIL_MAX_PNG_BYTES && read_png_header(data, size, h) && h.width > 0 &&
           h.height > 0 && h.width <= THUMBNAIL_MAX_SIDE && h.height <= THUMBNAIL_MAX_SIDE &&
           h.bit_depth <= 8 && h.interlace == 0;
}

void ScratchArena::reset(uint8_t* buffer, size_t size) {
    buffer_ = buffer;
    size_ = buffer ? size : 0;
    rewind();
}

void* ScratchArena::alloc(size_t size) {
    const size_t start = (used_ + ALIGN - 1) & ~(ALIGN - 1);
    if (!buffer_ || start > size_ || size > size_ - start) {
        return nullptr;
    }
    last_ = buffer_ + start;
    used_ = start + size;
    return last_;
}

void* ScratchArena::realloc(void* ptr, size_t old_size, size_t new_size) {
    if (!ptr) {
        return alloc(new_size);
    }
    auto* p = static_cast<uint8_t*>(ptr);
    if (p == last_) {
        const size_t start = static_cast<size_t>(p - buffer_);
        if (new_size > size_ - start) {
            return nullptr;
        }
        used_ = start + new_size;
        return p;
    }
    void* moved = alloc(new_size);
    if (moved) {
        std::memcpy(moved, ptr, old_size < new_size ? old_size : new_size);
    }
    return moved;
}

bool ScratchArena::owns(const void* ptr) const {
    auto* p = static_cast<const uint8_t*>(ptr);
    return buffer_ && p >= buffer_ && p < buffer_ + size_;
}

ScratchScope::ScratchScope(ScratchArena& arena) : previous_(t_arena) {
    arena.rewind();
    t_arena = &arena;
}

ScratchScope::~ScratchScope() {
    t_arena->rewind();
    t_arena = previous_;
}

} // namespace helix

extern "C" {

void* helix_stbi_malloc(size_t size) {
    return helix::t_arena ? helix::t_arena->alloc(size) : std::malloc(size);
}

void* helix_stbi_realloc_sized(void* ptr, size_t old_size, size_t new_size) {
    if (helix::t_arena && (!ptr || helix::t_arena->owns(ptr))) {
        return helix::t_arena->realloc(ptr, old_size, new_size);
    }
    return std::realloc(ptr, new_size);
}

void helix_stbi_free(void* ptr) {
    // Arena memory is dropped wholesale when its scope ends.
    if (!(helix::t_arena && helix::t_arena->owns(ptr))) {
        std::free(ptr);
    }
}

} // extern "C"
