// SPDX-FileCopyrightText: Copyright 2026 SnapRYU
// SPDX-License-Identifier: GPL-3.0-or-later

// b76 NCE_GPU (diagnostic builds only).
//
// An NCE store into a cached page faults once and invalidates the whole 4 KiB page
// (Memory::InvalidateNCE). BufferCache::WriteMemory then drops the GPU state of every byte of that
// page: bytes a shader wrote (gpu_modified_ranges) and their pending downloads. Their next upload
// copies guest memory, which never received the shader's bytes, over them on the host buffer. With
// the JIT only the bytes the store wrote lose their GPU state.
//
// EDEN_NCE_BUF_GPU:
//   detect (default) counts the GPU bytes an NCE page invalidation drops outside the faulting
//          store's bytes, and the uploads that later copy guest memory over them ("clobber").
//   keep   drops the GPU state of the faulting store's bytes only (fault address, 64 bytes, not past
//          the page); the rest of the page is CPU-modified as before, and uploads skip GPU bytes.
//   off    nothing.
// Logged: "NCE_GPU ready b76", NCE_GPU_WINDOW per 250 ms with drops or clobbers, NCE_GPU totals
// (5 s), NCE_GPU_DROP for the first drops (store pc, instruction, GPU bytes).
#pragma once

#if SNAPRYU_DIAGNOSTICS
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <fmt/format.h>
#include "common/common_types.h"
#include "common/logging.h"
#include "common/nce_write_note.h"
#include "common/range_sets.inc"

namespace VideoCommon::NceGpu {

enum class Mode : u8 { Off, Detect, Keep };

inline constexpr u64 PAGE = 0x1000;
inline constexpr u64 STORE_BYTES = 64;
inline constexpr size_t KINDS = 16;
inline constexpr size_t MAX_DETAILS = 200;
inline constexpr u64 GENERATION_FRAMES = 4; // dropped ranges are kept 4 to 8 frames
inline constexpr auto WINDOW = std::chrono::milliseconds{250};
inline constexpr auto TOTAL_PERIOD = std::chrono::seconds{5};

struct Range {
    DAddr addr = 0;
    u64 size = 0;
};

struct Counts {
    u64 events = 0;        // NCE page invalidations over GPU bytes
    u64 gpu_bytes = 0;     // GPU bytes in those pages
    u64 dropped_bytes = 0; // of them outside the store's bytes
    u64 pending = 0;       // events that cancelled a pending download
    u64 clobbers = 0;      // uploads that copied guest memory over dropped bytes
    u64 clobber_bytes = 0;
};

class State {
public:
    Mode GetMode() {
        if (!initialized) [[unlikely]] {
            Init();
        }
        return mode;
    }

    /// CPU thread, BufferCache::WriteMemory under the buffer cache mutex, for a write over GPU bytes.
    /// gpu lists the GPU-modified ranges [start, end) inside [addr, addr + size). Returns the range
    /// whose GPU state is to be dropped in keep mode, or nothing (size 0) to drop it all as before.
    template <typename Pairs>
    Range OnWrite(DAddr addr, u64 size, const Pairs& gpu, bool pending) {
        const Common::NceWriteNote::Note& note = Common::NceWriteNote::current;
        u64 gpu_bytes = 0;
        for (const auto& [start, end] : gpu) {
            gpu_bytes += end - start;
        }
        std::scoped_lock lk{mutex};
        if (!note.valid || size != PAGE || (addr & (PAGE - 1)) != 0) {
            ++other_events;
            other_bytes += gpu_bytes;
            return {};
        }
        const DAddr store_start = addr + (note.fault & (PAGE - 1));
        const DAddr store_end = (std::min)(store_start + STORE_BYTES, addr + PAGE);
        u64 dropped_bytes = 0;
        for (const auto& [start, end] : gpu) {
            const auto add = [&](DAddr a, DAddr b) {
                if (a < b) {
                    dropped_bytes += b - a;
                    if (mode == Mode::Detect) {
                        sets[cur].Add(a, b - a);
                        any_dropped.store(true, std::memory_order_relaxed);
                    }
                }
            };
            add(start, (std::min)(end, store_start));
            add((std::max)(start, store_end), end);
        }
        for (Counts* counts : std::array<Counts*, 2>{&total, &window}) {
            ++counts->events;
            counts->gpu_bytes += gpu_bytes;
            counts->dropped_bytes += dropped_bytes;
            counts->pending += pending ? 1 : 0;
        }
        if (details < MAX_DETAILS) {
            ++details;
            lines.push_back(fmt::format(
                "NCE_GPU_DROP n={} t_ms={} page={:#x} store={:#x}+{} gpu_bytes={} dropped={} "
                "ranges={} pending={} pc={:#x} insn={:#010x} fault={:#x}",
                details, NowMs(), addr, store_start, store_end - store_start, gpu_bytes,
                dropped_bytes, gpu.size(), pending, note.pc, note.insn, note.fault));
        }
        if (mode == Mode::Keep) {
            total.kept_bytes += dropped_bytes;
            return Range{store_start, store_end - store_start};
        }
        return {};
    }

