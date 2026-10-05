// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_gcode_preview_fetcher.cpp
 * @brief GcodePreviewFetcher::ensure_local: one transfer per path, every waiter
 *        told, and a copy trusted only at the server's size.
 *
 * Downloads are held by the transfer mock until the test releases them, so
 * callers can arrive before, during and after a transfer.
 */

#include "ui_update_queue.h"

#include "../helix_test_fixture.h"
#include "../test_helpers/print_status_preview_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "gcode_preview_fetcher.h"

#include <fstream>
#include <string>
#include <vector>

using helix::ui::GcodePreviewFetcher;
using print_status_preview_test::HeldFileTransfers;
using print_status_preview_test::HeldTransfersAPIMock;

namespace {

constexpr const char* REMOTE = "pause_markers_demo.gcode";

class FetcherFixture : public HelixTestFixture {
  public:
    FetcherFixture() : client_(MoonrakerClientMock::PrinterType::VORON_24) {
        state_.init_subjects(false);
        api_ = std::make_unique<HeldTransfersAPIMock>(client_, state_, transfers_);
        fetcher_.set_api(api_.get());
        local_ = (cache_.dir / "copy.gcode").string();
    }

    ~FetcherFixture() override {
        drain();
        state_.deinit_subjects();
    }

    void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    void ensure(uint64_t expected, std::vector<std::string>& ready,
                std::vector<GcodePreviewFetcher::Source>& sources, int& failed) {
        fetcher_.ensure_local(
            "gcodes", REMOTE, local_, expected,
            [&](const std::string& path, GcodePreviewFetcher::Source source) {
                ready.push_back(path);
                sources.push_back(source);
            },
            [&](GcodePreviewFetcher::Unavailable) { ++failed; });
    }

    print_status_preview_test::FreshCacheDir cache_;
    MoonrakerClientMock client_;
    helix::PrinterState state_;
    HeldFileTransfers transfers_{client_, ""};
    std::unique_ptr<MoonrakerAPIMock> api_;
    GcodePreviewFetcher fetcher_{"FetcherTest"};
    std::string local_;
};

} // namespace

TEST_CASE_METHOD(FetcherFixture, "Fetcher: callers during a download join it and are all told",
                 "[gcode_preview_fetcher]") {
    std::vector<std::string> ready;
    std::vector<GcodePreviewFetcher::Source> sources;
    int failed = 0;

    ensure(0, ready, sources, failed);
    ensure(0, ready, sources, failed);
    REQUIRE(transfers_.held_count() == 1);
    CHECK(fetcher_.is_downloading(local_));

    REQUIRE(transfers_.release(REMOTE));
    drain();

    CHECK(ready.size() == 2);
    CHECK(sources ==
          std::vector<GcodePreviewFetcher::Source>(2, GcodePreviewFetcher::Source::Download));
    CHECK(failed == 0);
    CHECK_FALSE(fetcher_.is_downloading(local_));
}

TEST_CASE_METHOD(FetcherFixture, "Fetcher: a copy of the expected size is used without a transfer",
                 "[gcode_preview_fetcher]") {
    {
        std::ofstream copy(local_, std::ios::binary);
        copy << "12345";
    }
    std::vector<std::string> ready;
    std::vector<GcodePreviewFetcher::Source> sources;
    int failed = 0;

    ensure(5, ready, sources, failed);

    CHECK(transfers_.held_count() == 0);
    REQUIRE(ready.size() == 1);
    CHECK(sources[0] == GcodePreviewFetcher::Source::Cache);
}

TEST_CASE_METHOD(FetcherFixture, "Fetcher: a copy of the wrong size is downloaded again",
                 "[gcode_preview_fetcher]") {
    {
        std::ofstream copy(local_, std::ios::binary);
        copy << "12345";
    }
    std::vector<std::string> ready;
    std::vector<GcodePreviewFetcher::Source> sources;
    int failed = 0;

    ensure(999, ready, sources, failed);

    CHECK(transfers_.held_count() == 1);
    CHECK(ready.empty());
}

TEST_CASE_METHOD(FetcherFixture, "Fetcher: a failed download tells every waiter",
                 "[gcode_preview_fetcher]") {
    std::vector<std::string> ready;
    std::vector<GcodePreviewFetcher::Source> sources;
    int failed = 0;

    fetcher_.ensure_local(
        "gcodes", "no_such_file_anywhere.gcode", local_, 0,
        [&](const std::string& path, GcodePreviewFetcher::Source) { ready.push_back(path); },
        [&](GcodePreviewFetcher::Unavailable) { ++failed; });
    fetcher_.ensure_local(
        "gcodes", "no_such_file_anywhere.gcode", local_, 0,
        [&](const std::string& path, GcodePreviewFetcher::Source) { ready.push_back(path); },
        [&](GcodePreviewFetcher::Unavailable) { ++failed; });
    REQUIRE(transfers_.held_count() == 1);

    REQUIRE(transfers_.release("no_such_file_anywhere.gcode"));
    drain();

    CHECK(failed == 2);
    CHECK(ready.empty());
}

TEST_CASE_METHOD(FetcherFixture, "Fetcher: a cancelled caller is not told, a later one is",
                 "[gcode_preview_fetcher]") {
    std::vector<std::string> ready;
    std::vector<GcodePreviewFetcher::Source> sources;
    int failed = 0;

    ensure(0, ready, sources, failed);
    fetcher_.cancel();
    ensure(0, ready, sources, failed);
    REQUIRE(transfers_.held_count() == 1);

    REQUIRE(transfers_.release(REMOTE));
    drain();

    CHECK(ready.size() == 1);
}

TEST_CASE_METHOD(FetcherFixture,
                 "Fetcher: a fetch during a download never trusts the partial file on disk",
                 "[gcode_preview_fetcher]") {
    std::vector<std::string> first_ready;
    std::vector<std::string> second_ready;
    int unavailable = 0;
    auto ready_into = [](std::vector<std::string>& into) {
        return [&into](const std::string& path) { into.push_back(path); };
    };

    fetcher_.fetch(REMOTE, ready_into(first_ready),
                   [&](GcodePreviewFetcher::Unavailable) { ++unavailable; });
    drain();
    REQUIRE(transfers_.held_count() == 1);

    // The running transfer has written part of the file.
    const auto partial =
        cache_.dir / "gcode_temp" / GcodePreviewFetcher::cache_file_name("print_view_", REMOTE);
    {
        std::ofstream copy(partial, std::ios::binary);
        copy << "partial";
    }

    // With the size lookup failing, a cached copy would be the fallback; the
    // partial file must not pass for one.
    helix::ScopedEnv metadata_fails("HELIX_MOCK_METADATA_404", "1");
    fetcher_.fetch(REMOTE, ready_into(second_ready),
                   [&](GcodePreviewFetcher::Unavailable) { ++unavailable; });
    drain();

    CHECK(second_ready.empty());
    CHECK(unavailable == 0);
    CHECK(transfers_.held_count() == 1);

    REQUIRE(transfers_.release(REMOTE));
    drain();
    CHECK(first_ready.empty());
    CHECK(second_ready.size() == 1);
}
