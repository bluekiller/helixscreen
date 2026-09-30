// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "i_moonraker_api.h"
#include "plugin_source.h"

namespace helix::plugin {

/// The plugin root inside Moonraker's config root.
constexpr const char* kPluginRootPath = "helixscreen/plugins/";

/// SourceDeps over the app's Moonraker API: `list` is one server.files.list of the config
/// root filtered to kPluginRootPath (Moonraker lists a root whole), `download` is
/// download_file_partial capped at `max_bytes` with the body written to the destination.
/// `api` must outlive the deps' callbacks; the caller guards them.
SourceDeps make_moonraker_source_deps(IMoonrakerAPI* api);

/// True when a notify_filelist_changed message touches config/helixscreen/plugins/: its
/// item, or a move/copy source_item, sits in the config root under kPluginRootPath. Pure.
bool is_plugin_filelist_change(const json& msg);

} // namespace helix::plugin
