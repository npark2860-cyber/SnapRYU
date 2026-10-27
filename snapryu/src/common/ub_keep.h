// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// UB_KEEP: GPU commands read constant pages as they were when the commands were submitted.
//
// Enabled by default for every fragment shader in SnapRYU. The user setting is latched when the
// buffer cache starts a session; changing it never changes an active session. Diagnostic builds
// allow EDEN_UB_KEEP / FS / PAGES / COPIES overrides, read anew for each session. Release builds use
// only the user setting and the validated R2 limits.
//
// Problem (TotK, measured with EDEN_CGT_WATCH_FS): the game submits frame N, then other guest threads
// rewrite constant buffers that frame N still reads near the end of its command list, without
// waiting for frame N. On hardware the GPU has run those draws by then. Eden's GPU thread can be far
// behind the guest, so the draws read the next layout's world matrices (polygons pop for a frame).
//
// Rule: a guest CPU write that lands after a command list was pushed is hidden from that list.
// Exception: after the GPU passes a semaphore acquire that a GPU command did not release (the CPU
// released it, or nobody did), the commands of that channel pushed before the acquire passed read
// memory as it is from then on.
//
// How:
//   - Pages read by fast uniform buffers of selected draws (EDEN_UB_KEEP_FS: comma list of fragment
//     shader hashes in hex, or "all"; default "all") are armed:
//     DeviceMemoryManager UpdatePagesCachedCount +1, so CPU writes take the rasterizer write path
//     (Memory::InvalidateNCE under NCE, Memory::HandleRasterizerWrite otherwise).
//   - The first CPU write to an armed page while submitted commands are not done copies the page
//     and disarms it, so later writes run at full speed. The copy holds the page as it was when the
//     last list was pushed: the page was armed (write protected) from that push until this write.
//     The page is armed again before the next command list is pushed (under the queue lock).
//   - Under NCE without EDEN_NCE_WRITE_STEP the store itself faults, so the copy is taken before
//     the store and a store that retries after the next push faults again: a write is hidden from
//     exactly the lists pushed before it landed. On the Memory::Write path (Dynarmic, write step,
//     HLE writes) the store follows the hook; a store racing a push without any guest ordering
//     between them can land on either side (on hardware such a race has no defined order either).
//   - Fast uniform buffer reads on the GPU thread use the oldest copy made for a list at or after
//     the current command. A copy is freed when the GPU thread finishes the last command it covers.
//   - GPU-thread writes to guest memory (MemoryManager::WriteBlock*, e.g. constant buffer data) are
//     applied to the copies of that page after they land, so commands keep seeing their own GPU
//     writes.
//   - Semaphores: GPU payload releases (rasterizer Query, QueryType::Payload) are remembered per
//     GPU address. An acquire satisfied by a remembered GPU release keeps every copy: that release
//     ran in an earlier command, so the writes it publishes were made before a push the copies
//     already include. Any other acquire makes the commands of its channel from the current one up
//     to the last pushed list skip copies made before the acquire; copies stay for other channels.
//   - DeviceMemoryManager::Unmap: armed pages in the range are disarmed and forgotten, with their
//     copies, while the range is still mapped, and no page in it is armed until the unmap is done.
//     An armed page keeps its guest page write protected through the cached count; once its device
//     address is gone, a CPU write there resolves to no device page, nothing disarms it and NCE
//     faults on that store forever (UBK-BN-all hang, 2026-10-13).
// Pages not read by a selected draw for STALE_SUBMITS submits are not armed again.
//
// Cost: protection changes (the arm hook, a VirtualProtect under NCE) never run under the ub_keep
// mutex, so the GPU thread's reads never wait for one. Each call takes a ticket under the mutex
// and applies its changes in ticket order, so every page's count changes in the order its state
// changed. BeforePush re-arms contiguous pages with one call per range.
//
// Diagnostic builds count and log UB_KEEP costs; release builds omit those measurements.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#if SNAPRYU_DIAGNOSTICS
#include <string_view>
#include <unordered_set>
#endif

#include "common/common_types.h"
#include "common/logging.h"

#if SNAPRYU_DIAGNOSTICS
#ifdef _WIN32
extern "C" __declspec(dllimport) short
#if !defined(_M_ARM64) && !defined(__aarch64__)
    __stdcall
#endif
    GetAsyncKeyState(int vKey);
#endif
#endif

namespace Common::UbKeep {

constexpr u64 PAGE_BITS = 12;
constexpr u64 PAGE_SIZE = u64{1} << PAGE_BITS;
constexpr u64 STALE_SUBMITS = 1024;
constexpr size_t MAX_GPU_RELEASES = 4096;

/// Device memory access, installed by the buffer cache (video_core) for the core hooks.
struct Hooks {
    void* ctx = nullptr;
    void (*arm)(void* ctx, u64 addr, u64 size, s32 delta) = nullptr; // UpdatePagesCachedCount
    u8* (*pointer)(void* ctx, u64 page) = nullptr;         // host pointer of a device page
};

/// How a semaphore acquire compares the memory word.
enum class AcquireKind : u32 {
    Equal,  // word == value
    Gequal, // word >= value
    Other,  // anything else: compared with the word read
};

#if SNAPRYU_DIAGNOSTICS
// b75 UB_WIDE (EDEN_UB_KEEP_WIDE=detect|keep, needs UB_KEEP on): the same rule for the buffer
// cache uploads that fast uniform reads do not cover (classic uniform, storage, vertex, index,
// indirect and texel buffers of draws and dispatches). A page uploaded a second time (data the CPU
// rewrites, not one-time level data) is armed like a page of a selected fast uniform buffer. Each
// such upload then checks whether the copies for the command running now would change the
// uploaded bytes: if so, the upload read bytes the CPU wrote after this command's list was pushed
// ("late"). detect only counts them. keep also uploads the copy bytes, and the buffer cache marks
// the range CPU-modified again so later lists upload guest memory as it is.
// Logged: "UB_WIDE ready b75", UB_WIDE totals (5 s), UB_WIDE_LATE for each 250 ms window with late
// bytes, UB_WIDE_MARK when the user presses F8 (right after seeing a polygon pop).
enum class WideMode : u8 { Off, Detect, Keep };
inline constexpr size_t WIDE_KINDS = 16;            // binding kind = bit index (BufTrace::Kind)
inline constexpr size_t WIDE_SEEN_MAX = size_t{1} << 20; // pages remembered as uploaded once
inline constexpr size_t WIDE_WHO_MAX = 4096;        // draws/dispatches counted per total
inline constexpr int WIDE_MARK_KEY = 0x77;          // VK_F8
inline constexpr u32 WIDE_DESCRIPTOR = u32{1} << 14; // TIC/TSC entries (after the 14 BufTrace kinds)
#endif

class State {
public:
    using Bytes = std::array<u8, PAGE_SIZE>;

