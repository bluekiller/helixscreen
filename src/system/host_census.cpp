// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/host_census.h"

#include "system/log_collector.h"
#include "system/moonraker_local_probe.h"
#include "text_io.h"

#include <algorithm>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace helix::diag {

namespace {

constexpr int MAX_FAILED_UNITS = 20;
constexpr size_t MAX_PROCESSES = 32;

bool is_display_device(const std::string& target) {
    return target.rfind("/dev/fb", 0) == 0 || target.rfind("/dev/dri/", 0) == 0;
}

std::string argv0_basename(const std::string& cmdline) {
    const std::string argv0 = cmdline.substr(0, cmdline.find(' '));
    return fs::path(argv0).filename().string();
}

} // namespace

const std::vector<std::string>& competing_ui_names() {
    static const std::vector<std::string> names = {
        "guppyscreen",   "GuppyScreen",   "grumpyscreen",  "Grumpyscreen",
        "KlipperScreen", "klipperscreen", "featherscreen", "FeatherScreen",
        "mksclient",     "qidi-client",   "qidiclient",    "makerbase-client"};
    return names;
}

std::string os_release_pretty_name(const std::string& os_release) {
    std::istringstream in(os_release);
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("PRETTY_NAME=", 0) != 0) {
            continue;
        }
        std::string value = line.substr(12);
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
            value.back() == value.front()) {
            value = value.substr(1, value.size() - 2);
        }
        return value;
    }
    return {};
}

std::vector<std::string> parse_failed_units(const std::string& list_units_output) {
    std::vector<std::string> units;
    std::istringstream in(list_units_output);
    for (std::string line; std::getline(in, line);) {
        std::istringstream fields(line);
        std::string unit;
        if (fields >> unit) {
            units.push_back(unit);
        }
    }
    return units;
}

std::vector<CensusProcess> census_processes(const std::string& proc_root, size_t cap) {
    std::vector<CensusProcess> out;
    for (const auto& proc : read_process_table(proc_root)) {
        if (out.size() >= cap) {
            break;
        }
        CensusProcess entry;
        entry.pid = proc.pid;
        entry.name = argv0_basename(proc.cmdline);
        for (const auto& needle : competing_ui_names()) {
            if (proc.cmdline.find(needle) != std::string::npos) {
                entry.reasons.push_back("competing_ui:" + needle);
                break;
            }
        }
        // Reading another user's fd table needs root; without it the process is
        // simply not seen as a holder.
        std::error_code ec;
        const fs::path fd_dir = fs::path(proc_root) / std::to_string(proc.pid) / "fd";
        for (const auto& fd : fs::directory_iterator(fd_dir, ec)) {
            std::error_code link_ec;
            const std::string target = fs::read_symlink(fd.path(), link_ec).string();
            if (!link_ec && is_display_device(target) &&
                std::find(entry.reasons.begin(), entry.reasons.end(), target) ==
                    entry.reasons.end()) {
                entry.reasons.push_back(target);
            }
        }
        if (!entry.reasons.empty()) {
            out.push_back(std::move(entry));
        }
    }
    return out;
}

HostCensus collect_host_census(const std::string& root) {
    HostCensus census;
    for (const char* rel : {"/etc/os-release", "/usr/lib/os-release"}) {
        if (auto body = helix::text_io::read_file(root + rel)) {
            census.os_pretty_name = os_release_pretty_name(*body);
            break;
        }
    }

    std::error_code ec;
    for (const char* dir : {"/usr/bin", "/bin", "/usr/sbin", "/sbin"}) {
        if (fs::exists(root + dir + "/systemctl", ec)) {
            census.has_systemctl = true;
            break;
        }
    }
    if (census.has_systemctl && root.empty()) {
        // `timeout` keeps a wedged systemd from stalling the bundle upload.
        census.failed_units = parse_failed_units(helix::logs::run_capture_tail(
            "timeout 5 systemctl list-units --state=failed --plain --no-legend --no-pager "
            "2>/dev/null",
            MAX_FAILED_UNITS, "systemctl"));
    }

    census.processes = census_processes(root.empty() ? "/proc" : root + "/proc", MAX_PROCESSES);
    return census;
}

} // namespace helix::diag
