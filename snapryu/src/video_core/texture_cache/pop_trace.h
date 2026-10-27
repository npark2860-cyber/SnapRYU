// SPDX-FileCopyrightText: Copyright 2026 SnapRYU
// SPDX-License-Identifier: GPL-3.0-or-later

// b76 (diagnostic builds only).
//
// ImageWide: the b75 UB_WIDE rule (common/ub_keep.h) for texture cache image uploads. An upload
// reads guest memory on the GPU thread while a command runs; if the game wrote those bytes after
// that command's list was pushed, the upload reads the next frame's data ("late"). detect counts
// such uploads, keep uploads the bytes as they were at the push and makes the image upload again
// once a later command runs.
//
// PopTrace: which images the draws sampled in each of the last frames, which were uploaded and
// which were written by the GPU. On F8 (the user just saw a polygon pop) and every 30 s (baseline)
// it reports, for the frames around that moment, the images sampled for only 1-2 frames among
// frames without them ("appear"), the images missing for only 1-2 frames among frames with them
// ("gap") and the images sampled steadily that were rewritten in only 1-2 frames ("rare_write").
// A pop that comes from a different image bound for a frame shows as appear/gap; one that comes
// from image contents shows as rare_write or as nothing.
#pragma once

#if SNAPRYU_DIAGNOSTICS
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <ankerl/unordered_dense.h>
#include <fmt/format.h>
#include "common/common_types.h"
#include "common/logging.h"
// b80: the read guard counters (guard_flushes, preemptive_reads, area_skips) per frame and at the mark.
#include "common/nce_read_guard.h"
#include "common/ub_keep.h"
// b77: with EDEN_COND_POP=1 each report also carries the render conditions of the same frames
// (COND_REPORT, COND_FRAME, COND_DIP).
#include "video_core/engines/cond_pop.h"
// b78: with EDEN_DRAW_POP=1 each report also carries the draws of the same frames
// (DRAW_REPORT, DRAW_FRAME, DRAW_DIP).
#include "video_core/engines/bind_pop.h"
#include "video_core/engines/draw_pop.h"
#include "video_core/memory_manager.h"
#include "video_core/texture_cache/image_base.h"

