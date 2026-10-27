// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>

#include "core/arm/arm_interface.h"
#include "core/arm/nce/guest_context.h"

namespace Core::Memory {
class Memory;
}

namespace Core::NCE {
#if defined(_WIN32)
class WindowsCrossThreadBreak;
class WindowsX18FallbackRunner;
#endif
} // namespace Core::NCE

namespace Core {

class System;

class ArmNce final : public ArmInterface {
public:
    ArmNce(System& system, bool uses_wall_clock, std::size_t core_index);
    ~ArmNce() override;

    void Initialize() override;

    Architecture GetArchitecture() const override {
        return Architecture::AArch64;
    }

    HaltReason RunThread(Kernel::KThread* thread) override;
    HaltReason StepThread(Kernel::KThread* thread) override;

    void GetContext(Kernel::Svc::ThreadContext& ctx) const override;
    void SetContext(const Kernel::Svc::ThreadContext& ctx) override;
    void SetTpidrroEl0(u64 value) override;

    void GetSvcArguments(std::span<uint64_t, 8> args) const override;
    void SetSvcArguments(std::span<const uint64_t, 8> args) override;
    u32 GetSvcNumber() const override;

    void SignalInterrupt(Kernel::KThread* thread) override;
    void ClearInstructionCache() override;
    void InvalidateCacheRange(u64 addr, std::size_t size) override;

    void LockThread(Kernel::KThread* thread) override;
    void UnlockThread(Kernel::KThread* thread) override;
#if defined(_WIN32)
    bool PrepareRunThread(Kernel::KThread* thread) override;
#endif

protected:
    const Kernel::DebugWatchpoint* HaltedWatchpoint() const override {
        return nullptr;
    }

    void RewindBreakpointInstruction() override {}

private:
#if !defined(_WIN32)
    // Linux/Android assembly and signal definitions.
    static HaltReason ReturnToRunCodeByTrampoline(void* tpidr, GuestContext* ctx,
                                                  u64 trampoline_addr);
    static HaltReason ReturnToRunCodeByExceptionLevelChange(int tid, void* tpidr);

    static void ReturnToRunCodeByExceptionLevelChangeSignalHandler(int sig, void* info,
                                                                   void* raw_context);
    static void BreakFromRunCodeSignalHandler(int sig, void* info, void* raw_context);
    static void GuestAlignmentFaultSignalHandler(int sig, void* info, void* raw_context);
    static void GuestAccessFaultSignalHandler(int sig, void* info, void* raw_context);

    // C++ implementation functions for Linux/Android assembly definitions.
    static void* RestoreGuestContext(void* raw_context);
    static void SaveGuestContext(GuestContext* ctx, void* raw_context);
    static bool HandleFailedGuestFault(GuestContext* ctx, void* info, void* raw_context);
    static bool HandleGuestAlignmentFault(GuestContext* ctx, void* info, void* raw_context);
    static bool HandleGuestAccessFault(GuestContext* ctx, void* info, void* raw_context);
    static void HandleHostAlignmentFault(int sig, void* info, void* raw_context);
    static void HandleHostAccessFault(int sig, void* info, void* raw_context);
#endif

    // NativeExecutionParameters synchronization exists on both host platforms. Linux/Android use
    // the existing AArch64 assembly implementation; Windows provides the equivalent C++ atomic
    // implementation in arm_nce_windows.cpp.
    static void LockThreadParameters(void* tpidr);
    static void UnlockThreadParameters(void* tpidr);

public:
    Core::System& m_system;

    // Members set on initialization.
    std::size_t m_core_index{};
#if defined(_WIN32)
    std::unique_ptr<NCE::WindowsCrossThreadBreak> m_windows_break{};
    std::unique_ptr<NCE::WindowsX18FallbackRunner> m_windows_x18_runner{};
    bool m_windows_pending_nce_fault{};
    u64 m_windows_pending_nce_fault_address{};
    u64 m_windows_pending_nce_fault_page{};
    bool m_windows_pending_nce_fault_read{};
    // NCE read guard: the page of the current run of read faults, when it began, how many.
    u64 m_windows_read_guard_page{};
    s64 m_windows_read_guard_since_ns{};
    u32 m_windows_read_guard_repeats{};

    // Guest page-table state of recently active guest stacks. Filled by PrepareRunThread, where
    // blocking on the guest page-table lock is safe; consumed by RunThread, which must never call
    // into the guest kernel because a blocked lock reschedules another thread onto this core.
    struct WindowsGuestStackState {
        u64 thread_id{};
        u64 begin{};
        u64 end{};
        bool valid{};
        bool query_ok{};
        bool is_stack{};
    };
    std::array<WindowsGuestStackState, 8> m_windows_stack_states{};
    std::size_t m_windows_stack_state_next{};

    // Cross-thread break delivery. SignalInterrupt only records a request here and wakes this
    // core's break worker; the worker performs the Windows suspend/capture/resume without holding
    // any core or parameter lock. Layout: generation << 5 | flags.
    //   Active   - RunThread has published a native epoch for this ArmNce.
    //   Busy     - a worker owns delivery for the epoch; RunThread may not retire it.
    //   Pending  - a request arrived while Busy; the worker runs again before releasing.
    //   Retiring - RunThread is leaving; the worker stops and releases.
    //   HostSide - R2b-B3: RunThread is in host code that reads esr_el1 before it can next enter
    //              guest code (set when the epoch opens and after each guest return, cleared
    //              right before that read). The worker stops and releases, as for Retiring.
    static constexpr u64 DeliveryActive = 1ULL << 0;
    static constexpr u64 DeliveryBusy = 1ULL << 1;
    static constexpr u64 DeliveryPending = 1ULL << 2;
    static constexpr u64 DeliveryRetiring = 1ULL << 3;
    static constexpr u64 DeliveryHostSide = 1ULL << 4;
    static constexpr u64 DeliveryFlagMask = 0x1F;
    std::atomic<u64> m_windows_delivery_gate{};

#if SNAPRYU_DIAGNOSTICS
    // R2a measurements only (windows_nce_r2a_trace.h); never read by delivery decisions.
    std::atomic<u8> m_r2a_phase{};
    std::atomic<u64> m_r2a_post_ns{};
    std::atomic<u32> m_r2a_merged{};
    // R2b-B3: post time of a request the worker left to RunThread's own esr_el1 read.
    std::atomic<u64> m_r2a_host_side_post_ns{};
#endif

    // Worker entry point. Runs with Busy held and returns only after releasing it.
    void DeliverPendingBreak();
#else
    pid_t m_thread_id{-1};
#endif

    // Core context.
    GuestContext m_guest_ctx{};
    Kernel::KThread* m_running_thread{};

#if !defined(_WIN32)
    // Stack for POSIX signal processing.
    std::unique_ptr<u8[]> m_stack{};
#endif
};

} // namespace Core
