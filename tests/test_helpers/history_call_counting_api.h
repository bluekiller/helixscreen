// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "moonraker_api.h"
#include "moonraker_history_api.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <utility>

namespace helix {

/// Records the history requests that actually reach the client, and keeps the
/// server's totals fixed for the life of the API.
///
/// Lets a test assert on requests issued rather than on their downstream
/// effects, which is the only way to tell a load apart from a no-op on a mock
/// whose history is short enough that every scope returns the same jobs.
class HistoryCallCountingAPI : public MoonrakerHistoryAPI {
  public:
    explicit HistoryCallCountingAPI(helix::IMoonrakerClient& client)
        : MoonrakerHistoryAPI(client) {}

    void get_history_list(int limit, int start, double since, double before,
                          HistoryListCallback on_success, ErrorCallback on_error) override {
        ++calls;
        last_limit.store(limit);
        MoonrakerHistoryAPI::get_history_list(limit, start, since, before, std::move(on_success),
                                              std::move(on_error));
    }

    /// Every totals request answers with the first answer. A printer's lifetime
    /// totals do not move while nothing prints, but the mock derives them from
    /// assets/test_gcodes on each call, and test processes running in parallel
    /// plant files there: two reads in one test could otherwise disagree.
    void get_history_totals(HistoryTotalsCallback on_success, ErrorCallback on_error) override {
        MoonrakerHistoryAPI::get_history_totals(
            [this, on_success = std::move(on_success)](const PrintHistoryTotals& fresh) {
                PrintHistoryTotals answer;
                {
                    std::lock_guard<std::mutex> lock(totals_mutex_);
                    if (!pinned_totals_) {
                        pinned_totals_ = fresh;
                    }
                    answer = *pinned_totals_;
                }
                if (on_success) {
                    on_success(answer);
                }
            },
            std::move(on_error));
    }

    std::atomic<int> calls{0};
    std::atomic<int> last_limit{0};

  private:
    std::mutex totals_mutex_;
    std::optional<PrintHistoryTotals> pinned_totals_;
};

/// MoonrakerAPI that installs the counting history API in place of the real one.
class HistoryCallCountingMoonrakerAPI : public MoonrakerAPI {
  public:
    HistoryCallCountingMoonrakerAPI(helix::IMoonrakerClient& client, helix::PrinterState& state)
        : MoonrakerAPI(client, state) {
        // history_api_ is protected; swap in the counting implementation.
        history_api_ = std::make_unique<HistoryCallCountingAPI>(client);
    }

    [[nodiscard]] int history_list_calls() const {
        return counting()->calls.load();
    }

    /// Job limit the most recent request carried.
    [[nodiscard]] int history_last_limit() const {
        return counting()->last_limit.load();
    }

  private:
    [[nodiscard]] const HistoryCallCountingAPI* counting() const {
        return static_cast<const HistoryCallCountingAPI*>(history_api_.get());
    }
};

} // namespace helix