namespace VideoCommon::ImageWide {

inline constexpr u32 KIND = u32{1} << 15; // after the 14 BufTrace kinds and WIDE_DESCRIPTOR

struct Kept {
    DAddr cpu_addr;
    u64 size;
    u64 fence;
};

inline std::vector<u8> buffer;  // texture cache mutex: a private copy of the guest bytes
inline std::vector<Kept> kept;  // texture cache mutex: images uploaded with copy bytes

inline bool Enabled() {
    return Common::UbKeep::Enabled() &&
           Common::UbKeep::Get().Mode() != Common::UbKeep::WideMode::Off;
}

/// Texture cache, GPU thread: bytes holds what an upload of image just read from guest memory at
/// image.gpu_addr. Returns true when keep replaced bytes with copy bytes.
inline bool Read(Tegra::MemoryManager& gpu_memory, const ImageBase& image, GPUVAddr gpu_addr,
                 std::span<u8> bytes) {
    if (bytes.empty()) {
        return false;
    }
    Common::UbKeep::State& state = Common::UbKeep::Get();
    const std::optional<DAddr> daddr = gpu_memory.GpuToCpuAddress(gpu_addr);
    if (!daddr || !gpu_memory.IsContinuousRange(gpu_addr, bytes.size())) {
        state.wide_image_skips.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const u64 shape = (u64{static_cast<u32>(image.info.format)} << 48) |
                      (u64{image.info.size.width & 0xffffffu} << 24) |
                      u64{image.info.size.height & 0xffffffu};
    const bool wrote =
        state.CachedRead(*daddr, bytes.data(), bytes.size(), KIND, 'I', gpu_addr, shape, false);
    if (wrote) {
        kept.push_back(Kept{*daddr, bytes.size(),
                            state.wide_current.load(std::memory_order_relaxed)});
    }
    return wrote;
}

/// Texture cache, GPU thread: guest holds the bytes an upload is about to unswizzle (it may point
/// into guest memory). Returns them, or a private copy with copy bytes when keep changed them.
inline std::span<const u8> ReadInput(Tegra::MemoryManager& gpu_memory, const ImageBase& image,
                                     std::span<const u8> guest) {
    if (!Enabled()) {
        return guest;
    }
    buffer.assign(guest.begin(), guest.end());
    if (Read(gpu_memory, image, image.gpu_addr, buffer)) {
        return buffer;
    }
    return guest;
}

/// Texture cache, GPU thread, at the start of a draw or dispatch (no image iteration running):
/// the kept ranges whose command is over. The caller marks the images there CPU-modified.
template <typename Func>
inline void TakeDone(Func&& func) {
    if (kept.empty()) {
        return;
    }
    const u64 now = Common::UbKeep::Get().wide_current.load(std::memory_order_relaxed);
    std::erase_if(kept, [&](const Kept& range) {
        if (now <= range.fence) {
            return false;
        }
        func(range.cpu_addr, range.size);
        return true;
    });
}

} // namespace VideoCommon::ImageWide

namespace VideoCommon::PopTrace {

inline constexpr size_t RING = 256;          // frames kept
inline constexpr u32 WINDOW = 160;           // frames reported
inline constexpr u32 AFTER = 4;              // frames after F8 before the report
inline constexpr u32 SHORT = 2;              // a run of at most 2 frames is short
inline constexpr u32 STEADY = 4;             // with at least 4 frames each side
inline constexpr size_t MAX_LISTED = 48;     // image lines per report
inline constexpr size_t MAX_INFOS = size_t{1} << 18;
inline constexpr auto AUTO_PERIOD = std::chrono::seconds{30};
inline constexpr int KEY = 0x77;             // VK_F8

struct Info {
    u32 index = 0;
    u32 format = 0;
    u32 width = 0;
    u32 height = 0;
    u32 depth = 0;
    u32 levels = 0;
    u32 layers = 0;
    u32 flags = 0;
    GPUVAddr gpu_addr = 0;
    DAddr cpu_addr = 0;
};

struct Frame {
    u64 tick = 0;
    u64 t_ms = 0;
    u64 guard_flushes = 0; // b80: NceReadGuard counters at the end of the frame (cumulative)
    u64 guard_pre = 0;
    u64 guard_area = 0;
    std::vector<u64> sampled; // image keys
    std::vector<u64> uploaded;
    std::vector<u64> written;
};

inline bool enabled = false;
inline bool initialized = false;
inline std::deque<Frame> frames;
inline Frame current;
inline std::vector<u64> last_sampled; // image index -> frame tick + 1
inline std::vector<u64> last_uploaded;
inline std::vector<u64> last_written;
inline ankerl::unordered_dense::map<u64, Info> infos;
inline u64 tick = 0;
inline u64 reports = 0;
inline u64 marks = 0;
inline u64 pending_mark_tick = 0; // frame of the F8 press, 0 = none
// b86 VIS_RING (texture_cache/vis_ring.h): a mark can also come from a glitch VisRing found in the
// presented frames themselves (RequestMark, reported as why=vis); key_marks and key_mark_tick tell
// VisRing of the F8 marks so it writes the thumbnails around them too.
inline const char* mark_why = "F8"; // why pending_mark_tick was set
inline u64 key_marks = 0;           // F8 marks that set pending_mark_tick
inline u64 key_mark_tick = 0;       // the frame of the last of them
inline bool key_down = false;
inline std::chrono::steady_clock::time_point start{};
inline std::chrono::steady_clock::time_point last_auto{};

inline u64 Key(u32 index, GPUVAddr gpu_addr) {
    return (u64{index} << 40) | (gpu_addr & ((u64{1} << 40) - 1));
}

inline u64 NowMs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count());
}

inline void Init() {
    initialized = true;
    const char* env = std::getenv("EDEN_POP_TRACE");
    enabled = !(env && std::string{env} == "0");
    start = std::chrono::steady_clock::now();
    last_auto = start;
    LOG_WARNING(Render_Vulkan,
                "POP_TRACE ready b76 enabled={} ring={} window={} short={} steady={} key=F8 "
                "auto_s={}",
                enabled, RING, WINDOW, SHORT, STEADY, AUTO_PERIOD.count());
}

inline bool Active() {
    if (!initialized) [[unlikely]] {
        Init();
    }
    return enabled;
}