    struct Copy {
        u64 upto;                     // last command fence that reads this copy
        u64 seq;                      // order of creation (acquire bypasses skip older copies)
        std::unique_ptr<Bytes> bytes; // the page as it was when list upto was pushed
    };

    struct Page {
        bool armed;
        u64 last_read; // submit count at the last selected read
    };

    /// Commands of one channel with fences in [lo, hi] skip copies with seq < min_seq.
    struct Bypass {
        s32 channel;
        u64 lo;
        u64 hi;
        u64 min_seq;
    };

    /// Time spent (logged as UB_KEEP_COST): GPU thread reads (sampled 1 in 16), BeforePush, trapping
    /// CPU writes, the arm hook itself, waiting for a ticket, and the rasterizer invalidation of NCE
    /// faults on armed pages (core/memory.cpp).
    struct Cost {
        u64 reads = 0;
        u64 read_ns = 0;
        u64 push_ns = 0;
        u64 trap_ns = 0;
        u64 arm_calls = 0;
        u64 arm_ns = 0;
        u64 wait_ns = 0;
        u64 faults = 0;
        u64 fault_ns = 0;
    };

    struct Counters {
        u64 traps = 0;          // CPU writes that found the page armed
        u64 copies = 0;         // page copies made
        u64 no_copy = 0;        // traps with every submitted command already done
        u64 overlay_reads = 0;  // fast uniform buffer reads served (partly) from a copy
        u64 overlay_bytes = 0;
        u64 rearms = 0;
        u64 stale = 0;          // pages dropped after STALE_SUBMITS submits without a read
        u64 gpu_mirror = 0;     // GPU-thread writes applied to copies
        u64 acquire_gpu = 0;    // acquires satisfied by a GPU release (copies kept)
        u64 acquire_other = 0;  // other acquires
        u64 bypasses = 0;       // other acquires that found copies (a bypass was added)
        u64 bypass_reads = 0;   // reads that skipped a copy because of a bypass
        u64 cap_drops = 0;      // copies not made (EDEN_UB_KEEP_COPIES reached)
        u64 page_cap = 0;       // pages not armed (EDEN_UB_KEEP_PAGES reached)
        u64 unmapped = 0;       // pages dropped by DeviceMemoryManager::Unmap
        u64 max_live = 0;
    };

    State() = default;

    [[nodiscard]] bool Enabled() const {
        return enabled.load(std::memory_order_acquire);
    }

    /// Start with a session snapshot, after all callers from the old session have stopped.
    /// OFF also resets state. Hooks stay valid until Uninstall returns, including failure cleanup.
    void Install(const Hooks& new_hooks, bool requested_enabled = true) {
        std::scoped_lock session_lk{session_mutex};
        UninstallLocked();
        const bool session_enabled = ConfigureSession(requested_enabled);
        std::scoped_lock lk{mutex};
        {
            std::scoped_lock arm_lk{arm_mutex};
            hooks = new_hooks;
        }
        accepting = session_enabled;
        enabled.store(session_enabled, std::memory_order_release);
    }

    /// Stop after the GPU thread has joined and guest submissions have stopped, while device
    /// memory still exists. CPU fault/unmap calls already on their way may finish: admission is
    /// closed under their existing mutex and all issued protection tickets are drained.
    /// An owner prevents a cache from detaching a different cache's registration.
    void Uninstall(const void* owner = nullptr) {
        std::scoped_lock session_lk{session_mutex};
        if (owner != nullptr && hooks.ctx != owner) {
            return;
        }
        UninstallLocked();
    }

    bool Selected(u64 fs) const {
        return all_fs || std::find(fs_list.begin(), fs_list.end(), fs) != fs_list.end();
    }

    /// GPU thread, fast uniform buffer bytes just read from guest memory into dst.
    void FastRead(u64 daddr, u8* dst, u64 size, bool selected) {
        if (size == 0) {
            return;
        }
#if SNAPRYU_DIAGNOSTICS
        const bool timed = (++gpu_reads & 15) == 0;
        const auto start = timed ? Clock::now() : Clock::time_point{};
#endif
        if (selected || live.load(std::memory_order_acquire) != 0) {
            std::vector<ArmOp> ops;
            u64 ticket = 0;
            {
                std::scoped_lock lk{mutex};
                if (!accepting) {
                    return;
                }
                if (selected) {
                    NoteRead(daddr, size, ops);
                }
                if (live.load(std::memory_order_relaxed) != 0) {
                    Overlay(daddr, dst, size);
                }
                ticket = Queue(ops);
            }
            Apply(ticket, ops);
        }
#if SNAPRYU_DIAGNOSTICS
        if (timed) {
            gpu_read_ns += 16 * Ns(Clock::now() - start);
        }
#endif
    }

#if SNAPRYU_DIAGNOSTICS
    /// b75 UB_WIDE, GPU thread: bytes a buffer cache upload (under the buffer cache mutex) or a
    /// TIC/TSC descriptor read just read from guest memory into dst, for kind_bits (BufTrace::Kind
    /// bits; WIDE_DESCRIPTOR for descriptors) of the draw or dispatch who_type/who_a/who_b (G: VS,
    /// FS; C: compute shader). arm_first arms the pages on the first read (descriptor tables are
    /// CPU data by design). Returns true when keep wrote copy bytes into dst.
    bool CachedRead(u64 daddr, u8* dst, u64 size, u32 kind_bits, char who_type, u64 who_a,
                    u64 who_b, bool arm_first) {
        if (wide_mode == WideMode::Off || size == 0) {
            return false;
        }
        u32 kind = 0;
        while (kind < WIDE_KINDS && (kind_bits & (u32{1} << kind)) == 0) {
            ++kind;
        }
        if (kind >= WIDE_KINDS) {
            return false;
        }
        std::vector<ArmOp> ops;
        u64 ticket = 0;
        bool wrote = false;
        {
            std::scoped_lock lk{mutex};
            if (!accepting) {
                return false;
            }
            WideCount& total = wide_total[kind];
            ++total.uploads;
            total.bytes += size;
            // Arm pages uploaded before: data the CPU keeps rewriting. One-time data stays unarmed.
            ForPages(daddr, size, [&](u64 page, u64, u64) {
                if (arm_first || wide_seen.size() >= WIDE_SEEN_MAX ||
                    !wide_seen.insert(page).second) {
                    NoteRead(page, PAGE_SIZE, ops);
                }
            });
            bool any_copy = false;
            if (live.load(std::memory_order_relaxed) != 0) {
                ForPages(daddr, size, [&](u64 page, u64, u64) {
                    any_copy = any_copy || copies.contains(page);
                });
            }
            if (any_copy) {
                wide_tmp.assign(dst, dst + size);
                const u64 saved_reads = counters.overlay_reads;
                const u64 saved_bytes = counters.overlay_bytes;
                const u64 saved_bypass = counters.bypass_reads;
                Overlay(daddr, wide_tmp.data(), size);
                const bool used = counters.overlay_reads != saved_reads;
                if (counters.bypass_reads != saved_bypass) {
                    ++wide_bypass_reads;
                }
                // The UB_KEEP line keeps counting fast uniform reads only.
                counters.overlay_reads = saved_reads;
                counters.overlay_bytes = saved_bytes;
                counters.bypass_reads = saved_bypass;
                if (used) {
                    ++total.late;
                    u64 diff = 0;
                    for (u64 i = 0; i < size; ++i) {
                        diff += wide_tmp[i] != dst[i] ? 1 : 0;
                    }
                    if (diff != 0) {
                        ++total.late_diff;
                        total.diff_bytes += diff;
                        WideCount& window = wide_window[kind];
                        ++window.late_diff;
                        window.diff_bytes += diff;
                        NoteWho(wide_window_who, who_type, who_a, who_b, diff);
                        NoteWho(wide_total_who, who_type, who_a, who_b, diff);
                        if (wide_mode == WideMode::Keep) {
                            std::memcpy(dst, wide_tmp.data(), size);
                            ++wide_kept;
                            wrote = true;
                        }
                    }
                }
            }
            ticket = Queue(ops);
        }
        Apply(ticket, ops);
        return wrote;
    }
#endif

