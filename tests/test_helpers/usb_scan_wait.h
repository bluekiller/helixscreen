// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file usb_scan_wait.h
 * @brief Let a PrintSelectUsbSource refresh finish.
 *
 * The USB walk runs on HttpExecutor::fast() and delivers through UpdateQueue,
 * so a test waits for the executor to go idle and then drains the queue.
 */

#include "ui_update_queue.h"

#include "http_executor.h"

#include <chrono>
#include <thread>

namespace helix::test {

inline void wait_for_usb_scan() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (helix::http::HttpExecutor::fast().inflight() > 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    helix::ui::UpdateQueue::instance().drain();
}

} // namespace helix::test
