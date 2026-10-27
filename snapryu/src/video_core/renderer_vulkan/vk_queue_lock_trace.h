// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#if SNAPRYU_DIAGNOSTICS
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <thread>

#include "common/common_types.h"
#include "common/logging.h"

namespace Vulkan::QueueLockTrace {

enum class Owner : size_t {
    Scheduler, PresentCopy, PresentDiscard, Present, LsfgReset, LsfgDestroyContext,
    LsfgGenerate, LsfgShutdown, Count
};
inline constexpr std::array NAMES{"scheduler_submit", "present_copy", "present_discard",
                                  "queue_present", "lsfg_reset", "lsfg_destroy_context",
                                  "lsfg_generate", "lsfg_shutdown"};
static_assert(NAMES.size() == static_cast<size_t>(Owner::Count));

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_QUEUE_LOCK_TRACE");
        const bool on = value && value[0] == '1' && value[1] == '\0';
        LOG_INFO(Render_Vulkan, "QUEUE_LOCK_TRACE enabled={} slow_us=20000 "
                               "log_first=256 then_every=128 summary_interval_us=10000000 "
                               "clock=steady_epoch_us extra_gpu_waits=0 extra_gpu_queries=0", on);
        return on;
    }();
    return enabled;
}

// Declare BEFORE the existing lock guard, then call Acquired AFTER that guard locks.
// Reverse destruction order releases the original mutex before any trace logging.
// These are CPU wall times, including possible thread preemption, not GPU timings.
template <typename Clock = std::chrono::steady_clock>
class Scope {
public:
    explicit Scope(Owner owner_) : owner{owner_}, enabled{Enabled()} {
        if (enabled) begin_us = Now();
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    void Acquired() noexcept {
        if (enabled) acquired_us = Now();
    }

    ~Scope() noexcept {
        if (!enabled || acquired_us < 0) return;
        const s64 released_us = Now();
        const s64 wait_us = acquired_us - begin_us;
        const s64 hold_us = released_us - acquired_us;
        auto& stats = counters[static_cast<size_t>(owner)];
        ++stats.calls;
        stats.wait_us += wait_us;
        stats.hold_us += hold_us;
        stats.max_wait_us = std::max(stats.max_wait_us, wait_us);
        stats.max_hold_us = std::max(stats.max_hold_us, hold_us);
        const bool slow = wait_us >= 20'000 || hold_us >= 20'000;
        stats.slow += slow;
        // A diagnostic must not turn a logging allocation failure into termination.
        try {
            if (slow && (stats.slow <= 256 || stats.slow % 128 == 0)) {
                LOG_WARNING(Render_Vulkan,
                    "GPU_QUEUE_LOCK owner={} event={} begin_us={} acquired_us={} released_us={} "
                    "wait_us={} hold_us={} thread={} logged_after_unlock=true "
                    "measurement=cpu_wall_scope log_first=256 then_every=128",
                    NAMES[static_cast<size_t>(owner)], stats.slow, begin_us, acquired_us,
                    released_us, wait_us, hold_us,
                    std::hash<std::thread::id>{}(std::this_thread::get_id()));
            }
            if (stats.last_report_us < 0) stats.last_report_us = released_us;
            if (released_us - stats.last_report_us >= 10'000'000) {
                LOG_INFO(Render_Vulkan,
                    "GPU_QUEUE_LOCK_TOTAL owner={} calls={} slow={} wait_us={} hold_us={} "
                    "max_wait_us={} max_hold_us={} thread={} cumulative=true logged_after_unlock=true",
                    NAMES[static_cast<size_t>(owner)], stats.calls, stats.slow, stats.wait_us,
                    stats.hold_us, stats.max_wait_us, stats.max_hold_us,
                    std::hash<std::thread::id>{}(std::this_thread::get_id()));
                stats.last_report_us = released_us;
            }
        } catch (...) {
        }
    }

private:
    static s64 Now() noexcept {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   Clock::now().time_since_epoch()).count();
    }
    struct Counters {
        u64 calls{}, slow{};
        s64 wait_us{}, hold_us{}, max_wait_us{}, max_hold_us{}, last_report_us{-1};
    };
    static inline thread_local std::array<Counters, static_cast<size_t>(Owner::Count)> counters{};
    const Owner owner;
    const bool enabled;
    s64 begin_us{}, acquired_us{-1};
};

} // namespace Vulkan::QueueLockTrace
#endif
