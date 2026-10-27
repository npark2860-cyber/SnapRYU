// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef _WIN32

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <thread>
#include <windows.h>

#include "common/common_types.h"
#include "common/hang_diag.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_hang_watch.h"

namespace Core::NCE {

// Timing of cross-thread break deliveries for the stall diagnostics (EDEN_HANG_DIAG). Nothing here
// changes delivery. While a step runs, the sending thread's HangWatch site names it
// (site=nce_break_<step>, value=target OS thread id), so a delivery that never returns shows up in
// the MasterSemaphore stall report. Completed deliveries are counted and summarized once a minute,
// and a slow one is logged. Logging happens only after ResumeThread: the suspended target may own
// a lock the logger needs.
namespace BreakWatch {

enum Step : u32 { Suspend, GetContext, Transform, SetContext, Resume, NumSteps };
inline constexpr std::array<const char*, NumSteps> STEP_NAMES{
    "suspend", "get_context", "transform", "set_context", "resume"};

inline constexpr s64 REPORT_NS = 60'000'000'000;
inline constexpr s64 SLOW_NS = 50'000'000;
inline constexpr u32 SLOW_LINES_PER_WINDOW = 16;

inline std::atomic<u64> g_deliveries{0};
inline std::atomic<u64> g_over_1ms{0};
inline std::atomic<u64> g_over_10ms{0};
inline std::atomic<u64> g_over_50ms{0};
inline std::atomic<u64> g_over_1s{0};
inline std::atomic<u64> g_max_ns{0};
inline std::atomic<u32> g_max_step{0};
inline std::atomic<u64> g_already_suspended{0};
inline std::atomic<u32> g_slow_lines{0};
inline std::atomic<u64> g_slow_suppressed{0};
inline std::atomic<u64> g_rescued{0};
inline std::atomic<s64> g_window_start_ns{0};

[[nodiscard]] inline s64 Now() noexcept {
    if constexpr (!Common::HangDiag::COMPILED) {
        return 0;
    }
    return Vulkan::HangWatch::NowNs();
}

struct Delivery {
    std::array<s64, NumSteps> step_ns{};
    s64 start_ns{};
    s64 mark_ns{};
    u64 target_tid{};
    DWORD previous_suspend_count{};
    bool get_ok{};
    bool set_called{};
    bool set_ok{};
    DWORD64 pc{};
    DWORD64 sp{};
    u32 rescue_stages{};

