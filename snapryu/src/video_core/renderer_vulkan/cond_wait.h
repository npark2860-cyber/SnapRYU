// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// When the CPU draw-condition path (no VK_EXT_conditional_rendering) waits for the GPU.
//
// The CPU path reads the condition words from guest memory. A forced fence release first makes
// pending query results land, but it also drains every earlier fence. EDEN_COND_WAIT picks when:
//   always   - before every guest-evaluated condition
//   targeted - only when a query behind the condition is still pending in the query cache
//              (same as EDEN_GPU_COND_SYNC_TARGETED=1)
//   gpu      - targeted, or when the buffer or texture cache still holds GPU-written data over
//              the condition words (RasterizerInterface::MustFlushRegion)
// Unset: EDEN_GPU_COND_SYNC_TARGETED=1 picks targeted, otherwise DefaultPolicy below applies, as in
// every release build (which reads no variable). EDEN_COND_WAIT_AUDIT=1 records, per condition
// address, whether the words changed across a wait and whether targeted or gpu would have waited,
// and logs each change (COND_WAIT_CHANGE, bounded) so changes can be lined up with the rest of the
// log.
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"

namespace Vulkan::CondWait {

enum class Policy : u32 { Always = 0, Targeted = 1, Gpu = 2 };
inline constexpr std::array<std::string_view, 3> POLICY_NAMES{"always", "targeted", "gpu"};

struct Config {
    Policy policy;
    bool from_env;
    bool valid;
};

/// value: EDEN_COND_WAIT (null when unset). legacy_targeted: EDEN_GPU_COND_SYNC_TARGETED=1.
/// from_env is false only when neither variable picks a policy; DefaultPolicy applies then.
[[nodiscard]] inline Config Parse(const char* value, bool legacy_targeted) {
    if (value == nullptr) {
        return {legacy_targeted ? Policy::Targeted : Policy::Always, legacy_targeted, true};
    }
    const std::string_view text{value};
    for (u32 i = 0; i < POLICY_NAMES.size(); ++i) {
        if (text == POLICY_NAMES[i]) {
            return {static_cast<Policy>(i), true, true};
        }
    }
    return {Policy::Always, true, false};
}

/// The policy when no variable picks one. Condition words that can still change are backed by a
/// pending query, so targeted reads the same words as always (COND_WAIT_AUDIT under always:
/// changed_not_pending=0). What always also did was drain every earlier fence. Under Immediate
/// guest fences the guest syncpoint is signalled at GPU-thread time, before the results queued with
/// that fence (buffer and texture downloads, deferred reports) reach guest memory
/// (FenceManager::SignalFenceImpl), so a game that waits on the syncpoint and then reads them can
/// read old contents. The drains kept the host GPU close behind and hid that. TotK shrine stairs:
/// under targeted with Immediate fences, 3 of 7 light draws were missing on most frames (strobing);
/// with Balanced fences there was no strobing. Fences that wait for the host GPU publish those
/// results before the syncpoint, so targeted is enough there; Immediate keeps always.
[[nodiscard]] inline Policy DefaultPolicy(bool guest_fences_immediate) {
    return guest_fences_immediate ? Policy::Always : Policy::Targeted;
}

/// Owned by one query runtime. Refresh on the GPU thread at command-list boundaries;
/// condition checks only read the resolved policy, just like an explicit diagnostic override.
class CachedPolicy {
public:
    [[nodiscard]] bool Refresh(const Config& config, bool guest_fences_immediate) {
        const Policy next = config.from_env ? config.policy : DefaultPolicy(guest_fences_immediate);
        const bool changed = !initialized || policy != next;
        policy = next;
        initialized = true;
        return changed;
    }

