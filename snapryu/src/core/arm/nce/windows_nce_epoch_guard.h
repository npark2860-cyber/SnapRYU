// SPDX-FileCopyrightText: Copyright 2026 SnapRYU contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// R2c-A check (SNAPRYU_DIAGNOSTICS only): guest scheduling must not start on a host thread between
// PhysicalCore's EnterContext and ExitContext, where that thread owns the core's execution
// context. Counted, never acted on. No windows.h here, so kernel files can include it.

#if SNAPRYU_DIAGNOSTICS

#include <array>
#include <atomic>
#include <cstddef>

#include "common/common_types.h"

namespace Core::NCE::EpochGuard {

enum Site : std::size_t {
    BeginWait = 0,      // KThread::BeginWait
    ScheduleSwitch = 1, // KScheduler::ScheduleImpl about to switch fibers
    SiteCount = 2,
};

inline thread_local u32 t_in_context = 0;
inline std::array<std::atomic<u64>, SiteCount> g_violations{};

inline void Enter() noexcept {
    ++t_in_context;
}

inline void Exit() noexcept {
    --t_in_context;
}

inline void Note(Site site) noexcept {
    if (t_in_context != 0) {
        g_violations[site].fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace Core::NCE::EpochGuard

#endif
