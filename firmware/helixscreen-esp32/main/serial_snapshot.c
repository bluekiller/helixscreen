// SPDX-License-Identifier: GPL-3.0-or-later
#include "serial_snapshot.h"

#include "app_boot.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "mbedtls/base64.h"
#include "miniz.h"
#include "touch_input.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static const char* TAG = "serial_snapshot";
static atomic_bool s_requested;
static atomic_bool s_notes_requested;
static atomic_int s_resend_line = -1;

// Reads the console UART through its driver: without one, nothing delivers
// received bytes. Log output keeps writing the same UART as before.
static void reader_task(void* arg) {
    (void)arg;
    char line[24];
    size_t len = 0;
    for (;;) {
        uint8_t byte;
        if (uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &byte, 1, pdMS_TO_TICKS(200)) != 1) {
            continue;
        }
        const int c = byte;
        if (c == '\r' || c == '\n') {
            line[len] = '\0';
            int x = 0;
            int y = 0;
            int seq = 0;
            if (strcmp(line, "snap") == 0) {
                atomic_store(&s_requested, true);
            } else if (strcmp(line, "notes") == 0) {
                atomic_store(&s_notes_requested, true);
            } else if (sscanf(line, "snapline %d", &seq) == 1) {
                atomic_store(&s_resend_line, seq);
            } else if (sscanf(line, "tap %d %d", &x, &y) == 2) {
                touch_input_inject_tap(x, y);
            }
            len = 0;
        } else if (len < sizeof(line) - 1) {
            line[len++] = (char)c;
        }
    }
}

void serial_snapshot_start(void) {
    if (!uart_is_driver_installed(CONFIG_ESP_CONSOLE_UART_NUM) &&
        uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 256, 0, 0, NULL, 0) != ESP_OK) {
        ESP_LOGW(TAG, "console UART driver unavailable; snapshots unavailable");
        return;
    }
    if (xTaskCreate(reader_task, "snap_rx", 2560, NULL, 1, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no memory for the console reader; snapshots unavailable");
    }
}

// The last dump stays in PSRAM so "snapline N" can resend one line of it.
// Task-context output (esp_log, printf) shares one locked stdout, so it never
// lands inside a line, though a log written in pieces (the WiFi driver's) can
// leave its prefix in front of one. ROM/ISR output such as a task_wdt report
// can split a line; its crc catches that.
#define SNAP_LINE_BYTES 57 // -> one 76-char base64 line

static unsigned char* s_dump;
static size_t s_dump_len;
static size_t s_dump_cap;

static mz_bool put_buf(const void* buf, int len, void* user) {
    (void)user;
    if (s_dump_len + (size_t)len > s_dump_cap) {
        const size_t cap = (s_dump_len + (size_t)len) * 2;
        unsigned char* grown = heap_caps_realloc(s_dump, cap, MALLOC_CAP_SPIRAM);
        if (!grown) {
            return MZ_FALSE;
        }
        s_dump = grown;
        s_dump_cap = cap;
    }
    memcpy(s_dump + s_dump_len, buf, (size_t)len);
    s_dump_len += (size_t)len;
    return MZ_TRUE;
}

static void print_line(size_t seq) {
    const unsigned char* raw = s_dump + seq * SNAP_LINE_BYTES;
    size_t len = s_dump_len - seq * SNAP_LINE_BYTES;
    if (len > SNAP_LINE_BYTES) {
        len = SNAP_LINE_BYTES;
    }
    unsigned char b64[80];
    size_t n = 0;
    mbedtls_base64_encode(b64, sizeof(b64), &n, raw, len);
    printf("SNAP:%u %08lx %.*s\n", (unsigned)seq, (unsigned long)mz_crc32(0, raw, len), (int)n,
           b64);
}

static size_t line_count(void) {
    return (s_dump_len + SNAP_LINE_BYTES - 1) / SNAP_LINE_BYTES;
}

void serial_snapshot_poll(void) {
    if (atomic_exchange(&s_notes_requested, false)) {
        app_boot_print_notifications();
    }
    const int resend = atomic_exchange(&s_resend_line, -1);
    if (resend >= 0 && (size_t)resend < line_count()) {
        print_line((size_t)resend);
    }
    if (!atomic_exchange(&s_requested, false)) {
        return;
    }
    lv_draw_buf_t* snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    if (!snap) {
        printf("\n=====HELIX-SNAP-ERROR no memory for the snapshot\n");
        return;
    }
    tdefl_compressor* comp = heap_caps_malloc(sizeof(tdefl_compressor), MALLOC_CAP_SPIRAM);
    if (!comp) {
        lv_draw_buf_destroy(snap);
        printf("\n=====HELIX-SNAP-ERROR no memory for the compressor\n");
        return;
    }
    const uint32_t w = snap->header.w;
    const uint32_t h = snap->header.h;
    const uint32_t stride = snap->header.stride;
    s_dump_len = 0;
    tdefl_init(comp, put_buf, NULL, 128);
    tdefl_status status = TDEFL_STATUS_OKAY;
    for (uint32_t y = 0; y < h && status == TDEFL_STATUS_OKAY; ++y) {
        status = tdefl_compress_buffer(comp, snap->data + y * stride, w * 2,
                                       y + 1 == h ? TDEFL_FINISH : TDEFL_NO_FLUSH);
    }
    heap_caps_free(comp);
    lv_draw_buf_destroy(snap);
    if (status != TDEFL_STATUS_DONE) {
        s_dump_len = 0;
        printf("\n=====HELIX-SNAP-ERROR no memory for the compressed image\n");
        return;
    }

    // Streaming at 115200 baud takes seconds; yielding every line keeps this
    // CPU's idle task, and so the task watchdog, fed meanwhile.
    const size_t lines = line_count();
    printf("\n=====HELIX-SNAP %lu %lu RGB565 DEFLATE %u %08lx %u\n", (unsigned long)w,
           (unsigned long)h, (unsigned)s_dump_len, (unsigned long)mz_crc32(0, s_dump, s_dump_len),
           (unsigned)lines);
    for (size_t seq = 0; seq < lines; ++seq) {
        print_line(seq);
        vTaskDelay(1);
    }
    printf("=====HELIX-SNAP-END %u\n", (unsigned)s_dump_len);
    // The host repeats "snap" until a dump starts; those repeats are answered
    // by this dump, and another would replace the lines it may still resend.
    atomic_store(&s_requested, false);
}