    [[nodiscard]] Policy Get() const {
        return policy;
    }

private:
    Policy policy{Policy::Always};
    bool initialized{};
};

[[nodiscard]] inline bool Wait(Policy policy, bool pending, bool gpu_modified) {
    return policy == Policy::Always || pending || (policy == Policy::Gpu && gpu_modified);
}

inline constexpr u64 UNKNOWN_DRAM = ~u64{0};

/// A condition check that follows this long a pause in condition checks on the GPU thread counts
/// as "after a stall" (a pipeline build, a loading screen or a long CPU-side wait).
inline constexpr s64 STALL_MS = 50;

struct Check {
    u64 dram = UNKNOWN_DRAM; ///< DRAM offset of the first condition word (PRED_DECISION dram).
    u32 mode = 0;            ///< 2 Conditional, 3 IfEqual, 4 IfNotEqual.
    u32 gap_ms = 0;          ///< Time since the previous condition check (capped).
    u32 wait_us = 0;         ///< Time spent in the wait (capped).
    bool tracked = false;    ///< The query cache holds a report for one of the condition words.
    bool pending = false;    ///< A query behind the condition is pending in the query cache.
    bool gpu_modified = false;
    bool waited = false;
    bool measured = false;   ///< before/after hold the words around the wait.
    std::array<u32, 8> before{};
    std::array<u32, 8> after{};
};

struct Counts {
    u64 checks = 0;
    u64 tracked = 0;
    u64 pending = 0;
    u64 gpu_modified = 0;
    u64 waited = 0;
    u64 after_stall = 0;
    u64 changed = 0;
    u64 changed_not_pending = 0; ///< targeted would have read the old words.
    u64 changed_neither = 0;     ///< gpu would have read the old words too.
    u64 changed_after_stall = 0;
    u64 unchanged_waits = 0;     ///< The wait did not change the words.
    u64 wait_us = 0;             ///< Total time spent waiting.

    void Add(const Check& c, bool changed_now) {
        const bool stalled = c.gap_ms >= STALL_MS;
        ++checks;
        tracked += c.tracked;
        pending += c.pending;
        gpu_modified += c.gpu_modified;
        waited += c.waited;
        after_stall += stalled;
        changed += changed_now;
        changed_not_pending += changed_now && !c.pending;
        changed_neither += changed_now && !c.pending && !c.gpu_modified;
        changed_after_stall += changed_now && stalled;
        unchanged_waits += c.measured && !changed_now;
        wait_us += c.wait_us;
    }
};

/// GPU thread only.
class Audit {
public:
    static constexpr size_t MAX_ADDRESSES = 8192;
    static constexpr size_t TOP = 8;
    static constexpr s64 LOG_INTERVAL_MS = 60'000;
    static constexpr u64 MAX_CHANGE_LOGS = 8192;
    static constexpr u32 MAX_CHANGE_LOGS_PER_ADDRESS = 4;

    void Note(const Check& c, s64 now_ms) {
        const bool changed_now = c.measured && c.before != c.after;
        totals.Add(c, changed_now);
        if (entries.size() >= MAX_ADDRESSES && !entries.contains(c.dram)) {
            entries.clear();
            ++cleared;
        }
        Entry& entry = entries[c.dram];
        entry.mode = c.mode;
        entry.counts.Add(c, changed_now);
        if (changed_now) {
            entry.last_pending = c.pending;
            entry.last_gpu = c.gpu_modified;
            entry.last_tracked = c.tracked;
            entry.before = c.before;
            entry.after = c.after;
            if (change_logs < MAX_CHANGE_LOGS && entry.change_logs < MAX_CHANGE_LOGS_PER_ADDRESS) {
                ++change_logs;
                ++entry.change_logs;
                LogChange(c);
            }
        }
        if (last_log_ms == 0) {
            last_log_ms = now_ms;
        } else if (now_ms - last_log_ms >= LOG_INTERVAL_MS) {
            last_log_ms = now_ms;
            Log();
        }
    }

    void Log() const {
        LOG_INFO(Render_Vulkan,
                 "COND_WAIT_AUDIT checks={} tracked={} pending={} gpu_modified={} waited={} "
                 "after_stall={} changed={} changed_not_pending={} changed_neither={} "
                 "changed_after_stall={} unchanged_waits={} wait_ms={} addresses={} cleared={} "
                 "change_logs={}",
                 totals.checks, totals.tracked, totals.pending, totals.gpu_modified, totals.waited,
                 totals.after_stall, totals.changed, totals.changed_not_pending,
                 totals.changed_neither, totals.changed_after_stall, totals.unchanged_waits,
                 totals.wait_us / 1000, entries.size(), cleared, change_logs);
        LogTop("changed_not_pending", [](const Counts& c) { return c.changed_not_pending; });
        LogTop("unchanged_waits", [](const Counts& c) { return c.unchanged_waits; });
        LogTop("wait_time", [](const Counts& c) { return c.wait_us; });
    }

