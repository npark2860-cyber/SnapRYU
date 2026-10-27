// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <mutex>

#include "common/common_types.h"
#include "common/hang_diag.h"

// Stall diagnostics. Threads mark where they block, the GPU thread summarizes what each
// submission recorded, and the MasterSemaphore watch thread reports both when the GPU timeline
// or the frame count stops advancing. Nothing here changes rendering. Built with EDEN_HANG_DIAG
// OFF, the recording helpers below do nothing and the marks stay off; ValveMs is behavior, not a
// diagnostic, and stays.
namespace Vulkan::HangWatch {

[[nodiscard]] inline s64 NowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct WaitSlot {
    std::atomic<const char*> role{};
    std::atomic<const char*> site{};
    std::atomic<const char*> outer{};
    std::atomic<u64> value{};
    std::atomic<s64> since_ns{};
};

inline constexpr size_t MAX_WAIT_SLOTS = 64;
inline std::array<WaitSlot, MAX_WAIT_SLOTS> g_wait_slots{};
inline std::atomic<size_t> g_wait_slot_count{0};
inline thread_local WaitSlot* t_wait_slot = nullptr;
inline thread_local bool t_wait_slot_claimed = false;
inline thread_local const char* t_role = nullptr; ///< Reported as "other" while unset.

/// Names the calling thread in stall reports.
inline void SetThreadRole(const char* role) noexcept {
    if constexpr (!Common::HangDiag::COMPILED) {
        return;
    }
    t_role = role;
    if (t_wait_slot != nullptr) {
        t_wait_slot->role.store(role, std::memory_order_relaxed);
    }
}

/// Names the calling thread unless it already has a role.
inline void SetThreadRoleIfUnset(const char* role) noexcept {
    if (t_role == nullptr) {
        SetThreadRole(role);
    }
}

[[nodiscard]] inline WaitSlot* ThisSlot() noexcept {
    if (!t_wait_slot_claimed) {
        t_wait_slot_claimed = true;
        const size_t index = g_wait_slot_count.fetch_add(1, std::memory_order_relaxed);
        if (index < MAX_WAIT_SLOTS) {
            t_wait_slot = &g_wait_slots[index];
            t_wait_slot->role.store(t_role, std::memory_order_relaxed);
        }
    }
    return t_wait_slot;
}

/// Marks the calling thread as blocked (or busy) at site until the scope ends. Scopes nest; a
/// report shows the innermost site and the one around it.
class Scope {
public:
    explicit Scope(const char* site, u64 value = 0) noexcept
        : slot{Common::HangDiag::COMPILED ? ThisSlot() : nullptr} {
        if (slot == nullptr) {
            return;
        }
        prev_site = slot->site.load(std::memory_order_relaxed);
        prev_outer = slot->outer.load(std::memory_order_relaxed);
        prev_value = slot->value.load(std::memory_order_relaxed);
        prev_since = slot->since_ns.load(std::memory_order_relaxed);
        slot->outer.store(prev_site, std::memory_order_relaxed);
        slot->value.store(value, std::memory_order_relaxed);
        slot->since_ns.store(NowNs(), std::memory_order_relaxed);
        slot->site.store(site, std::memory_order_release);
    }