    /// The GPU wrote [addr, addr + size) again (MarkWrittenBuffer): no longer a stale candidate.
    void GpuWrote(DAddr addr, u64 size) {
        if (!any_dropped.load(std::memory_order_relaxed)) {
            return;
        }
        std::scoped_lock lk{mutex};
        sets[0].Subtract(addr, size);
        sets[1].Subtract(addr, size);
    }

    /// GPU thread, BufferCache::SynchronizeBuffer: guest memory [addr, addr + size) is uploaded
    /// for binding kind (BufTrace::Kind bit index) of the draw or dispatch who_type/a/b.
    void Uploaded(DAddr addr, u64 size, u32 kind_bits, char who_type, u64 who_a, u64 who_b) {
        if (!any_dropped.load(std::memory_order_relaxed)) {
            return;
        }
        std::scoped_lock lk{mutex};
        u64 bytes = 0;
        for (Common::RangeSet<DAddr>* set : {&sets[0], &sets[1]}) {
            set->ForEachInRange(addr, size, [&](DAddr a, DAddr b) {
                const DAddr lo = (std::max)(a, addr);
                const DAddr hi = (std::min)(b, addr + size);
                bytes += hi > lo ? hi - lo : 0;
            });
            set->Subtract(addr, size);
        }
        if (bytes == 0) {
            return;
        }
        u32 kind = 0;
        while (kind + 1 < KINDS && (kind_bits & (u32{1} << kind)) == 0) {
            ++kind;
        }
        for (Counts* counts : std::array<Counts*, 2>{&total, &window}) {
            ++counts->clobbers;
            counts->clobber_bytes += bytes;
        }
        window_kinds[kind] += bytes;
        total_kinds[kind] += bytes;
        const std::string who = fmt::format("{}:{:016x}:{:016x}", who_type, who_a, who_b);
        auto it = std::find_if(window_who.begin(), window_who.end(),
                               [&](const auto& entry) { return entry.first == who; });
        if (it != window_who.end()) {
            it->second += bytes;
        } else if (window_who.size() < 64) {
            window_who.emplace_back(who, bytes);
        }
    }

