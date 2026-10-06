// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "card_thumbnail_plan.h"

#include <algorithm>

namespace helix {

CardThumbnailPlan plan_card_thumbnails(const std::vector<CardThumbnailState>& files, size_t first,
                                       size_t end, size_t in_flight, size_t estimate, size_t budget,
                                       bool lane_refused) {
    CardThumbnailPlan plan;
    end = std::min(end, files.size());
    first = std::min(first, end);

    size_t committed = in_flight * estimate;
    for (size_t i = 0; i < files.size(); ++i) {
        const CardThumbnailState& f = files[i];
        if (i >= first && i < end) {
            // A thumbnail fills a whole card-sized slot, however small its image.
            committed += f.held ? std::max(f.held, estimate) : 0;
        } else if (f.held || f.tried) {
            plan.drop.push_back(i);
        }
    }

    for (size_t i = first; i < end && !lane_refused; ++i) {
        const CardThumbnailState& f = files[i];
        if (!f.fetchable || f.tried || f.held) {
            continue;
        }
        if (committed > budget || budget - committed < estimate) {
            break;
        }
        committed += estimate;
        plan.fetch.push_back(i);
    }
    return plan;
}

} // namespace helix
