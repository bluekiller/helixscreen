// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// ESP_PLATFORM is defined by ESP-IDF for every component, so this header behaves the same in
// the firmware's app and network components.
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_pthread.h"
#endif

#include <cstddef>

namespace helix {

/// While alive, threads the calling thread starts get a `stack_bytes` stack named `name` in
/// external RAM. On the ESP32 internal RAM belongs to WiFi, lwIP and the WebSocket task, and
/// its largest block can be smaller than one thread stack. A no-op everywhere else.
///
/// A thread with an external stack must never start a flash write (esp_partition, nvs,
/// spi_flash, LittleFS): the cache it would disable is what reaches its own stack.
class ScopedExternalThreadStack {
  public:
    ScopedExternalThreadStack(const char* name, size_t stack_bytes) {
#if defined(ESP_PLATFORM)
        had_cfg_ = esp_pthread_get_cfg(&saved_) == ESP_OK;
        esp_pthread_cfg_t cfg = had_cfg_ ? saved_ : esp_pthread_get_default_config();
        cfg.stack_size = stack_bytes;
        cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        cfg.inherit_cfg = false;
        cfg.thread_name = name;
        esp_pthread_set_cfg(&cfg);
#else
        (void)name;
        (void)stack_bytes;
#endif
    }

    // esp_pthread's cfg is thread-local and sticky, so the caller's is put back.
    ~ScopedExternalThreadStack() {
#if defined(ESP_PLATFORM)
        if (had_cfg_) {
            esp_pthread_set_cfg(&saved_);
        } else {
            const esp_pthread_cfg_t default_cfg = esp_pthread_get_default_config();
            esp_pthread_set_cfg(&default_cfg);
        }
#endif
    }

    ScopedExternalThreadStack(const ScopedExternalThreadStack&) = delete;
    ScopedExternalThreadStack& operator=(const ScopedExternalThreadStack&) = delete;

  private:
#if defined(ESP_PLATFORM)
    esp_pthread_cfg_t saved_{};
    bool had_cfg_ = false;
#endif
};

} // namespace helix
