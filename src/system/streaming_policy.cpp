// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "streaming_policy.h"

#include "config.h"
#include "memory_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>

namespace helix {

StreamingPolicy& StreamingPolicy::instance() {
    static StreamingPolicy instance;
    return instance;
}

void StreamingPolicy::load_from_config() {
    // Priority 1: Environment variable (highest)
    const char* force_env = std::getenv("HELIX_FORCE_STREAMING");
    if (force_env != nullptr) {
        std::string val(force_env);
        if (val == "1" || val == "true" || val == "on") {
            spdlog::info("[StreamingPolicy] Force streaming enabled via HELIX_FORCE_STREAMING");
            force_streaming_.store(true);
            return;
        }
    }

    // Priority 2: Config file
    Config* config = Config::get_instance();
    // Check force_streaming option
    bool force = config->get<bool>("/streaming/force_streaming", false);
    if (force) {
        spdlog::info("[StreamingPolicy] Force streaming enabled via config");
        force_streaming_.store(true);
    }

    // Check threshold override (0 = auto-detect)
    int threshold_mb = config->get<int>("/streaming/threshold_mb", 0);
    if (threshold_mb > 0) {
        size_t threshold_bytes = static_cast<size_t>(threshold_mb) * 1024 * 1024;
        threshold_bytes_.store(threshold_bytes);
        spdlog::info("[StreamingPolicy] Threshold set to {}MB via config", threshold_mb);
    }

    // Log current settings
    log_settings();
}

bool StreamingPolicy::should_stream(size_t file_size_bytes) const {
    // Force streaming mode bypasses size check
    if (force_streaming_.load()) {
        return true;
    }

    size_t threshold = get_threshold_bytes();
    return file_size_bytes > threshold;
}

size_t StreamingPolicy::get_threshold_bytes() const {
    size_t configured = threshold_bytes_.load();

    // If explicitly configured (non-zero), use that value
    if (configured > 0) {
        return configured;
    }

    // Auto-detect based on available RAM
    return auto_detect_threshold();
}

void StreamingPolicy::set_threshold_bytes(size_t bytes) {
    threshold_bytes_.store(bytes);

    if (bytes == 0) {
        spdlog::info("[StreamingPolicy] Threshold set to auto-detect");
    } else {
        spdlog::info("[StreamingPolicy] Threshold set to {} bytes ({:.1f} MB)", bytes,
                     static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
}

void StreamingPolicy::set_force_streaming(bool force) {
    force_streaming_.store(force);

    if (force) {
        spdlog::info("[StreamingPolicy] Force streaming enabled - all file operations will stream");
    } else {
        spdlog::debug("[StreamingPolicy] Force streaming disabled");
    }
}

size_t StreamingPolicy::auto_detect_threshold() const {
    MemoryInfo mem = get_system_memory_info();

    // If we can't read memory info, use conservative fallback
    if (mem.available_kb == 0) {
        spdlog::debug("[StreamingPolicy] Cannot read memory info, using fallback threshold {}MB",
                      FALLBACK_THRESHOLD / (1024 * 1024));
        return FALLBACK_THRESHOLD;
    }

    // Calculate threshold as percentage of available RAM
    size_t available_bytes = mem.available_kb * 1024;
    size_t calculated =
        static_cast<size_t>(static_cast<double>(available_bytes) * RAM_THRESHOLD_PERCENT);

    // Clamp to reasonable bounds
    size_t threshold = std::clamp(calculated, MIN_THRESHOLD, MAX_THRESHOLD);

    spdlog::trace("[StreamingPolicy] Auto-detected threshold: {} bytes ({:.1f} MB) "
                  "[available RAM: {}MB, {}% = {}MB, clamped to [{}-{}]MB]",
                  threshold, static_cast<double>(threshold) / (1024.0 * 1024.0),
                  mem.available_kb / 1024, static_cast<int>(RAM_THRESHOLD_PERCENT * 100),
                  calculated / (1024 * 1024), MIN_THRESHOLD / (1024 * 1024),
                  MAX_THRESHOLD / (1024 * 1024));

    return threshold;
}

void StreamingPolicy::log_settings() const {
    size_t threshold = get_threshold_bytes();
    bool forced = force_streaming_.load();
    size_t configured = threshold_bytes_.load();

    if (forced) {
        spdlog::debug("[StreamingPolicy] Settings: FORCE_STREAMING=true (all files stream)");
    } else if (configured > 0) {
        spdlog::debug("[StreamingPolicy] Settings: threshold={} bytes ({:.1f} MB) [configured]",
                      threshold, static_cast<double>(threshold) / (1024.0 * 1024.0));
    } else {
        MemoryInfo mem = get_system_memory_info();
        spdlog::debug(
            "[StreamingPolicy] Settings: threshold={} bytes ({:.1f} MB) [auto: {}% of {}MB RAM]",
            threshold, static_cast<double>(threshold) / (1024.0 * 1024.0),
            static_cast<int>(RAM_THRESHOLD_PERCENT * 100), mem.available_kb / 1024);
    }
}

} // namespace helix
