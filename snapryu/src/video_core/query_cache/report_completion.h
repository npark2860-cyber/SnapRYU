// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace VideoCommon {
// One instance per query runtime. Report/Capture/Ready run on the GPU thread;
// the captured callback runs at the END of the existing FIFO publication batch.
// A submitted GPU fence alone never completes this tracker.
class ReportCompletion {
    struct State { std::atomic<std::uint64_t> completed{}; };
public:
    void Report() { ++issued; }
    std::function<void()> Capture() {
        if (issued == captured) return {};
        captured = issued;
        return [completion_state = state, generation = issued] {
            completion_state->completed.store(generation, std::memory_order_release);
        };
    }
    bool Ready() const {
        return issued != 0 && state->completed.load(std::memory_order_acquire) == issued;
    }
private:
    const std::shared_ptr<State> state = std::make_shared<State>();
    std::uint64_t issued{}, captured{};
};
} // namespace VideoCommon
