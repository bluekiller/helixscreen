// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Which print-file cards hold a decoded thumbnail, decided without the panel:
// only cards in the visible window hold one, and the window's thumbnails, held
// and in flight, stay within a byte budget. The panel applies the plan.

#include <cstddef>
#include <vector>

namespace helix {

/// One file as the planner sees it.
struct CardThumbnailState {
    bool fetchable = false; ///< a file (not a directory) with a thumbnail to fetch
    bool tried = false;     ///< a fetch was started while its card was on screen
    size_t held = 0;        ///< bytes its decoded thumbnail occupies, 0 when it holds none
};

struct CardThumbnailPlan {
    std::vector<size_t> drop;  ///< off-screen files to release: thumbnail and tried mark
    std::vector<size_t> fetch; ///< files to start fetching now, in this order
};

/**
 * @brief Plans thumbnails for the card window [first, end).
 *
 * Every file outside the window that holds a thumbnail or a tried mark is
 * dropped. Inside it, files that are fetchable, untried and hold nothing are
 * fetched in order while the held thumbnails, the fetches already in flight
 * and the ones planned fit @p budget; the rest wait for a later pass. Each
 * counts at least @p estimate, the size of the slot it decodes into, so the
 * plan never starts more decodes than budget / estimate slots can take.
 *
 * @p lane_refused says the HTTP lane turned a fetch away and no slot has freed
 * since: nothing is fetched until one does, however often the window is
 * re-planned, since every fetch would be refused the same way.
 */
CardThumbnailPlan plan_card_thumbnails(const std::vector<CardThumbnailState>& files, size_t first,
                                       size_t end, size_t in_flight, size_t estimate, size_t budget,
                                       bool lane_refused = false);

} // namespace helix
