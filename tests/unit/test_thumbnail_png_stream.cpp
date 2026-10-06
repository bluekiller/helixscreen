// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "stb_image.h"
#include "thumbnail_downscale.h"
#include "thumbnail_png_stream.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::PngHeader;
using helix::PngPalette;
using helix::PngRowDecoder;

namespace {

std::vector<uint8_t> fixture(const char* name) {
    std::string dir = __FILE__;
    const auto pos = dir.rfind("/tests/unit/");
    dir = pos != std::string::npos ? dir.substr(0, pos) + "/tests/fixtures/" : "tests/fixtures/";
    std::ifstream in(dir + name, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// RGBA rows from the streaming decoder, its inflated input fed in @p piece-byte
/// pieces the way an inflater's window hands it over. The test inflates with
/// stb_image's zlib; the firmware inflates with the ROM's tinfl.
std::vector<uint8_t> stream_decode(const std::vector<uint8_t>& png, size_t piece, PngHeader& h) {
    REQUIRE(helix::read_png_header(png.data(), png.size(), h));
    REQUIRE(helix::png_thumbnail_supported(h));
    PngPalette palette;
    std::vector<uint8_t> zdata;
    REQUIRE(
        helix::for_each_png_idat(png.data(), png.size(), palette, [&](const uint8_t* d, size_t n) {
            zdata.insert(zdata.end(), d, d + n);
            return true;
        }));
    int raw_len = 0;
    char* raw = stbi_zlib_decode_malloc(reinterpret_cast<const char*>(zdata.data()),
                                        static_cast<int>(zdata.size()), &raw_len);
    REQUIRE(raw != nullptr);

    std::vector<uint8_t> image;
    PngRowDecoder rows(h, palette, [&](const uint8_t* rgba) {
        image.insert(image.end(), rgba, rgba + static_cast<size_t>(h.width) * 4);
    });
    for (int at = 0; at < raw_len; at += static_cast<int>(piece)) {
        const size_t n = std::min(piece, static_cast<size_t>(raw_len - at));
        REQUIRE(rows.feed(reinterpret_cast<const uint8_t*>(raw) + at, n));
    }
    stbi_image_free(raw);
    CHECK(rows.complete());
    return image;
}

std::vector<uint8_t> stb_decode(const std::vector<uint8_t>& png) {
    int w = 0, h = 0, n = 0;
    uint8_t* px = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &n, 4);
    REQUIRE(px != nullptr);
    std::vector<uint8_t> out(px, px + static_cast<size_t>(w) * h * 4);
    stbi_image_free(px);
    return out;
}

} // namespace

TEST_CASE("the row decoder matches a whole-image decode for every filter and colour type",
          "[thumbnail][png_stream]") {
    // Each fixture cycles its rows through all five PNG filter types.
    for (const char* name : {"thumb_filters_rgba.png", "thumb_filters_rgb_key.png",
                             "thumb_filters_grey_alpha.png", "thumb_filters_palette.png"}) {
        CAPTURE(name);
        const auto png = fixture(name);
        REQUIRE(!png.empty());
        for (size_t piece : {size_t{1}, size_t{7}, size_t{4096}}) {
            CAPTURE(piece);
            PngHeader h;
            CHECK(stream_decode(png, piece, h) == stb_decode(png));
        }
    }
}

TEST_CASE("decoding row by row into the downscale equals downscaling the whole image",
          "[thumbnail][png_stream]") {
    const auto png = fixture("thumbnail_300_rgba.png");
    PngHeader h;
    const auto image = stream_decode(png, 32768, h);
    const helix::ThumbnailDims dims = helix::fit_thumbnail(h.width, h.height, 166, 166);

    std::vector<uint8_t> whole(helix::rgb565a8_size(dims));
    helix::downscale_rgba_to_rgb565a8(stb_decode(png).data(), h.width, h.height, dims,
                                      whole.data());

    std::vector<uint8_t> streamed(helix::rgb565a8_size(dims), 0xAA);
    helix::RowDownscaler scaler(h.width, h.height, dims, streamed.data());
    for (int y = 0; y < h.height; ++y) {
        scaler.add_row(image.data() + static_cast<size_t>(y) * h.width * 4);
    }
    CHECK(scaler.complete());
    CHECK(streamed == whole);
}

TEST_CASE("only 8-bit, non-interlaced thumbnails within the size cap are decoded",
          "[thumbnail][png_stream]") {
    auto header = [](int w, int h, int depth, int color, int interlace) {
        PngHeader p;
        p.width = w;
        p.height = h;
        p.bit_depth = depth;
        p.color_type = color;
        p.interlace = interlace;
        return p;
    };
    for (int color : {0, 2, 3, 4, 6}) {
        CAPTURE(color);
        CHECK(helix::png_thumbnail_supported(header(300, 300, 8, color, 0)));
    }
    CHECK(helix::png_thumbnail_supported(header(helix::THUMBNAIL_MAX_SIDE, 10, 8, 6, 0)));
    CHECK_FALSE(helix::png_thumbnail_supported(header(helix::THUMBNAIL_MAX_SIDE + 1, 10, 8, 6, 0)));
    CHECK_FALSE(helix::png_thumbnail_supported(header(300, 300, 16, 6, 0)));
    CHECK_FALSE(helix::png_thumbnail_supported(header(300, 300, 4, 3, 0)));
    CHECK_FALSE(helix::png_thumbnail_supported(header(300, 300, 8, 6, 1)));
    CHECK_FALSE(helix::png_thumbnail_supported(header(300, 300, 8, 5, 0)));
    CHECK_FALSE(helix::png_thumbnail_supported(header(0, 300, 8, 6, 0)));
}

TEST_CASE("a cut-short PNG or a bad filter byte fails the decode", "[thumbnail][png_stream]") {
    const auto png = fixture("thumb_filters_rgba.png");
    PngPalette palette;
    const std::vector<uint8_t> cut(png.begin(), png.begin() + static_cast<long>(png.size() / 2));
    CHECK_FALSE(helix::for_each_png_idat(cut.data(), cut.size(), palette,
                                         [](const uint8_t*, size_t) { return true; }));

    PngHeader h;
    REQUIRE(helix::read_png_header(png.data(), png.size(), h));
    PngRowDecoder rows(h, palette, [](const uint8_t*) {});
    const uint8_t bad_filter = 5;
    CHECK_FALSE(rows.feed(&bad_filter, 1));
    CHECK_FALSE(rows.complete());
}