inline bool FirstInFrame(std::vector<u64>& last, u32 index) {
    if (index >= last.size()) {
        last.resize(static_cast<size_t>(index) + 1024, 0);
    }
    if (last[index] == tick + 1) {
        return false;
    }
    last[index] = tick + 1;
    return true;
}

inline void Remember(u32 index, const ImageBase& image) {
    if (infos.size() >= MAX_INFOS) {
        infos.clear();
    }
    infos[Key(index, image.gpu_addr)] = Info{index,
                                             static_cast<u32>(image.info.format),
                                             image.info.size.width,
                                             image.info.size.height,
                                             image.info.size.depth,
                                             static_cast<u32>(image.info.resources.levels),
                                             static_cast<u32>(image.info.resources.layers),
                                             static_cast<u32>(image.flags),
                                             image.gpu_addr,
                                             image.cpu_addr};
}

/// A draw or dispatch samples the image (texture cache, GPU thread).
inline void Sampled(u32 index, const ImageBase& image) {
    if (!Active() || !FirstInFrame(last_sampled, index)) {
        return;
    }
    current.sampled.push_back(Key(index, image.gpu_addr));
    Remember(index, image);
}

/// The image was uploaded from guest memory.
inline void Uploaded(u32 index, const ImageBase& image) {
    if (!Active() || !FirstInFrame(last_uploaded, index)) {
        return;
    }
    current.uploaded.push_back(Key(index, image.gpu_addr));
}

/// The GPU wrote the image (render target, storage, alias or join copy).
inline void Wrote(u32 index, const ImageBase& image) {
    if (!Active() || !FirstInFrame(last_written, index)) {
        return;
    }
    current.written.push_back(Key(index, image.gpu_addr));
}

inline bool KeyPressed() {
#ifdef _WIN32
    const bool down = (GetAsyncKeyState(KEY) & 0x8000) != 0;
#else
    const bool down = false;
#endif
    const bool pressed = down && !key_down;
    key_down = down;
    return pressed;
}

inline std::string Describe(u64 key) {
    const auto it = infos.find(key);
    if (it == infos.end()) {
        return fmt::format("img={} gpu={:#x} info=none", key >> 40, key & ((u64{1} << 40) - 1));
    }
    const Info& i = it->second;
    return fmt::format("img={} gpu={:#x} cpu={:#x} fmt={} {}x{}x{} levels={} layers={} "
                       "flags={:#x}",
                       i.index, i.gpu_addr, i.cpu_addr, i.format, i.width, i.height, i.depth,
                       i.levels, i.layers, i.flags);
}

// Frames [first, first + count) of a presence row, relative to the mark frame, as "a..b".
inline std::string Rel(size_t first, size_t count, size_t mark) {
    const long long a = static_cast<long long>(first) - static_cast<long long>(mark);
    return fmt::format("{}..{}", a, a + static_cast<long long>(count) - 1);
}

