// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The thumbnail decisions every backend shares (thumbnail_rules.h), and the
// writers that must apply them: nothing the cache names `.png` may hold
// anything but PNG bytes.

#include "../helix_test_fixture.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/thumbnail_processor_test_access.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "test_helpers/unique_temp_dir.h"
#include "thumbnail_cache.h"
#include "thumbnail_processor.h"
#include "thumbnail_rules.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ImageFormat;

namespace {

std::vector<uint8_t> read_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

const char* kPngAsset = "assets/images/benchy_thumbnail_white.png";
const char* kJpegAsset = "assets/test_timelapse/benchy_timelapse_20260310.thumb.jpg";

std::vector<uint8_t> qoi_bytes() {
    // "qoif", 1x1, RGBA, sRGB, one QOI_OP_RGBA pixel, then the end marker.
    return {'q',  'o', 'i', 'f', 0, 0, 0, 1, 0, 0, 0, 1, 4, 0,
            0xFF, 1,   2,   3,   4, 0, 0, 0, 0, 0, 0, 0, 1};
}

/// A scratch directory removed on scope exit.
struct ScratchDir {
    explicit ScratchDir(const std::string& prefix) : path(helix::test::unique_temp_dir(prefix)) {
        std::filesystem::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::string path;
};

} // namespace

TEST_CASE("sniff_image_format reads the magic bytes, not the name", "[thumbnail][rules]") {
    CHECK(helix::sniff_image_format(read_bytes(kPngAsset)) == ImageFormat::Png);
    CHECK(helix::sniff_image_format(read_bytes(kJpegAsset)) == ImageFormat::Jpeg);
    CHECK(helix::sniff_image_format(qoi_bytes()) == ImageFormat::Qoi);

    CHECK(helix::sniff_image_format(std::vector<uint8_t>{}) == ImageFormat::Unknown);
    CHECK(helix::sniff_image_format(nullptr, 64) == ImageFormat::Unknown);
    // A signature cut short is not that format.
    CHECK(helix::sniff_image_format(std::vector<uint8_t>{0x89, 'P', 'N', 'G'}) ==
          ImageFormat::Unknown);
    CHECK(helix::sniff_image_format(std::vector<uint8_t>{0xFF, 0xD8}) == ImageFormat::Unknown);
    CHECK(helix::sniff_image_format(std::string_view("<html>404</html>")) == ImageFormat::Unknown);
}

TEST_CASE("is_complete_image wants the format's end marker", "[thumbnail][rules]") {
    const auto png = read_bytes(kPngAsset);
    const auto jpeg = read_bytes(kJpegAsset);
    REQUIRE(png.size() > 64);
    REQUIRE(jpeg.size() > 64);

    CHECK(helix::is_complete_image(png));
    CHECK(helix::is_complete_image(jpeg));

    CHECK_FALSE(helix::is_complete_image({png.begin(), png.begin() + png.size() / 2}));
    CHECK_FALSE(helix::is_complete_image({jpeg.begin(), jpeg.begin() + jpeg.size() / 2}));
    // QOI is never decoded here, complete or not.
    CHECK_FALSE(helix::is_complete_image(qoi_bytes()));
}

TEST_CASE("ensure_png passes PNG, re-encodes JPEG, refuses the rest", "[thumbnail][rules]") {
    const auto png = read_bytes(kPngAsset);
    CHECK(helix::ensure_png(png) == png);

    const auto from_jpeg = helix::ensure_png(read_bytes(kJpegAsset));
    REQUIRE_FALSE(from_jpeg.empty());
    CHECK(helix::sniff_image_format(from_jpeg) == ImageFormat::Png);
    CHECK(helix::is_complete_image(from_jpeg));

    CHECK(helix::ensure_png(qoi_bytes()).empty());
    CHECK(helix::ensure_png({0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46}).empty());
    CHECK(helix::ensure_png({}).empty());
}

TEST_CASE("save_raw_png stores a JPEG thumbnail as a real PNG", "[thumbnail][rules][cache]") {
    ThumbnailCache& cache = get_thumbnail_cache();
    const std::string id = "rules_jpeg_" + helix::test::unique_suffix();

    const std::string saved = cache.save_raw_png(id, read_bytes(kJpegAsset));
    REQUIRE_FALSE(saved.empty());
    CHECK(helix::sniff_image_format(read_bytes(saved.substr(2))) == ImageFormat::Png);
    cache.invalidate(id);

    const std::string qoi_id = "rules_qoi_" + helix::test::unique_suffix();
    CHECK(cache.save_raw_png(qoi_id, qoi_bytes()).empty());
    CHECK_FALSE(std::filesystem::exists(cache.get_cache_path(qoi_id)));
}

TEST_CASE_METHOD(LVGLTestFixture, "Mock thumbnail download writes PNG bytes for a JPEG source",
                 "[thumbnail][rules][mock]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    ScratchDir dir("rules_mock_dl");
    const std::string cache_path = dir.path + "/thumb.png";

    bool ok = false;
    api.transfers().download_thumbnail(
        kJpegAsset, cache_path, [&](const std::string&) { ok = true; },
        [](const MoonrakerError&) {});
    REQUIRE(ok);
    CHECK(helix::sniff_image_format(read_bytes(cache_path)) == ImageFormat::Png);
}

TEST_CASE_METHOD(HelixTestFixture, "ThumbnailProcessor pre-scales a JPEG thumbnail",
                 "[thumbnail][rules][processor]") {
    ScratchDir dir("rules_proc_jpeg");
    auto* proc = ThumbnailProcessorTestAccess::make();
    proc->set_cache_dir(dir.path);
    helix::ThumbnailTarget target;
    target.width = 120;
    target.height = 120;

    const auto jpeg = read_bytes(kJpegAsset);
    auto ok = proc->process_sync(jpeg, "timelapse.mp4", target);
    CHECK(ok.success);
    CHECK(ok.error.empty());

    // A cut JPEG never reaches the decoder.
    auto cut =
        proc->process_sync({jpeg.begin(), jpeg.begin() + jpeg.size() / 2}, "cut.mp4", target);
    CHECK_FALSE(cut.success);

    ThumbnailProcessorTestAccess::destroy(proc);
}