    void EndStep(Step step) noexcept {
        const s64 now = Now();
        step_ns[step] = now - mark_ns;
        mark_ns = now;
    }
};

inline void Finish(const Delivery& delivery) noexcept {
    if constexpr (!Common::HangDiag::COMPILED) {
        return;
    }
    const s64 total_ns = delivery.mark_ns - delivery.start_ns;
    u32 slowest = 0;
    for (u32 step = 1; step < NumSteps; ++step) {
        if (delivery.step_ns[step] > delivery.step_ns[slowest]) {
            slowest = step;
        }
    }
    g_deliveries.fetch_add(1, std::memory_order_relaxed);
    if (delivery.previous_suspend_count != 0) {
        g_already_suspended.fetch_add(1, std::memory_order_relaxed);
    }
    if (total_ns >= 1'000'000) {
        g_over_1ms.fetch_add(1, std::memory_order_relaxed);
    }
    if (total_ns >= 10'000'000) {
        g_over_10ms.fetch_add(1, std::memory_order_relaxed);
    }
    if (total_ns >= SLOW_NS) {
        g_over_50ms.fetch_add(1, std::memory_order_relaxed);
    }
    if (total_ns >= 1'000'000'000) {
        g_over_1s.fetch_add(1, std::memory_order_relaxed);
    }
    u64 max_ns = g_max_ns.load(std::memory_order_relaxed);
    while (static_cast<u64>(total_ns) > max_ns &&
           !g_max_ns.compare_exchange_weak(max_ns, static_cast<u64>(total_ns),
                                           std::memory_order_relaxed)) {
    }
    if (static_cast<u64>(total_ns) > max_ns) {
        g_max_step.store(slowest, std::memory_order_relaxed);
    }
    if (delivery.rescue_stages != 0) {
        g_rescued.fetch_add(1, std::memory_order_relaxed);
    }

    try {
        if (total_ns >= SLOW_NS || delivery.previous_suspend_count != 0 ||
            delivery.rescue_stages != 0) {
            if (g_slow_lines.fetch_add(1, std::memory_order_relaxed) < SLOW_LINES_PER_WINDOW) {
                LOG_WARNING(Core_ARM,
                            "NCE_BREAK_SLOW total_ms={} slowest={} suspend_us={} get_context_us={} "
                            "transform_us={} set_context_us={} resume_us={} target_tid={} "
                            "sender_tid={} prev_suspend={} get_ok={} set={} set_ok={} pc={:#x} "
                            "sp={:#x} rescue_stages={:#x}",
                            total_ns / 1'000'000, STEP_NAMES[slowest],
                            delivery.step_ns[Suspend] / 1000, delivery.step_ns[GetContext] / 1000,
                            delivery.step_ns[Transform] / 1000,
                            delivery.step_ns[SetContext] / 1000, delivery.step_ns[Resume] / 1000,
                            delivery.target_tid, GetCurrentThreadId(),
                            delivery.previous_suspend_count, delivery.get_ok,
                            delivery.set_called, delivery.set_ok, delivery.pc, delivery.sp,
                            delivery.rescue_stages);
            } else {
                g_slow_suppressed.fetch_add(1, std::memory_order_relaxed);
            }
        }

        s64 window_start = g_window_start_ns.load(std::memory_order_relaxed);
        if (window_start == 0) {
            g_window_start_ns.compare_exchange_strong(window_start, delivery.mark_ns,
                                                      std::memory_order_relaxed);
            return;
        }
        if (delivery.mark_ns - window_start < REPORT_NS ||
            !g_window_start_ns.compare_exchange_strong(window_start, delivery.mark_ns,
                                                       std::memory_order_relaxed)) {
            return;
        }
        const u64 window_max_ns = g_max_ns.exchange(0, std::memory_order_relaxed);
        LOG_INFO(Core_ARM,
                 "NCE_BREAK_STATS window_ms={} deliveries={} over_1ms={} over_10ms={} "
                 "over_50ms={} over_1s={} max_us={} max_step={} already_suspended={} "
                 "slow_suppressed={} rescued={}",
                 (delivery.mark_ns - window_start) / 1'000'000,
                 g_deliveries.exchange(0, std::memory_order_relaxed),
                 g_over_1ms.exchange(0, std::memory_order_relaxed),
                 g_over_10ms.exchange(0, std::memory_order_relaxed),
                 g_over_50ms.exchange(0, std::memory_order_relaxed),
                 g_over_1s.exchange(0, std::memory_order_relaxed), window_max_ns / 1000,
                 STEP_NAMES[g_max_step.load(std::memory_order_relaxed)],
                 g_already_suspended.exchange(0, std::memory_order_relaxed),
                 g_slow_suppressed.exchange(0, std::memory_order_relaxed),
                 g_rescued.exchange(0, std::memory_order_relaxed));
        g_slow_lines.store(0, std::memory_order_relaxed);
    } catch (...) {
    }
}

} // namespace BreakWatch

// Experiment, off unless EDEN_NCE_BREAK_RESCUE=1: which outside action releases a GetThreadContext
// that never returns. Seen twice on Windows build 26220 (WATERWAIT at 5706 s, CONDFLIP at 309 s):
// the sender sat in GetThreadContext on the target it had just suspended, holding the target's
// NativeExecutionParameters lock and the PhysicalCore guard, so the guest stopped submitting GPU
// work. Both times it returned when a dump tool finished; the dump also suspended the sender, so
// which thread and which operation released the wait is unknown. While one delivery has waited
// longer than a stage's delay, the watch thread starts that stage on its own short-lived kick
// thread, in this order:
//   target_read     GetThreadContext on the target            after 20 ms
//   sender_read     GetThreadContext on the sending thread    after 1 s
//   target_suspend  SuspendThread + ResumeThread on the target after 2 s
//   sender_suspend  SuspendThread + ResumeThread on the sender after 3 s
// and repeats a stage every 10 s while the same delivery still waits. A read does not change a
// thread and each suspend is paired with a resume, so a stage that was not needed only pauses a
// thread briefly. The watch thread never calls into another thread itself: a kick that blocks
// holds only its kick thread and that thread's own handle, and its stage is not started again
// until it returns. The sender logs NCE_BREAK_RESCUE_RESULT after ResumeThread with when each
// stage started and when the wait ended. This is a measurement, not a fix.
namespace BreakRescue {

enum Stage : u32 { TargetRead, SenderRead, TargetSuspend, SenderSuspend, NumStages };
inline constexpr std::array<const char*, NumStages> STAGE_NAMES{
    "target_read", "sender_read", "target_suspend", "sender_suspend"};
/// Ascending: the watch loop stops at the first stage that is not due yet.
inline constexpr std::array<s64, NumStages> STAGE_AFTER_NS{20'000'000, 1'000'000'000,
                                                          2'000'000'000, 3'000'000'000};
inline constexpr s64 STAGE_REPEAT_NS = 10'000'000'000;
inline constexpr DWORD POLL_MS = 5;
inline constexpr std::size_t MAX_SLOTS = 64;

struct Slot {
    /// Guards `handle` so Unregister cannot close it while the watch thread duplicates it. Held only
    /// around CloseHandle/DuplicateHandle, never around a call that waits on another thread.
    SRWLOCK handle_lock = SRWLOCK_INIT;
    /// THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME handle to the target, owned by the slot.
    HANDLE handle{};
    std::atomic<DWORD> target_tid{};
    std::atomic<bool> used{};
    /// Bumped by the sender before each wait and never reset, so records of an earlier wait (or an
    /// earlier owner of the slot) never match a later one.
    std::atomic<u64> generation{};
    /// Non-zero while the sender is inside GetThreadContext on this target.
    std::atomic<s64> waiting_since_ns{};
    std::atomic<DWORD> sender_tid{};
    /// Per stage, written by the watch thread only. The values belong to stage_generation.
    std::array<std::atomic<u64>, NumStages> stage_generation{};
    std::array<std::atomic<s64>, NumStages> stage_first_ns{};
    std::array<std::atomic<s64>, NumStages> stage_last_ns{};
    std::array<std::atomic<u32>, NumStages> stage_count{};
    /// True while a kick thread of the stage has not returned.
    std::array<std::atomic<bool>, NumStages> stage_busy{};
};

inline std::array<Slot, MAX_SLOTS> g_slots{};
inline std::atomic<std::size_t> g_slot_count{0};
inline std::atomic<bool> g_started{false};
/// Register/Unregister only; the watch thread never takes it.
inline SRWLOCK g_register_lock = SRWLOCK_INIT;

[[nodiscard]] inline bool Enabled() noexcept {
#if SNAPRYU_DIAGNOSTICS
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NCE_BREAK_RESCUE");
        return value == nullptr || !(value[0] == '0' && value[1] == '\0');
    }();
    return enabled;
#else
    return true;
#endif
}

[[nodiscard]] inline s64 SteadyNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Kick {
    Slot* slot;
    Stage stage;
    HANDLE thread;
};

inline void RunKick(Kick kick) noexcept {
    if (kick.stage == TargetRead || kick.stage == SenderRead) {
        ARM64_NT_CONTEXT context{};
        context.ContextFlags = CONTEXT_ARM64 | CONTEXT_CONTROL;
        static_cast<void>(GetThreadContext(kick.thread, reinterpret_cast<PCONTEXT>(&context)));
    } else if (SuspendThread(kick.thread) != static_cast<DWORD>(-1)) {
        static_cast<void>(ResumeThread(kick.thread));
    }
    CloseHandle(kick.thread);
    kick.slot->stage_busy[kick.stage].store(false, std::memory_order_release);
}

inline void StartKick(Slot& slot, Stage stage, u64 generation, s64 since, s64 now) noexcept {
    HANDLE thread{};
    const DWORD sender_tid = slot.sender_tid.load(std::memory_order_acquire);
    if (stage == TargetRead || stage == TargetSuspend) {
        AcquireSRWLockExclusive(&slot.handle_lock);
        const HANDLE process = GetCurrentProcess();
        if (slot.handle == nullptr ||
            !DuplicateHandle(process, slot.handle, process, &thread, 0, FALSE,
                             DUPLICATE_SAME_ACCESS)) {
            thread = nullptr;
        }
        ReleaseSRWLockExclusive(&slot.handle_lock);
    } else if (sender_tid != 0) {
        thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME |
                                THREAD_QUERY_LIMITED_INFORMATION,
                            FALSE, sender_tid);
        if (thread != nullptr && GetProcessIdOfThread(thread) != GetCurrentProcessId()) {
            CloseHandle(thread);
            thread = nullptr;
        }
    }
    if (thread == nullptr) {
        return;
    }
    // The handle pins one thread object. It is this delivery's target or sender only if the same
    // wait is still going on after the handle was taken: a sender inside the wait cannot exit, so
    // its thread id cannot have been reused in between.
    if (slot.generation.load(std::memory_order_acquire) != generation ||
        slot.waiting_since_ns.load(std::memory_order_acquire) != since) {
        CloseHandle(thread);
        return;
    }

