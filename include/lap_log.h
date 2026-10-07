// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <spdlog/spdlog.h>

#include <chrono>

namespace helix {

/// Logs how long each step of a multi-step UI-thread operation took, so a slow cycle on
/// a device can be broken down from its log alone.
class LapLog {
  public:
    explicit LapLog(const char* scope) : scope_(scope), start_(now()), last_(start_) {}

    void lap(const char* step) {
        const auto t = now();
        spdlog::info("[{}] {} {} ms (total {} ms)", scope_, step, ms(t - last_), ms(t - start_));
        last_ = t;
    }

  private:
    using clock = std::chrono::steady_clock;
    static clock::time_point now() {
        return clock::now();
    }
    static long long ms(clock::duration d) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
    }

    const char* scope_;
    clock::time_point start_;
    clock::time_point last_;
};

} // namespace helix
