// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugin_source_app.h"

#include "json_utils.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include <string_view>
#include <utility>
#include <vector>

namespace helix::plugin {

bool is_plugin_filelist_change(const json& msg) {
    const json* payload = helix::json_util::notification_payload(msg);
    if (!payload)
        return false;
    auto in_root = [](const json& item) {
        if (!item.is_object())
            return false;
        std::string root = helix::json_util::safe_string(item, "root");
        std::string path = helix::json_util::safe_string(item, "path");
        std::string_view prefix(kPluginRootPath);
        std::string_view bare = prefix.substr(0, prefix.size() - 1); // the folder itself
        return root == "config" && (path.rfind(prefix, 0) == 0 || path == bare);
    };
    auto it = payload->find("item");
    auto src = payload->find("source_item");
    return (it != payload->end() && in_root(*it)) || (src != payload->end() && in_root(*src));
}

SourceDeps make_moonraker_source_deps(IMoonrakerAPI* api) {
    SourceDeps deps;
    deps.list = [api](std::function<void(bool ok, std::vector<RemoteFile>)> done) {
        api->files().list_files(
            "config", "", false,
            [done](const std::vector<FileInfo>& files) {
                std::vector<RemoteFile> out;
                const size_t prefix_len = std::string_view(kPluginRootPath).size();
                for (const FileInfo& f : files) {
                    if (f.is_dir || f.path.rfind(kPluginRootPath, 0) != 0)
                        continue;
                    out.push_back({f.path.substr(prefix_len), f.size, f.modified});
                }
                done(true, std::move(out));
            },
            [done](const MoonrakerError& error) {
                spdlog::warn("[PluginSource] config listing failed: {}", error.message);
                done(false, {});
            });
    };
    deps.download = [api](const std::string& path, const std::string& dest, size_t max_bytes,
                          std::function<void(bool ok, std::string error)> done) {
        // A partial download never moves more than max_bytes over the wire, so an
        // oversized or stale file cannot fill flash before PluginSource rejects it.
        api->transfers().download_file_partial(
            "config", std::string(kPluginRootPath) + path, max_bytes,
            [dest, done](const std::string& body) {
                if (!text_io::write_file(dest, body)) {
                    done(false, "cannot write " + dest);
                    return;
                }
                done(true, {});
            },
            [done](const MoonrakerError& error) { done(false, error.message); });
    };
    return deps;
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
