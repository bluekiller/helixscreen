// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "spoolman_catalog_search.h"

#include "moonraker_error.h"
#include "text_io.h"

#include <cctype>
#include <optional>

namespace helix {

namespace {

struct CachedAvailability {
    uint64_t connection_generation = 0;
    bool available = false;
};

std::optional<CachedAvailability> s_cached;

} // namespace

SpoolmanCatalogSearch::Availability
SpoolmanCatalogSearch::availability(uint64_t connection_generation) {
    if (!s_cached || s_cached->connection_generation != connection_generation) {
        return Availability::Unknown;
    }
    return s_cached->available ? Availability::Available : Availability::Unavailable;
}

void SpoolmanCatalogSearch::record_success(uint64_t connection_generation) {
    s_cached = CachedAvailability{connection_generation, true};
}

void SpoolmanCatalogSearch::record_error(uint64_t connection_generation,
                                         const MoonrakerError& err) {
    if (err.code == 404) {
        s_cached = CachedAvailability{connection_generation, false};
    }
}

void SpoolmanCatalogSearch::reset_cache() {
    s_cached.reset();
}

bool SpoolmanCatalogSearch::is_searchable(const std::string& query) {
    return text_io::trim(query).size() >= kMinQueryLength;
}

uint64_t SpoolmanCatalogSearch::begin() {
    return ++generation_;
}

namespace spoolman {

std::string normalize_color_hex(const std::string& hex) {
    std::string s = hex;
    if (!s.empty() && s[0] == '#') {
        s.erase(0, 1);
    }
    if (s.size() != 6 && s.size() != 8) {
        return "";
    }
    for (char& c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return "";
        }
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

const FilamentInfo* find_matching_filament(const std::vector<FilamentInfo>& filaments,
                                           const std::string& material,
                                           const std::string& color_hex) {
    const std::string needle = normalize_color_hex(color_hex);
    if (needle.empty()) {
        return nullptr;
    }
    for (const auto& f : filaments) {
        if (f.material == material && normalize_color_hex(f.color_hex) == needle) {
            return &f;
        }
    }
    return nullptr;
}

} // namespace spoolman
} // namespace helix
