// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_state_offmain_reads.cpp
 * @brief AmsState queries that FilamentSensorManager::update_from_status makes
 *        from the WebSocket thread while the main thread mutates AmsState.
 *
 * Plain builds rarely fail these; they exist to go red under ASAN/TSAN.
 */

#include "../lvgl_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"

#include <atomic>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {
constexpr int kCycles = 300;

struct OffMainFixture : LVGLTestFixture {
    OffMainFixture() {
        auto& ams = AmsState::instance();
        ams.clear_backends();
        ams.init_subjects(false);
    }
    ~OffMainFixture() override {
        auto& ams = AmsState::instance();
        ams.clear_backends();
        ams.set_action(AmsAction::IDLE);
    }
};
} // namespace

TEST_CASE_METHOD(OffMainFixture, "AmsState::is_filament_operation_active is safe off-main",
                 "[ams][threading]") {
    auto& ams = AmsState::instance();
    std::atomic<bool> stop{false};

    std::thread ws([&] {
        while (!stop.load()) {
            (void)ams.is_filament_operation_active();
        }
    });

    for (int i = 0; i < kCycles; ++i) {
        ams.set_action(i % 2 ? AmsAction::LOADING : AmsAction::IDLE);
    }
    stop = true;
    ws.join();

    ams.set_action(AmsAction::LOADING);
    CHECK(ams.is_filament_operation_active());
    ams.set_action(AmsAction::IDLE);
    CHECK_FALSE(ams.is_filament_operation_active());
}

TEST_CASE_METHOD(OffMainFixture, "AmsState backend type query is safe against clear_backends",
                 "[ams][threading]") {
    auto& ams = AmsState::instance();
    std::atomic<bool> stop{false};

    std::thread ws([&] {
        while (!stop.load()) {
            if (auto* b = ams.get_backend()) {
                (void)b->get_type();
            }
        }
    });

    for (int i = 0; i < kCycles; ++i) {
        ams.clear_backends();
        ams.add_backend(std::make_unique<AmsBackendMock>(4));
    }
    stop = true;
    ws.join();
}