inline void Report(bool by_key) {
    const size_t count = (std::min)(frames.size(), size_t{WINDOW});
    if (count < 2 * STEADY + 1) {
        return;
    }
    const size_t first = frames.size() - count;
    ++reports;
    const u64 n = by_key ? marks : 0;
    // Index of the F8 frame inside the window (by_key), else the last frame.
    size_t mark = count - 1;
    for (size_t f = 0; f < count; ++f) {
        if (by_key && frames[first + f].tick == pending_mark_tick) {
            mark = f;
        }
    }
    struct Row {
        std::vector<u8> sampled;
        std::vector<u8> touched; // uploaded or written
    };
    ankerl::unordered_dense::map<u64, Row> rows;
    const auto row = [&](u64 key) -> Row& {
        Row& r = rows[key];
        if (r.sampled.empty()) {
            r.sampled.assign(count, 0);
            r.touched.assign(count, 0);
        }
        return r;
    };
    std::vector<u64> sampled_n(count), uploaded_n(count), written_n(count);
    for (size_t f = 0; f < count; ++f) {
        const Frame& frame = frames[first + f];
        sampled_n[f] = frame.sampled.size();
        uploaded_n[f] = frame.uploaded.size();
        written_n[f] = frame.written.size();
        for (const u64 key : frame.sampled) {
            row(key).sampled[f] = 1;
        }
        for (const u64 key : frame.uploaded) {
            row(key).touched[f] |= 1;
        }
        for (const u64 key : frame.written) {
            row(key).touched[f] |= 2;
        }
    }
    std::vector<u64> appear_n(count), gap_n(count), rare_n(count);
    std::vector<std::string> listed;
    u64 appear_total = 0, gap_total = 0, rare_total = 0;
    const auto touched_at = [&](const Row& r) {
        std::string out;
        for (size_t f = 0; f < count; ++f) {
            if (r.touched[f] != 0) {
                out += fmt::format("{}{}{}", out.empty() ? "" : ",",
                                   static_cast<long long>(f) - static_cast<long long>(mark),
                                   r.touched[f] == 1 ? "u" : (r.touched[f] == 2 ? "w" : "uw"));
            }
        }
        return out.empty() ? std::string{"-"} : out;
    };
    for (const auto& [key, r] : rows) {
        size_t used = 0;
        for (const u8 v : r.sampled) {
            used += v;
        }
        // Runs of equal values; a short run between two steady runs of the other value.
        size_t s = 0;
        while (s < count) {
            size_t e = s;
            while (e + 1 < count && r.sampled[e + 1] == r.sampled[s]) {
                ++e;
            }
            const size_t len = e - s + 1;
            if (len <= SHORT && s >= STEADY && e + STEADY < count) {
                bool before = true, after = true;
                for (size_t k = 1; k <= STEADY; ++k) {
                    before = before && r.sampled[s - k] != r.sampled[s];
                    after = after && r.sampled[e + k] != r.sampled[s];
                }
                if (before && after) {
                    const bool appear = r.sampled[s] != 0;
                    for (size_t f = s; f <= e; ++f) {
                        ++(appear ? appear_n : gap_n)[f];
                    }
                    ++(appear ? appear_total : gap_total);
                    if (listed.size() < MAX_LISTED) {
                        listed.push_back(fmt::format(
                            "POP_IMG n={} what={} rel={} used={} touched={} {}", n,
                            appear ? "appear" : "gap", Rel(s, len, mark), used, touched_at(r),
                            Describe(key)));
                    }
                }
            }
            s = e + 1;
        }
        // Sampled steadily but rewritten in only 1-2 frames of the window.
        size_t touched_frames = 0;
        size_t last_touch = 0;
        for (size_t f = 0; f < count; ++f) {
            if (r.touched[f] != 0) {
                ++touched_frames;
                last_touch = f;
            }
        }
        if (touched_frames >= 1 && touched_frames <= SHORT && used >= 2 * STEADY) {
            bool sampled_around = true;
            for (size_t k = 1; k <= STEADY && sampled_around; ++k) {
                const bool has_before = last_touch >= k && r.sampled[last_touch - k] != 0;
                const bool has_after = last_touch + k < count && r.sampled[last_touch + k] != 0;
                sampled_around = has_before || has_after;
            }
            if (sampled_around) {
                ++rare_n[last_touch];
                ++rare_total;
                if (listed.size() < MAX_LISTED) {
                    listed.push_back(fmt::format("POP_IMG n={} what=rare_write rel={} used={} "
                                                 "touched={} {}",
                                                 n, Rel(last_touch, 1, mark), used, touched_at(r),
                                                 Describe(key)));
                }
            }
        }
    }
    u64 sampled_sum = 0;
    for (const u64 v : sampled_n) {
        sampled_sum += v;
    }
    std::vector<std::string> lines;
    lines.push_back(fmt::format(
        "POP_REPORT n={} why={} t_ms={} mark_tick={} frames={} first_tick={} last_tick={} "
        "sampled_avg={} images={} appear={} gap={} rare_write={} listed={}",
        n, by_key ? mark_why : "auto", NowMs(), frames[first + mark].tick, count,
        frames[first].tick, frames.back().tick, sampled_sum / count, rows.size(), appear_total,
        gap_total, rare_total, listed.size()));
    for (size_t f = 0; f < count; ++f) {
        const Frame& frame = frames[first + f];
        // b80: the frame's own read guard events (difference to the frame before it).
        const Frame* const prev = first + f > 0 ? &frames[first + f - 1] : nullptr;
        const auto delta = [&](u64 Frame::*field) {
            return prev != nullptr ? frame.*field - (*prev).*field : u64{0};
        };
        lines.push_back(fmt::format(
            "POP_FRAME n={} rel={} tick={} t_ms={} sampled={} uploaded={} written={} appear={} "
            "gap={} rare_write={} guard_flush={} guard_pre={} guard_area={}",
            n, static_cast<long long>(f) - static_cast<long long>(mark), frame.tick, frame.t_ms,
            sampled_n[f], uploaded_n[f], written_n[f], appear_n[f], gap_n[f], rare_n[f],
            delta(&Frame::guard_flushes), delta(&Frame::guard_pre), delta(&Frame::guard_area)));
    }
    for (std::string& line : listed) {
        lines.push_back(std::move(line));
    }
    if (CondPop::Active()) {
        CondPop::Report(n, frames[first].tick, frames[first + mark].tick, frames.back().tick, lines);
    }
    if (DrawPop::Active()) {
        DrawPop::Report(n, frames[first].tick, frames[first + mark].tick, frames.back().tick, lines);
    }
    if (BindPop::Active()) {
        BindPop::Report(n, frames[first].tick, frames[first + mark].tick, frames.back().tick, lines);
    }
    lines.push_back(fmt::format("POP_REPORT end n={}", n));
    for (const std::string& line : lines) {
        LOG_WARNING(Render_Vulkan, "{}", line);
    }
}

