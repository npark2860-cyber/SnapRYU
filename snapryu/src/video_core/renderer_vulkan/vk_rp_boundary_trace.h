// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#if SNAPRYU_DIAGNOSTICS
#include <array>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <thread>

#include "common/common_types.h"
#include "common/logging.h"

namespace Vulkan::RpBoundaryTrace {
inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_RP_BOUNDARY_TRACE");
        const bool on = value && value[0] == '1' && value[1] == '\0';
        LOG_INFO(Render_Vulkan, "RP_BOUNDARY_TRACE enabled={} slots=64 "
                               "scope=outside_requests_closing_active_or_deferred_pass "
                               "extra_gpu_queries=0 extra_gpu_waits=0", on);
        return on;
    }();
    return enabled;
}
struct Entry {
    const void* caller{};
    u64 closures{}, images{}, last_bound_shader_match{};
};
struct Counters {
    std::array<Entry, 64> entries{};
    u32 size{};
    u64 closures{}, images{}, matched{}, untracked{}, polls{}, reports{};
    s64 last_report_us{};

    void Note(const void* caller, u32 image_count, bool shader_match) {
        ++closures;
        images += image_count;
        matched += shader_match;
        for (u32 i = 0; i < size; ++i) {
            if (entries[i].caller != caller) continue;
            ++entries[i].closures;
            entries[i].images += image_count;
            entries[i].last_bound_shader_match += shader_match;
            return;
        }
        if (size == entries.size()) {
            ++untracked;
            return;
        }
        entries[size++] = {caller, 1, image_count, u64{shader_match}};
    }
    bool Due(s64 now_us) {
        if (last_report_us == 0) {
            last_report_us = now_us;
            return false;
        }
        if (now_us - last_report_us < 10'000'000) return false;
        last_report_us = now_us;
        return true;
    }
};
inline thread_local Counters counters{};

inline void Note(const void* caller, u32 image_count, bool shader_match) {
    if (Enabled()) counters.Note(caller, image_count, shader_match);
}
inline void Report() {
    if (!Enabled() || counters.closures == 0) return;
    const u64 report = ++counters.reports;
    const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
    LOG_INFO(Render_Vulkan,
             "GPU_RP_BOUNDARY_TOTAL report={} closures={} images={} last_bound_shader_match={} "
             "untracked={} thread={} anchor_pc={} cumulative_process_thread=true "
             "match_vs=46cd8b9ce104b053 match_fs=53fe89d883ee502c",
             report, counters.closures, counters.images, counters.matched, counters.untracked,
             thread, reinterpret_cast<const void*>(&Enabled));
    for (u32 i = 0; i < counters.size; ++i) {
        const auto& entry = counters.entries[i];
        LOG_INFO(Render_Vulkan,
                 "GPU_RP_BOUNDARY_CALLER report={} caller={} closures={} images={} "
                 "last_bound_shader_match={} thread={} tail_calls_may_name_parent=true",
                 report, entry.caller, entry.closures, entry.images,
                 entry.last_bound_shader_match, thread);
    }
}
// Called at existing logical submissions. No per-draw clock read, GPU query or wait.
inline void MaybeReport() {
    if (!Enabled() || (++counters.polls & 255) != 0) return;
    const s64 now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (counters.Due(now)) Report();
}
} // namespace Vulkan::RpBoundaryTrace
#endif
