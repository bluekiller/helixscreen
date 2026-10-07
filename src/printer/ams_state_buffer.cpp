// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_state.h"
#include "ams_state_internal.h"
#include "buffer_reading.h"

#include <iterator>

namespace helix {
using ams_state_detail::assert_main_thread;

void AmsState::sync_buffer_from_info(const AmsSystemInfo& info, int64_t now_ms) {
    const int unit_count = static_cast<int>(info.units.size());
    // A unit that is gone has no trace to show.
    for (auto it = buffer_traces_.begin(); it != buffer_traces_.end();) {
        it = it->first >= unit_count ? buffer_traces_.erase(it) : std::next(it);
    }

    const BufferReading system = buffer_reading(info, -1);
    buffer_traces_[-1].record(now_ms, system.has_slider, system.bias);
    for (int u = 0; u < unit_count; ++u) {
        const BufferReading r = buffer_reading(info, u);
        buffer_traces_[u].record(now_ms, r.has_slider, r.bias);
    }
}

const BufferTrace& AmsState::buffer_trace(int unit) const {
    assert_main_thread();
    static const BufferTrace kEmpty;
    const auto it = buffer_traces_.find(unit);
    return it == buffer_traces_.end() ? kEmpty : it->second;
}

} // namespace helix
