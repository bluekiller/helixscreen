// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "usb_printer_detector.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// UsbPrinterDetector static method tests (no USB hardware needed)
// ============================================================================

TEST_CASE("USB printer detector", "[label-printer][usb-detect]") {
    SECTION("known printer table includes Phomemo M110") {
        auto known = UsbPrinterDetector::known_printers();
        bool found = false;
        for (const auto& p : known) {
            if (p.vid == 0x0493 && p.pid == 0x8760) {
                found = true;
                REQUIRE(p.name == "Phomemo M110");
            }
        }
        REQUIRE(found);
    }

    SECTION("is_known_printer matches Phomemo VID:PID") {
        REQUIRE(UsbPrinterDetector::is_known_printer(0x0493, 0x8760));
    }

    SECTION("is_known_printer rejects unknown VID:PID") {
        REQUIRE_FALSE(UsbPrinterDetector::is_known_printer(0x1234, 0x5678));
    }

    SECTION("get_printer_name returns name for known device") {
        auto name = UsbPrinterDetector::get_printer_name(0x0493, 0x8760);
        REQUIRE(name == "Phomemo M110");
    }

    SECTION("get_printer_name returns empty for unknown device") {
        auto name = UsbPrinterDetector::get_printer_name(0x1234, 0x5678);
        REQUIRE(name.empty());
    }

    SECTION("known printer table is not empty") {
        REQUIRE_FALSE(UsbPrinterDetector::known_printers().empty());
    }

    SECTION("default state is not polling") {
        UsbPrinterDetector detector;
        REQUIRE_FALSE(detector.is_polling());
    }
}

TEST_CASE("USB printer detector ignores the generic STM32 CDC-ACM id",
          "[label-printer][usb-detect]") {
    // 0483:5740 is ST's stock virtual COM port id, shared by unrelated boards.
    // A CDC-ACM device gets a tty, never the /dev/usb/lpN node printing needs.
    CHECK_FALSE(UsbPrinterDetector::is_known_printer(0x0483, 0x5740));
}