    /// Guest thread, before a CPU write of [daddr, daddr + size) lands in device memory.
    void OnCpuWrite(u64 daddr, u64 size) {
        if (page_count.load(std::memory_order_acquire) == 0) {
            return;
        }
#if SNAPRYU_DIAGNOSTICS
        const auto start = Clock::now();
#endif
        std::vector<ArmOp> ops;
        u64 ticket = 0;
        {
            std::scoped_lock lk{mutex};
            if (!accepting) {
                return;
            }
            ForPages(daddr, std::max<u64>(size, 1), [&](u64 page, u64, u64) {
                const auto it = pages.find(page);
                if (it == pages.end() || !it->second.armed) {
                    return;
                }
#if SNAPRYU_DIAGNOSTICS
                ++counters.traps;
#endif
                const u64 upto = pushed;
                if (done.load(std::memory_order_acquire) >= upto) {
#if SNAPRYU_DIAGNOSTICS
                    ++counters.no_copy;
#endif
                } else {
                    MakeCopy(page, upto);
                }
                AddOp(ops, page, -1);
                it->second.armed = false;
                if (submits - it->second.last_read > STALE_SUBMITS) {
#if SNAPRYU_DIAGNOSTICS
                    ++counters.stale;
#endif
                    pages.erase(it);
                    page_count.store(pages.size(), std::memory_order_release);
                } else {
                    disarmed.push_back(page);
                }
            });
            ticket = Queue(ops);
        }
        if (ops.empty()) {
            return;
        }
        Apply(ticket, ops);
#if SNAPRYU_DIAGNOSTICS
        trap_ns.fetch_add(Ns(Clock::now() - start), std::memory_order_relaxed);
#endif
    }

    /// Guest thread, under the GPU thread queue lock, before a command list with this fence is
    /// queued. The pages are protected again when this returns.
    void BeforePush(u64 fence) {
#if SNAPRYU_DIAGNOSTICS
        const auto start = Clock::now();
#endif
        std::vector<ArmOp> ops;
        u64 ticket = 0;
        {
            std::scoped_lock lk{mutex};
            if (!accepting) {
                return;
            }
            pushed = fence;
            ++submits;
            std::vector<u64> rearm;
            for (const u64 page : disarmed) {
                const auto it = pages.find(page);
                if (it == pages.end() || it->second.armed) {
                    continue;
                }
                if (submits - it->second.last_read > STALE_SUBMITS) {
#if SNAPRYU_DIAGNOSTICS
                    ++counters.stale;
#endif
                    pages.erase(it);
                    continue;
                }
                rearm.push_back(page);
                it->second.armed = true;
#if SNAPRYU_DIAGNOSTICS
                ++counters.rearms;
#endif
            }
            disarmed.clear();
            std::sort(rearm.begin(), rearm.end());
            for (const u64 page : rearm) {
                AddOp(ops, page, 1);
            }
            page_count.store(pages.size(), std::memory_order_release);
            ticket = Queue(ops);
        }
        Apply(ticket, ops);
#if SNAPRYU_DIAGNOSTICS
        push_ns.fetch_add(Ns(Clock::now() - start), std::memory_order_relaxed);
#endif
    }

    /// core/memory.cpp: rasterizer invalidation time of an NCE fault on a cached page.
    void AddFaultCost(u64 ns) {
#if SNAPRYU_DIAGNOSTICS
        faults.fetch_add(1, std::memory_order_relaxed);
        fault_ns.fetch_add(ns, std::memory_order_relaxed);
#else
        static_cast<void>(ns);
#endif
    }

    /// GPU thread, before a queued command runs. channel is the submit list's channel, or -1.
    void BeginCommand(u64 fence, s32 channel) {
        std::scoped_lock lk{mutex};
        if (!accepting) {
            return;
        }
        current = fence;
        current_channel = channel;
#if SNAPRYU_DIAGNOSTICS
        wide_current.store(fence, std::memory_order_relaxed);
#endif
    }

    /// GPU thread, after a queued command ran.
    void EndCommand(u64 fence) {
        if (!Enabled()) {
            return;
        }
        done.store(fence, std::memory_order_release);
        // Walk the copies only when one may be done: min_upto_hint is lowered by MakeCopy and reset
        // by the walk, both under the mutex. A copy made while this reads it is freed by a later
        // command; commands after this one never read it (its upto is below their fence).
        if ((live.load(std::memory_order_acquire) != 0 &&
             fence >= min_upto_hint.load(std::memory_order_acquire)) ||
            !bypasses.empty()) {
            std::scoped_lock lk{mutex};
            u64 lowest = ~u64{0};
            for (auto it = copies.begin(); it != copies.end();) {
                auto& list = it->second;
                for (auto copy = list.begin(); copy != list.end();) {
                    if (copy->upto <= fence) {
                        Release(std::move(copy->bytes));
                        copy = list.erase(copy);
                    } else {
                        lowest = std::min(lowest, copy->upto);
                        ++copy;
                    }
                }
                it = list.empty() ? copies.erase(it) : std::next(it);
            }
            min_upto_hint.store(lowest, std::memory_order_release);
            std::erase_if(bypasses, [fence](const Bypass& bypass) { return bypass.hi <= fence; });
        }
#if SNAPRYU_DIAGNOSTICS
        if ((++end_count & 255) == 0) {
            MaybeLog();
        }
        if (wide_mode != WideMode::Off && (end_count & 15) == 0) {
            WideTick();
        }
#endif
    }