    // Recorded before the kick thread starts, so a kick that ends the wait at once is never missing
    // from the sender's outcome.
    u32 count = 1;
    if (slot.stage_generation[stage].load(std::memory_order_relaxed) != generation) {
        slot.stage_first_ns[stage].store(now, std::memory_order_relaxed);
        slot.stage_last_ns[stage].store(now, std::memory_order_relaxed);
        slot.stage_count[stage].store(1, std::memory_order_relaxed);
        slot.stage_generation[stage].store(generation, std::memory_order_release);
    } else {
        slot.stage_last_ns[stage].store(now, std::memory_order_relaxed);
        count = slot.stage_count[stage].fetch_add(1, std::memory_order_relaxed) + 1;
    }
    slot.stage_busy[stage].store(true, std::memory_order_release);
    try {
        std::thread{RunKick, Kick{&slot, stage, thread}}.detach();
    } catch (...) {
        CloseHandle(thread);
        slot.stage_busy[stage].store(false, std::memory_order_release);
        return;
    }

    if (count == 1) {
        try {
            LOG_WARNING(Core_ARM,
                        "NCE_BREAK_RESCUE_KICK stage={} target_tid={} sender_tid={} waited_ms={}",
                        STAGE_NAMES[stage], slot.target_tid.load(std::memory_order_relaxed),
                        sender_tid, (now - since) / 1'000'000);
        } catch (...) {
        }
    }
}

