// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/arm/nce/windows_nce_transition.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "core/arm/nce/arm_nce.h"
#include "core/arm/nce/arm_nce_asm_definitions.h"
#include "core/arm/nce/current_nce_context.h"
#include "core/arm/nce/windows_cross_thread_break.h"
#include "core/arm/nce/windows_exception_context.h"

namespace Core::NCE {

extern "C" void WindowsNceGuestStackBridge() noexcept;
extern "C" void WindowsNceHostStackBridge() noexcept;

namespace {
constexpr std::uint32_t NzcvMask = 0xF0000000U;

using NtContinueFn = LONG(NTAPI*)(PCONTEXT, BOOLEAN);
std::once_flag g_nt_continue_once;
NtContinueFn g_nt_continue{};

void ResolveNtContinue() noexcept {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    g_nt_continue =
        ntdll != nullptr ? reinterpret_cast<NtContinueFn>(GetProcAddress(ntdll, "NtContinue"))
                         : nullptr;
}

} // namespace

static_assert(offsetof(GuestContext, cpu_registers) == 0x000);
static_assert(offsetof(GuestContext, sp) == GuestContextSp);
static_assert(offsetof(GuestContext, fpcr) == 0x108);
static_assert(offsetof(GuestContext, fpsr) == 0x10C);
static_assert(offsetof(GuestContext, vector_registers) == 0x110);
static_assert(offsetof(GuestContext, pstate) == 0x310);
static_assert(offsetof(GuestContext, host_ctx) == GuestContextHostContext);
static_assert(offsetof(HostContext, host_saved_regs) == HostContextRegs);
static_assert(offsetof(HostContext, host_saved_vregs) == HostContextVregs);
static_assert(offsetof(HostContext, host_sp) == HostContextSpTpidrEl0);
static_assert(offsetof(HostContext, host_fpcr) == HostContextFpcr);

bool WindowsNceTransition::Initialize() noexcept {
    std::call_once(g_nt_continue_once, ResolveNtContinue);
    return g_nt_continue != nullptr;
}

[[noreturn]] void WindowsNceTransition::ContinueContext(ARM64_NT_CONTEXT& context) noexcept {
    const auto nt_continue = g_nt_continue;
    if (nt_continue == nullptr) {
        std::abort();
    }

    (void)nt_continue(reinterpret_cast<PCONTEXT>(&context), FALSE);
    std::abort();
}

extern "C" [[noreturn]] void WindowsNceContinueGuestContext(
    ARM64_NT_CONTEXT* context) noexcept {
    if (context == nullptr) {
        std::abort();
    }
    WindowsNceTransition::ContinueContext(*context);
}

extern "C" [[noreturn]] void WindowsNceRestoreGuestContext(GuestContext* guest) noexcept {
    ARM64_NT_CONTEXT context{};
    RtlCaptureContext(reinterpret_cast<PCONTEXT>(&context));

    // Load guest state into the captured Windows context. WindowsExceptionContext deliberately
    // skips architectural x18 because Windows owns physical x18 as the user-mode TEB pointer.
    // RtlCaptureContext does not reliably leave a usable platform x18 in the captured CONTEXT on
    // the physical Windows ARM64 path, so publish the current TEB explicitly before NtContinue.
    const auto platform_cpsr = context.Cpsr & ~NzcvMask;
    WindowsExceptionContext::LoadGuestState(*guest, context);
    context.X[18] = reinterpret_cast<std::uint64_t>(NtCurrentTeb());

    // Native NCE owns guest NZCV only. Preserve the platform-owned non-NZCV PSTATE bits captured
    // from Windows rather than allowing guest state to alter them.
    context.Cpsr = platform_cpsr | (guest->pstate & NzcvMask);
    context.ContextFlags =
        CONTEXT_ARM64 | CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT;

    // Match the existing Linux NCE ownership handoff: PhysicalCore enters RunThread with the
    // NativeExecutionParameters lock held, but native guest code owns an unlocked interval so SVC,
    // fault and cross-thread-break paths can acquire it. This is the last host-side state mutation
    // before the Windows context transition transfers control to guest SP/PC.
    auto* const parameters = CurrentNceContext::Get();
    if (parameters == nullptr || parameters->native_context != guest) {
        std::abort();
    }
    parameters->lock.store(SpinLockUnlocked, std::memory_order_release);

    // RunThread has already validated the active guest stack immediately before this arbitrary-PC
    // restore and published its exact allocation bounds in GuestContext. Re-querying and walking
    // the same VirtualQuery allocation here once per x18 fallback re-entry became the dominant P2C
    // cost after private-stack memcpy traffic was removed. Consume the already-validated bounds
    // instead; keep strict range checks so a stale/invalid transition contract still fails closed.
    const auto allocation_base =
        static_cast<std::uintptr_t>(guest->windows_guest_stack_limit);
    const auto allocation_end =
        static_cast<std::uintptr_t>(guest->windows_guest_stack_base);
    if (allocation_base == 0 || allocation_end <= allocation_base ||
        guest->sp <= allocation_base || guest->sp > allocation_end) {
        std::abort();
    }

    ARM64_NT_CONTEXT bridge_context = context;
    bridge_context.Pc = reinterpret_cast<std::uint64_t>(&WindowsNceGuestStackBridge);
    bridge_context.X0 = reinterpret_cast<std::uint64_t>(&context);
    bridge_context.X[1] = static_cast<std::uint64_t>(allocation_end);
    bridge_context.X[2] = static_cast<std::uint64_t>(allocation_base);

    // The first NtContinue installs guest SP before WindowsNceGuestStackBridge can run. At that
    // validation point StackBase is already guest-owned, so leaving the old host StackLimit in the
    // TEB creates an inverted/mixed stack interval and Windows ARM64 can reject the CONTEXT with
    // FAST_FAIL_INVALID_SET_OF_CONTEXT. Publish the guest limit only at this final host-side seam:
    // all host-side probes above have already completed, and the bridge will re-publish both bounds.
    reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackLimit =
        reinterpret_cast<void*>(allocation_base);

    WindowsNceTransition::ContinueContext(bridge_context);
}

void WindowsNceTransition::RedirectToHost(ARM64_NT_CONTEXT& interrupted, GuestContext& guest,
                                          bool save_guest_state,
                                          std::uint64_t return_value) noexcept {
    if (save_guest_state) {
        WindowsExceptionContext::SaveGuestState(guest, interrupted);
    }

    const HostContext& host = guest.host_ctx;

    for (std::size_t i = 0; i < host.host_saved_regs.size(); ++i) {
        interrupted.X[19 + i] = host.host_saved_regs[i];
    }
    std::memcpy(&interrupted.V[8], host.host_saved_vregs.data(),
                sizeof(host.host_saved_vregs));
    interrupted.Fpcr = host.host_fpcr;

    auto* const nce = guest.parent;
    if (nce == nullptr || nce->m_windows_break == nullptr ||
        !nce->m_windows_break->IsBound()) {
        std::abort();
    }

    const auto host_pc = host.host_saved_regs[11];
    // Keep the guest SP through Windows exception/context resume. The bridge publishes
    // host TEB bounds first, then switches SP itself without re-entering the dispatcher.
    interrupted.Pc = reinterpret_cast<std::uint64_t>(&WindowsNceHostStackBridge);
    interrupted.X0 = return_value;
    interrupted.X[1] = static_cast<std::uint64_t>(nce->m_windows_break->HostStackHigh());
    interrupted.X[2] = static_cast<std::uint64_t>(nce->m_windows_break->HostStackLow());
    interrupted.X[3] = host.host_sp;
    interrupted.X[16] = host_pc;
    interrupted.ContextFlags =
        CONTEXT_ARM64 | CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT;
}

// Crash record for GetCurrentNceContextForGeneratedCode() returning null. It is written without
// any I/O (the caller runs on a small guest stack) and is meant to be read from a minidump that
// includes data segments (WER CustomDumpFlags with MiniDumpWithDataSegs). Both magic words read
// "NCENULL!" in memory.
struct WindowsNceNullContextReport {
    std::uint64_t magic;
    std::uint32_t hits;
    std::uint32_t thread_id;
    std::uint64_t qpc_now;
    std::uint64_t qpc_frequency;
    std::uint64_t return_address;
    std::uint32_t history_position;
    std::uint32_t reserved;
    CurrentNceContext::Event history[CurrentNceContext::HistorySize];
    std::uint64_t last_params;
    std::uint64_t last_params_native_context;
    std::uint32_t last_params_lock;
    std::uint32_t last_params_is_running;
    std::uint64_t last_guest_pc;
    std::uint64_t last_guest_sp;
    std::uint64_t magic_end;
};

extern "C" {
WindowsNceNullContextReport g_eden_nce_null_context_report{
    .magic = 0x214C4C554E45434EULL,
    .magic_end = 0x214C4C554E45434EULL,
};
}

namespace {

#if defined(_MSC_VER)
__declspec(noinline)
#else
[[gnu::noinline, gnu::cold]]
#endif
void CaptureNullNceContext(void* return_address) noexcept {
    auto& report = g_eden_nce_null_context_report;
    ++report.hits;
    report.thread_id = static_cast<std::uint32_t>(GetCurrentThreadId());
    LARGE_INTEGER now{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    report.qpc_now = static_cast<std::uint64_t>(now.QuadPart);
    report.qpc_frequency = static_cast<std::uint64_t>(frequency.QuadPart);
    report.return_address = reinterpret_cast<std::uint64_t>(return_address);

    const auto& history = CurrentNceContext::History();
    const std::uint32_t position = CurrentNceContext::HistoryPosition();
    report.history_position = position;
    CurrentNceContext::Parameters* last = nullptr;
    for (std::size_t i = 0; i < CurrentNceContext::HistorySize; ++i) {
        report.history[i] = history[i];
    }
    for (std::size_t back = 1; back <= CurrentNceContext::HistorySize && back <= position;
         ++back) {
        const auto& event = history[(position - back) % CurrentNceContext::HistorySize];
        if (event.params != nullptr) {
            last = event.params;
            break;
        }
    }

    report.last_params = reinterpret_cast<std::uint64_t>(last);
    if (last != nullptr) {
        report.last_params_native_context = reinterpret_cast<std::uint64_t>(last->native_context);
        report.last_params_lock = last->lock.load(std::memory_order_relaxed);
        report.last_params_is_running = last->is_running ? 1U : 0U;
        if (const auto* guest = static_cast<const GuestContext*>(last->native_context)) {
            report.last_guest_pc = guest->pc;
            report.last_guest_sp = guest->sp;
        }
    }
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

} // namespace

extern "C"
#if defined(_MSC_VER)
__declspec(noinline)
#endif
void* GetCurrentNceContextForGeneratedCode() noexcept {
    auto* const context = CurrentNceContext::Get();
    if (context == nullptr) [[unlikely]] {
#if defined(_MSC_VER) && !defined(__clang__)
        CaptureNullNceContext(_ReturnAddress());
#else
        CaptureNullNceContext(__builtin_return_address(0));
#endif
        if (IsDebuggerPresent()) {
            DebugBreak();
        }
    }
    return context;
}

} // namespace Core::NCE