    /// GPU thread, BufferCache::TickFrame (buffer cache mutex held).
    void Tick() {
        std::vector<std::string> out;
        {
            std::scoped_lock lk{mutex};
            if (++frames % GENERATION_FRAMES == 0) {
                // The older set goes; it becomes the current one (RangeSet move assignment does
                // not return in range_sets.inc, so the sets are not moved).
                cur ^= 1;
                sets[cur].Clear();
            }
            const auto now = std::chrono::steady_clock::now();
            out.swap(lines);
            if (now - window_start >= WINDOW) {
                if (window.events != 0 || window.clobbers != 0) {
                    std::sort(window_who.begin(), window_who.end(),
                              [](const auto& a, const auto& b) { return a.second > b.second; });
                    std::string who;
                    for (size_t i = 0; i < window_who.size() && i < 4; ++i) {
                        who += fmt::format("{}{}/{}", who.empty() ? "" : ",", window_who[i].first,
                                           window_who[i].second);
                    }
                    out.push_back(fmt::format(
                        "NCE_GPU_WINDOW t_ms={} window_ms={} events={} gpu_bytes={} dropped={} "
                        "pending={} clobbers={} clobber_bytes={} kinds={} who={}",
                        NowMs(), WINDOW.count(), window.events, window.gpu_bytes,
                        window.dropped_bytes, window.pending, window.clobbers,
                        window.clobber_bytes, Kinds(window_kinds), who.empty() ? "-" : who));
                    ++windows_logged;
                }
                window = Counts{};
                window_kinds = {};
                window_who.clear();
                window_start = now;
            }
            if (now - total_log >= TOTAL_PERIOD) {
                total_log = now;
                out.push_back(fmt::format(
                    "NCE_GPU mode={} t_ms={} events={} gpu_bytes={} dropped={} pending={} "
                    "clobbers={} clobber_bytes={} kept_bytes={} other_events={} other_bytes={} "
                    "windows={} kinds={}",
                    ModeName(), NowMs(), total.events, total.gpu_bytes, total.dropped_bytes,
                    total.pending, total.clobbers, total.clobber_bytes, total.kept_bytes,
                    other_events, other_bytes, windows_logged, Kinds(total_kinds)));
            }
        }
        for (const std::string& line : out) {
            LOG_WARNING(Render_Vulkan, "{}", line);
        }
    }

    struct Totals : Counts {
        u64 kept_bytes = 0;
    };
    Totals total;
    u64 other_events = 0; // JIT or host writes over GPU bytes (exact ranges)
    u64 other_bytes = 0;

private:
    void Init() {
        initialized = true;
        const char* env = std::getenv("EDEN_NCE_BUF_GPU");
        const std::string value = env ? env : "detect";
        mode = value == "off" ? Mode::Off : (value == "keep" ? Mode::Keep : Mode::Detect);
        start_time = std::chrono::steady_clock::now();
        window_start = start_time;
        total_log = start_time;
        LOG_WARNING(Render_Vulkan, "NCE_GPU ready b76 mode={} asked={} store_bytes={} page={}",
                    ModeName(), env ? env : "-", STORE_BYTES, PAGE);
    }

    const char* ModeName() const {
        return mode == Mode::Off ? "off" : (mode == Mode::Keep ? "keep" : "detect");
    }

    u64 NowMs() const {
        return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - start_time)
                                    .count());
    }

    static std::string Kinds(const std::array<u64, KINDS>& kinds) {
        std::string out;
        for (size_t kind = 0; kind < KINDS; ++kind) {
            if (kinds[kind] != 0) {
                out += fmt::format("{}{}:{}", out.empty() ? "" : ",", kind, kinds[kind]);
            }
        }
        return out.empty() ? std::string{"-"} : out;
    }

    bool initialized = false;
    Mode mode = Mode::Off;
    std::mutex mutex;
    std::atomic<bool> any_dropped{false};
    std::array<Common::RangeSet<DAddr>, 2> sets; // dropped GPU bytes: current and previous
    size_t cur = 0;
    u64 frames = 0;
    Counts window;
    std::array<u64, KINDS> window_kinds{};
    std::array<u64, KINDS> total_kinds{};
    std::vector<std::pair<std::string, u64>> window_who;
    std::vector<std::string> lines;
    u64 details = 0;
    u64 windows_logged = 0;
    std::chrono::steady_clock::time_point start_time{};
    std::chrono::steady_clock::time_point window_start{};
    std::chrono::steady_clock::time_point total_log{};
};

inline State& Get() {
    static State state;
    return state;
}

inline bool Active() {
    return Get().GetMode() != Mode::Off;
}

} // namespace VideoCommon::NceGpu
#endif