inline void WatchLoop() noexcept {
    for (;;) {
        Sleep(POLL_MS);
        const std::size_t count =
            (std::min)(g_slot_count.load(std::memory_order_acquire), MAX_SLOTS);
        for (std::size_t index = 0; index < count; ++index) {
            Slot& slot = g_slots[index];
            // The sender stores generation before waiting_since_ns, so an unchanged generation
            // around the read means `since` belongs to that generation.
            const u64 generation = slot.generation.load(std::memory_order_acquire);
            const s64 since = slot.waiting_since_ns.load(std::memory_order_acquire);
            if (since == 0 || slot.generation.load(std::memory_order_acquire) != generation) {
                continue;
            }
            const s64 now = SteadyNs();
            for (u32 stage = 0; stage < NumStages; ++stage) {
                if (now - since < STAGE_AFTER_NS[stage]) {
                    break;
                }
                if (slot.stage_busy[stage].load(std::memory_order_acquire)) {
                    continue;
                }
                if (slot.stage_generation[stage].load(std::memory_order_relaxed) == generation &&
                    now - slot.stage_last_ns[stage].load(std::memory_order_relaxed) <
                        STAGE_REPEAT_NS) {
                    continue;
                }
                StartKick(slot, static_cast<Stage>(stage), generation, since, now);
            }
        }
    }
}