    /// GPU thread, after a write of guest memory by a command landed (constant buffer data,
    /// inline data).
    void OnGpuWrite(u64 daddr, const void* src, u64 size) {
        if (live.load(std::memory_order_acquire) == 0 || size == 0) {
            return;
        }
        std::scoped_lock lk{mutex};
        if (!accepting) {
            return;
        }
        ForPages(daddr, size, [&](u64 page, u64 begin, u64 end) {
            const auto it = copies.find(page);
            if (it == copies.end()) {
                return;
            }
            for (Copy& copy : it->second) {
                std::memcpy(copy.bytes->data() + (begin - page),
                            static_cast<const u8*>(src) + (begin - daddr), end - begin);
#if SNAPRYU_DIAGNOSTICS
                ++counters.gpu_mirror;
#endif
            }
        });
    }

    /// GPU thread, a semaphore payload release (rasterizer Query with QueryType::Payload).
    void OnGpuRelease(u64 gpu_addr, u32 payload) {
        if (!Enabled()) {
            return;
        }
        if (gpu_releases.size() >= MAX_GPU_RELEASES && !gpu_releases.contains(gpu_addr)) {
            gpu_releases.clear(); // forgotten releases count as not GPU: copies are skipped
        }
        gpu_releases[gpu_addr] = payload;
    }

    /// GPU thread, a semaphore acquire passed (word: the value it read last).
    void OnGpuAcquire(u64 gpu_addr, u32 value, AcquireKind kind, u32 word) {
        const auto it = gpu_releases.find(gpu_addr);
        bool by_gpu = false;
        if (it != gpu_releases.end()) {
            switch (kind) {
            case AcquireKind::Equal:
                by_gpu = it->second == value;
                break;
            case AcquireKind::Gequal:
                by_gpu = it->second >= value;
                break;
            case AcquireKind::Other:
                by_gpu = it->second == word;
                break;
            }
        }
        std::scoped_lock lk{mutex};
        if (!accepting) {
            return;
        }
        if (by_gpu) {
#if SNAPRYU_DIAGNOSTICS
            ++counters.acquire_gpu;
#endif
            return;
        }
#if SNAPRYU_DIAGNOSTICS
        ++counters.acquire_other;
#endif
        if (live.load(std::memory_order_acquire) == 0) {
            return; // no copy exists, so none is older than this point
        }
#if SNAPRYU_DIAGNOSTICS
        ++counters.bypasses;
#endif
        for (Bypass& bypass : bypasses) {
            if (bypass.channel == current_channel && bypass.hi == pushed) {
                // Commands before the current one are done, so widening lo changes nothing.
                bypass.lo = std::min(bypass.lo, current);
                bypass.min_seq = next_seq;
                return;
            }
        }
        bypasses.push_back(Bypass{current_channel, current, pushed, next_seq});
    }

    Counters Snapshot() {
#if SNAPRYU_DIAGNOSTICS
        std::scoped_lock lk{mutex};
        return counters;
#else
        return {};
#endif
    }

    /// DeviceMemoryManager::Unmap, before [daddr, daddr + size) is unmapped (still mapped here).
    void UnmapBegin(u64 daddr, u64 size) {
        std::vector<ArmOp> ops;
        u64 ticket = 0;
        {
            std::scoped_lock lk{mutex};
            if (!accepting) {
                return;
            }
            UnmapLocked(daddr, size, ops);
            ticket = Queue(ops);
        }
        Apply(ticket, ops);
    }

    /// DeviceMemoryManager::Unmap, after the range is unmapped.
    void UnmapEnd(u64 daddr, u64 size) {
        std::scoped_lock lk{mutex};
        const auto it = std::find(unmapping.begin(), unmapping.end(),
                                  std::pair<u64, u64>{daddr, daddr + size});
        if (it != unmapping.end()) {
            unmapping.erase(it);
        }
    }

    size_t LiveCopies() const {
        return live.load(std::memory_order_acquire);
    }

    bool IsArmed(u64 page) {
        std::scoped_lock lk{mutex};
        const auto it = pages.find(page);
        return it != pages.end() && it->second.armed;
    }

    Cost CostSnapshot() {
#if SNAPRYU_DIAGNOSTICS
        Cost c;
        {
            std::scoped_lock lk{arm_mutex};
            c.arm_calls = arm_calls;
            c.arm_ns = arm_ns;
            c.wait_ns = wait_ns;
        }
        c.reads = gpu_reads;
        c.read_ns = gpu_read_ns;
        c.push_ns = push_ns.load(std::memory_order_relaxed);
        c.trap_ns = trap_ns.load(std::memory_order_relaxed);
        c.faults = faults.load(std::memory_order_relaxed);
        c.fault_ns = fault_ns.load(std::memory_order_relaxed);
        return c;
#else
        return {};
#endif
    }

private:
    /// A protection change: arm hook over [addr, addr + size).
    struct ArmOp {
        u64 addr;
        u64 size;
        s32 delta;
    };

    bool ConfigureSession(bool requested_enabled) {
        all_fs = true;
        fs_list.clear();
        max_pages = 8192;
        max_copies = 8192;
#if SNAPRYU_DIAGNOSTICS
        if (const char* const on = std::getenv("EDEN_UB_KEEP")) {
            requested_enabled = !(on[0] == '0' && on[1] == '\0');
        }
        const char* const fs = std::getenv("EDEN_UB_KEEP_FS");
        const std::string list = fs != nullptr && fs[0] != '\0' ? fs : "all";
        all_fs = list == "all";
        if (!all_fs) {
            size_t start = 0;
            while (start <= list.size()) {
                const size_t end = std::min(list.find(',', start), list.size());
                const std::string item = list.substr(start, end - start);
                if (!item.empty()) {
                    fs_list.push_back(std::strtoull(item.c_str(), nullptr, 16));
                }
                start = end + 1;
            }
        }
        max_pages = EnvSize("EDEN_UB_KEEP_PAGES", 8192);
        max_copies = EnvSize("EDEN_UB_KEEP_COPIES", 8192);
        LOG_WARNING(Render_Vulkan, "UB_KEEP enabled={} fs={} pages_max={} copies_max={}",
                    requested_enabled, all_fs ? std::string{"all"} : list, max_pages, max_copies);
        // b75 UB_WIDE: off unless asked for, and only with UB_KEEP on.
        const char* const wide = std::getenv("EDEN_UB_KEEP_WIDE");
        const std::string_view wide_text = wide != nullptr ? wide : "";
        wide_mode = !requested_enabled         ? WideMode::Off
                    : wide_text == "detect" ? WideMode::Detect
                    : wide_text == "keep"   ? WideMode::Keep
                                            : WideMode::Off;
        wide_start = Clock::now();
        wide_window_start = wide_start;
        wide_total_log = wide_start;
        LOG_WARNING(Render_Vulkan, "UB_WIDE ready b75 mode={} asked={} ub_keep={} key=F8",
                    WideModeName(), wide_text.empty() ? std::string_view{"-"} : wide_text,
                    requested_enabled);
#endif
        return requested_enabled;
    }

