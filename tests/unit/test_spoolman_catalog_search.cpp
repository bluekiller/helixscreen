// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_error.h"
#include "spoolman_catalog_search.h"

#include "../catch_amalgamated.hpp"

using helix::SpoolmanCatalogSearch;
using Availability = helix::SpoolmanCatalogSearch::Availability;

namespace {

struct CacheReset {
    CacheReset() {
        SpoolmanCatalogSearch::reset_cache();
    }
    ~CacheReset() {
        SpoolmanCatalogSearch::reset_cache();
    }
};

MoonrakerError error_with_code(int code) {
    MoonrakerError err = MoonrakerError::json_rpc_error("server.spoolman.proxy", "x");
    err.code = code;
    return err;
}

} // namespace

TEST_CASE("SpoolmanCatalogSearch caches availability per connection", "[spoolman_db]") {
    CacheReset reset;

    CHECK(SpoolmanCatalogSearch::availability(5) == Availability::Unknown);

    SECTION("a successful search makes it available on that connection only") {
        SpoolmanCatalogSearch::record_success(5);
        CHECK(SpoolmanCatalogSearch::availability(5) == Availability::Available);
        CHECK(SpoolmanCatalogSearch::availability(6) == Availability::Unknown);
    }

    SECTION("a 404 makes it unavailable") {
        SpoolmanCatalogSearch::record_error(5, error_with_code(404));
        CHECK(SpoolmanCatalogSearch::availability(5) == Availability::Unavailable);
    }

    SECTION("any other error proves nothing about the route") {
        SpoolmanCatalogSearch::record_success(5);
        SpoolmanCatalogSearch::record_error(5, error_with_code(500));
        CHECK(SpoolmanCatalogSearch::availability(5) == Availability::Available);

        SpoolmanCatalogSearch::record_error(7, MoonrakerError::connection_lost("x"));
        CHECK(SpoolmanCatalogSearch::availability(7) == Availability::Unknown);
    }

    SECTION("a new connection asks again") {
        SpoolmanCatalogSearch::record_error(5, error_with_code(404));
        CHECK(SpoolmanCatalogSearch::availability(6) == Availability::Unknown);
        SpoolmanCatalogSearch::record_success(6);
        CHECK(SpoolmanCatalogSearch::availability(6) == Availability::Available);
    }
}

TEST_CASE("SpoolmanCatalogSearch sends queries of two characters or more", "[spoolman_db]") {
    CHECK_FALSE(SpoolmanCatalogSearch::is_searchable(""));
    CHECK_FALSE(SpoolmanCatalogSearch::is_searchable("p"));
    CHECK_FALSE(SpoolmanCatalogSearch::is_searchable("  p  "));
    CHECK(SpoolmanCatalogSearch::is_searchable("po"));
}

TEST_CASE("SpoolmanCatalogSearch tickets go stale when a newer query starts", "[spoolman_db]") {
    SpoolmanCatalogSearch search;
    const uint64_t older = search.begin();
    CHECK(search.is_current(older));

    const uint64_t newer = search.begin();
    CHECK_FALSE(search.is_current(older));
    CHECK(search.is_current(newer));

    search.invalidate();
    CHECK_FALSE(search.is_current(newer));
}

TEST_CASE("find_matching_filament is the one Spoolman filament match", "[spoolman_db]") {
    using helix::spoolman::find_matching_filament;
    using helix::spoolman::normalize_color_hex;

    FilamentInfo red;
    red.id = 4;
    red.material = "PLA";
    red.color_hex = "ff0000";
    FilamentInfo clear;
    clear.id = 9;
    clear.material = "PETG";
    clear.color_hex = "FFFFFF33";
    const std::vector<FilamentInfo> filaments{red, clear};

    SECTION("colour compares without case or '#'") {
        const FilamentInfo* f = find_matching_filament(filaments, "PLA", "#FF0000");
        REQUIRE(f != nullptr);
        CHECK(f->id == 4);
    }

    SECTION("an alpha channel is part of the colour") {
        const FilamentInfo* f = find_matching_filament(filaments, "PETG", "ffffff33");
        REQUIRE(f != nullptr);
        CHECK(f->id == 9);
        CHECK(find_matching_filament(filaments, "PETG", "FFFFFF") == nullptr);
    }

    SECTION("material must match") {
        CHECK(find_matching_filament(filaments, "PETG", "FF0000") == nullptr);
    }

    SECTION("no colour matches nothing") {
        CHECK(find_matching_filament(filaments, "PLA", "") == nullptr);
        CHECK(normalize_color_hex("red").empty());
        CHECK(normalize_color_hex("12345").empty());
    }
}
