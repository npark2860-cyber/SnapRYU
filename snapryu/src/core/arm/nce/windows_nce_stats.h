// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Opt-in Windows NCE hot-path counters. Set EDEN_NCE_STATS=1 before launching Eden; once per
// second a "NCE_STATS" line is written to the log with per-interval counts and average costs.
// When the variable is unset every helper reduces to a cached bool test.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"

namespace Core::NCE::Stats {

enum Counter : std::size_t {
    NativeEntries,
    NativeNanos,
    ExitSvc,
    ExitX18Fallback,
    ExitFault,
    ExitStackRefresh,
    ExitOther,
    ExitBreakLoop,
    ExitNoReason,
    NativeOutliers,
    HostFrameMismatch,
    RedirectSignal,
    RedirectX18Trap,
    RedirectVehBreakpoint,
    RedirectVehAccess,
    DirectX18Pending,
    DirectX18ThreadSwitch,
    HostAccessFault,
    PrepNanos,
    FaultInline,
    FaultSkipped,
    FaultEmulated,
    CacheOpEmulated,
    FallbackNanos,
    DirectX18Steps,
    DirectX18Nanos,
    LeaseAcquires,
    LeaseRestores,
    LeaseBytes,
    LeaseNanos,
    CounterCount,
};

[[nodiscard]] inline bool Enabled() noexcept {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NCE_STATS");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

inline std::array<std::atomic<u64>, CounterCount> g_counters{};
inline std::atomic<s64> g_next_report_ns{0};

// Guest PCs that left native execution through the x18 BRK trap path, with the original
// instruction. Reported as the top sites once per interval.
inline std::mutex g_trap_sites_mutex;
inline std::unordered_map<u64, std::pair<u32, u64>> g_trap_sites;

inline std::unordered_map<u64, std::pair<u32, u64>> g_direct_sites;

inline void RecordDirectSite(u64 pc, u32 instruction) {
    if (!Enabled()) {
        return;
    }
    std::scoped_lock lock{g_trap_sites_mutex};
    auto& entry = g_direct_sites[pc];
    entry.first = instruction;
    ++entry.second;
}

inline void RecordTrapSite(u64 pc, u32 instruction) {
    if (!Enabled()) {
        return;
    }
    std::scoped_lock lock{g_trap_sites_mutex};
    auto& entry = g_trap_sites[pc];
    entry.first = instruction;
    ++entry.second;
}
inline std::atomic<s64> g_last_report_ns{0};

[[nodiscard]] inline s64 SteadyNanos() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Returns 0 when disabled so callers can unconditionally compute Now() deltas.
[[nodiscard]] inline u64 Now() noexcept {
    return Enabled() ? static_cast<u64>(SteadyNanos()) : 0;
}

inline void Add(Counter counter, u64 value = 1) noexcept {
    if (Enabled()) {
        g_counters[counter].fetch_add(value, std::memory_order_relaxed);
    }
}

[[nodiscard]] inline u64 Take(Counter counter) noexcept {
    return g_counters[counter].exchange(0, std::memory_order_relaxed);
}

[[nodiscard]] inline u64 Average(u64 total, u64 count) noexcept {
    return count != 0 ? total / count : 0;
}

inline void MaybeReport() noexcept {
    if (!Enabled()) {
        return;
    }
    constexpr s64 Interval = 1'000'000'000;
    const s64 now = SteadyNanos();
    s64 next = g_next_report_ns.load(std::memory_order_relaxed);
    if (now < next) {
        return;
    }
    if (!g_next_report_ns.compare_exchange_strong(next, now + Interval,
                                                  std::memory_order_relaxed)) {
        return;
    }
    const s64 last = g_last_report_ns.exchange(now, std::memory_order_relaxed);
    if (last == 0) {
        for (auto& counter : g_counters) {
            counter.store(0, std::memory_order_relaxed);
        }
        return;
    }

    const u64 elapsed_ms = static_cast<u64>((now - last) / 1'000'000);
    const u64 entries = Take(NativeEntries);
    const u64 native_ns = Take(NativeNanos);
    const u64 svc = Take(ExitSvc);
    const u64 fallback = Take(ExitX18Fallback);
    const u64 fault = Take(ExitFault);
    const u64 refresh = Take(ExitStackRefresh);
    const u64 other = Take(ExitOther);
    const u64 break_loop = Take(ExitBreakLoop);
    const u64 no_reason = Take(ExitNoReason);
    const u64 outliers = Take(NativeOutliers);
    const u64 frame_mismatch = Take(HostFrameMismatch);
    const u64 redirect_signal = Take(RedirectSignal);
    const u64 redirect_x18 = Take(RedirectX18Trap);
    const u64 redirect_brk = Take(RedirectVehBreakpoint);
    const u64 redirect_av = Take(RedirectVehAccess);
    const u64 direct_pending = Take(DirectX18Pending);
    const u64 direct_switch = Take(DirectX18ThreadSwitch);
    const u64 host_av = Take(HostAccessFault);
    const u64 prep_ns = Take(PrepNanos);
    const u64 fault_inline = Take(FaultInline);
    const u64 fault_skipped = Take(FaultSkipped);
    const u64 fault_emulated = Take(FaultEmulated);
    const u64 cache_ops = Take(CacheOpEmulated);
    const u64 fallback_ns = Take(FallbackNanos);
    const u64 direct = Take(DirectX18Steps);
    const u64 direct_ns = Take(DirectX18Nanos);
    const u64 lease_acquires = Take(LeaseAcquires);
    const u64 lease_restores = Take(LeaseRestores);
    const u64 lease_bytes = Take(LeaseBytes);
    const u64 lease_ns = Take(LeaseNanos);

    LOG_INFO(Core_ARM,
             "NCE_STATS interval={}ms entries={} native_ms={} prep_ms={} fault_inline={} fault_skipped={} fault_emulated={} cache_ops={} | exits svc={} x18_fallback={} "
             "fault={} stack_refresh={} break_loop={} no_reason={} other={} native_outliers={} | "
             "x18_fallback_ms={} (avg {}ns) | "
             "direct_x18={} direct_x18_ms={} (avg {}ns) | lease acquire={} restore={} "
             "copied_MiB={} lease_ms={} | redirect signal={} x18trap={} brk={} av={} "
             "direct_x18_pending={} direct_x18_thread_switch={} host_frame_mismatch={} host_av={}",
             elapsed_ms, entries, native_ns / 1'000'000, prep_ns / 1'000'000, fault_inline, fault_skipped, fault_emulated, cache_ops, svc, fallback, fault, refresh, break_loop, no_reason, other, outliers,
             fallback_ns / 1'000'000, Average(fallback_ns, fallback), direct,
             direct_ns / 1'000'000, Average(direct_ns, direct), lease_acquires, lease_restores,
             lease_bytes >> 20, lease_ns / 1'000'000, redirect_signal, redirect_x18,
             redirect_brk, redirect_av, direct_pending, direct_switch, frame_mismatch, host_av);

    std::unordered_map<u64, std::pair<u32, u64>> sites;
    std::unordered_map<u64, std::pair<u32, u64>> direct_sites;
    {
        std::scoped_lock lock{g_trap_sites_mutex};
        sites.swap(g_trap_sites);
        direct_sites.swap(g_direct_sites);
    }
    if (!direct_sites.empty()) {
        std::vector<std::pair<u64, std::pair<u32, u64>>> top(direct_sites.begin(),
                                                             direct_sites.end());
        const std::size_t count = (std::min)(top.size(), std::size_t{16});
        std::partial_sort(top.begin(), top.begin() + count, top.end(),
                          [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
        for (std::size_t i = 0; i < count; ++i) {
            LOG_INFO(Core_ARM, "NCE_DIRECT_TOP #{} pc={:#x} insn={:08x} count={} (sites={})", i,
                     top[i].first, top[i].second.first, top[i].second.second, top.size());
        }
    }
    if (!sites.empty()) {
        std::vector<std::pair<u64, std::pair<u32, u64>>> top(sites.begin(), sites.end());
        const std::size_t count = (std::min)(top.size(), std::size_t{8});
        std::partial_sort(top.begin(), top.begin() + count, top.end(),
                          [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
        for (std::size_t i = 0; i < count; ++i) {
            LOG_INFO(Core_ARM, "NCE_TRAP_TOP #{} pc={:#x} insn={:08x} count={} (sites={})", i,
                     top[i].first, top[i].second.first, top[i].second.second, top.size());
        }
    }
}

} // namespace Core::NCE::Stats