    // session_mutex held. GPU-only state has no readers now; the existing mutex closes admission
    // for late CPU faults, submits and unmaps without a new per-call reference count or lock.
    void UninstallLocked() {
        std::vector<ArmOp> ops;
        u64 ticket = 0;
        u64 drained_ticket = 0;
        {
            std::scoped_lock lk{mutex};
            accepting = false;
            enabled.store(false, std::memory_order_release);
            std::vector<u64> armed;
            for (const auto& [page, state] : pages) {
                if (state.armed) {
                    armed.push_back(page);
                }
            }
            std::sort(armed.begin(), armed.end());
            for (const u64 page : armed) {
                AddOp(ops, page, -1);
            }
            ticket = Queue(ops);
            drained_ticket = next_ticket;
        }
        // Queued +1/-1 operations use the old hooks; our -1 follows them in the same order.
        Apply(ticket, ops);
        {
            std::unique_lock arm_lk{arm_mutex};
            arm_cv.wait(arm_lk, [&] { return arm_turn == drained_ticket; });
            hooks = {};
            arm_turn = 0;
        }
        std::scoped_lock lk{mutex};
        pages.clear();
        disarmed.clear();
        copies.clear();
        pool.clear();
        bypasses.clear();
        gpu_releases.clear();
        unmapping.clear();
        page_count.store(0, std::memory_order_release);
        live.store(0, std::memory_order_release);
        done.store(0, std::memory_order_release);
        min_upto_hint.store(~u64{0}, std::memory_order_release);
        pushed = 0;
        submits = 0;
        current = 0;
        current_channel = -1;
        next_seq = 0;
        next_ticket = 0;
#if SNAPRYU_DIAGNOSTICS
        wide_mode = WideMode::Off;
        wide_current.store(0, std::memory_order_relaxed);
        wide_reset.fetch_add(1, std::memory_order_relaxed);
        wide_seen.clear();
        wide_tmp.clear();
        wide_total = {};
        wide_window = {};
        wide_total_who.clear();
        wide_window_who.clear();
        wide_bypass_reads = 0;
        wide_kept = 0;
        wide_windows_logged = 0;
        wide_marks = 0;
        wide_key_down = false;
#endif
    }

    using Clock = std::chrono::steady_clock;

    static u64 Ns(Clock::duration d) {
        return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
    }

    static void AddOp(std::vector<ArmOp>& ops, u64 page, s32 delta) {
        if (!ops.empty() && ops.back().delta == delta && ops.back().addr + ops.back().size == page) {
            ops.back().size += PAGE_SIZE;
            return;
        }
        ops.push_back(ArmOp{page, PAGE_SIZE, delta});
    }

    /// Under the mutex: the ticket of these changes (none when empty).
    u64 Queue(const std::vector<ArmOp>& ops) {
        return ops.empty() ? 0 : next_ticket++;
    }

    /// Without the mutex: applies the changes when every earlier ticket is applied.
    void Apply(u64 ticket, const std::vector<ArmOp>& ops) {
        if (ops.empty()) {
            return;
        }
#if SNAPRYU_DIAGNOSTICS
        const auto start = Clock::now();
#endif
        std::unique_lock lk{arm_mutex};
        arm_cv.wait(lk, [&] { return arm_turn == ticket; });
#if SNAPRYU_DIAGNOSTICS
        const auto ready = Clock::now();
#endif
        if (hooks.arm != nullptr) {
            for (const ArmOp& op : ops) {
                hooks.arm(hooks.ctx, op.addr, op.size, op.delta);
            }
        }
        ++arm_turn;
#if SNAPRYU_DIAGNOSTICS
        arm_calls += ops.size();
        arm_ns += Ns(Clock::now() - ready);
        wait_ns += Ns(ready - start);
#endif
        lk.unlock();
        arm_cv.notify_all();
    }

    void UnmapLocked(u64 daddr, u64 size, std::vector<ArmOp>& ops) {
        unmapping.emplace_back(daddr, daddr + size);
        const auto drop = [&](u64 page) {
            bool dropped = false;
            const auto it = pages.find(page);
            if (it != pages.end()) {
                if (it->second.armed) {
                    AddOp(ops, page, -1);
                }
                pages.erase(it);
                dropped = true;
            }
            const auto copy_it = copies.find(page);
            if (copy_it != copies.end()) {
                for (Copy& copy : copy_it->second) {
                    Release(std::move(copy.bytes));
                }
                copies.erase(copy_it);
                dropped = true;
            }
            if (dropped) {
#if SNAPRYU_DIAGNOSTICS
                ++counters.unmapped;
#endif
            }
        };
        const u64 first = daddr & ~(PAGE_SIZE - 1);
        const u64 end = daddr + size;
        if ((end - first) / PAGE_SIZE <= pages.size() + copies.size()) {
            ForPages(daddr, size, [&](u64 page, u64, u64) { drop(page); });
        } else {
            // Large unmaps (whole heaps): walk the tracked pages instead of the range.
            std::vector<u64> hit;
            for (const auto& [page, info] : pages) {
                if (page >= first && page < end) {
                    hit.push_back(page);
                }
            }
            for (const auto& [page, list] : copies) {
                if (page >= first && page < end) {
                    hit.push_back(page);
                }
            }
            std::sort(hit.begin(), hit.end());
            hit.erase(std::unique(hit.begin(), hit.end()), hit.end());
            for (const u64 page : hit) {
                drop(page);
            }
        }
        page_count.store(pages.size(), std::memory_order_release);
    }

    static size_t EnvSize(const char* name, size_t fallback) {
        const char* const value = std::getenv(name);
        if (value == nullptr || value[0] == '\0') {
            return fallback;
        }
        const size_t parsed = static_cast<size_t>(std::strtoull(value, nullptr, 10));
        return parsed != 0 ? parsed : fallback;
    }

