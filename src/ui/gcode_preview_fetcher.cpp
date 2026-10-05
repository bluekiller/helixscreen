// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_preview_fetcher.h"

#include "ui_filename_utils.h"

#include "app_globals.h"
#include "config.h"
#include "gcode_preview_setup.h"
#include "i_moonraker_api.h"
#include "memory_utils.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <vector>

namespace tio = helix::text_io;
using helix::gcode::resolve_gcode_filename;

namespace helix::ui {

void GcodePreviewFetcher::fetch(const std::string& filename, ReadyCb on_ready,
                                UnavailableCb on_unavailable) {
    auto req = std::make_shared<Request>();
    req->filename = filename;
    req->on_ready = std::move(on_ready);
    req->on_unavailable = std::move(on_unavailable);

    // Thumbnail Only skips all gcode downloading/parsing.
    if (!preview_viewer_enabled()) {
        spdlog::info("[{}] G-code render mode is Thumbnail Only - skipping G-code load", log_tag_);
        give_up(req, Unavailable::Disabled);
        return;
    }

    // Config option to disable 3D rendering entirely
    auto* cfg = Config::get_instance();
    if (!cfg->get<bool>("/display/gcode_3d_enabled", true)) {
        spdlog::info("[{}] G-code 3D rendering disabled via config - using thumbnail only",
                     log_tag_);
        give_up(req, Unavailable::Disabled);
        return;
    }

    // Persistent cache directory (not /tmp, which may be RAM-backed on embedded)
    std::string cache_dir = get_helix_cache_dir("gcode_temp");
    if (cache_dir.empty()) {
        spdlog::warn("[{}] No writable cache directory - skipping G-code preview", log_tag_);
        give_up(req, Unavailable::NoCacheDir);
        return;
    }
    req->temp_path =
        cache_dir + "/print_view_" + std::to_string(std::hash<std::string>{}(filename)) + ".gcode";

    // Metadata gives the size, which decides whether to download at all. This
    // prevents OOM on memory-constrained devices like AD5M.
    const std::string metadata_filename = resolve_gcode_filename(filename);

    if (helix::gcode::is_3mf(filename)) {
        list_qidi_shadow(req, metadata_filename);
        return;
    }
    lookup_metadata(req, metadata_filename, "gcodes", filename);
}

void GcodePreviewFetcher::discard_file() {
    if (owned_path_.empty()) {
        return;
    }
    if (std::remove(owned_path_.c_str()) == 0) {
        spdlog::debug("[{}] Cleaned up temp G-code file: {}", log_tag_, owned_path_);
    } else {
        spdlog::trace("[{}] Temp G-code file already removed: {}", log_tag_, owned_path_);
    }
    owned_path_.clear();
}

// Every callback below fires on a background thread (get_file_metadata and
// list_files on libhv's WS event loop, download_file_to_path on
// HttpExecutor::slow()). Each marshals to the main thread through the token
// before touching the fetcher or calling the owner.

void GcodePreviewFetcher::list_qidi_shadow(const RequestPtr& req,
                                           const std::string& metadata_filename) {
    auto token = lifetime_.token();
    auto fall_back = [this, req, metadata_filename]() {
        lookup_metadata(req, metadata_filename, "gcodes", req->filename);
    };
    api_->files().list_files(
        ".temp", "", false,
        [this, token, req, fall_back](const std::vector<FileInfo>& files) {
            token.defer("GcodePreviewFetcher::qidi_3mf_shadow_list_ok", [this, req, files,
                                                                         fall_back]() {
                spdlog::debug("[{}] .temp returned {} entries for QIDI native 3MF preview lookup",
                              log_tag_, files.size());

                // A multi-plate .3mf can leave several shadow_native_plate_*.gcode
                // files in .temp, and Moonraker exposes no plate index for the
                // active print. The active plate's shadow is (re)written at print
                // start, so the newest-modified match is the best proxy for "the
                // plate currently printing".
                const FileInfo* best = nullptr;
                for (const auto& file : files) {
                    if (!helix::gcode::is_native_3mf_shadow(file.path)) {
                        continue;
                    }
                    if (best == nullptr || file.modified > best->modified) {
                        best = &file;
                    }
                }

                if (best != nullptr) {
                    spdlog::debug("[{}] Selected QIDI native 3MF shadow G-code (newest of "
                                  "matches): .temp/{} ({} bytes, modified {})",
                                  log_tag_, best->path, best->size, best->modified);
                    stream_if_safe(req, ".temp", best->path, best->size);
                    return;
                }

                spdlog::debug("[{}] No QIDI native 3MF shadow G-code found; falling back to "
                              "active filename",
                              log_tag_);
                fall_back();
            });
        },
        [this, token, fall_back](const MoonrakerError& err) {
            token.defer("GcodePreviewFetcher::qidi_3mf_shadow_list_err", [this, err, fall_back]() {
                spdlog::debug("[{}] Failed to list .temp for QIDI native 3MF preview: {}; "
                              "falling back to active filename",
                              log_tag_, err.message);
                fall_back();
            });
        });
}

void GcodePreviewFetcher::lookup_metadata(const RequestPtr& req, const std::string& metadata_target,
                                          const std::string& root,
                                          const std::string& download_target) {
    auto token = lifetime_.token();
    api_->files().get_file_metadata(
        metadata_target,
        [this, token, req, root, download_target](const FileMetadata& metadata) {
            token.defer("GcodePreviewFetcher::metadata_ok",
                        [this, req, root, download_target, size = metadata.size]() {
                            stream_if_safe(req, root, download_target, size);
                        });
        },
        [this, token, req](const MoonrakerError& err) {
            token.defer("GcodePreviewFetcher::metadata_err", [this, req, err]() {
                // Metadata only decides whether we need to DOWNLOAD the file. If
                // the owner already renders it, or a cached copy exists (size
                // unknown, so any non-empty copy is trusted), a metadata miss must
                // not blank the preview. Reachable on a flaky link or while
                // Moonraker is rescanning, and the error is silent (no toast), so
                // giving up here would leave a blank preview for the rest of the
                // print.
                if (rendered_probe_ && rendered_probe_()) {
                    spdlog::debug("[{}] G-code metadata unavailable for '{}': {} - keeping "
                                  "already-loaded render",
                                  log_tag_, req->filename, err.message);
                    return;
                }
                const size_t cached_size =
                    static_cast<size_t>(tio::file_size(req->temp_path).value_or(0));
                if (preview_cache_is_current(cached_size, 0)) {
                    if (helix::is_gcode_2d_streaming_safe(cached_size)) {
                        spdlog::info("[{}] G-code metadata unavailable for '{}': {} - using "
                                     "cached copy ({} bytes)",
                                     log_tag_, req->filename, err.message, cached_size);
                        hand_over(req, req->temp_path);
                        return;
                    }
                    std::remove(req->temp_path.c_str());
                }
                spdlog::debug(
                    "[{}] Failed to get G-code metadata for '{}': {} - skipping 3D render",
                    log_tag_, req->filename, err.message);
                give_up(req, Unavailable::MetadataFailed);
            });
        },
        true // silent - don't trigger RPC_ERROR event/toast
    );
}

void GcodePreviewFetcher::stream_if_safe(const RequestPtr& req, const std::string& root,
                                         const std::string& download_target, uint64_t size) {
    if (!helix::is_gcode_2d_streaming_safe(size)) {
        auto mem = helix::get_system_memory_info();
        spdlog::warn("[{}] G-code too large for 2D streaming: file={} bytes, available RAM={}MB - "
                     "using thumbnail only",
                     log_tag_, size, mem.available_mb());
        give_up(req, Unavailable::TooLarge);
        return;
    }

    // The cache is keyed by file name alone; the server's size says whether it
    // still holds this file.
    const size_t cached_size = static_cast<size_t>(tio::file_size(req->temp_path).value_or(0));
    if (preview_cache_is_current(cached_size, size)) {
        spdlog::info("[{}] Using cached G-code file ({} bytes): {}", log_tag_, cached_size,
                     req->temp_path);
        hand_over(req, req->temp_path);
        return;
    }

    spdlog::debug("[{}] G-code size {} bytes - safe to render, streaming to disk...", log_tag_,
                  size);
    download(req, root, download_target);
}

void GcodePreviewFetcher::download(const RequestPtr& req, const std::string& root,
                                   const std::string& download_target) {
    if (!owned_path_.empty() && owned_path_ != req->temp_path) {
        std::remove(owned_path_.c_str());
        owned_path_.clear();
    }

    auto token = lifetime_.token();
    api_->transfers().download_file_to_path(
        root, download_target, req->temp_path,
        [this, token, req](const std::string& path) {
            token.defer("GcodePreviewFetcher::download_ok", [this, req, path]() {
                spdlog::debug("[{}] Streamed G-code to disk: {}", log_tag_, path);
                hand_over(req, path);
            });
        },
        [this, token, req](const MoonrakerError& err) {
            token.defer("GcodePreviewFetcher::download_err", [this, req, err]() {
                spdlog::warn("[{}] Failed to stream G-code for viewing '{}': {}", log_tag_,
                             req->filename, err.message);
                give_up(req, Unavailable::DownloadFailed);
            });
        });
}

void GcodePreviewFetcher::hand_over(const RequestPtr& req, const std::string& path) {
    owned_path_ = path;
    if (req->on_ready) {
        req->on_ready(path);
    }
}

void GcodePreviewFetcher::give_up(const RequestPtr& req, Unavailable why) {
    if (req->on_unavailable) {
        req->on_unavailable(why);
    }
}

} // namespace helix::ui
