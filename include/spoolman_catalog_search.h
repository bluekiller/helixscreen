// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "moonraker_error.h"
#include "spoolman_types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace helix {

/**
 * @brief SpoolmanDB search state: whether the server has the route, and which
 * query is the latest.
 *
 * Spoolman serves /external/filament/search from 0.26.0; an older server answers
 * 404. That answer is cached per Moonraker connection, so the search UI asks the
 * server once and then stays shown or hidden. Main thread only.
 */
class SpoolmanCatalogSearch {
  public:
    enum class Availability { Unknown, Available, Unavailable };

    static constexpr int kResultLimit = 25;
    static constexpr size_t kMinQueryLength = 2;

    /// What is known for connection @p connection_generation; Unknown when the
    /// cached answer belongs to another connection or nothing was asked yet.
    static Availability availability(uint64_t connection_generation);

    /// Records how connection @p connection_generation answered a search: a
    /// success means available, a 404 unavailable. Any other error proves
    /// nothing about the route and records nothing.
    static void record_success(uint64_t connection_generation);
    static void record_error(uint64_t connection_generation, const MoonrakerError& err);

    /// Forgets the cached answer.
    static void reset_cache();

    /// Whether @p query is long enough to send.
    static bool is_searchable(const std::string& query);

    /// Starts a query. Its ticket stays current until the next begin() or
    /// invalidate(); a response carrying any other ticket is stale.
    uint64_t begin();
    bool is_current(uint64_t ticket) const {
        return ticket == generation_;
    }
    /// Makes every outstanding ticket stale (the wizard closed, the query cleared).
    void invalidate() {
        ++generation_;
    }

  private:
    uint64_t generation_ = 0;
};

namespace spoolman {

/// @p hex uppercased without '#' when it is 6 or 8 hex digits (Spoolman's
/// colour form, alpha last), else empty.
std::string normalize_color_hex(const std::string& hex);

/// The first of @p filaments with @p material and the same colour, or nullptr.
/// The one rule for "Spoolman already has this filament".
const FilamentInfo* find_matching_filament(const std::vector<FilamentInfo>& filaments,
                                           const std::string& material,
                                           const std::string& color_hex);

} // namespace spoolman
} // namespace helix
