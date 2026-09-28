// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ethernet_backend.h"

#include <string>

/**
 * @brief Mock Ethernet backend for simulator and testing
 *
 * Provides fake Ethernet functionality with static data:
 * - Always reports interface as available
 * - Returns fixed IP address (192.168.1.150)
 * - Connected status
 * - Fake MAC address
 *
 * Perfect for:
 * - macOS/simulator development
 * - UI testing without real Ethernet hardware
 * - Automated testing scenarios
 * - Fallback when platform backends fail
 */
class EthernetBackendMock : public EthernetBackend {
  public:
    EthernetBackendMock();
    ~EthernetBackendMock() override;

    // ========================================================================
    // EthernetBackend Interface Implementation
    // ========================================================================

    bool has_interface() override;
    EthernetInfo get_info() override;

    /// Test helper — drive connected state directly, mirroring
    /// WifiBackendMock::set_connected_state. The interface itself stays
    /// present (has_interface() is a hardware-presence question, not a link
    /// one); only get_info()'s connected/status reflect this.
    void set_connected_state(bool connected) {
        connected_ = connected;
    }

  private:
    std::string real_mac_; ///< Real MAC from system for realistic demo display
    bool connected_ = true;
};
