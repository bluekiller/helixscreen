// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "card_thumbnail_plan.h"

#include <vector>

#include "../catch_amalgamated.hpp"

using helix::CardThumbnailPlan;
using helix::CardThumbnailState;
using helix::plan_card_thumbnails;

namespace {

constexpr size_t KB = 1024;
constexpr size_t EST = 80 * KB; // one card's image

std::vector<CardThumbnailState> files(size_t n) {
    std::vector<CardThumbnailState> v(n);
    for (auto& f : v) {
        f.fetchable = true;
    }
    return v;
}

using Indices = std::vector<size_t>;

} // namespace

TEST_CASE("cards in the window are fetched in order, within the budget", "[card_thumbnail_plan]") {
    const auto f = files(30);
    const CardThumbnailPlan plan = plan_card_thumbnails(f, 8, 20, 0, EST, 5 * EST);
    CHECK(plan.fetch == Indices{8, 9, 10, 11, 12}); // 5 fit; the rest wait
    CHECK(plan.drop.empty());
}

TEST_CASE("held and in-flight thumbnails count against the budget", "[card_thumbnail_plan]") {
    auto f = files(10);
    f[0].held = EST;
    f[1].tried = true; // in flight
    const CardThumbnailPlan plan = plan_card_thumbnails(f, 0, 10, /*in_flight=*/1, EST, 4 * EST);
    // 80K held + 80K in flight leaves room for two more.
    CHECK(plan.fetch == Indices{2, 3});
}

TEST_CASE("wide thumbnails smaller than a slot still take a whole slot", "[card_thumbnail_plan]") {
    // A 16:9 image fills a third of the card box, but decodes into a full slot;
    // counting its bytes would plan more decodes than the pool has slots.
    const size_t slots = 12;
    auto f = files(30);
    for (size_t i = 0; i < 6; ++i) {
        f[i].held = EST / 3;
    }
    const CardThumbnailPlan plan = plan_card_thumbnails(f, 0, 30, 0, EST, slots * EST);
    CHECK(plan.fetch.size() == slots - 6);
}

TEST_CASE("after a lane refusal, re-planning fetches nothing until a slot frees",
          "[card_thumbnail_plan]") {
    // The lane refused card 2; card 1 is still in flight. A listing re-sync
    // re-plans the same window with no completion in between.
    auto f = files(10);
    f[0].held = EST;
    f[1].tried = true;
    const CardThumbnailPlan resync =
        plan_card_thumbnails(f, 0, 10, /*in_flight=*/1, EST, 12 * EST, /*lane_refused=*/true);
    CHECK(resync.fetch.empty());

    // Cards that left the window still go: a refusal holds back fetches only.
    const CardThumbnailPlan scrolled = plan_card_thumbnails(f, 4, 10, 1, EST, 12 * EST, true);
    CHECK(scrolled.drop == Indices{0, 1});
    CHECK(scrolled.fetch.empty());

    // A completion frees a slot and clears the refusal: card 2 is fetched again.
    f[1].tried = false;
    f[1].held = EST;
    const CardThumbnailPlan freed = plan_card_thumbnails(f, 0, 10, 0, EST, 12 * EST, false);
    CHECK(freed.fetch.front() == 2);
}

TEST_CASE("a window already over budget starts nothing, even a backlog of refused fetches",
          "[card_thumbnail_plan]") {
    // Nine cards whose fetches the lane refused earlier come back untried; with
    // a 220px target each is ~145KB, and the budget holds only six.
    auto f = files(9);
    const size_t big = 220 * 220 * 3;
    const CardThumbnailPlan first_pass = plan_card_thumbnails(f, 0, 9, 0, big, 960 * KB);
    CHECK(first_pass.fetch.size() == 6);

    for (size_t i : first_pass.fetch) {
        f[i].held = big; // they all landed
    }
    const CardThumbnailPlan second_pass = plan_card_thumbnails(f, 0, 9, 0, big, 960 * KB);
    CHECK(second_pass.fetch.empty());

    // In flight counts the same as held.
    const CardThumbnailPlan in_flight_pass =
        plan_card_thumbnails(files(9), 0, 9, /*in_flight=*/7, big, 960 * KB);
    CHECK(in_flight_pass.fetch.empty());
}

TEST_CASE("cards leaving the window are dropped, and only those that hold or tried something",
          "[card_thumbnail_plan]") {
    auto f = files(12);
    f[1].held = EST;       // scrolled out, holding
    f[2].tried = true;     // scrolled out, fetch in flight or failed
    f[3].fetchable = true; // scrolled out, never touched
    f[6].held = EST;       // still on screen
    const CardThumbnailPlan plan = plan_card_thumbnails(f, 4, 12, 0, EST, 960 * KB);
    CHECK(plan.drop == Indices{1, 2});
    // On-screen bytes alone count: the dropped 80K does not block a fetch.
    CHECK(plan.fetch == Indices{4, 5, 7, 8, 9, 10, 11});
}

TEST_CASE("directories, tried and holding files are not fetched", "[card_thumbnail_plan]") {
    auto f = files(4);
    f[0].fetchable = false; // a directory, or a file with no thumbnail
    f[1].tried = true;      // failed while on screen: not retried until it leaves
    f[2].held = EST;
    const CardThumbnailPlan plan = plan_card_thumbnails(f, 0, 4, 0, EST, 960 * KB);
    CHECK(plan.fetch == Indices{3});
}

TEST_CASE("a window past the list end is clamped", "[card_thumbnail_plan]") {
    const auto f = files(3);
    CHECK(plan_card_thumbnails(f, 2, 50, 0, EST, 960 * KB).fetch == Indices{2});
    CHECK(plan_card_thumbnails(f, 40, 50, 0, EST, 960 * KB).fetch.empty());
    CHECK(plan_card_thumbnails({}, 0, 10, 0, EST, 960 * KB).fetch.empty());
}
