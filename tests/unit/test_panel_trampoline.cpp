// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_panel_trampoline.cpp
 * @brief Unit tests for PANEL_TRAMPOLINE macros
 *
 * Tests that the trampoline macros correctly:
 * - Define static member functions with the expected signatures
 * - Delegate to the correct handler methods
 * - Handle exceptions safely without propagating them
 * - Work with both global accessor and user_data patterns
 *
 * These macros reduce the repetitive 5-line trampoline pattern to a single line,
 * saving ~150 LOC across the codebase.
 */

#include "ui/ui_event_trampoline.h"

// Include LVGL private header to access lv_event_t structure
#include "misc/lv_event_private.h"

#include <stdexcept>
#include <string>

#include "../catch_amalgamated.hpp"

// ============================================================================
// Mock Panel for Testing Global Accessor Pattern
// ============================================================================

namespace {
class MockPanel {
  public:
    // Track which handlers were called
    bool foo_clicked_called = false;
    bool bar_pressed_called = false;
    bool exception_handler_called = false;

    void handle_foo_clicked() {
        foo_clicked_called = true;
    }
    void handle_bar_pressed() {
        bar_pressed_called = true;
    }
    void handle_exception_test() {
        exception_handler_called = true;
        throw std::runtime_error("Test exception");
    }

    // Static callbacks declared - will be defined by macros
    static void on_foo_clicked(lv_event_t* e);
    static void on_bar_pressed(lv_event_t* e);
    static void on_exception_test(lv_event_t* e);
};
} // namespace

// Global instance for testing global accessor pattern
static MockPanel* g_mock_panel = nullptr;

MockPanel& get_mock_panel() {
    if (!g_mock_panel) {
        throw std::runtime_error("Mock panel not initialized");
    }
    return *g_mock_panel;
}

// Define trampolines using the macro
PANEL_TRAMPOLINE(MockPanel, get_mock_panel, foo_clicked)
PANEL_TRAMPOLINE(MockPanel, get_mock_panel, bar_pressed)
PANEL_TRAMPOLINE(MockPanel, get_mock_panel, exception_test)

// ============================================================================
// Mock Panel for Testing User Data Pattern
// ============================================================================

class MockDialogPanel {
  public:
    bool confirm_called = false;
    bool cancel_called = false;

    void handle_confirm() {
        confirm_called = true;
    }
    void handle_cancel() {
        cancel_called = true;
    }

    static void on_confirm(lv_event_t* e);
    static void on_cancel(lv_event_t* e);
};

// Define trampolines using the user_data macro
PANEL_TRAMPOLINE_USERDATA(MockDialogPanel, confirm)
PANEL_TRAMPOLINE_USERDATA(MockDialogPanel, cancel)

// ============================================================================
// Helper to create lv_event_t with user_data for testing
// ============================================================================

// Create a minimal lv_event_t with user_data set
static lv_event_t make_event_with_user_data(void* user_data) {
    lv_event_t e = {};
    e.user_data = user_data;
    return e;
}

// ============================================================================
// TESTS
// ============================================================================

TEST_CASE("PANEL_TRAMPOLINE: delegates to handler via global accessor", "[trampoline][global]") {
    MockPanel panel;
    g_mock_panel = &panel;

    SECTION("foo_clicked handler is called") {
        REQUIRE_FALSE(panel.foo_clicked_called);

        // Call the trampoline (event parameter is unused for global accessor pattern)
        MockPanel::on_foo_clicked(nullptr);

        REQUIRE(panel.foo_clicked_called);
        REQUIRE_FALSE(panel.bar_pressed_called);
    }

    SECTION("bar_pressed handler is called") {
        REQUIRE_FALSE(panel.bar_pressed_called);

        MockPanel::on_bar_pressed(nullptr);

        REQUIRE(panel.bar_pressed_called);
        REQUIRE_FALSE(panel.foo_clicked_called);
    }

    g_mock_panel = nullptr;
}

TEST_CASE("PANEL_TRAMPOLINE: catches exceptions safely", "[trampoline][exception]") {
    MockPanel panel;
    g_mock_panel = &panel;

    REQUIRE_FALSE(panel.exception_handler_called);

    // This should NOT throw - exception is caught inside the trampoline
    REQUIRE_NOTHROW(MockPanel::on_exception_test(nullptr));

    // Handler was still called before throwing
    REQUIRE(panel.exception_handler_called);

    g_mock_panel = nullptr;
}

TEST_CASE("PANEL_TRAMPOLINE_USERDATA: delegates via event user_data", "[trampoline][userdata]") {
    MockDialogPanel panel;
    lv_event_t event = make_event_with_user_data(&panel);

    SECTION("confirm handler is called with valid user_data") {
        REQUIRE_FALSE(panel.confirm_called);

        MockDialogPanel::on_confirm(&event);

        REQUIRE(panel.confirm_called);
        REQUIRE_FALSE(panel.cancel_called);
    }

    SECTION("cancel handler is called with valid user_data") {
        REQUIRE_FALSE(panel.cancel_called);

        MockDialogPanel::on_cancel(&event);

        REQUIRE(panel.cancel_called);
        REQUIRE_FALSE(panel.confirm_called);
    }
}

TEST_CASE("PANEL_TRAMPOLINE_USERDATA: handles null user_data safely", "[trampoline][userdata]") {
    MockDialogPanel panel;
    lv_event_t event = make_event_with_user_data(nullptr);

    // Should not crash, just skip the handler call
    REQUIRE_NOTHROW(MockDialogPanel::on_confirm(&event));

    // Handler was NOT called because user_data was null
    REQUIRE_FALSE(panel.confirm_called);
}

TEST_CASE("PANEL_TRAMPOLINE_USERDATA: handles null event safely", "[trampoline][userdata]") {
    MockDialogPanel panel;

    // Should not crash with null event
    REQUIRE_NOTHROW(MockDialogPanel::on_confirm(nullptr));

    // Handler was NOT called because event was null
    REQUIRE_FALSE(panel.confirm_called);
}