    [[nodiscard]] const Counts& Totals() const {
        return totals;
    }

private:
    struct Entry {
        u32 mode = 0;
        u32 change_logs = 0;
        Counts counts;
        bool last_tracked = false;
        bool last_pending = false;
        bool last_gpu = false;
        std::array<u32, 8> before{};
        std::array<u32, 8> after{};
    };

    void LogChange(const Check& c) const {
        LOG_INFO(Render_Vulkan,
                 "COND_WAIT_CHANGE n={} dram={:x} mode={} tracked={} pending={} gpu_modified={} "
                 "gap_ms={} wait_us={} "
                 "before={:08x},{:08x},{:08x},{:08x},{:08x},{:08x},{:08x},{:08x} "
                 "after={:08x},{:08x},{:08x},{:08x},{:08x},{:08x},{:08x},{:08x}",
                 change_logs, c.dram, c.mode, c.tracked, c.pending, c.gpu_modified, c.gap_ms,
                 c.wait_us,
                 c.before[0], c.before[1], c.before[2], c.before[3], c.before[4], c.before[5],
                 c.before[6], c.before[7], c.after[0], c.after[1], c.after[2], c.after[3],
                 c.after[4], c.after[5], c.after[6], c.after[7]);
    }

    template <typename Key>
    void LogTop(std::string_view list, Key&& key) const {
        std::vector<std::pair<u64, const Entry*>> ranked;
        for (const auto& [dram, entry] : entries) {
            if (key(entry.counts) != 0) {
                ranked.emplace_back(dram, &entry);
            }
        }
        const size_t count = (std::min)(ranked.size(), TOP);
        std::partial_sort(ranked.begin(), ranked.begin() + static_cast<std::ptrdiff_t>(count),
                          ranked.end(), [&key](const auto& a, const auto& b) {
                              return key(a.second->counts) > key(b.second->counts);
                          });
        for (size_t rank = 0; rank < count; ++rank) {
            const auto& [dram, e] = ranked[rank];
            const Counts& c = e->counts;
            LOG_INFO(Render_Vulkan,
                     "COND_WAIT_ADDR list={} rank={} dram={:x} mode={} checks={} tracked={} "
                     "pending={} gpu_modified={} waited={} after_stall={} changed={} "
                     "changed_not_pending={} changed_neither={} changed_after_stall={} "
                     "unchanged_waits={} wait_ms={} last_tracked={} last_pending={} last_gpu={} "
                     "before={:08x},{:08x},{:08x},{:08x},{:08x},{:08x},{:08x},{:08x} "
                     "after={:08x},{:08x},{:08x},{:08x},{:08x},{:08x},{:08x},{:08x}",
                     list, rank, dram, e->mode, c.checks, c.tracked, c.pending, c.gpu_modified,
                     c.waited, c.after_stall, c.changed, c.changed_not_pending, c.changed_neither,
                     c.changed_after_stall, c.unchanged_waits, c.wait_us / 1000, e->last_tracked,
                     e->last_pending,
                     e->last_gpu, e->before[0], e->before[1], e->before[2], e->before[3],
                     e->before[4], e->before[5], e->before[6], e->before[7], e->after[0],
                     e->after[1], e->after[2], e->after[3], e->after[4], e->after[5], e->after[6],
                     e->after[7]);
        }
    }

    Counts totals;
    std::unordered_map<u64, Entry> entries;
    u64 cleared = 0;
    u64 change_logs = 0;
    s64 last_log_ms = 0;
};

[[nodiscard]] inline Audit& GetAudit() {
    static Audit audit;
    return audit;
}

[[nodiscard]] inline s64 NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] inline s64 NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Milliseconds since the previous call (0 on the first), capped to fit a u32. GPU thread only.
[[nodiscard]] inline u32 GapSincePreviousCheck(s64 now_ms) {
    static s64 previous_ms = 0;
    const s64 gap = previous_ms == 0 ? 0 : now_ms - previous_ms;
    previous_ms = now_ms;
    return static_cast<u32>(std::clamp<s64>(gap, 0, 0xFFFF'FFFF));
}

} // namespace Vulkan::CondWait
