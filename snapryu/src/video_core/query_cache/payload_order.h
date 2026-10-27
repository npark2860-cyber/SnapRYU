// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Payload semaphore reports keep their GPU-thread order under immediate guest fences.
//
// A Payload report without the fence flag carries a value that is known when the GPU thread reads
// the command. With High GPU accuracy it is published by a deferred operation that runs only after
// the host GPU completes the next fence. Under the Immediate guest-fence policy, syncpoints and
// fence reports are signalled at GPU-thread time instead, so the guest can see a later syncpoint,
// store to the same address, and then have that store overwritten by the earlier payload.
// When guest fences are immediate, write the payload at GPU-thread time as well (the path
// CounterReport already uses below High accuracy), unless an older report to the same bytes is
// still unpublished; that one keeps the deferred path so the two land in order.
// EDEN_PAYLOAD_IN_ORDER=0 restores the deferred publication for every payload.
#pragma once

#include <atomic>
#include <cstdlib>
#include <string_view>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/settings.h"

namespace VideoCommon::PayloadOrder {

/// Same policy test as the delay_fence choice in FenceManager::SignalFenceImpl, inverted.
[[nodiscard]] inline bool GuestFencesImmediate() {
    return Settings::IsGPUFenceBehaviorDefault()
               ? !Settings::IsGPULevelHigh()
               : !Settings::IsGPUFenceBehaviorBalanced() && !Settings::IsGPUFenceBehaviorAccurate() &&
                     !Settings::IsGPUFenceBehaviorStrict();
}

/// Unset or any value other than "0" keeps the switch on.
[[nodiscard]] inline bool ParseEnabled(const char* value) {
    return value == nullptr || std::string_view{value} != "0";
}

[[nodiscard]] inline bool Enabled() {
#if SNAPRYU_DIAGNOSTICS
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_PAYLOAD_IN_ORDER");
        const bool on = ParseEnabled(value);
        LOG_INFO(HW_GPU, "PAYLOAD_IN_ORDER enabled={} value={}", on,
                 value != nullptr ? value : "unset");
        return on;
    }();
    return enabled;
#else
    return true;
#endif
}

struct Counters {
    std::atomic<u64> immediate{};
    std::atomic<u64> kept_order{};
};
inline Counters counters;

/// Logs at 1, 2, 4, 8, ... so the totals stay visible without a timer.
inline void Count(std::atomic<u64>& counter) {
#if SNAPRYU_DIAGNOSTICS
    const u64 value = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((value & (value - 1)) == 0) {
        LOG_INFO(HW_GPU, "PAYLOAD_IN_ORDER immediate={} kept_order={} gpu_high={} fences_immediate={}",
                 counters.immediate.load(std::memory_order_relaxed),
                 counters.kept_order.load(std::memory_order_relaxed), Settings::IsGPULevelHigh(),
                 GuestFencesImmediate());
    }
#else
    static_cast<void>(counter);
#endif
}

/// Decides one non-fence Payload report under High accuracy. has_older_unpublished reports
/// whether an earlier report to overlapping bytes is still waiting for its deferred publication.
template <typename Func>
[[nodiscard]] bool WriteNow(Func&& has_older_unpublished) {
    if (!Enabled() || !GuestFencesImmediate()) {
        return false;
    }
    if (has_older_unpublished()) {
        Count(counters.kept_order);
        return false;
    }
    Count(counters.immediate);
    return true;
}

} // namespace VideoCommon::PayloadOrder
