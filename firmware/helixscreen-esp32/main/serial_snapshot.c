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
            if (strcmp(line, "snap") == 0) {
                atomic_store(&s_requested, true);
            } else if (strcmp(line, "notes") == 0) {
                atomic_store(&s_notes_requested, true);
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

// Each printf writes one whole line, so log lines from other tasks interleave
// between lines rather than inside one; the host keeps only "SNAP:" lines.
typedef struct {
    unsigned char raw[57]; // 57 bytes -> one 76-char base64 line
    size_t fill;
    size_t total;
} line_out_t;

static void flush_line(line_out_t* out) {
    if (out->fill == 0) {
        return;
    }
    unsigned char b64[80];
    size_t n = 0;
    mbedtls_base64_encode(b64, sizeof(b64), &n, out->raw, out->fill);
    printf("SNAP:%.*s\n", (int)n, b64);
    out->fill = 0;
}

static mz_bool put_buf(const void* buf, int len, void* user) {
    line_out_t* out = user;
    const unsigned char* p = buf;
    out->total += (size_t)len;
    while (len > 0) {
        size_t take = sizeof(out->raw) - out->fill;
        if ((size_t)len < take) {
            take = (size_t)len;
        }
        memcpy(out->raw + out->fill, p, take);
        out->fill += take;
        p += take;
        len -= (int)take;
        if (out->fill == sizeof(out->raw)) {
            flush_line(out);
        }
    }
    return MZ_TRUE;
}

void serial_snapshot_poll(void) {
    if (atomic_exchange(&s_notes_requested, false)) {
        app_boot_print_notifications();
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
    printf("\n=====HELIX-SNAP %lu %lu RGB565 DEFLATE\n", (unsigned long)w, (unsigned long)h);

    line_out_t out = {0};
    tdefl_init(comp, put_buf, &out, 128);
    for (uint32_t y = 0; y < h; ++y) {
        tdefl_compress_buffer(comp, snap->data + y * stride, w * 2,
                              y + 1 == h ? TDEFL_FINISH : TDEFL_NO_FLUSH);
    }
    flush_line(&out);
    printf("=====HELIX-SNAP-END %u\n", (unsigned)out.total);

    heap_caps_free(comp);
    lv_draw_buf_destroy(snap);
}
