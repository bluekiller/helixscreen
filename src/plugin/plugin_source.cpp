// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugin_source.h"

#include "ui_update_queue.h"

#include "json_utils.h"
#include "plugin_manifest.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include <set>
#include <system_error>

#include "hv/json.hpp"

namespace helix::plugin {

bool split_plugin_file(const std::string& path, std::string& id, std::string& rest) {
    auto slash = path.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= path.size())
        return false;
    id = path.substr(0, slash);
    rest = path.substr(slash + 1);
    if (!is_valid_plugin_id(id))
        return false;
    if (rest.find('\\') != std::string::npos || rest.find('\0') != std::string::npos)
        return false;
    size_t start = 0;
    while (start <= rest.size()) {
        auto end = rest.find('/', start);
        std::string seg =
            rest.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (seg.empty() || seg == "." || seg == "..")
            return false;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return true;
}

PluginSource::PluginSource(SourceDeps deps, std::string cache_dir)
    : deps_(std::move(deps)), cache_dir_(std::move(cache_dir)) {}

PluginSource::~PluginSource() = default;

void PluginSource::sync(std::function<void(const SyncResult&)> done) {
    if (syncing_) {
        queued_.push_back(std::move(done));
        return;
    }
    start({std::move(done)});
}

void PluginSource::start(DoneList done) {
    syncing_ = true;
    done_ = std::move(done);
    result_ = SyncResult{};
    index_.clear();
    staged_.clear();
    failed_.clear();
    seen_.clear();
    queue_.clear();
    queue_pos_ = 0;

    std::error_code ec;
    std::filesystem::create_directories(cache_dir_, ec);
    clean_leftovers();

    LifetimeToken tok = guard_.token();
    deps_.list([this, tok](bool ok, std::vector<RemoteFile> files) {
        hop(tok, "PluginSource::on_listed",
            [this, ok, files = std::move(files)] { on_listed(ok, files); });
    });
}

void PluginSource::hop(const LifetimeToken& tok, const char* tag, std::function<void()> fn) {
    // Inline only on the main thread, where the destructor (also main thread) cannot
    // interleave with the check-and-run; a dep answering on a worker thread hops through
    // the token, which re-checks the generation before the body runs.
    if (helix::ui::is_main_thread()) {
        if (!tok.expired())
            fn();
        return;
    }
    tok.defer(tag, std::move(fn));
}

void PluginSource::on_listed(bool ok, const std::vector<RemoteFile>& files) {
    if (!ok) {
        spdlog::warn("[PluginSource] listing failed; cache left untouched");
        finish(SyncResult{});
        return;
    }

    std::map<std::string, PluginFiles> remote;
    for (const auto& file : files) {
        std::string id, rest;
        if (!split_plugin_file(file.path, id, rest)) {
            spdlog::warn("[PluginSource] listing path refused: {}", file.path);
            continue;
        }
        remote[id][rest] = {file.size, file.modified};
    }

    // Ids arrive sorted (the grouping map); the first kMaxPlugins are kept, the rest
    // are rejected whole.
    std::vector<std::string> accepted;
    for (const auto& [id, plugin_files] : remote) {
        seen_.insert(id);
        if (accepted.size() >= kMaxPlugins) {
            spdlog::warn("[PluginSource] {}: rejected, over the {} plugin limit", id, kMaxPlugins);
            result_.rejected.push_back(id);
            continue;
        }
        if (!within_limits(id, plugin_files)) {
            result_.rejected.push_back(id);
            continue;
        }
        accepted.push_back(id);
    }

    index_ = load_index();
    for (const auto& id : accepted) {
        const PluginFiles& plugin_files = remote[id];
        const auto indexed = index_.find(id);
        if (indexed != index_.end() && indexed->second == plugin_files)
            continue;
        for (const auto& [rest, entry] : plugin_files)
            queue_.push_back({id, rest, entry.first, entry.second});
    }

    download_next();
}

bool PluginSource::within_limits(const std::string& id, const PluginFiles& plugin_files) const {
    if (plugin_files.size() > kMaxFilesPerPlugin) {
        spdlog::warn("[PluginSource] {}: rejected, {} files over the {} file limit", id,
                     plugin_files.size(), kMaxFilesPerPlugin);
        return false;
    }
    uint64_t total = 0;
    for (const auto& [rest, entry] : plugin_files) {
        if (entry.first > kMaxBytesPerFile) {
            spdlog::warn("[PluginSource] {}: {} is {} bytes, over the {} byte file limit", id, rest,
                         entry.first, kMaxBytesPerFile);
            return false;
        }
        total += entry.first;
    }
    if (total > kMaxBytesPerPlugin) {
        spdlog::warn("[PluginSource] {}: rejected, {} bytes over the {} byte limit", id, total,
                     kMaxBytesPerPlugin);
        return false;
    }
    return true;
}

void PluginSource::download_next() {
    while (queue_pos_ < queue_.size()) {
        const Pending& pending = queue_[queue_pos_];
        if (failed_.count(pending.id)) {
            ++queue_pos_;
            continue;
        }
        const std::filesystem::path dest = staging_dir(pending.id) / pending.rest;
        std::error_code ec;
        std::filesystem::create_directories(dest.parent_path(), ec);
        if (ec) {
            spdlog::warn("[PluginSource] cannot create {}: {}", dest.parent_path().string(),
                         ec.message());
            failed_.insert(pending.id);
            result_.failed.push_back(pending.id);
            ++queue_pos_;
            continue;
        }
        const size_t index = queue_pos_;
        LifetimeToken tok = guard_.token();
        deps_.download(pending.id + "/" + pending.rest, dest.string(),
                       [this, tok, index](bool ok, std::string error) {
                           hop(tok, "PluginSource::on_downloaded",
                               [this, index, ok, error = std::move(error)] {
                                   on_downloaded(index, ok, error);
                               });
                       });
        return; // one transfer at a time; on_downloaded advances the queue
    }
    complete();
}

void PluginSource::on_downloaded(size_t index, bool ok, const std::string& error) {
    const Pending& pending = queue_[index];
    if (ok) {
        staged_[pending.id][pending.rest] = {pending.size, pending.modified};
        queue_pos_ = index + 1;
        download_next();
        return;
    }
    spdlog::warn("[PluginSource] download failed for {}/{}: {}", pending.id, pending.rest, error);
    failed_.insert(pending.id);
    staged_.erase(pending.id);
    std::error_code ec;
    std::filesystem::remove_all(staging_dir(pending.id), ec);
    result_.failed.push_back(pending.id);
    queue_pos_ = index + 1;
    download_next();
}

void PluginSource::complete() {
    // Swap every fully downloaded plugin in as one step: no plugin runs Lua against a
    // cache where some ids are new and some are old mid-batch.
    for (const auto& [id, plugin_files] : staged_) {
        std::error_code ec;
        const bool had_previous = std::filesystem::exists(plugin_dir(id), ec);
        if (had_previous) {
            std::filesystem::rename(plugin_dir(id), old_dir(id), ec);
            if (ec) {
                spdlog::error("[PluginSource] cannot move {} aside: {}", plugin_dir(id).string(),
                              ec.message());
                result_.failed.push_back(id);
                continue;
            }
        }
        std::filesystem::rename(staging_dir(id), plugin_dir(id), ec);
        if (ec) {
            spdlog::error("[PluginSource] cannot swap {} in: {}", id, ec.message());
            if (had_previous) {
                std::error_code restore_ec;
                std::filesystem::rename(old_dir(id), plugin_dir(id), restore_ec);
            }
            std::filesystem::remove_all(staging_dir(id), ec);
            result_.failed.push_back(id);
            continue;
        }
        std::filesystem::remove_all(old_dir(id), ec);
        index_[id] = plugin_files;
        result_.changed.push_back(id);
    }

    // A plugin absent from a successful listing is gone: drop its cache directory and
    // its index entry. Rejected ids count as present, so their previous version stays.
    std::set<std::string> gone;
    for (const auto& entry : index_)
        if (!seen_.count(entry.first))
            gone.insert(entry.first);
    std::error_code ec;
    for (std::filesystem::directory_iterator it(cache_dir_, ec), end; it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (!seen_.count(name) && is_valid_plugin_id(name))
            gone.insert(name);
    }
    for (const auto& id : gone) {
        std::error_code remove_ec;
        std::filesystem::remove_all(plugin_dir(id), remove_ec);
        index_.erase(id);
        result_.removed.push_back(id);
    }

    if (!save_index(index_))
        spdlog::error("[PluginSource] cannot write {}", index_path().string());

    SyncResult result = std::move(result_);
    result_ = SyncResult{};
    finish(std::move(result));
}

void PluginSource::finish(SyncResult result) {
    syncing_ = false;
    DoneList done;
    done.swap(done_);
    for (const auto& cb : done)
        cb(result);
    if (queued_.empty())
        return;
    DoneList next;
    next.swap(queued_);
    start(std::move(next));
}

void PluginSource::clean_leftovers() {
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(cache_dir_) / ".staging", ec);
    for (std::filesystem::directory_iterator it(cache_dir_, ec), end; it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.rfind(".old-", 0) == 0)
            std::filesystem::remove_all(it->path(), ec);
    }
}

