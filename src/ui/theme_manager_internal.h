// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Declarations shared between the src/ui/theme_*.cpp files that implement
// theme_manager.h. Nothing here is public API: callers outside those files go
// through include/theme_manager.h.

namespace helix::theme_detail {

/// Canonical ui_xml directory for token discovery, resolved once through the
/// asset-root seam. On CWD-less targets (ESP-IDF VFS) this is an absolute path
/// under the mount the firmware configured via helix::set_asset_root().
const char* ui_xml_dir();

} // namespace helix::theme_detail