    /// Under the mutex: bytes of copies for the current command over dst.
    void Overlay(u64 daddr, u8* dst, u64 size) {
        u64 min_seq = 0;
        for (const Bypass& bypass : bypasses) {
            if (bypass.channel == current_channel && bypass.lo <= current && current <= bypass.hi) {
                min_seq = std::max(min_seq, bypass.min_seq);
            }
        }
        bool used = false;
        bool skipped = false;
        ForPages(daddr, size, [&](u64 page, u64 begin, u64 end) {
            const auto it = copies.find(page);
            if (it == copies.end()) {
                return;
            }
            const Copy* best = nullptr;
            for (const Copy& copy : it->second) {
                if (copy.upto < current) {
                    continue;
                }
                if (copy.seq < min_seq) {
                    skipped = true;
                    continue;
                }
                if (best == nullptr || copy.upto < best->upto ||
                    (copy.upto == best->upto && copy.seq < best->seq)) {
                    best = &copy;
                }
            }
            if (best == nullptr) {
                return;
            }
            std::memcpy(dst + (begin - daddr), best->bytes->data() + (begin - page), end - begin);
#if SNAPRYU_DIAGNOSTICS
            counters.overlay_bytes += end - begin;
#endif
            used = true;
        });
        if (used) {
#if SNAPRYU_DIAGNOSTICS
            ++counters.overlay_reads;
#endif
        }
        if (skipped) {
#if SNAPRYU_DIAGNOSTICS
            ++counters.bypass_reads;
#endif
        }
    }

    template <typename Func>
    static void ForPages(u64 daddr, u64 size, Func&& func) {
        const u64 end = daddr + size;
        for (u64 page = daddr & ~(PAGE_SIZE - 1); page < end; page += PAGE_SIZE) {
            func(page, std::max(page, daddr), std::min(page + PAGE_SIZE, end));
        }
    }

    /// Under the mutex.
    void NoteRead(u64 daddr, u64 size, std::vector<ArmOp>& ops) {
        ForPages(daddr, size, [&](u64 page, u64, u64) {
            const auto it = pages.find(page);
            if (it != pages.end()) {
                it->second.last_read = submits;
                return;
            }
            if (Unmapping(page)) {
                return;
            }
            if (pages.size() >= max_pages) {
#if SNAPRYU_DIAGNOSTICS
                ++counters.page_cap;
#endif
                return;
            }
            pages.emplace(page, Page{true, submits});
            AddOp(ops, page, 1);
        });
        page_count.store(pages.size(), std::memory_order_release);
    }

    bool Unmapping(u64 page) const {
        for (const auto& [begin, end] : unmapping) {
            if (page + PAGE_SIZE > begin && page < end) {
                return true;
            }
        }
        return false;
    }

    void MakeCopy(u64 page, u64 upto) {
        auto& list = copies[page];
        for (const Copy& copy : list) {
            if (copy.upto == upto) {
                return;
            }
        }
        u8* const source = hooks.pointer != nullptr ? hooks.pointer(hooks.ctx, page) : nullptr;
        if (source == nullptr || live.load(std::memory_order_relaxed) >= max_copies) {
#if SNAPRYU_DIAGNOSTICS
            ++counters.cap_drops;
#endif
            if (list.empty()) {
                copies.erase(page);
            }
            return;
        }
        std::unique_ptr<Bytes> bytes = Acquire();
        std::memcpy(bytes->data(), source, PAGE_SIZE);
        list.push_back(Copy{upto, next_seq++, std::move(bytes)});
        if (upto < min_upto_hint.load(std::memory_order_relaxed)) {
            min_upto_hint.store(upto, std::memory_order_release);
        }
#if SNAPRYU_DIAGNOSTICS
        ++counters.copies;
#endif
#if SNAPRYU_DIAGNOSTICS
        const size_t now = live.fetch_add(1, std::memory_order_acq_rel) + 1;
        counters.max_live = std::max<u64>(counters.max_live, now);
#else
        live.fetch_add(1, std::memory_order_acq_rel);
#endif
    }

    std::unique_ptr<Bytes> Acquire() {
        if (pool.empty()) {
            return std::make_unique<Bytes>();
        }
        std::unique_ptr<Bytes> bytes = std::move(pool.back());
        pool.pop_back();
        return bytes;
    }

    void Release(std::unique_ptr<Bytes> bytes) {
        live.fetch_sub(1, std::memory_order_acq_rel);
        if (pool.size() < 64) {
            pool.push_back(std::move(bytes));
        }
    }

    void MaybeLog() {
#if SNAPRYU_DIAGNOSTICS
        const auto now = std::chrono::steady_clock::now();
        if (now - last_log < std::chrono::seconds{5}) {
            return;
        }
        Counters c;
        size_t page_total;
        {
            std::scoped_lock lk{mutex};
            c = counters;
            page_total = pages.size();
        }
        if (c.traps == logged_traps && c.overlay_reads == logged_overlay &&
            c.acquire_other == logged_other) {
            return;
        }
        last_log = now;
        logged_traps = c.traps;
        logged_overlay = c.overlay_reads;
        logged_other = c.acquire_other;
        LOG_WARNING(Render_Vulkan,
                    "UB_KEEP pages={} traps={} copies={} no_copy={} overlay_reads={} "
                    "overlay_bytes={} rearms={} stale={} gpu_mirror={} acquire_gpu={} "
                    "acquire_other={} bypasses={} bypass_reads={} cap_drops={} page_cap={} "
                    "unmapped={} max_live={}",
                    page_total, c.traps, c.copies, c.no_copy, c.overlay_reads, c.overlay_bytes,
                    c.rearms, c.stale, c.gpu_mirror, c.acquire_gpu, c.acquire_other, c.bypasses,
                    c.bypass_reads, c.cap_drops, c.page_cap, c.unmapped, c.max_live);
        const Cost k = CostSnapshot();
        LOG_WARNING(Render_Vulkan,
                    "UB_KEEP_COST reads={} read_ms={} push_ms={} trap_ms={} arm_calls={} arm_ms={} "
                    "wait_ms={} faults={} fault_ms={}",
                    k.reads, k.read_ns / 1000000, k.push_ns / 1000000, k.trap_ns / 1000000,
                    k.arm_calls, k.arm_ns / 1000000, k.wait_ns / 1000000, k.faults,
                    k.fault_ns / 1000000);
#else

#endif
    }

#if SNAPRYU_DIAGNOSTICS
    struct WideCount {
        u64 uploads = 0;
        u64 bytes = 0;
        u64 late = 0;       // uploads a copy for the running command covered
        u64 late_diff = 0;  // of those, uploads whose bytes the copy changes
        u64 diff_bytes = 0; // bytes the copies change
    };

    struct WideWho {
        char type;
        u64 a;
        u64 b;
        bool operator==(const WideWho&) const = default;
    };