PluginSource::CacheIndex PluginSource::load_index() const {
    CacheIndex out;
    // A missing or corrupt index reads as empty, which makes every plugin "changed" and
    // so re-downloads it: the cache never silently diverges from a broken index.
    const auto text = text_io::read_file(index_path().string());
    if (!text)
        return out;
    json j = json::parse(*text, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return out;
    for (auto id = j.begin(); id != j.end(); ++id) {
        if (!id.value().is_object())
            continue;
        PluginFiles files;
        for (auto file = id.value().begin(); file != id.value().end(); ++file) {
            if (!file.value().is_array() || file.value().size() < 2)
                continue;
            files[file.key()] = {json_util::as_uint64(file.value()[0]),
                                 json_util::as_double(file.value()[1])};
        }
        out[id.key()] = files;
    }
    return out;
}

bool PluginSource::save_index(const CacheIndex& index) const {
    json out = json::object();
    for (const auto& [id, files] : index) {
        json entry = json::object();
        for (const auto& [rest, file] : files)
            entry[rest] = json::array({file.first, file.second});
        out[id] = entry;
    }
    return text_io::write_file_atomic(index_path().string(), json_util::safe_dump(out));
}

std::filesystem::path PluginSource::plugin_dir(const std::string& id) const {
    return std::filesystem::path(cache_dir_) / id;
}

std::filesystem::path PluginSource::staging_dir(const std::string& id) const {
    return std::filesystem::path(cache_dir_) / ".staging" / id;
}

std::filesystem::path PluginSource::old_dir(const std::string& id) const {
    return std::filesystem::path(cache_dir_) / (".old-" + id);
}

std::filesystem::path PluginSource::index_path() const {
    return std::filesystem::path(cache_dir_) / ".index.json";
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
