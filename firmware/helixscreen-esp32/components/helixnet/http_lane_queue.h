// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pure, platform-independent logic used by EspHttpLane (Task 10: R2 bounded
// queue, R3 size cap). Deliberately free of ESP-IDF/pthread/esp_http_client
// includes so it can be compiled by both the ESP32 IDF build
// (esp_http_lane.cpp) and the desktop host test suite
// (tests/unit/test_esp32_http_lane_queue.cpp) via the repo-root include path
// both builds already have. Mirrors reconnect_backoff.h's extraction pattern
// from Task 9.

#pragma once

#include <cstddef>
#include <cstdlib>
#include <string>

namespace helix::http {

// Hard ceiling on any single in-memory fetch the lane will perform, chosen
// from the largest thumbnail size measured on the Voron's .thumbs/ directory
// during Task 10 (see esp32p4-task-10-report.md) plus margin. Enforced
// regardless of what a caller asks for — protects the PSRAM accumulation
// buffer even if a future caller passes an unbounded request.
inline constexpr size_t HARD_CAP_BYTES = 512 * 1024;

// Clamps a caller's requested max_bytes to the lane's hard ceiling. A
// requested size of 0 means "no explicit cap" (the caller wants the whole
// response, up to whatever the lane allows), which also resolves to the hard
// ceiling rather than an unbounded fetch.
inline constexpr size_t clamp_fetch_cap(size_t requested_max_bytes) {
    if (requested_max_bytes == 0 || requested_max_bytes > HARD_CAP_BYTES) {
        return HARD_CAP_BYTES;
    }
    return requested_max_bytes;
}

// First accumulation buffer for a response. A known Content-Length (for a
// Range request, the length of the range) sizes it exactly; an unknown one
// (<= 0) starts small and grows. Allocating the whole cap up front holds
// 512 KB of PSRAM for a 5 KB thumbnail and fragments the heap.
inline constexpr size_t UNKNOWN_LENGTH_START_BYTES = 16 * 1024;

inline constexpr size_t initial_buffer_bytes(size_t cap, long long content_length) {
    if (content_length > 0) {
        return static_cast<unsigned long long>(content_length) < cap
                   ? static_cast<size_t>(content_length)
                   : cap;
    }
    return UNKNOWN_LENGTH_START_BYTES < cap ? UNKNOWN_LENGTH_START_BYTES : cap;
}

// The next buffer size once @p current is full: doubled, never past @p cap.
inline constexpr size_t next_buffer_bytes(size_t current, size_t cap) {
    return current >= cap / 2 ? cap : current * 2;
}

// What std::string::reserve(@p bytes) allocates from @p capacity: libstdc++
// grows to at least twice the old capacity, so 128 KB -> 200 KB takes 256 KB.
inline constexpr size_t reserve_allocation_bytes(size_t capacity, size_t bytes, size_t max_size) {
    if (bytes > capacity && bytes < 2 * capacity) {
        return 2 * capacity < max_size ? 2 * capacity : max_size;
    }
    return bytes;
}

// std::string::reserve() without the abort. The firmware builds
// -fno-exceptions, so a failed std::string allocation calls abort(); malloc
// returns null instead, so it probes first. Returns false, leaving @p s
// unchanged, when @p bytes cannot be had.
// ponytail: another task can take the block between the probe's free and the
// reserve; a nothrow allocator in the string type would close that window.
inline bool try_reserve(std::string& s, size_t bytes) {
    if (bytes <= s.capacity()) {
        return true;
    }
    if (bytes > s.max_size()) {
        return false;
    }
    void* probe = std::malloc(reserve_allocation_bytes(s.capacity(), bytes, s.max_size()) + 1);
    if (!probe) {
        return false;
    }
    std::free(probe);
    s.reserve(bytes);
    return true;
}

// Bounded-queue depth accounting. The lane owns one instance guarded by its
// own mutex; submit_get() calls try_acquire() before queuing a job and
// worker_loop() calls release() once that job (success or error) completes.
// Extracted as a standalone, mutex-free counter so the accept/reject decision
// is unit-testable without pthread/esp_http_client — the real class supplies
// the thread safety.
class BoundedSlotCounter {
  public:
    explicit constexpr BoundedSlotCounter(size_t max_depth) : max_depth_(max_depth) {}

    // Returns false (does not acquire a slot) when already at max_depth —
    // callers must treat this as "reject the submission", never block or grow
    // the queue past max_depth.
    constexpr bool try_acquire() {
        if (in_flight_ >= max_depth_) {
            return false;
        }
        ++in_flight_;
        return true;
    }

    constexpr void release() {
        if (in_flight_ > 0) {
            --in_flight_;
        }
    }

    [[nodiscard]] constexpr size_t in_flight() const {
        return in_flight_;
    }

    [[nodiscard]] constexpr size_t max_depth() const {
        return max_depth_;
    }

  private:
    size_t max_depth_;
    size_t in_flight_ = 0;
};

} // namespace helix::http
