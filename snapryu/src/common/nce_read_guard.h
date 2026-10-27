// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// NCE_READ_GUARD (b72): counters of the Windows NCE read guard (core/memory.cpp). Diagnostic
// builds only. b73: pages skipped as guarded already, the async download guard (texture_cache.h)
// and a watch on a page whose reads keep faulting. b74: only the ready line changed.
#if SNAPRYU_DIAGNOSTICS
#include <atomic>
#include <chrono>
#include <mutex>

#include "common/common_types.h"
#include "common/logging.h"

namespace Common::NceReadGuard {

inline std::atomic<u64> guard_runs{};    // page runs protected against reads
inline std::atomic<u64> guard_pages{};
inline std::atomic<u64> release_runs{};  // page runs let through again
inline std::atomic<u64> release_pages{};
inline std::atomic<u64> faults{};        // guest read faults taken through the Dynarmic read path
inline std::atomic<u64> slow_faults{};   // of those, 1 ms or longer (a download waited for the GPU)
inline std::atomic<u64> wait_us{};
inline std::atomic<u64> max_wait_us{};
inline std::atomic<u64> host_reads{};    // host read faults let through without a download
inline std::atomic<u64> fallbacks{};     // pages sent to InvalidateNCE after repeated read faults
inline std::atomic<u64> skip_pages{};    // b73: pages a guard found guarded already (no Protect)
inline std::atomic<s64> last_log_s{};

// b73 async download guard (texture_cache.h): queued image downloads written to guest memory, and
// those dropped because the image's memory changed after they were queued.
inline std::atomic<u64> pops_written{};
inline std::atomic<u64> pops_dropped{};
inline std::atomic<u64> commits_dropped{};
inline std::atomic<u64> drops_logged{};
inline std::atomic<s64> async_last_log_s{};

// b73 watch: a page whose reads keep faulting. Its read faults (arm_nce_windows.cpp) and the image
// events over it (texture_cache.h) are logged, at most WatchLines lines per watch, 4 watches.
inline constexpr u32 WatchLines = 96;
inline std::atomic<u64> watch_vaddr{};
inline std::atomic<u64> watch_dev{};
inline std::atomic<u32> watch_lines{};
inline std::atomic<u32> watches{};
inline std::atomic<u64> repeat_page{};
inline std::atomic<s64> repeat_since_ns{};
inline std::atomic<u32> repeat_count{};

inline void NoteReady(bool enabled, bool nce, bool reactive_flushing) {
    static std::once_flag once;
    std::call_once(once, [&] {
        LOG_INFO(Render_Vulkan, "NCE_READ_GUARD ready b74 enabled={} nce={} reactive_flushing={}",
                 enabled, nce, reactive_flushing);
    });
}

inline void NoteProtect(bool guard, u64 pages) {
    (guard ? guard_runs : release_runs).fetch_add(1, std::memory_order_relaxed);
    (guard ? guard_pages : release_pages).fetch_add(pages, std::memory_order_relaxed);
}

inline s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// One more log line for the current watch, if it has lines left. Ends the watch after the last.
inline bool TakeWatchLine() {
    const u32 n = watch_lines.fetch_add(1, std::memory_order_relaxed);
    if (n >= WatchLines) {
        return false;
    }
    if (n + 1 == WatchLines) {
        watch_vaddr.store(0, std::memory_order_relaxed);
        watch_dev.store(0, std::memory_order_relaxed);
    }
    return true;
}

// A handled read fault on `page` (any thread). 64 faults on the same page within 50 ms start a
// watch on it (approximate across threads; diagnostics only). `dev` gives its device address.
template <typename DevFn>
inline void NoteRepeat(u64 page, DevFn&& dev) {
    constexpr auto relaxed = std::memory_order_relaxed;
    const s64 now = NowNs();
    if (repeat_page.load(relaxed) != page || now - repeat_since_ns.load(relaxed) > 50'000'000) {
        repeat_page.store(page, relaxed);
        repeat_since_ns.store(now, relaxed);
        repeat_count.store(0, relaxed);
    }
    if (repeat_count.fetch_add(1, relaxed) + 1 != 64 || watch_vaddr.load(relaxed) != 0 ||
        watches.load(relaxed) >= 4) {
        return;
    }
    const u64 dev_page = dev();
    watches.fetch_add(1, relaxed);
    watch_lines.store(0, relaxed);
    watch_dev.store(dev_page, relaxed);
    watch_vaddr.store(page, relaxed);
    LOG_WARNING(Render_Vulkan, "NCE_READ_GUARD watch page={:#x} dev={:#x} n={}", page, dev_page,
                watches.load(relaxed));
}

// An image event over the watched page (texture_cache.h).
inline void WatchImage(const char* what, u32 id, u64 cpu, u64 size, u32 format, u32 width,
                       u32 height, u32 levels, u32 flags, u64 frame, u64 gen) {
    const u64 dev = watch_dev.load(std::memory_order_relaxed);
    if (dev == 0 || !TakeWatchLine()) {
        return;
    }
    LOG_INFO(Render_Vulkan,
             "NCE_READ_GUARD watch_image what={} dev={:#x} image={} cpu={:#x} size={:#x} "
             "format={} {}x{} levels={} flags={:#x} frame={} gen={}",
             what, dev, id, cpu, size, format, width, height, levels, flags, frame, gen);
}

// Async download guard summary, at most every 10 seconds.
inline void MaybeLogAsync() {
    constexpr auto relaxed = std::memory_order_relaxed;
    const s64 now_s = NowNs() / 1'000'000'000;
    s64 last = async_last_log_s.load(relaxed);
    if (now_s - last < 10 || !async_last_log_s.compare_exchange_strong(last, now_s, relaxed)) {
        return;
    }
    LOG_INFO(Render_Vulkan, "ASYNC_DL_GUARD written={} dropped_pop={} dropped_commit={}",
             pops_written.load(relaxed), pops_dropped.load(relaxed), commits_dropped.load(relaxed));
}

// One guarded guest read fault, handled in `us` microseconds (the download included). Logs the
// first 16, then at every power of two and at most every 10 seconds otherwise.
inline void NoteFault(u64 us) {
    constexpr auto relaxed = std::memory_order_relaxed;
    wait_us.fetch_add(us, relaxed);
    if (us >= 1000) {
        slow_faults.fetch_add(1, relaxed);
    }
    u64 max = max_wait_us.load(relaxed);
    while (us > max && !max_wait_us.compare_exchange_weak(max, us, relaxed)) {
    }
    const u64 n = faults.fetch_add(1, relaxed) + 1;
    const s64 now_s = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
    s64 last = last_log_s.load(relaxed);
    const bool timed = now_s - last >= 10 && last_log_s.compare_exchange_strong(last, now_s, relaxed);
    if (n > 16 && (n & (n - 1)) != 0 && !timed) {
        return;
    }
    LOG_INFO(Render_Vulkan,
             "NCE_READ_GUARD faults={} slow={} wait_us={} max_wait_us={} last_us={} host_reads={} "
             "fallbacks={} guard_runs={} guard_pages={} release_runs={} release_pages={} "
             "skip_pages={}",
             n, slow_faults.load(relaxed), wait_us.load(relaxed), max_wait_us.load(relaxed), us,
             host_reads.load(relaxed), fallbacks.load(relaxed), guard_runs.load(relaxed),
             guard_pages.load(relaxed), release_runs.load(relaxed), release_pages.load(relaxed),
             skip_pages.load(relaxed));
}

} // namespace Common::NceReadGuard
#endif
