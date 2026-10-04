// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_display_idle_decision.cpp
 * @brief Truth table for decide_idle(): the dim / saver / sleep / wake tick.
 *
 * Each row is one tick's inputs and the action it must produce. Row labels name
 * the rule the row pins.
 */

#include "display_idle_decision.h"

#include <vector>

#include "../../catch_amalgamated.hpp"

using helix::IdleAction;
using helix::IdleInputs;
using helix::IdleState;

namespace {

struct Row {
    const char* label;
    IdleInputs in;
    IdleAction want;
};

IdleInputs make(IdleState st, uint32_t inactive_ms, int dim_s, int sleep_s) {
    IdleInputs in;
    in.state = st;
    in.inactive_ms = inactive_ms;
    in.dim_timeout_sec = dim_s;
    in.sleep_timeout_sec = sleep_s;
    return in;
}

template <typename F> IdleInputs with(IdleInputs in, F f) {
    f(in);
    return in;
}

} // namespace

TEST_CASE("decide_idle truth table", "[display][idle_decision]") {
    const auto A = IdleState::Awake;
    const auto D = IdleState::Dimmed;
    const auto S = IdleState::Sleeping;

    const std::vector<Row> rows = {
        // --- Awake: timeouts ---
        {"awake, fresh activity", with(make(A, 0, 60, 120), [](auto& i) { i.can_dim = true; }),
         IdleAction::None},
        {"awake, below dim", with(make(A, 59999, 60, 120), [](auto& i) { i.can_dim = true; }),
         IdleAction::None},
        {"awake, exactly at dim, backlight",
         with(make(A, 60000, 60, 120), [](auto& i) { i.can_dim = true; }), IdleAction::Dim},
        {"awake, past dim, backlight",
         with(make(A, 90000, 60, 120), [](auto& i) { i.can_dim = true; }), IdleAction::Dim},
        {"awake, exactly at sleep",
         with(make(A, 120000, 60, 120), [](auto& i) { i.can_dim = true; }), IdleAction::Sleep},
        {"awake, one ms below sleep",
         with(make(A, 119999, 60, 120), [](auto& i) { i.can_dim = true; }), IdleAction::Dim},
        // --- Awake: dim 0 / sleep 0 ---
        {"dim 0, past dim, no sleep",
         with(make(A, 999999, 0, 0), [](auto& i) { i.can_dim = true; }), IdleAction::None},
        {"dim 0, sleep set, below sleep",
         with(make(A, 50000, 0, 120), [](auto& i) { i.can_dim = true; }), IdleAction::None},
        {"dim 0, sleep set, at sleep",
         with(make(A, 120000, 0, 120), [](auto& i) { i.can_dim = true; }), IdleAction::Sleep},
        {"sleep 0, dim set, at dim", with(make(A, 60000, 60, 0), [](auto& i) { i.can_dim = true; }),
         IdleAction::Dim},
        {"sleep 0, dim set, very late",
         with(make(A, 4000000, 60, 0), [](auto& i) { i.can_dim = true; }), IdleAction::Dim},
        // --- Awake: sleep < dim (sleep is checked first) ---
        {"sleep<dim, at sleep", with(make(A, 30000, 60, 30), [](auto& i) { i.can_dim = true; }),
         IdleAction::Sleep},
        {"sleep<dim, below sleep", with(make(A, 29999, 60, 30), [](auto& i) { i.can_dim = true; }),
         IdleAction::None},
        {"sleep<dim, past both", with(make(A, 70000, 60, 30), [](auto& i) { i.can_dim = true; }),
         IdleAction::Sleep},
        // --- Awake: can_dim / has_screensaver ---
        {"past dim, neither dim nor saver", make(A, 60000, 60, 0), IdleAction::None},
        {"past dim, saver only, not running",
         with(make(A, 60000, 60, 0), [](auto& i) { i.has_screensaver = true; }),
         IdleAction::StartSaver},
        {"past dim, saver and backlight, not running",
         with(make(A, 60000, 60, 0),
              [](auto& i) {
                  i.has_screensaver = true;
                  i.can_dim = true;
              }),
         IdleAction::StartSaver},
        {"past dim, saver already running falls to Dim",
         with(make(A, 60000, 60, 0),
              [](auto& i) {
                  i.has_screensaver = true;
                  i.can_dim = true;
                  i.saver_running = true;
              }),
         IdleAction::Dim},
        {"saver only, no sleep timeout, sleep does not preempt",
         with(make(A, 60000, 60, 0), [](auto& i) { i.has_screensaver = true; }),
         IdleAction::StartSaver},
        {"saver only, at sleep",
         with(make(A, 120000, 60, 120), [](auto& i) { i.has_screensaver = true; }),
         IdleAction::Sleep},
        // --- Awake: inhibit ---
        {"awake, inhibited, past dim",
         with(make(A, 90000, 60, 0),
              [](auto& i) {
                  i.can_dim = true;
                  i.inhibit_entry = true;
              }),
         IdleAction::None},
        {"awake, inhibited, past sleep",
         with(make(A, 999999, 60, 120),
              [](auto& i) {
                  i.can_dim = true;
                  i.inhibit_entry = true;
              }),
         IdleAction::None},
        // --- Awake: wake flags are irrelevant ---
        {"awake, wake_requested ignored",
         with(make(A, 0, 60, 120), [](auto& i) { i.wake_requested = true; }), IdleAction::None},
        {"awake, host_resumed ignored",
         with(make(A, 0, 60, 120), [](auto& i) { i.host_resumed = true; }), IdleAction::None},
        // --- Dimmed ---
        {"dimmed, idle, below sleep", make(D, 90000, 60, 120), IdleAction::None},
        {"dimmed, at sleep", make(D, 120000, 60, 120), IdleAction::Sleep},
        {"dimmed, sleep 0, very late", make(D, 4000000, 60, 0), IdleAction::None},
        {"dimmed, activity 499ms", make(D, 499, 60, 120), IdleAction::Wake},
        {"dimmed, 500ms is no longer activity", make(D, 500, 60, 120), IdleAction::None},
        {"dimmed, wake_requested",
         with(make(D, 90000, 60, 120), [](auto& i) { i.wake_requested = true; }), IdleAction::Wake},
        {"dimmed, inhibited, at sleep",
         with(make(D, 120000, 60, 120), [](auto& i) { i.inhibit_entry = true; }), IdleAction::None},
        {"dimmed, inhibited, activity still wakes",
         with(make(D, 100, 60, 120), [](auto& i) { i.inhibit_entry = true; }), IdleAction::Wake},
        {"dimmed, host_resumed alone does not wake",
         with(make(D, 90000, 60, 120), [](auto& i) { i.host_resumed = true; }), IdleAction::None},
        // --- Dimmed: preview grace ---
        {"preview, activity at 749ms is ignored",
         with(make(D, 100, 60, 120),
              [](auto& i) {
                  i.is_preview = true;
                  i.preview_elapsed_ms = 749;
              }),
         IdleAction::None},
        {"preview, activity at 750ms dismisses",
         with(make(D, 100, 60, 120),
              [](auto& i) {
                  i.is_preview = true;
                  i.preview_elapsed_ms = 750;
              }),
         IdleAction::Wake},
        {"preview, wake_requested wakes inside the grace",
         with(make(D, 100, 60, 120),
              [](auto& i) {
                  i.is_preview = true;
                  i.preview_elapsed_ms = 10;
                  i.wake_requested = true;
              }),
         IdleAction::Wake},
        {"non-preview, activity at 10ms wakes",
         with(make(D, 100, 60, 120), [](auto& i) { i.preview_elapsed_ms = 10; }), IdleAction::Wake},
        // --- Sleeping ---
        {"sleeping, idle", make(S, 999999, 60, 120), IdleAction::None},
        {"sleeping, activity", make(S, 100, 60, 120), IdleAction::Wake},
        {"sleeping, 500ms is not activity", make(S, 500, 60, 120), IdleAction::None},
        {"sleeping, wake_requested",
         with(make(S, 999999, 60, 120), [](auto& i) { i.wake_requested = true; }),
         IdleAction::Wake},
        {"sleeping, host_resumed",
         with(make(S, 999999, 60, 120), [](auto& i) { i.host_resumed = true; }), IdleAction::Wake},
        {"sleeping, inhibited, wake_requested is honoured",
         with(make(S, 999999, 60, 120),
              [](auto& i) {
                  i.inhibit_entry = true;
                  i.wake_requested = true;
              }),
         IdleAction::Wake},
        {"sleeping, inhibited, activity is honoured",
         with(make(S, 10, 60, 120), [](auto& i) { i.inhibit_entry = true; }), IdleAction::Wake},
        {"sleeping, inhibited, idle stays asleep",
         with(make(S, 999999, 60, 120), [](auto& i) { i.inhibit_entry = true; }), IdleAction::None},
        {"sleeping, preview flag ignored",
         with(make(S, 100, 60, 120),
              [](auto& i) {
                  i.is_preview = true;
                  i.preview_elapsed_ms = 0;
              }),
         IdleAction::Wake},
    };

    REQUIRE(rows.size() >= 40);
    for (const auto& r : rows) {
        INFO(r.label);
        CHECK(helix::decide_idle(r.in).action == r.want);
    }
}