/// b86 VIS_RING (GPU thread): a mark at a frame still in the ring (VisRing found a glitch there);
/// false while another mark waits for its report or when the frame already left the ring.
inline bool RequestMark(u64 at_tick) {
    if (!Active() || pending_mark_tick != 0) {
        return false;
    }
    const auto it = std::find_if(frames.begin(), frames.end(),
                                 [at_tick](const Frame& f) { return f.tick == at_tick; });
    if (it == frames.end()) {
        return false;
    }
    ++marks;
    pending_mark_tick = at_tick;
    mark_why = "vis";
    LOG_WARNING(Render_Vulkan,
                "POP_MARK n={} tick={} t_ms={} guard_flushes={} guard_pre={} guard_area={} why=vis",
                marks, pending_mark_tick, it->t_ms, it->guard_flushes, it->guard_pre,
                it->guard_area);
    return true;
}

/// Texture cache TickFrame, after frame_tick moved on: closes the frame just drawn.
inline void EndFrame(u64 frame_tick) {
    if (!Active()) {
        return;
    }
    if (CondPop::Active()) {
        CondPop::EndFrame(tick); // b77: the same frame boundary
    }
    if (DrawPop::Active()) {
        DrawPop::EndFrame(tick); // b78: the same frame boundary
    }
    if (BindPop::Active()) {
        BindPop::EndFrame(tick); // b79: the same frame boundary
    }
    current.tick = tick;
    current.t_ms = NowMs();
    current.guard_flushes = Common::NceReadGuard::guard_flushes.load(std::memory_order_relaxed);
    current.guard_pre = Common::NceReadGuard::preemptive_reads.load(std::memory_order_relaxed);
    current.guard_area = Common::NceReadGuard::area_skips.load(std::memory_order_relaxed);
    frames.push_back(std::move(current));
    current = Frame{};
    if (frames.size() > RING) {
        frames.pop_front();
    }
    tick = frame_tick;
    if (KeyPressed()) {
        ++marks;
        if (pending_mark_tick == 0) {
            pending_mark_tick = frames.back().tick;
            mark_why = "F8"; // b86
            ++key_marks;
            key_mark_tick = pending_mark_tick;
            LOG_WARNING(Render_Vulkan,
                        "POP_MARK n={} tick={} t_ms={} guard_flushes={} guard_pre={} guard_area={}",
                        marks, pending_mark_tick, frames.back().t_ms, frames.back().guard_flushes,
                        frames.back().guard_pre, frames.back().guard_area);
        }
    }
    if (pending_mark_tick != 0 && frames.back().tick >= pending_mark_tick + AFTER) {
        Report(true);
        pending_mark_tick = 0;
        last_auto = std::chrono::steady_clock::now();
        return;
    }
    if (pending_mark_tick == 0 && std::chrono::steady_clock::now() - last_auto >= AUTO_PERIOD) {
        last_auto = std::chrono::steady_clock::now();
        Report(false);
    }
}

} // namespace VideoCommon::PopTrace
#endif
