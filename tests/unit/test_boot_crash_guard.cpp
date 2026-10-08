// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_boot_crash_guard.cpp
 * @brief A run of crash resets stops boot from reconnecting to the printer behind them.
 */

#include "boot_crash_guard.h"

#include "../catch_amalgamated.hpp"

using helix::BOOT_CRASH_FALLBACK_THRESHOLD;
using helix::choose_boot_printer;

namespace {
const std::vector<std::string> kPrinters = {"voron", "ender"};
}

TEST_CASE("A deliberate restart or power-on leaves the crash streak alone", "[boot_crash_guard]") {
    const auto choice =
        choose_boot_printer(false, BOOT_CRASH_FALLBACK_THRESHOLD - 1, "ender", "voron", kPrinters);
    CHECK(choice.crash_streak == BOOT_CRASH_FALLBACK_THRESHOLD - 1);
    CHECK(choice.fallback_id.empty());
    CHECK(choice.auto_connect);
}

TEST_CASE("A crash below the threshold counts and still connects", "[boot_crash_guard]") {
    const auto choice = choose_boot_printer(true, 0, "ender", "voron", kPrinters);
    CHECK(choice.crash_streak == 1);
    CHECK(choice.fallback_id.empty());
    CHECK(choice.auto_connect);

    const auto second =
        choose_boot_printer(true, BOOT_CRASH_FALLBACK_THRESHOLD - 2, "ender", "voron", kPrinters);
    CHECK(second.crash_streak == BOOT_CRASH_FALLBACK_THRESHOLD - 1);
    CHECK(second.fallback_id.empty());
    CHECK(second.auto_connect);
}

TEST_CASE("The threshold crash falls back to the previous printer", "[boot_crash_guard]") {
    const auto choice =
        choose_boot_printer(true, BOOT_CRASH_FALLBACK_THRESHOLD - 1, "ender", "voron", kPrinters);
    CHECK(choice.crash_streak == 0);
    CHECK(choice.fallback_id == "voron");
    CHECK(choice.auto_connect);
}

TEST_CASE("With no printer to go back to, the threshold crash stops auto-connect",
          "[boot_crash_guard]") {
    const int at = BOOT_CRASH_FALLBACK_THRESHOLD - 1;

    SECTION("no previous printer") {
        const auto choice = choose_boot_printer(true, at, "ender", "", kPrinters);
        CHECK(choice.fallback_id.empty());
        CHECK_FALSE(choice.auto_connect);
        CHECK(choice.crash_streak == 0);
    }
    SECTION("previous printer was removed") {
        const auto choice = choose_boot_printer(true, at, "ender", "prusa", kPrinters);
        CHECK(choice.fallback_id.empty());
        CHECK_FALSE(choice.auto_connect);
    }
    SECTION("previous printer is the one crashing") {
        const auto choice = choose_boot_printer(true, at, "ender", "ender", kPrinters);
        CHECK(choice.fallback_id.empty());
        CHECK_FALSE(choice.auto_connect);
    }
}

TEST_CASE("A streak past the threshold still falls back", "[boot_crash_guard]") {
    const auto choice =
        choose_boot_printer(true, BOOT_CRASH_FALLBACK_THRESHOLD + 4, "ender", "voron", kPrinters);
    CHECK(choice.fallback_id == "voron");
    CHECK(choice.crash_streak == 0);
}
