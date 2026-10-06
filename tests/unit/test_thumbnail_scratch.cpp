// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "thumbnail_scratch.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// A private stb_image routed through the scratch hooks, the way the firmware
// builds its own.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_MALLOC(sz) helix_stbi_malloc(sz)
#define STBI_REALLOC_SIZED(p, oldsz, newsz) helix_stbi_realloc_sized(p, oldsz, newsz)
#define STBI_FREE(p) helix_stbi_free(p)
#include "stb_image.h"

#include "../catch_amalgamated.hpp"

using helix::ScratchArena;
using helix::ScratchScope;

namespace {

/// A PNG signature plus an IHDR chunk; nothing after it is read.
std::vector<uint8_t> png_header(uint32_t w, uint32_t h, uint8_t depth, uint8_t interlace) {
    std::vector<uint8_t> d = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n',
                              0,    0,   0,   13,  'I',  'H',  'D',  'R'};
    for (uint32_t v : {w, h}) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            d.push_back(static_cast<uint8_t>(v >> shift));
        }
    }
    d.insert(d.end(), {depth, 6, 0, 0, interlace, 0, 0, 0, 0});
    return d;
}

bool accepted(const std::vector<uint8_t>& d) {
    return helix::thumbnail_fits_scratch(d.data(), d.size());
}

std::vector<uint8_t> fixture_png() {
    std::string dir = __FILE__;
    const auto pos = dir.rfind("/tests/unit/");
    dir = pos != std::string::npos ? dir.substr(0, pos) + "/tests/fixtures/" : "tests/fixtures/";
    std::ifstream in(dir + "thumbnail_300_rgba.png", std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("only a thumbnail the scratch can decode is accepted", "[thumbnail][scratch]") {
    CHECK(accepted(png_header(300, 300, 8, 0)));
    CHECK(accepted(png_header(300, 200, 8, 0)));
    CHECK_FALSE(accepted(png_header(301, 300, 8, 0)));
    CHECK_FALSE(accepted(png_header(300, 301, 8, 0)));
    CHECK_FALSE(accepted(png_header(300, 300, 16, 0))); // a 16-bit decode needs another buffer
    CHECK_FALSE(accepted(png_header(300, 300, 8, 1)));  // so does an interlaced one
    CHECK_FALSE(accepted(png_header(0, 300, 8, 0)));

    auto big = png_header(300, 300, 8, 0);
    big.resize(helix::THUMBNAIL_MAX_PNG_BYTES + 1);
    CHECK_FALSE(accepted(big));

    auto not_png = png_header(300, 300, 8, 0);
    not_png[1] = 'J';
    CHECK_FALSE(accepted(not_png));
    CHECK_FALSE(helix::thumbnail_fits_scratch(nullptr, 0));
}

TEST_CASE("the scratch arena bumps, grows its last block in place, and refuses overflow",
          "[thumbnail][scratch]") {
    std::vector<uint8_t> buf(256);
    ScratchArena arena;
    arena.reset(buf.data(), buf.size());

    auto* a = static_cast<uint8_t*>(arena.alloc(10));
    REQUIRE(a == buf.data());
    auto* b = static_cast<uint8_t*>(arena.alloc(10));
    CHECK(b == buf.data() + 16); // 16-byte aligned
    CHECK(arena.realloc(b, 10, 100) == b);
    CHECK(arena.used() == 116);

    // Not the last block: moves, keeping its bytes.
    std::memset(a, 0x5A, 10);
    auto* moved = static_cast<uint8_t*>(arena.realloc(a, 10, 20));
    REQUIRE(moved != nullptr);
    CHECK(moved != a);
    CHECK(moved[9] == 0x5A);

    CHECK(arena.alloc(200) == nullptr);
    CHECK(arena.owns(b));
    int outside = 0;
    CHECK_FALSE(arena.owns(&outside));
    arena.rewind();
    CHECK(arena.used() == 0);
}

TEST_CASE("stb_image allocates from the arena only inside a scope", "[thumbnail][scratch]") {
    std::vector<uint8_t> buf(1024);
    ScratchArena arena;
    arena.reset(buf.data(), buf.size());
    {
        ScratchScope scope(arena);
        void* p = helix_stbi_malloc(64);
        CHECK(arena.owns(p));
        helix_stbi_free(p); // dropped with the scope, not freed
        CHECK(arena.used() >= 64);
    }
    CHECK(arena.used() == 0);

    void* heap = helix_stbi_malloc(64);
    CHECK_FALSE(arena.owns(heap));
    helix_stbi_free(heap);
}

TEST_CASE("the largest accepted thumbnail decodes inside the scratch and not in less",
          "[thumbnail][scratch]") {
    const auto png = fixture_png();
    REQUIRE(helix::thumbnail_fits_scratch(png.data(), png.size()));
    // The fixture is as large as accepted, so it exercises the compressed-data
    // share of the budget as well as the pixel buffers.
    REQUIRE(png.size() > helix::THUMBNAIL_MAX_PNG_BYTES * 9 / 10);

    std::vector<uint8_t> buf(helix::thumbnail_scratch_bytes());
    ScratchArena arena;
    arena.reset(buf.data(), buf.size());
    int w = 0, h = 0, n = 0;
    {
        ScratchScope scope(arena);
        uint8_t* px =
            stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &n, 0);
        REQUIRE(px != nullptr);
        CHECK(arena.owns(px));
        CHECK(w == 300);
        CHECK(h == 300);
        CHECK(n == 4);
    }

    // Without the decoded image's share the same decode fails cleanly.
    arena.reset(buf.data(), helix::thumbnail_scratch_bytes() - 300 * 300 * 4);
    {
        ScratchScope scope(arena);
        CHECK(stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &n, 0) ==
              nullptr);
    }
}