/// Takes ownership of a THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME handle for the calling target
/// thread. Returns the slot index, or -1 (and closes the handle) when every slot is taken.
[[nodiscard]] inline int Register(HANDLE thread, DWORD target_tid) noexcept {
    int index = -1;
    AcquireSRWLockExclusive(&g_register_lock);
    const std::size_t count = g_slot_count.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < count; ++i) {
        if (!g_slots[i].used.load(std::memory_order_relaxed)) {
            index = static_cast<int>(i);
            break;
        }
    }
    if (index < 0 && count < MAX_SLOTS) {
        index = static_cast<int>(count);
    }
    if (index >= 0) {
        Slot& slot = g_slots[static_cast<std::size_t>(index)];
        slot.waiting_since_ns.store(0, std::memory_order_relaxed);
        AcquireSRWLockExclusive(&slot.handle_lock);
        slot.handle = thread;
        slot.target_tid.store(target_tid, std::memory_order_relaxed);
        ReleaseSRWLockExclusive(&slot.handle_lock);
        slot.used.store(true, std::memory_order_relaxed);
        if (static_cast<std::size_t>(index) == count) {
            g_slot_count.store(count + 1, std::memory_order_release);
        }
    }
    ReleaseSRWLockExclusive(&g_register_lock);
    if (index < 0) {
        CloseHandle(thread);
        return -1;
    }

    bool expected = false;
    if (g_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        try {
            std::thread{WatchLoop}.detach();
            LOG_WARNING(Core_ARM,
                        "NCE_BREAK_RESCUE enabled experiment=1 target_read_ms={} "
                        "sender_read_ms={} target_suspend_ms={} sender_suspend_ms={} "
                        "repeat_ms={}",
                        STAGE_AFTER_NS[TargetRead] / 1'000'000,
                        STAGE_AFTER_NS[SenderRead] / 1'000'000,
                        STAGE_AFTER_NS[TargetSuspend] / 1'000'000,
                        STAGE_AFTER_NS[SenderSuspend] / 1'000'000, STAGE_REPEAT_NS / 1'000'000);
        } catch (...) {
            g_started.store(false, std::memory_order_release);
        }
    }
    return index;
}

/// Frees a slot and closes its handle. Kick threads hold their own duplicated handles, so closing
/// this one never pulls a handle out from under a kick.
inline void Unregister(int index) noexcept {
    if (index < 0 || static_cast<std::size_t>(index) >= MAX_SLOTS) {
        return;
    }
    Slot& slot = g_slots[static_cast<std::size_t>(index)];
    AcquireSRWLockExclusive(&g_register_lock);
    slot.waiting_since_ns.store(0, std::memory_order_release);
    AcquireSRWLockExclusive(&slot.handle_lock);
    const HANDLE thread = slot.handle;
    slot.handle = nullptr;
    slot.target_tid.store(0, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&slot.handle_lock);
    slot.used.store(false, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_register_lock);
    if (thread != nullptr) {
        CloseHandle(thread);
    }
}

[[nodiscard]] inline u64 BeginWait(Slot& slot) noexcept {
    slot.sender_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    const u64 generation = slot.generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    slot.waiting_since_ns.store(SteadyNs(), std::memory_order_release);
    return generation;
}

struct Outcome {
    s64 since_ns{};
    s64 end_ns{};
    std::array<s64, NumStages> first_ns{};
    std::array<s64, NumStages> last_ns{};
    std::array<u32, NumStages> count{};
    u32 started_mask{};
    u32 busy_mask{};
};

[[nodiscard]] inline Outcome EndWait(Slot& slot, u64 generation) noexcept {
    Outcome outcome{};
    outcome.since_ns = slot.waiting_since_ns.exchange(0, std::memory_order_acq_rel);
    outcome.end_ns = SteadyNs();
    for (u32 stage = 0; stage < NumStages; ++stage) {
        if (slot.stage_generation[stage].load(std::memory_order_acquire) != generation) {
            continue;
        }
        outcome.first_ns[stage] = slot.stage_first_ns[stage].load(std::memory_order_relaxed);
        outcome.last_ns[stage] = slot.stage_last_ns[stage].load(std::memory_order_relaxed);
        outcome.count[stage] = slot.stage_count[stage].load(std::memory_order_relaxed);
        outcome.started_mask |= 1U << stage;
        if (slot.stage_busy[stage].load(std::memory_order_acquire)) {
            outcome.busy_mask |= 1U << stage;
        }
    }
    return outcome;
}

