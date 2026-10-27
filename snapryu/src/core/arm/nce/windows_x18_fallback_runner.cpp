// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/arm/nce/windows_x18_fallback_runner.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <limits>
#include <optional>

#include "core/arm/dynarmic/arm_dynarmic_64.h"
#include "core/arm/dynarmic/dynarmic_exclusive_monitor.h"
#include "core/arm/nce/arm_nce.h"
#include "core/arm/nce/guest_context.h"
#include "core/arm/nce/windows_x18_fallback_trap.h"
#include "core/arm/nce/windows_nce_r2a_trace.h"
#include "core/arm/nce/windows_nce_stats.h"
#include "core/arm/nce/x18_host_fpcr.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"

namespace Core::NCE {

WindowsX18FallbackRunner::WindowsX18FallbackRunner(System& system, bool uses_wall_clock,
                                                   Kernel::KProcess* process,
                                                   std::size_t core_index)
    : m_exclusive_monitor{std::make_unique<DynarmicExclusiveMonitor>(
          process->GetMemory(), Core::Hardware::NUM_CPU_CORES)},
      m_backend{std::make_unique<ArmDynarmic64>(system, uses_wall_clock, process,
                                                *m_exclusive_monitor, core_index, true)} {
    m_backend->SetNceFallbackMode();
}

WindowsX18FallbackRunner::~WindowsX18FallbackRunner() = default;

WindowsX18FallbackDispatchResult WindowsX18FallbackRunner::Dispatch(
    u64 transition_result, Kernel::KThread* thread, GuestContext& guest,
    const X18FallbackMetadata& metadata, u64 private_stack_base,
    u64 private_stack_size) {
    WindowsX18FallbackDispatchResult result{};

    if (transition_result != WindowsX18FallbackTrap::ReturnMarker) {
        return result;
    }

    result.handled = true;
    const auto instruction = WindowsX18FallbackTrap::FindOriginalInstruction(guest.pc, metadata);
    if (!instruction) {
        return result;
    }

    result.metadata_found = true;

    if (private_stack_base != 0 && private_stack_size != 0) {
        m_backend->SetPrivateMemoryView(private_stack_base, private_stack_size);
    } else {
        m_backend->ClearPrivateMemoryView();
    }

    result.step = X18Fallback::Step(*m_backend, thread, guest, *instruction);
    m_backend->ClearPrivateMemoryView();
    return result;
}

void WindowsX18FallbackRunner::SetDirectPrivateStackView(u64 base, u64 size) noexcept {
    if (base == 0 || size == 0 || base > std::numeric_limits<u64>::max() - size) {
        ClearDirectPrivateStackView();
        return;
    }
    m_direct_private_stack_base = base;
    m_direct_private_stack_size = size;
}

void WindowsX18FallbackRunner::ClearDirectPrivateStackView() noexcept {
    m_direct_private_stack_base = 0;
    m_direct_private_stack_size = 0;
}

X18FallbackStepResult WindowsX18FallbackRunner::ExecuteDirect(
    Kernel::KThread* thread, GuestContext& guest, u32 instruction) {
    if (m_direct_private_stack_base != 0 && m_direct_private_stack_size != 0) {
        m_backend->SetPrivateMemoryView(m_direct_private_stack_base,
                                        m_direct_private_stack_size);
    } else {
        m_backend->ClearPrivateMemoryView();
    }

    const auto step = X18Fallback::Step(*m_backend, thread, guest, instruction);
    m_backend->ClearPrivateMemoryView();
    return step;
}

namespace {
u64 ExecuteDirectX18(GuestContext* guest, u32 instruction) noexcept;
} // Anonymous namespace

extern "C" u64 WindowsNceExecuteDirectX18(GuestContext* guest, u32 instruction) noexcept {
#if SNAPRYU_DIAGNOSTICS
    // R2a: mark the direct helper for the delivery measurements; back to Transition on return.
    std::optional<R2a::PhaseScope> r2a_scope;
    if (guest != nullptr && guest->parent != nullptr) {
        r2a_scope.emplace(guest->parent->m_r2a_phase, R2a::Phase::DirectX18,
                          R2a::Phase::Transition);
    }
#endif
    // EDEN_NCE_X18_HOST_FPCR (x18_host_fpcr.h): counts only; the FPCR itself is set by the
    // trampoline. A null guest is left to ExecuteDirectX18.
    if (guest == nullptr || X18HostFpcr::GetMode() == X18HostFpcr::Mode::Off) {
        return ExecuteDirectX18(guest, instruction);
    }
    X18HostFpcr::NoteEntry(guest->fpcr, guest->host_ctx.host_fpcr, X18HostFpcr::ReadLiveFpcr());
    const u64 result = ExecuteDirectX18(guest, instruction);
    X18HostFpcr::NoteResult(result, guest->host_ctx.host_fpcr, X18HostFpcr::ReadLiveFpcr());
    return result;
}

namespace {
u64 ExecuteDirectX18(GuestContext* guest, u32 instruction) noexcept {
    if (guest == nullptr || guest->parent == nullptr) {
        return static_cast<u64>(HaltReason::PrefetchAbort);
    }

    auto* const nce = guest->parent;
    auto* const thread = nce->m_running_thread;
    auto* const runner = nce->m_windows_x18_runner.get();
    if (thread == nullptr || runner == nullptr) {
        return static_cast<u64>(HaltReason::PrefetchAbort);
    }

    const u64 pending_before = guest->esr_el1.exchange(0, std::memory_order_acq_rel);
    if (pending_before != 0) {
        Stats::Add(Stats::DirectX18Pending);
        return pending_before;
    }

    // P3C: native guest code may change SP between direct-x18 sites without leaving the current
    // RunThread epoch. P3A/P3B removed the per-x18 host return that previously let RunThread
    // refresh the private stack lease and TEB bounds. Never execute a direct x18 against stale
    // stack metadata: request one internal RunThread refresh before this instruction instead.
    const u64 stack_limit = guest->windows_guest_stack_limit;
    const u64 stack_base = guest->windows_guest_stack_base;
    if (stack_limit == 0 || stack_base <= stack_limit ||
        guest->sp <= stack_limit || guest->sp > stack_base) {
        return WindowsX18FallbackRunner::DirectStackRefreshMarker;
    }

    Stats::RecordDirectSite(guest->pc, instruction);
    const u64 stats_direct_t0 = Stats::Now();
    const DWORD stats_direct_os_thread = GetCurrentThreadId();
    const auto step = runner->ExecuteDirect(thread, *guest, instruction);
    Stats::Add(Stats::DirectX18Steps);
    if (stats_direct_os_thread != GetCurrentThreadId()) {
        // The helper resumed on a different OS thread: something inside the Dynarmic step
        // rescheduled this host fiber.
        Stats::Add(Stats::DirectX18ThreadSwitch);
    }
    Stats::Add(Stats::DirectX18Nanos, Stats::Now() - stats_direct_t0);

    const u64 pending_after = guest->esr_el1.exchange(0, std::memory_order_acq_rel);
    if (pending_after != 0) {
        Stats::Add(Stats::DirectX18Pending);
        return pending_after;
    }

    const u64 halt_reason = static_cast<u64>(step.halt_reason);
    if (!step.completed && halt_reason == 0) {
        return static_cast<u64>(HaltReason::PrefetchAbort);
    }
    return halt_reason;
}
} // Anonymous namespace

} // namespace Core::NCE
