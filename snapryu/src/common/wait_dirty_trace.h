// Diagnostic trial: count only deferred CPU ranges collected after a mandatory wait.
#pragma once
#include <chrono>
#include "common/common_types.h"
#include "common/logging.h"

namespace Common::WaitDirtyTrace {
#if SNAPRYU_DIAGNOSTICS
struct Counters {
    u64 waits{}, hits{}, ranges{}, bytes{};
    bool active{}, hit{};
    std::chrono::steady_clock::time_point last_log{};
};
inline thread_local Counters counters;

inline void NoteRange(u64 size) {
    if (!counters.active) return;
    ++counters.ranges;
    counters.bytes += size;
    if (!counters.hit) {
        counters.hit = true;
        ++counters.hits;
    }
}

class RefreshScope {
public:
    RefreshScope() {
        ++counters.waits;
        counters.active = true;
        counters.hit = false;
    }
    ~RefreshScope() {
        counters.active = false;
        // Clock/log work is sparse; empty waits only increment one thread-local counter.
        if (counters.waits != 1 && (counters.waits & 1023) != 0 &&
            !(counters.hit && counters.hits <= 4)) return;
        const auto now = std::chrono::steady_clock::now();
        if (counters.waits == 1 || (counters.hit && counters.hits <= 4) ||
            now - counters.last_log >= std::chrono::seconds(15)) {
            counters.last_log = now;
            LOG_INFO(Render_Vulkan, "WAIT_CPU_REFRESH waits={} hits={} ranges={} bytes={} "
                     "extra_gpu_waits=0 timing_or_visibility_causality=unproven",
                     counters.waits, counters.hits, counters.ranges, counters.bytes);
        }
    }
};
#else
inline void NoteRange(u64) {}
class RefreshScope {
public:
    RefreshScope() {}
    ~RefreshScope() {}
};
#endif
} // namespace Common::WaitDirtyTrace
