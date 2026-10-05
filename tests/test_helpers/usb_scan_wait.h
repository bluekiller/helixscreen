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

/// A delivered result can start the next walk, so this repeats until a drain
/// leaves the executor idle.
inline void wait_for_usb_scan() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto& executor = helix::http::HttpExecutor::fast();
    do {
        while (executor.inflight() > 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        helix::ui::UpdateQueue::instance().drain();
    } while (executor.inflight() > 0 && std::chrono::steady_clock::now() < deadline);
}

} // namespace helix::test