/// Called by the sender after ResumeThread. last_kick is the stage started most recently before
/// the wait ended, release_after_ms how long after that start it ended, and busy lists stages
/// whose kick thread had not returned yet when it ended.
inline void LogOutcome(const Outcome& outcome, DWORD target_tid, bool get_ok) noexcept {
#if SNAPRYU_DIAGNOSTICS
    if (outcome.started_mask == 0) {
        return;
    }
    u32 last = 0;
    for (u32 stage = 1; stage < NumStages; ++stage) {
        if (outcome.last_ns[stage] > outcome.last_ns[last]) {
            last = stage;
        }
    }
    const auto at_ms = [&outcome](u32 stage) -> s64 {
        return (outcome.started_mask & (1U << stage)) != 0
                   ? (outcome.first_ns[stage] - outcome.since_ns) / 1'000'000
                   : -1;
    };
    try {
        LOG_WARNING(Core_ARM,
                    "NCE_BREAK_RESCUE_RESULT target_tid={} sender_tid={} waited_ms={} get_ok={} "
                    "target_read_ms={} sender_read_ms={} target_suspend_ms={} "
                    "sender_suspend_ms={} repeats={} last_kick={} release_after_ms={} busy={:#x}",
                    target_tid, GetCurrentThreadId(),
                    (outcome.end_ns - outcome.since_ns) / 1'000'000, get_ok, at_ms(TargetRead),
                    at_ms(SenderRead), at_ms(TargetSuspend), at_ms(SenderSuspend),
                    (std::max)({outcome.count[TargetRead], outcome.count[SenderRead],
                                outcome.count[TargetSuspend], outcome.count[SenderSuspend]}),
                    STAGE_NAMES[last], (outcome.end_ns - outcome.last_ns[last]) / 1'000'000,
                    outcome.busy_mask);
    } catch (...) {
    }
#else
    static_cast<void>(outcome);
    static_cast<void>(target_tid);
    static_cast<void>(get_ok);
#endif
}

} // namespace BreakRescue

class WindowsCrossThreadBreak {
public:
    using ContextTransform = bool (*)(ARM64_NT_CONTEXT& context, void* opaque) noexcept;
    // Runs after SetThreadContext was attempted for an accepted transform and before ResumeThread,
    // so the caller can publish or give back what the transform prepared while the target is still
    // stopped. set_ok tells which.
    using ContextCommit = void (*)(bool set_ok, void* opaque) noexcept;

    // Outcome of one SuspendTransformResume transaction. Every state except Delivered and
    // ResumeFailed leaves the target running with its original CONTEXT.
    enum class DeliveryResult {
        SuspendFailed, // Target was never suspended.
        GetFailed,     // Suspended and resumed; no state was read or changed.
        NotTransformed, // The transform declined; target resumed unchanged.
        SetFailed,     // The transform accepted but SetThreadContext failed; target resumed
                       // unchanged. Anything the transform consumed must be given back.
        Delivered,     // The new CONTEXT is installed and the target was resumed.
        ResumeFailed,  // ResumeThread failed. The target may still be suspended; check set_ok.
    };

    // R2b-S12: the Windows call a transaction failed in, with its GetLastError read right after
    // that call and before any other one. Filled only for SuspendFailed, GetFailed, SetFailed and
    // ResumeFailed.
    enum class FailedApi : u32 { None, Unbound, Suspend, GetContext, SetContext, Resume };
    struct DeliveryError {
        FailedApi api{};
        DWORD last_error{};
        DWORD previous_suspend_count{};
    };

    WindowsCrossThreadBreak() = default;

    ~WindowsCrossThreadBreak() {
        Reset();
    }

    // The thread handle may be used concurrently by SignalInterrupt on another host thread, so the
    // object is pinned in place and never moved.
    WindowsCrossThreadBreak(const WindowsCrossThreadBreak&) = delete;
    WindowsCrossThreadBreak& operator=(const WindowsCrossThreadBreak&) = delete;
    WindowsCrossThreadBreak(WindowsCrossThreadBreak&&) = delete;
    WindowsCrossThreadBreak& operator=(WindowsCrossThreadBreak&&) = delete;

