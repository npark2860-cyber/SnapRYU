// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Diagnostic trial: choose which callers of a forced fence release wait for GPU completion.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string_view>

#include "common/common_types.h"
#include "common/logging.h"

namespace VideoCommon::ForceWaitSite {

// Callers of RasterizerInterface::ReleaseFences(true). Untagged callers (the puller's fence
// acquire and semaphore acquire loops) count as Other.
enum class Site : u32 { Other = 0, CondRender = 1, GuestSync = 2 };
inline constexpr std::array<std::string_view, 3> NAMES{"other", "cond", "guest"};
inline constexpr u32 ALL = 0b111;

inline thread_local Site current = Site::Other;

#if SNAPRYU_DIAGNOSTICS
class Scope {
public:
    explicit Scope(Site site) : previous{current} {
        current = site;
    }
    ~Scope() {
        current = previous;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Site previous;
};
#else
class Scope {
public:
    explicit Scope(Site) noexcept {}
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
#endif

struct Config {
    u32 mask;
    bool active;
};

// EDEN_FORCE_WAIT_SITES: comma list of cond, guest, other, or "all"/"none". A listed site waits
// for completion (EXPLICIT-WAIT); any other site gets the earlier behaviour, where Immediate fence
// policy runs the completion callback at once. Unset or invalid: every site waits.
inline Config Parse(const char* value) {
    if (value == nullptr) {
        return {ALL, false};
    }
    u32 mask = 0;
    std::string_view rest{value};
    while (true) {
        const size_t comma = rest.find(',');
        const std::string_view token = rest.substr(0, comma);
        if (token == "all") {
            mask |= ALL;
        } else if (token != "none") {
            u32 bit = 0;
            for (u32 i = 0; i < NAMES.size(); ++i) {
                if (token == NAMES[i]) {
                    bit = 1U << i;
                }
            }
            if (bit == 0) {
                return {ALL, false};
            }
            mask |= bit;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(comma + 1);
    }
    return {mask, true};
}

inline const Config& Get() {
    static const Config config = [] {
        const char* const value = std::getenv("EDEN_FORCE_WAIT_SITES");
        const Config parsed = Parse(value);
        if (parsed.active) {
            LOG_INFO(HW_GPU, "FORCE_WAIT_SITES value={} wait cond={} guest={} other={}", value,
                     (parsed.mask >> 1) & 1U, (parsed.mask >> 2) & 1U, parsed.mask & 1U);
        } else if (value != nullptr) {
            LOG_ERROR(HW_GPU, "FORCE_WAIT_SITES invalid value={} every site waits", value);
        }
        return parsed;
    }();
    return config;
}

inline bool RequireCompletion() {
#if SNAPRYU_DIAGNOSTICS
    return ((Get().mask >> static_cast<u32>(current)) & 1U) != 0;
#else
    return true;
#endif
}

struct Counters {
    std::array<std::atomic<u64>, 3> waited{};
    std::array<std::atomic<u64>, 3> returned{};
    std::array<std::atomic<u64>, 3> delayed{};
    std::atomic<s64> last_log_ms{};
};
inline Counters counters;

// returned: the completion callback had already run when the fence was signalled, so the caller
// did not wait. delayed: the site was not listed but the fence policy delayed the callback anyway.
inline void Note(bool waited, bool callback_done) {
#if SNAPRYU_DIAGNOSTICS
    if (!Get().active) {
        return;
    }
    const u32 site = static_cast<u32>(current);
    auto& counter = waited ? counters.waited[site]
                           : (callback_done ? counters.returned[site] : counters.delayed[site]);
    if (counter.fetch_add(1, std::memory_order_relaxed) == 0 && !waited) {
        LOG_INFO(HW_GPU, "FORCE_WAIT_SITES first {} site={}",
                 callback_done ? "early_return" : "delayed_by_policy", NAMES[site]);
    }
    const s64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    s64 last = counters.last_log_ms.load(std::memory_order_relaxed);
    if (last == 0) {
        counters.last_log_ms.compare_exchange_strong(last, now, std::memory_order_relaxed);
        return;
    }
    if (now - last < 60000 ||
        !counters.last_log_ms.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
        return;
    }
    const auto get = [](const std::atomic<u64>& value) {
        return value.load(std::memory_order_relaxed);
    };
    LOG_INFO(HW_GPU,
             "FORCE_WAIT_STATS waited/early_return/delayed_by_policy cond={}/{}/{} "
             "guest={}/{}/{} other={}/{}/{}",
             get(counters.waited[1]), get(counters.returned[1]), get(counters.delayed[1]),
             get(counters.waited[2]), get(counters.returned[2]), get(counters.delayed[2]),
             get(counters.waited[0]), get(counters.returned[0]), get(counters.delayed[0]));
#else
    static_cast<void>(waited);
    static_cast<void>(callback_done);
#endif
}

} // namespace VideoCommon::ForceWaitSite
