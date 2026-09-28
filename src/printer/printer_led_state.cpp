// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file printer_led_state.cpp
 * @brief LED state management extracted from PrinterState
 *
 * Manages LED subjects including RGBW channels, brightness, and on/off state.
 * Extracted from PrinterState as part of god class decomposition.
 */

#include "printer_led_state.h"

#include "led/led_color_utils.h"
#include "state/subject_macros.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix {

void PrinterLedState::init_subjects(bool register_xml) {
    if (subjects_initialized_) {
        spdlog::debug("[PrinterLedState] Subjects already initialized, skipping");
        return;
    }

    spdlog::trace("[PrinterLedState] Initializing subjects (register_xml={})", register_xml);

    // LED state subject (0=off, 1=on, derived from LED color data)
    INIT_SUBJECT_INT(led_state, 0, subjects_, register_xml);

    // LED RGBW channel subjects (0-255 range)
    INIT_SUBJECT_INT(led_r, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(led_g, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(led_b, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(led_w, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(led_brightness, 0, subjects_, register_xml);

    subjects_initialized_ = true;
    spdlog::trace("[PrinterLedState] Subjects initialized successfully");
}

void PrinterLedState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::debug("[PrinterLedState] Deinitializing subjects");
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void PrinterLedState::update_from_status(const nlohmann::json& status) {
    // Update LED state if we're tracking an LED
    // LED object names in Moonraker are like "neopixel chamber_light" or "led status_led"
    if (tracked_led_name_.empty() || !status.contains(tracked_led_name_)) {
        return;
    }

    const auto& led = status[tracked_led_name_];

    // Handle output_pin format: {"value": 0.0-1.0} (no color_data)
    // Only enter this path for actual output_pin objects, not for native LEDs with missing data
    if (!led.contains("color_data") && tracked_led_name_.rfind("output_pin ", 0) == 0) {
        if (led.contains("value") && led["value"].is_number()) {
            double val = led["value"].get<double>();
            int intensity = led::to_channel_byte(val);
            // Share the native path's conversion: a dim-but-lit pin (value 0.004)
            // must not round to 0% while led_state below still reports ON.
            int brightness = led::channel_to_percent(static_cast<uint8_t>(intensity));

            if (lv_subject_get_int(&led_r_) != intensity)
                lv_subject_set_int(&led_r_, intensity);
            if (lv_subject_get_int(&led_g_) != intensity)
                lv_subject_set_int(&led_g_, intensity);
            if (lv_subject_get_int(&led_b_) != intensity)
                lv_subject_set_int(&led_b_, intensity);
            if (lv_subject_get_int(&led_w_) != 0)
                lv_subject_set_int(&led_w_, 0);
            if (lv_subject_get_int(&led_brightness_) != brightness)
                lv_subject_set_int(&led_brightness_, brightness);

            bool is_on = (val > 0.0);
            int new_state = is_on ? 1 : 0;
            int old_state = lv_subject_get_int(&led_state_);
            if (new_state != old_state) {
                lv_subject_set_int(&led_state_, new_state);
                spdlog::debug(
                    "[PrinterLedState] Output pin {} state: {} (value={:.2f} brightness={}%)",
                    tracked_led_name_, is_on ? "ON" : "OFF", val, brightness);
            }
        }
        return;
    }

    // For on/off, we check if any color component of the first LED is > 0
    led::RgbwF parsed;
    if (!led::parse_color_data(led, parsed)) {
        return;
    }

    // Convert 0.0-1.0 range to 0-255 integer range (clamp for safety)
    int r_int = led::to_channel_byte(parsed.r);
    int g_int = led::to_channel_byte(parsed.g);
    int b_int = led::to_channel_byte(parsed.b);
    int w_int = led::to_channel_byte(parsed.w);

    // Compute brightness as max of RGBW channels (0-100%)
    int max_channel = std::max({r_int, g_int, b_int, w_int});
    int brightness = led::channel_to_percent(static_cast<uint8_t>(max_channel));

    // Update RGBW subjects (skip if unchanged to avoid redundant observer notifications)
    if (lv_subject_get_int(&led_r_) != r_int)
        lv_subject_set_int(&led_r_, r_int);
    if (lv_subject_get_int(&led_g_) != g_int)
        lv_subject_set_int(&led_g_, g_int);
    if (lv_subject_get_int(&led_b_) != b_int)
        lv_subject_set_int(&led_b_, b_int);
    if (lv_subject_get_int(&led_w_) != w_int)
        lv_subject_set_int(&led_w_, w_int);
    if (lv_subject_get_int(&led_brightness_) != brightness)
        lv_subject_set_int(&led_brightness_, brightness);

    // LED is "on" if any channel is non-zero
    bool is_on = (max_channel > 0);
    int new_state = is_on ? 1 : 0;

    int old_state = lv_subject_get_int(&led_state_);
    if (new_state != old_state) {
        lv_subject_set_int(&led_state_, new_state);
        spdlog::debug("[PrinterLedState] LED {} state: {} (R={} G={} B={} W={} brightness={}%)",
                      tracked_led_name_, is_on ? "ON" : "OFF", r_int, g_int, b_int, w_int,
                      brightness);
    }
}

void PrinterLedState::set_tracked_led(const std::string& led_name) {
    tracked_led_name_ = led_name;
    if (!led_name.empty()) {
        spdlog::debug("[PrinterLedState] Tracking LED: {}", led_name);
    } else {
        spdlog::debug("[PrinterLedState] LED tracking disabled");
    }
}

} // namespace helix