    // Must be called by the target NCE host thread before another thread can request a break.
    //
    // ArmNce::Initialize() runs on every PhysicalCore::RunThread entry. The duplicated thread handle
    // is therefore created only once per OS thread: closing and re-duplicating it on every entry
    // raced with SignalInterrupt, which could call SuspendThread/SetThreadContext on a handle that
    // had just been closed (or recycled for an unrelated object). Only the host stack bounds are
    // refreshed per entry, because each guest KThread runs RunThread on its own host fiber stack.
    [[nodiscard]] bool BindCurrentThread() noexcept {
        const DWORD current_thread_id = GetCurrentThreadId();
        if (m_thread.load(std::memory_order_acquire) == nullptr ||
            m_thread_id.load(std::memory_order_relaxed) != current_thread_id) {
            HANDLE duplicate{};
            const HANDLE process = GetCurrentProcess();
            constexpr DWORD rights =
                THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT;
            if (!DuplicateHandle(process, GetCurrentThread(), process, &duplicate, rights, FALSE,
                                 0)) {
                return false;
            }

            // Rebinding to a different OS thread is not expected for a per-core ArmNce. If it ever
            // happens, the previous handle is intentionally leaked instead of closed so a
            // concurrent SuspendTransformResume can never observe a closed/recycled handle.
            m_thread_id.store(current_thread_id, std::memory_order_relaxed);
            m_thread.store(duplicate, std::memory_order_release);

            HANDLE rescue{};
            if (BreakRescue::Enabled() &&
                DuplicateHandle(process, GetCurrentThread(), process, &rescue,
                                THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, 0)) {
                BreakRescue::Unregister(m_rescue_slot.load(std::memory_order_relaxed));
                m_rescue_slot.store(BreakRescue::Register(rescue, current_thread_id),
                                    std::memory_order_release);
            }
        }

        const auto* const tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
        // Physical x18 of this thread whenever it runs native code; read by the break worker to
        // check a captured CONTEXT really belongs to this thread before reading its TEB slots.
        m_teb.store(reinterpret_cast<std::uintptr_t>(tib), std::memory_order_relaxed);
        m_host_stack_low.store(reinterpret_cast<std::uintptr_t>(tib->StackLimit),
                               std::memory_order_relaxed);
        m_host_stack_high.store(reinterpret_cast<std::uintptr_t>(tib->StackBase),
                                std::memory_order_release);
        return true;
    }

    // Suspend the target without executing any callback on its user stack, capture its native ARM64
    // state, allow the caller to classify/mutate that state, and resume it. Returning false from the
    // transform leaves the captured state unchanged and merely resumes the target. This is only the
    // Windows delivery primitive; deciding whether the captured PC is guest code or an IMP-005
    // transition window belongs to the transition owner.
    [[nodiscard]] DeliveryResult SuspendTransformResume(ContextTransform transform,
                                                        void* opaque = nullptr,
                                                        bool* set_ok = nullptr,
                                                        ContextCommit commit = nullptr,
                                                        DeliveryError* error = nullptr) const noexcept {
        if (set_ok != nullptr) {
            *set_ok = false;
        }
        DeliveryError failure{};
        const auto fail = [&](DeliveryResult result) noexcept {
            if (error != nullptr) {
                *error = failure;
            }
            return result;
        };
        const HANDLE thread = m_thread.load(std::memory_order_acquire);
        if (thread == nullptr || transform == nullptr) {
            failure.api = FailedApi::Unbound;
            return fail(DeliveryResult::SuspendFailed);
        }

        BreakWatch::Delivery watch{};
        watch.target_tid = m_thread_id.load(std::memory_order_relaxed);
        watch.start_ns = BreakWatch::Now();
        watch.mark_ns = watch.start_ns;

        DWORD previous_suspend_count;
        {
            const Vulkan::HangWatch::Scope site{"nce_break_suspend", watch.target_tid};
            previous_suspend_count = SuspendThread(thread);
        }
        if (previous_suspend_count == static_cast<DWORD>(-1)) {
            failure.api = FailedApi::Suspend;
            failure.last_error = GetLastError();
            return fail(DeliveryResult::SuspendFailed);
        }
        watch.EndStep(BreakWatch::Suspend);
        failure.previous_suspend_count = previous_suspend_count;
        watch.previous_suspend_count = previous_suspend_count;

        ARM64_NT_CONTEXT context{};
        context.ContextFlags =
            CONTEXT_ARM64 | CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT |
            CONTEXT_ARM64_X18;

        const int rescue_index = m_rescue_slot.load(std::memory_order_acquire);
        BreakRescue::Slot* const rescue =
            rescue_index >= 0 ? &BreakRescue::g_slots[static_cast<std::size_t>(rescue_index)]
                              : nullptr;
        const u64 rescue_generation = rescue != nullptr ? BreakRescue::BeginWait(*rescue) : 0;
        bool success;
        bool set_success = false;
        {
            const Vulkan::HangWatch::Scope site{"nce_break_get_context", watch.target_tid};
            success = GetThreadContext(thread, reinterpret_cast<PCONTEXT>(&context)) != FALSE;
            if (!success) {
                failure.api = FailedApi::GetContext;
                failure.last_error = GetLastError();
            }
        }
        BreakRescue::Outcome rescue_outcome{};
        if (rescue != nullptr) {
            rescue_outcome = BreakRescue::EndWait(*rescue, rescue_generation);
            watch.rescue_stages = rescue_outcome.started_mask;
        }
        watch.EndStep(BreakWatch::GetContext);
        watch.get_ok = success;
        watch.pc = context.Pc;
        watch.sp = context.Sp;

        bool transformed = false;
        if (success) {
            const Vulkan::HangWatch::Scope site{"nce_break_transform", watch.target_tid};
            transformed = transform(context, opaque);
        }
        watch.EndStep(BreakWatch::Transform);
        if (transformed) {
            const Vulkan::HangWatch::Scope site{"nce_break_set_context", watch.target_tid};
            set_success = SetThreadContext(thread, reinterpret_cast<PCONTEXT>(&context)) != FALSE;
            if (!set_success) {
                failure.api = FailedApi::SetContext;
                failure.last_error = GetLastError();
            }
            watch.set_called = true;
            watch.set_ok = set_success;
        }
        if (set_ok != nullptr) {
            *set_ok = set_success;
        }
        if (transformed && commit != nullptr) {
            commit(set_success, opaque);
        }
        watch.EndStep(BreakWatch::SetContext);

        DWORD resume_count;
        {
            const Vulkan::HangWatch::Scope site{"nce_break_resume", watch.target_tid};
            resume_count = ResumeThread(thread);
        }
        if (resume_count == static_cast<DWORD>(-1)) {
            // The target may still be stopped holding a lock the logger needs: return before the
            // reports below, which log.
            failure.api = FailedApi::Resume;
            failure.last_error = GetLastError();
            return fail(DeliveryResult::ResumeFailed);
        }
        watch.EndStep(BreakWatch::Resume);
        BreakWatch::Finish(watch);
        BreakRescue::LogOutcome(rescue_outcome, static_cast<DWORD>(watch.target_tid), watch.get_ok);
        if (!success) {
            return fail(DeliveryResult::GetFailed);
        }
        if (!transformed) {
            return DeliveryResult::NotTransformed;
        }
        return set_success ? DeliveryResult::Delivered : fail(DeliveryResult::SetFailed);
    }