    struct WideWhoHash {
        size_t operator()(const WideWho& who) const noexcept {
            const u64 h = (who.a * 0x9E3779B97F4A7C15ULL) ^
                          ((who.b + static_cast<u8>(who.type)) * 0xC2B2AE3D27D4EB4FULL);
            return static_cast<size_t>(h ^ (h >> 29));
        }
    };

    using WideWhoMap = std::unordered_map<WideWho, std::array<u64, 2>, WideWhoHash>;

    const char* WideModeName() const {
        return wide_mode == WideMode::Detect ? "detect"
               : wide_mode == WideMode::Keep ? "keep"
                                             : "off";
    }

    /// Under the mutex: one late upload (diff bytes) by this draw or dispatch.
    static void NoteWho(WideWhoMap& map, char type, u64 a, u64 b, u64 diff) {
        const WideWho who{type, a, b};
        auto it = map.find(who);
        if (it == map.end()) {
            if (map.size() >= WIDE_WHO_MAX) {
                it = map.try_emplace(WideWho{'?', 0, 0}).first;
            } else {
                it = map.try_emplace(who).first;
            }
        }
        ++it->second[0];
        it->second[1] += diff;
    }

    /// "kind:late_diff/diff_bytes" for the kinds with late bytes (kind = BufTrace::Kind bit index).
    static std::string WideKinds(const std::array<WideCount, WIDE_KINDS>& counts) {
        std::string text;
        for (size_t kind = 0; kind < WIDE_KINDS; ++kind) {
            if (counts[kind].late_diff == 0) {
                continue;
            }
            text += fmt::format("{}{}:{}/{}", text.empty() ? "" : ",", kind, counts[kind].late_diff,
                                counts[kind].diff_bytes);
        }
        return text.empty() ? std::string{"-"} : text;
    }

    /// The draws/dispatches with the most late bytes: "G:vs:fs:uploads/bytes".
    static std::string WideTop(const WideWhoMap& map, size_t top) {
        std::vector<std::pair<WideWho, std::array<u64, 2>>> rows(map.begin(), map.end());
        std::sort(rows.begin(), rows.end(),
                  [](const auto& x, const auto& y) { return x.second[1] > y.second[1]; });
        std::string text;
        for (size_t i = 0; i < rows.size() && i < top; ++i) {
            const auto& [who, n] = rows[i];
            text += fmt::format("{}{}:{:016x}:{:016x}:{}/{}", text.empty() ? "" : ",", who.type,
                                who.a, who.b, n[0], n[1]);
        }
        return text.empty() ? std::string{"-"} : text;
    }

    /// GPU thread (EndCommand, every 16 commands): F8 marks, 250 ms windows, 5 s totals.
    void WideTick() {
#ifdef _WIN32
        const bool down = (GetAsyncKeyState(WIDE_MARK_KEY) & 0x8000) != 0;
#else
        const bool down = false;
#endif
        const auto now = Clock::now();
        std::vector<std::string> lines;
        {
            std::scoped_lock lk{mutex};
            if (!accepting) {
                return;
            }
            const u64 t_ms = Ns(now - wide_start) / 1000000;
            if (down && !wide_key_down) {
                ++wide_marks;
                lines.push_back(fmt::format("UB_WIDE_MARK n={} t_ms={} pushed={} current={}",
                                            wide_marks, t_ms, pushed, current));
            }
            wide_key_down = down;
            if (now - wide_window_start >= std::chrono::milliseconds{250}) {
                u64 late = 0;
                for (const WideCount& count : wide_window) {
                    late += count.late_diff;
                }
                if (late != 0) {
                    ++wide_windows_logged;
                    lines.push_back(fmt::format(
                        "UB_WIDE_LATE t_ms={} window_ms={} late_uploads={} kinds={} who={}", t_ms,
                        Ns(now - wide_window_start) / 1000000, late, WideKinds(wide_window),
                        WideTop(wide_window_who, 4)));
                }
                wide_window = {};
                wide_window_who.clear();
                wide_window_start = now;
            }
            if (now - wide_total_log >= std::chrono::seconds{5}) {
                wide_total_log = now;
                WideCount sum;
                for (const WideCount& count : wide_total) {
                    sum.uploads += count.uploads;
                    sum.bytes += count.bytes;
                    sum.late += count.late;
                    sum.late_diff += count.late_diff;
                    sum.diff_bytes += count.diff_bytes;
                }
                lines.push_back(fmt::format(
                    "UB_WIDE mode={} t_ms={} uploads={} bytes={} late={} late_diff={} "
                    "diff_bytes={} kept={} bypass_reads={} pages={} seen={} page_cap={} "
                    "cap_drops={} live={} windows={} marks={} kinds={} top={}",
                    WideModeName(), t_ms, sum.uploads, sum.bytes, sum.late, sum.late_diff,
                    sum.diff_bytes, wide_kept, wide_bypass_reads, pages.size(), wide_seen.size(),
                    counters.page_cap, counters.cap_drops, live.load(std::memory_order_relaxed),
                    wide_windows_logged, wide_marks, WideKinds(wide_total),
                    WideTop(wide_total_who, 8)));
            }
        }
        for (const std::string& line : lines) {
            LOG_WARNING(Render_Vulkan, "{}", line);
        }
    }

public:
    std::atomic<u64> wide_current{0}; // GPU thread: the running command (BeginCommand)
    std::atomic<u64> wide_reset{0};   // bumped per session: drops the GPU thread's noted pages

    WideMode Mode() const {
        return wide_mode;
    }

private:
    WideMode wide_mode = WideMode::Off; // set per session before enabled; read under the mutex
                                        // or after Enabled()
    std::unordered_set<u64> wide_seen;  // under mutex
    std::vector<u8> wide_tmp;           // under mutex
    std::array<WideCount, WIDE_KINDS> wide_total{};
    std::array<WideCount, WIDE_KINDS> wide_window{};
    WideWhoMap wide_total_who;
    WideWhoMap wide_window_who;
    u64 wide_bypass_reads = 0;
    u64 wide_kept = 0;
    u64 wide_windows_logged = 0;
    u64 wide_marks = 0;
    bool wide_key_down = false;
    Clock::time_point wide_start{};
    Clock::time_point wide_window_start{};
    Clock::time_point wide_total_log{};
#endif

    bool all_fs = true;
    std::vector<u64> fs_list;
    size_t max_pages = 8192;
    size_t max_copies = 8192;

