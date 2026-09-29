// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_frequency_response_chart_style.cpp
 * @brief Per-series styles, sweep cursor and tier gating for the FR chart
 *
 * The effective-style rule is a pure function; the chart stores the effective
 * style per series and switches glow/fill series to its custom draw pass while
 * default-styled series (the input shaper panel's) keep their built-in LVGL
 * series.
 */

#include "../../include/platform_capabilities.h"
#include "../../include/ui_frequency_response_chart.h"
#include "../lvgl_test_fixture.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("chart style: STANDARD with animations keeps glow and fill", "[chart][belt]") {
    FrChartSeriesStyle req{3, true, true};
    auto s = fr_chart_effective_style(req, helix::PlatformTier::STANDARD, true);
    CHECK(s.glow);
    CHECK(s.fill);
    CHECK(s.line_width == 3);
}

TEST_CASE("chart style: no animations or BASIC drops glow, keeps fill", "[chart][belt]") {
    FrChartSeriesStyle req{3, true, true};
    CHECK_FALSE(fr_chart_effective_style(req, helix::PlatformTier::STANDARD, false).glow);
    auto basic = fr_chart_effective_style(req, helix::PlatformTier::BASIC, true);
    CHECK_FALSE(basic.glow);
    CHECK(basic.fill);
}

TEST_CASE_METHOD(LVGLTestFixture, "chart stores the effective style per series", "[chart][belt]") {
    auto* chart = ui_frequency_response_chart_create(lv_screen_active());
    ui_frequency_response_chart_configure_for_platform(chart, helix::PlatformTier::BASIC);
    int id = ui_frequency_response_chart_add_series(chart, "A", lv_color_hex(0x4FA3F7));
    ui_frequency_response_chart_set_series_style(chart, id, {3, true, true});
    CHECK_FALSE(ui_frequency_response_chart_get_series_style(chart, id).glow);
    ui_frequency_response_chart_set_cursor(chart, 74.0f, lv_color_hex(0xF2994A));
    ui_frequency_response_chart_clear_cursor(chart);
    ui_frequency_response_chart_destroy(chart);
}

TEST_CASE_METHOD(LVGLTestFixture, "chart style: default style keeps the built-in series",
                 "[chart][belt]") {
    auto* chart = ui_frequency_response_chart_create(lv_screen_active());
    ui_frequency_response_chart_configure_for_platform(chart, helix::PlatformTier::STANDARD);
    int id = ui_frequency_response_chart_add_series(chart, "A", lv_color_hex(0x4FA3F7));

    // The input shaper panel's series never request glow or fill; they must
    // stay on the built-in lv_chart renderer.
    ui_frequency_response_chart_set_series_style(chart, id, {2, false, false});
    CHECK_FALSE(ui_frequency_response_chart_series_uses_custom_draw(chart, id));

    // Fill survives every tier, so it alone switches a series to custom draw.
    ui_frequency_response_chart_set_series_style(chart, id, {3, false, true});
    CHECK(ui_frequency_response_chart_series_uses_custom_draw(chart, id));
    CHECK(ui_frequency_response_chart_get_series_style(chart, id).fill);

    ui_frequency_response_chart_destroy(chart);
}