    [[nodiscard]] bool IsHostStackPointer(DWORD64 sp) const noexcept {
        const auto high = m_host_stack_high.load(std::memory_order_acquire);
        const auto low = m_host_stack_low.load(std::memory_order_relaxed);
        const auto value = static_cast<std::uintptr_t>(sp);
        return low != 0 && value >= low && value < high;
    }

    // TEB of the bound thread, published before the host stack bounds by BindCurrentThread.
    [[nodiscard]] std::uintptr_t Teb() const noexcept {
        if (m_host_stack_high.load(std::memory_order_acquire) == 0) {
            return 0;
        }
        return m_teb.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool IsBound() const noexcept {
        return m_thread.load(std::memory_order_acquire) != nullptr;
    }

    [[nodiscard]] std::uintptr_t HostStackLow() const noexcept {
        return m_host_stack_low.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uintptr_t HostStackHigh() const noexcept {
        return m_host_stack_high.load(std::memory_order_acquire);
    }

private:
    void Reset() noexcept {
        if (const HANDLE thread = m_thread.exchange(nullptr, std::memory_order_acq_rel);
            thread != nullptr) {
            CloseHandle(thread);
        }
        BreakRescue::Unregister(m_rescue_slot.exchange(-1, std::memory_order_acq_rel));
        m_thread_id.store(0, std::memory_order_relaxed);
        m_host_stack_low.store(0, std::memory_order_relaxed);
        m_host_stack_high.store(0, std::memory_order_relaxed);
        m_teb.store(0, std::memory_order_relaxed);
    }

    std::atomic<HANDLE> m_thread{};
    std::atomic<DWORD> m_thread_id{};
    std::atomic<std::uintptr_t> m_host_stack_low{};
    std::atomic<std::uintptr_t> m_host_stack_high{};
    std::atomic<std::uintptr_t> m_teb{};
    std::atomic<int> m_rescue_slot{-1};
};

} // namespace Core::NCE

#endif // _WIN32