    std::mutex session_mutex; // Install/Uninstall only; never acquired by an emulation hook
    std::atomic<bool> enabled{false}; // immutable while a game runs
    bool accepting = false; // protected by mutex; closes ticket admission before draining
    std::mutex mutex;
    Hooks hooks; // written under mutex and arm_mutex, read under either
    std::unordered_map<u64, Page> pages;
    std::vector<u64> disarmed;
    std::unordered_map<u64, std::vector<Copy>> copies;
    std::vector<Bypass> bypasses;                  // GPU thread (changed under mutex)
    std::vector<std::pair<u64, u64>> unmapping;    // [begin, end) ranges inside Unmap
    std::unordered_map<u64, u32> gpu_releases;     // GPU thread only
    std::vector<std::unique_ptr<Bytes>> pool;
    Counters counters;
    std::atomic<size_t> page_count{0};
    std::atomic<size_t> live{0};
    std::atomic<u64> done{0};
    u64 pushed = 0;          // fence of the last command list queued (under mutex)
    u64 submits = 0;         // command lists queued (under mutex)
    u64 next_seq = 0;        // seq of the next copy (under mutex)
    u64 current = 0;         // fence of the command the GPU thread runs (under mutex)
    s32 current_channel = -1; // its channel (under mutex)
    u64 end_count = 0;
    std::atomic<u64> min_upto_hint{~u64{0}}; // lowest upto of a live copy, or lower
    u64 next_ticket = 0;                     // under mutex

    std::mutex arm_mutex;
    std::condition_variable arm_cv;
    u64 arm_turn = 0;  // under arm_mutex: the next ticket to apply
    u64 arm_calls = 0; // under arm_mutex
    u64 arm_ns = 0;
    u64 wait_ns = 0;
    u64 gpu_reads = 0; // GPU thread only
    u64 gpu_read_ns = 0;
    std::atomic<u64> push_ns{0};
    std::atomic<u64> trap_ns{0};
    std::atomic<u64> faults{0};
    std::atomic<u64> fault_ns{0};
    std::chrono::steady_clock::time_point last_log{};
    u64 logged_traps = 0;
    u64 logged_overlay = 0;
    u64 logged_other = 0;
};

inline State& Get() {
    static State state;
    return state;
}

inline bool Enabled() {
    return Get().Enabled();
}

/// Set for the buffer bindings of one draw (PrepareDraw); FastRead arms pages only inside it.
inline thread_local bool draw_selected = false;

#if SNAPRYU_DIAGNOSTICS
/// b75 UB_WIDE: the fragment shader of the draw being prepared (DrawScope), 0 outside draws.
inline thread_local u64 wide_draw_fs = 0;
#endif

class DrawScope {
public:
    explicit DrawScope(u64 fs) {
#if SNAPRYU_DIAGNOSTICS
        wide_draw_fs = fs;
#endif
        if (Enabled()) {
            draw_selected = Get().Selected(fs);
        }
    }
    ~DrawScope() {
        draw_selected = false;
#if SNAPRYU_DIAGNOSTICS
        wide_draw_fs = 0;
#endif
    }
    DrawScope(const DrawScope&) = delete;
    DrawScope& operator=(const DrawScope&) = delete;
};

inline void Install(const Hooks& hooks, bool enabled = true) {
    Get().Install(hooks, enabled);
}

inline void Uninstall(const void* owner = nullptr) {
    Get().Uninstall(owner);
}

inline void FastRead(u64 daddr, u8* dst, u64 size) {
    if (Enabled()) {
        Get().FastRead(daddr, dst, size, draw_selected);
    }
}

#if SNAPRYU_DIAGNOSTICS
/// b75 UB_WIDE: BufferCache::MappedUploadMemory, after a binding upload read guest memory into dst.
inline bool WideRead(u64 daddr, u8* dst, u64 size, u32 kind_bits, char who_type, u64 who_a,
                     u64 who_b) {
    return Enabled() &&
           Get().CachedRead(daddr, dst, size, kind_bits, who_type, who_a, who_b, false);
}

/// b75 UB_WIDE: DescriptorTable::Read (GPU thread), after a TIC/TSC entry was read into dst.
/// Every draw reads its descriptors, so a page noted in the last 64 commands with no copy alive
/// skips the mutex.
inline void WideDescriptorRead(u64 daddr, u8* dst, u64 size) {
    if (!Enabled()) {
        return;
    }
    State& state = Get();
    if (state.Mode() == WideMode::Off) {
        return;
    }
    thread_local std::unordered_map<u64, u64> noted;
    thread_local u64 noted_reset = ~u64{0};
    const u64 reset = state.wide_reset.load(std::memory_order_relaxed);
    if (noted_reset != reset) {
        noted.clear();
        noted_reset = reset;
    }
    const u64 page = daddr & ~(PAGE_SIZE - 1);
    const u64 now = state.wide_current.load(std::memory_order_relaxed);
    const auto it = noted.find(page);
    if (it != noted.end() && now - it->second < 64 && state.LiveCopies() == 0) {
        return;
    }
    noted[page] = now;
    state.CachedRead(daddr, dst, size, WIDE_DESCRIPTOR, wide_draw_fs != 0 ? 'G' : '?', 0,
                     wide_draw_fs, true);
}
#endif

inline void OnCpuWrite(u64 daddr, u64 size) {
    if (Enabled()) {
        Get().OnCpuWrite(daddr, size);
    }
}

inline void BeforePush(u64 fence) {
    if (Enabled()) {
        Get().BeforePush(fence);
    }
}

inline void BeginCommand(u64 fence, s32 channel) {
    if (Enabled()) {
        Get().BeginCommand(fence, channel);
    }
}

inline void EndCommand(u64 fence) {
    if (Enabled()) {
        Get().EndCommand(fence);
    }
}

inline void OnGpuWrite(u64 daddr, const void* src, u64 size) {
    if (Enabled()) {
        Get().OnGpuWrite(daddr, src, size);
    }
}

inline void OnGpuRelease(u64 gpu_addr, u32 payload) {
    if (Enabled()) {
        Get().OnGpuRelease(gpu_addr, payload);
    }
}

inline void OnGpuAcquire(u64 gpu_addr, u32 value, AcquireKind kind, u32 word) {
    if (Enabled()) {
        Get().OnGpuAcquire(gpu_addr, value, kind, word);
    }
}

inline void AddFaultCost(u64 ns) {
    if (Enabled()) {
        Get().AddFaultCost(ns);
    }
}

/// DeviceMemoryManager::Unmap: disarms the range before it is unmapped and keeps it unarmed until
/// the unmap is done.
class UnmapScope {
public:
    UnmapScope(u64 daddr, u64 size) : daddr_{daddr}, size_{size} {
        if (Enabled()) {
            Get().UnmapBegin(daddr_, size_);
        }
    }
    ~UnmapScope() {
        if (Enabled()) {
            Get().UnmapEnd(daddr_, size_);
        }
    }
    UnmapScope(const UnmapScope&) = delete;
    UnmapScope& operator=(const UnmapScope&) = delete;

private:
    u64 daddr_;
    u64 size_;
};

} // namespace Common::UbKeep