    ~Scope() {
        if (slot == nullptr) {
            return;
        }
        slot->outer.store(prev_outer, std::memory_order_relaxed);
        slot->value.store(prev_value, std::memory_order_relaxed);
        slot->since_ns.store(prev_since, std::memory_order_relaxed);
        slot->site.store(prev_site, std::memory_order_release);
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    WaitSlot* slot;
    const char* prev_site{};
    const char* prev_outer{};
    u64 prev_value{};
    s64 prev_since{};
};

enum Counter : size_t {
    Draws,
    PredicatedDraws,
    IndirectDraws,
    Dispatches,
    Clears,
    RenderPasses,
    QueryBegins,
    QueryCopySlots,
    QueryPasses,
    Resolves,
    Publications,
    CapWaits,
    Frames,
    StreamReuse, ///< Upload stream bytes handed out again before the GPU finished reading them.
    ArgsChanged, ///< Predicated draw arguments that changed while their submission was in flight.
    NumCounters,
};

inline constexpr std::array<const char*, NumCounters> COUNTER_NAMES{
    "draws",        "pred_draws",   "indirect",     "dispatches",   "clears",
    "renderpasses", "query_begins", "query_copies", "query_passes", "resolves",
    "publications", "cap_waits",    "frames",       "stream_reuse", "args_changed",
};

/// Counts for the submission being recorded, and totals for the current log window.
inline std::array<std::atomic<u32>, NumCounters> g_current{};
inline std::array<std::atomic<u64>, NumCounters> g_window{};
inline std::atomic<u64> g_window_submits{0};

inline void Count(Counter counter, u32 amount = 1) noexcept {
    if constexpr (!Common::HangDiag::COMPILED) {
        return;
    }
    g_current[counter].fetch_add(amount, std::memory_order_relaxed);
}

/// MSAA resolve shadow lifetime (review item C3): totals since start, logged every 5 s as
/// RESOLVE_SHADOW. A stale use is a render pass request on a framebuffer whose resolve shadow
/// was replaced or erased after the framebuffer was made; stale_destroyed counts those whose
/// shadow image was already destroyed, so the GPU would write freed memory.
enum ShadowEvent : size_t {
    ShadowCreated,
    ShadowReplaced,
    ShadowErased,
    ShadowDestroyed,
    ShadowFramebuffers,
    ShadowUses,
    ShadowStaleUses,
    ShadowStaleDestroyed,
    NumShadowEvents,
};

inline constexpr std::array<const char*, NumShadowEvents> SHADOW_EVENT_NAMES{
    "created",      "replaced", "erased",     "destroyed",
    "framebuffers", "uses",     "stale_uses", "stale_destroyed",
};

inline std::array<std::atomic<u64>, NumShadowEvents> g_shadow{};
inline std::atomic<u32> g_shadow_logs{0};
inline constexpr u32 MAX_SHADOW_LOGS = 16;

inline void CountShadow(ShadowEvent event) noexcept {
    if constexpr (!Common::HangDiag::COMPILED) {
        return;
    }
    g_shadow[event].fetch_add(1, std::memory_order_relaxed);
}

/// Guest shaders recorded for a submission, deduplicated, in first use order: candidates, not
/// proof of execution (the worker can still skip a command; helper passes are not listed).
/// Graphics entries are (vertex hash, fragment hash); compute entries are (compute hash,
/// COMPUTE_TAG).
inline constexpr size_t PIPE_SLOTS = 48;
inline constexpr u64 COMPUTE_TAG = ~u64{0};
struct PipeList {
    std::array<std::atomic<u64>, PIPE_SLOTS * 2> words{};
    std::atomic<u32> count{};
    std::atomic<u32> dropped{}; ///< Distinct shaders past PIPE_SLOTS.
};

inline PipeList g_current_pipes{};
inline std::mutex g_pipe_mutex;
inline const void* g_last_pipe = nullptr; ///< Guarded by g_pipe_mutex.
inline std::atomic<u64> g_pipe_epoch{0};  ///< Advances when a submission takes the list.
inline thread_local const void* t_last_pipe = nullptr;
inline thread_local u64 t_last_pipe_epoch = ~u64{0};

/// Optional GPU progress marks (default off), diagnostic only. EDEN_HANG_GPU_MARKS, exact text:
/// "1": two timestamps per master submission, a TOP_OF_PIPE at the end of its upload command buffer
///      (start mark) and a BOTTOM_OF_PIPE at the end of its main one (end mark).
/// "2": four per submission: a TOP_OF_PIPE and a BOTTOM_OF_PIPE at the end of the upload command
///      buffer, a TOP_OF_PIPE as the first command of the main one and a BOTTOM_OF_PIPE at its end.
/// "3": "2" plus step marks, a BOTTOM_OF_PIPE after each render pass, compute dispatch and query
///      helper pass, at most 60 per submission.
/// A report only says whether each mark was observed. A timestamp follows the commands before it,
/// not the commands after it, and may be written late: an unobserved mark does not prove that the
/// work before it is unfinished. Marks add GPU work and change timing.
[[nodiscard]] inline u32 GpuMarksLevel() noexcept {
    if constexpr (!Common::HangDiag::COMPILED) {
        return 0;
    }
    static const u32 value = [] {
        const char* const text = std::getenv("EDEN_HANG_GPU_MARKS");
        if (text == nullptr || text[0] < '1' || text[0] > '3' || text[1] != '\0') {
            return u32{0};
        }
        return static_cast<u32>(text[0] - '0');
    }();
    return value;
}

/// Level 3 step marks: guest shaders noted on this thread (the GPU thread) since its last step
/// mark, in first use order. The Scheduler copies and clears it at each step mark.
inline constexpr size_t STEP_PIPES = 4;
struct StepPipes {
    std::array<u64, STEP_PIPES * 2> words{};
    u32 count = 0;
    u32 dropped = 0; ///< Uses of shaders not listed once STEP_PIPES distinct ones are.
};
inline thread_local StepPipes t_step_pipes{};

inline void NoteStepPipe(u64 a, u64 b) noexcept {
    StepPipes& pipes = t_step_pipes;
    for (u32 i = 0; i < pipes.count; ++i) {
        if (pipes.words[2 * i] == a && pipes.words[2 * i + 1] == b) {
            return;
        }
    }
    if (pipes.count >= STEP_PIPES) {
        ++pipes.dropped;
        return;
    }
    pipes.words[2 * pipes.count] = a;
    pipes.words[2 * pipes.count + 1] = b;
    ++pipes.count;
}

/// Level 3 step marks: the largest draw and dispatch sizes recorded on this thread (the GPU
/// thread) since its last step mark. Direct arguments only; indirect ones live in GPU memory and
/// are counted with their CPU-side maximum count. The Scheduler copies and clears it with the
/// pipes.
struct StepDraws {
    u32 max_vertices = 0;  ///< Vertex (or index) count of one direct draw.
    u32 max_instances = 0; ///< Instance count of one direct draw.
    u64 max_draw_work = 0; ///< Vertices x instances of one direct draw.
    u32 indirect_draws = 0;
    u32 max_indirect_count = 0; ///< Largest draw count bound of one indirect draw command.
    u32 indirect_dispatches = 0;
    std::array<u32, 3> max_groups{}; ///< Workgroups of the largest direct dispatch.
};
inline thread_local StepDraws t_step_draws{};

inline void NoteStepDraw(u32 vertices, u32 instances) noexcept {
    if (GpuMarksLevel() < 3) {
        return;
    }
    StepDraws& draws = t_step_draws;
    draws.max_vertices = (std::max)(draws.max_vertices, vertices);
    draws.max_instances = (std::max)(draws.max_instances, instances);
    draws.max_draw_work = (std::max)(draws.max_draw_work, u64{vertices} * instances);
}

inline void NoteStepIndirectDraw(u32 max_count) noexcept {
    if (GpuMarksLevel() < 3) {
        return;
    }
    StepDraws& draws = t_step_draws;
    ++draws.indirect_draws;
    draws.max_indirect_count = (std::max)(draws.max_indirect_count, max_count);
}

inline void NoteStepDispatch(u32 x, u32 y, u32 z) noexcept {
    if (GpuMarksLevel() < 3) {
        return;
    }
    StepDraws& draws = t_step_draws;
    const auto& groups = draws.max_groups;
    if (u64{x} * y * z > u64{groups[0]} * groups[1] * groups[2]) {
        draws.max_groups = {x, y, z};
    }
}

inline void NoteStepIndirectDispatch() noexcept {
    if (GpuMarksLevel() < 3) {
        return;
    }
    ++t_step_draws.indirect_dispatches;
}

/// Draw or dispatch thread: notes the pipeline in the submission being recorded.
inline void NotePipeline(const void* pipeline, u64 a, u64 b) {
    if constexpr (!Common::HangDiag::COMPILED) {
        return;
    }
    if (GpuMarksLevel() >= 3) {
        NoteStepPipe(a, b);
    }
    const u64 epoch = g_pipe_epoch.load(std::memory_order_relaxed);
    if (pipeline == t_last_pipe && epoch == t_last_pipe_epoch) {
        return;
    }
    t_last_pipe = pipeline;
    t_last_pipe_epoch = epoch;
    std::scoped_lock lock{g_pipe_mutex};
    if (pipeline == g_last_pipe) {
        return;
    }
    g_last_pipe = pipeline;
    const u32 count = g_current_pipes.count.load(std::memory_order_relaxed);
    for (u32 i = 0; i < count; ++i) {
        if (g_current_pipes.words[2 * i].load(std::memory_order_relaxed) == a &&
            g_current_pipes.words[2 * i + 1].load(std::memory_order_relaxed) == b) {
            return;
        }
    }
    if (count >= PIPE_SLOTS) {
        g_current_pipes.dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_current_pipes.words[2 * count].store(a, std::memory_order_relaxed);
    g_current_pipes.words[2 * count + 1].store(b, std::memory_order_relaxed);
    g_current_pipes.count.store(count + 1, std::memory_order_relaxed);
}

struct SubmitRecord {
    std::atomic<u64> tick{};
    std::atomic<s64> time_ns{};
    std::atomic<u64> queue_seq{}; ///< Queue order of its vkQueueSubmit; 0 until submitted.
    std::atomic<s64> submit_ns{};
    std::array<std::atomic<u32>, NumCounters> counts{};
    PipeList pipes{};
};

inline constexpr size_t SUBMIT_RING = 1024;
inline std::array<SubmitRecord, SUBMIT_RING> g_submits{};

/// GPU thread, when a submission gets its tick: files what was recorded since the last one.
inline void RecordSubmit(u64 tick) noexcept {
    if constexpr (!Common::HangDiag::COMPILED) {
        return;
    }
    SubmitRecord& record = g_submits[tick % SUBMIT_RING];
    record.tick.store(0, std::memory_order_relaxed);
    record.queue_seq.store(0, std::memory_order_relaxed);
    record.submit_ns.store(0, std::memory_order_relaxed);
    for (size_t i = 0; i < NumCounters; ++i) {
        const u32 count = g_current[i].exchange(0, std::memory_order_relaxed);
        record.counts[i].store(count, std::memory_order_relaxed);
        g_window[i].fetch_add(count, std::memory_order_relaxed);
    }
    try {
        std::scoped_lock lock{g_pipe_mutex};
        const u32 count = g_current_pipes.count.load(std::memory_order_relaxed);
        for (u32 i = 0; i < count * 2; ++i) {
            record.pipes.words[i].store(g_current_pipes.words[i].load(std::memory_order_relaxed),
                                        std::memory_order_relaxed);
        }
        record.pipes.count.store(count, std::memory_order_relaxed);
        record.pipes.dropped.store(g_current_pipes.dropped.exchange(0, std::memory_order_relaxed),
                                   std::memory_order_relaxed);
        g_current_pipes.count.store(0, std::memory_order_relaxed);
        g_last_pipe = nullptr;
        g_pipe_epoch.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
        record.pipes.count.store(0, std::memory_order_relaxed);
    }
    g_window_submits.fetch_add(1, std::memory_order_relaxed);
    record.time_ns.store(NowNs(), std::memory_order_relaxed);
    record.tick.store(tick, std::memory_order_release);
}

/// Highest master tick handed to vkQueueSubmit (mirror of the MasterSemaphore value).
inline std::atomic<u64> g_enqueued_tick{0};

/// Order of operations on the graphics queue. Master submissions, present copies and presents
/// each take the next number while holding Scheduler::submit_mutex, right before the call.
inline std::atomic<u64> g_queue_seq{0};

[[nodiscard]] inline u64 NextQueueSeq() noexcept {
    return g_queue_seq.fetch_add(1, std::memory_order_relaxed) + 1;
}

/// Worker thread, under submit_mutex: the master submission of tick takes its queue position.
inline void NoteQueueSubmit(u64 tick) noexcept {
    if constexpr (!Common::HangDiag::COMPILED) {
        return;
    }
    const u64 seq = NextQueueSeq();
    SubmitRecord& record = g_submits[tick % SUBMIT_RING];
    if (record.tick.load(std::memory_order_acquire) == tick) {
        record.submit_ns.store(NowNs(), std::memory_order_relaxed);
        record.queue_seq.store(seq, std::memory_order_release);
    }
}

/// Present path state for stall reports: the PresentManager registers a probe that logs its
/// frames. Guarded by g_present_probe_mutex so the manager can unregister before it is destroyed.
/// pending_seq is the queue position of the oldest unfinished master submission (0 if unknown).
using PresentProbe = void (*)(void* context, u64 completed_tick, u64 pending_seq);
inline std::mutex g_present_probe_mutex;
inline PresentProbe g_present_probe = nullptr;
inline void* g_present_probe_context = nullptr;

/// GPU progress marks of any level, see GpuMarksLevel.
[[nodiscard]] inline bool GpuMarksEnabled() noexcept {
    return GpuMarksLevel() != 0;
}

/// The Scheduler registers the mark reader; same lifetime rule as the present probe.
using MarkProbe = void (*)(void* context, u64 completed_tick, u64 issued);
inline std::mutex g_mark_probe_mutex;
inline MarkProbe g_mark_probe = nullptr;
inline void* g_mark_probe_context = nullptr;

/// Frames composited by the GPU thread and presented by the present thread.
inline std::atomic<u64> g_frames{0};
inline std::atomic<u64> g_presents{0};

/// Monotonic counts outside the renderer. A stall report shows which of them still move.
enum Progress : size_t {
    GpuCmdsPushed,  ///< Commands queued for the GPU thread (lists, ticks, flushes).
    GpuListsPushed, ///< Command lists queued for the GPU thread.
    GpuCmdsDone,    ///< Commands the GPU thread finished.
    SemAcquires,    ///< Puller semaphore acquires.
    SemSpins,       ///< Re-reads while a puller semaphore acquire waited.
    SemBreaks,      ///< Puller semaphore acquires given up after the valve time.
    SyncOpWaits,    ///< Waits for a GPU-thread sync operation (CPU flushes, composites).
    NvEventWaits,   ///< Guest syncpoint waits handed to an event.
    NvStallWaits,   ///< Guest syncpoint waits that stopped the application on the host.
    NvStallBreaks,  ///< Such waits given up after the valve time.
    NumProgress,
};

inline constexpr std::array<const char*, NumProgress> PROGRESS_NAMES{
    "gpu_cmds_pushed", "gpu_lists_pushed", "gpu_cmds_done",  "sem_acquires",
    "sem_spins",       "sem_breaks",       "sync_op_waits",  "nv_event_waits",
    "nv_stall_waits",  "nv_stall_breaks",
};

inline std::array<std::atomic<u64>, NumProgress> g_progress{};

inline void Bump(Progress counter, u64 amount = 1) noexcept {
    if constexpr (!Common::HangDiag::COMPILED) {
        return;
    }
    g_progress[counter].fetch_add(amount, std::memory_order_relaxed);
}

/// Milliseconds after which a wait that real hardware would not hold forever is given up and
/// logged: the puller semaphore acquire and the application-stopping guest syncpoint wait.
/// EDEN_HANG_VALVE_MS, decimal; 0 keeps the unbounded waits. Default 2000.
[[nodiscard]] inline s64 ValveMs() noexcept {
    static const s64 value = [] {
        const char* const text = std::getenv("EDEN_HANG_VALVE_MS");
        if (text == nullptr || text[0] == '\0') {
            return s64{2000};
        }
        char* end = nullptr;
        const long parsed = std::strtol(text, &end, 10);
        if (end == text || *end != '\0' || parsed < 0 || parsed > 600'000) {
            return s64{2000};
        }
        return static_cast<s64>(parsed);
    }();
    return value;
}

/// The puller semaphore acquire that is waiting, or the last one that waited.
inline std::atomic<u64> g_sem_address{};
inline std::atomic<u32> g_sem_expected{};
inline std::atomic<u32> g_sem_word{};
inline std::atomic<s64> g_sem_since_ns{}; ///< 0 while no acquire waits.

/// Guest syncpoint waits handed to events, newest last. A report lists the unsignalled ones.
struct NvWait {
    std::atomic<u64> serial{}; ///< 0 while being written.
    std::atomic<u32> slot{};
    std::atomic<u32> syncpoint{};
    std::atomic<u32> target{};
    std::atomic<u32> max_at_wait{};
    std::atomic<u32> host_at_wait{};
    std::atomic<s64> time_ns{};
    std::atomic<bool> signalled{};
};

inline constexpr size_t NV_WAIT_RING = 16;
inline std::array<NvWait, NV_WAIT_RING> g_nv_waits{};
inline std::atomic<u64> g_nv_wait_serial{0};

/// Host1x syncpoint manager, for the current values in reports. Set by guest syncpoint waits.
inline std::atomic<const void*> g_host1x_syncpoints{};

/// Records a wait handed to an event; returns the serial for MarkNvWaitSignalled.
inline u64 RecordNvWait(u32 slot, u32 syncpoint, u32 target, u32 max_value,
                        u32 host_value) noexcept {
    if constexpr (!Common::HangDiag::COMPILED) {
        return 0;
    }
    const u64 serial = g_nv_wait_serial.fetch_add(1, std::memory_order_relaxed) + 1;
    NvWait& wait = g_nv_waits[serial % NV_WAIT_RING];
    wait.serial.store(0, std::memory_order_relaxed);
    wait.slot.store(slot, std::memory_order_relaxed);
    wait.syncpoint.store(syncpoint, std::memory_order_relaxed);
    wait.target.store(target, std::memory_order_relaxed);
    wait.max_at_wait.store(max_value, std::memory_order_relaxed);
    wait.host_at_wait.store(host_value, std::memory_order_relaxed);
    wait.time_ns.store(NowNs(), std::memory_order_relaxed);
    wait.signalled.store(false, std::memory_order_relaxed);
    wait.serial.store(serial, std::memory_order_release);
    return serial;
}

inline void MarkNvWaitSignalled(u64 serial) noexcept {
    if constexpr (!Common::HangDiag::COMPILED) {
        return;
    }
    NvWait& wait = g_nv_waits[serial % NV_WAIT_RING];
    if (wait.serial.load(std::memory_order_acquire) == serial) {
        wait.signalled.store(true, std::memory_order_release);
    }
}

/// The guest stopped waiting on this event (it timed out and cancelled the wait).
inline void MarkNvSlotCancelled(u32 slot) noexcept {
    if constexpr (!Common::HangDiag::COMPILED) {
        return;
    }
    for (NvWait& wait : g_nv_waits) {
        if (wait.serial.load(std::memory_order_acquire) != 0 &&
            wait.slot.load(std::memory_order_relaxed) == slot) {
            wait.signalled.store(true, std::memory_order_release);
        }
    }
}

} // namespace Vulkan::HangWatch
