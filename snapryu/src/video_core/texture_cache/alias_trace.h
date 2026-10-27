// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// ALIAS_TRACE / SQUARES_MARK: diagnostic traces for terrain squares after a save reload (TotK NCE,
// 2026-10-17).
//
// Question 1: does the texture cache copy older content over guest data it has just uploaded?
//   SynchronizeAliases and JoinImages copy an alias or overlap onto an image when the source holds
//   the newer modification tick. An upload from guest memory does not advance that tick, and
//   neither path checks whether the source is CpuModified, i.e. whether the CPU wrote its memory
//   after the source's content was made. Such a copy is counted as stale: it puts older content
//   over newer guest data, and the image is then usually GpuModified, which TEX_STALE skips.
//
// Question 2: when squares are on screen, does each sampled texture still match guest memory?
//   F7 (Windows) arms a report for the next frame. Every image sampled in that frame is classified
//   once:
//     stale_marked    its content came from a stale copy (each is listed)
//     gpu_content     GpuModified: content made by the GPU, not comparable (the largest are listed)
//     cpu_pending     CpuModified: an upload is still due
//     clean           neither; guest memory is hashed in full (no budget) and compared with the
//                     hash taken at its last upload
//     clean_mismatch  ... and differs: a write the cache was never told about (each is listed)
//     no_snapshot     clean, but no upload hash for this image and address
//
// ALIAS_TRACE counters (logged every 5 s on change):
//   alias_copies / join_copies   copies made by SynchronizeAliases / JoinImages
//   stale_alias / stale_join     ... whose source was CpuModified
//   stale_alias_fresh            ... onto an image PrepareImage had just uploaded (JoinImages always
//                                uploads the new image first, so every stale_join is fresh)
//   refreshed                    fresh stale sources uploaded from guest memory first (experiment)
//   marked_slots                 image slots whose content last came from a stale copy
//
// Experiment (EDEN_ALIAS_EXP, read once):
//   refresh   a CpuModified source is uploaded from guest memory before it is copied onto an image
//             that was just uploaded. Without a fresh upload the CPU did not write the destination,
//             and the source may hold the newest GPU content for the shared part, so it is kept.
//
// Diagnostic builds only; release builds compile none of this. Used by the GPU thread only.
#pragma once

#if SNAPRYU_DIAGNOSTICS
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <ankerl/unordered_dense.h>
#include <fmt/format.h>
#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/memory_manager.h"
#include "video_core/texture_cache/image_base.h"
#include "video_core/texture_cache/remap_trace.h"

#ifdef _WIN32
extern "C" __declspec(dllimport) short
#if !defined(_M_ARM64) && !defined(__aarch64__)
    __stdcall
#endif
    GetAsyncKeyState(int vKey);
#endif

namespace VideoCommon::AliasTrace {

enum Counter : size_t {
    AliasCopies,
    JoinCopies,
    StaleAlias,
    StaleAliasFresh,
    StaleJoin,
    Refreshed,
    MarksDropped,
    NumCounters,
};

enum class Experiment { None, Refresh };

enum class Kind : u8 { Alias, Join, JoinAlias };

inline std::array<std::atomic<u64>, NumCounters> counters{};
inline std::atomic<u32> stale_details{};
inline constexpr u32 MAX_DETAILS = 64;
inline constexpr size_t MAX_MARKS = size_t{1} << 16;
inline constexpr int MARK_KEY = 0x76; // VK_F7
inline constexpr size_t MAX_LISTED = 96;
inline constexpr size_t MAX_GPU_LISTED = 16;
// b66: one press reports BURST_FRAMES frames in a row (k=0..7, same n), and each frame also counts
// every sampled image by shape (SQUARES_FRAME, SQUARES_SHAPE). In b65 the reported frame of a
// skydive showed 208 images and no terrain tile, where b64 frames of the same scene showed 425-708
// images and 16-31 tiles. Several frames tell a frame that briefly drew no tiles from tiles that
// stay missing.
inline constexpr u32 BURST_FRAMES = 8;
inline constexpr size_t MAX_SHAPES = 64;
// The terrain tile format; TileTrace::TrackedFormat sets it (default PixelFormat 52, R8G8_UNORM).
inline u32 tile_format = 52;

inline const char* ExperimentName(Experiment experiment) {
    return experiment == Experiment::Refresh ? "refresh" : "none";
}

inline const char* KindName(Kind kind) {
    switch (kind) {
    case Kind::Alias:
        return "alias";
    case Kind::Join:
        return "join";
    default:
        return "join_alias";
    }
}

inline Experiment GetExperiment() {
    static const Experiment experiment = [] {
        const char* const value = std::getenv("EDEN_ALIAS_EXP");
        const std::string_view mode = value ? value : "";
        const Experiment result = mode == "refresh" ? Experiment::Refresh : Experiment::None;
        LOG_INFO(Render_Vulkan, "ALIAS_TRACE experiment requested={} active={} mark_key=F7",
                 mode.empty() ? "none" : mode, ExperimentName(result));
        return result;
    }();
    return experiment;
}

inline void Add(Counter counter, u64 amount = 1) {
    counters[counter].fetch_add(amount, std::memory_order_relaxed);
}

inline std::string Describe(u32 index, const ImageBase& image) {
    return fmt::format("id={} gpu={:#x} cpu={:#x} size={:#x} format={} {}x{}x{} levels={} "
                       "layers={} flags={:#x} mod_tick={}",
                       index, image.gpu_addr, image.cpu_addr, image.guest_size_bytes,
                       static_cast<u32>(image.info.format), image.info.size.width,
                       image.info.size.height, image.info.size.depth,
                       image.info.resources.levels, image.info.resources.layers,
                       static_cast<u32>(image.flags), image.modification_tick);
}

// Content of an image that last came from a stale copy. Cleared when the image is uploaded again.
struct Mark {
    GPUVAddr gpu_addr{};
    u32 size{};
    u64 copy_frame{};
    Kind kind{};
    bool fresh{};
    std::string source;
};

inline ankerl::unordered_dense::map<u32, Mark> marks;

// Set by PrepareImage right before SynchronizeAliases: the image was CpuModified, so it has just
// been uploaded from guest memory.
inline bool prepare_fresh = false;

inline bool TakePrepareFresh() {
    const bool fresh = prepare_fresh;
    prepare_fresh = false;
    return fresh;
}

// Called for every copy the two paths make, before it is made. Returns true when the source is
// CpuModified (its content is older than guest memory).
inline bool IsStaleSource(Kind kind, const ImageBase& source, bool fresh) {
    Add(kind == Kind::Alias ? AliasCopies : JoinCopies);
    if (False(source.flags & ImageFlagBits::CpuModified)) {
        return false;
    }
    if (kind == Kind::Alias) {
        Add(StaleAlias);
        if (fresh) {
            Add(StaleAliasFresh);
        }
    } else {
        Add(StaleJoin);
    }
    return true;
}

inline bool ShouldRefresh(bool fresh) {
    return fresh && GetExperiment() == Experiment::Refresh;
}

// A copy with a stale source, before it is made: log the first events and, unless the source is
// refreshed first, mark the destination.
inline void RecordStale(Kind kind, u32 dst_index, const ImageBase& dst, u32 src_index,
                        const ImageBase& src, u64 frame_tick, bool fresh, bool refreshed) {
    const std::string source = Describe(src_index, src);
    if (stale_details.fetch_add(1, std::memory_order_relaxed) < MAX_DETAILS) {
        LOG_INFO(Render_Vulkan,
                 "ALIAS_STALE kind={} frame={} fresh={} refreshed={} dst {} | src {}",
                 KindName(kind), frame_tick, fresh, refreshed, Describe(dst_index, dst), source);
    }
    if (refreshed) {
        return;
    }
    if (marks.size() >= MAX_MARKS && !marks.contains(dst_index)) {
        Add(MarksDropped);
        return;
    }
    marks.insert_or_assign(dst_index, Mark{
                                          .gpu_addr = dst.gpu_addr,
                                          .size = dst.guest_size_bytes,
                                          .copy_frame = frame_tick,
                                          .kind = kind,
                                          .fresh = fresh,
                                          .source = source,
                                      });
}

inline void Clear(u32 index) {
    marks.erase(index);
}

inline const Mark* FindMark(u32 index, const ImageBase& image) {
    const auto it = marks.find(index);
    if (it == marks.end()) {
        return nullptr;
    }
    const Mark& mark = it->second;
    if (mark.gpu_addr != image.gpu_addr || mark.size != image.guest_size_bytes) {
        return nullptr;
    }
    return &mark;
}

// SQUARES_MARK state: armed by the key, collects during the next frame, reports at its end.
struct GpuImage {
    u32 size;
    std::string line;
};

// Every image sampled in a reported frame, by format and size.
struct Shape {
    u32 format = 0;
    u32 width = 0;
    u32 height = 0;
    s32 levels = 0;
    s32 layers = 0;
    u32 count = 0;
    u32 gpu = 0; // GpuModified
};

struct Report {
    u32 number = 0;
    u32 k = 0; // frame of the burst, 0..BURST_FRAMES-1
    u64 frame = 0;
    u32 sampled = 0;
    u32 stale_marked = 0;
    u32 gpu_content = 0;
    u32 cpu_pending = 0;
    u32 clean = 0;
    u32 clean_mismatch = 0;
    u32 no_snapshot = 0;
    u64 checked_bytes = 0;
    std::vector<std::string> listed;
    u32 not_listed = 0;
    std::vector<GpuImage> gpu_images;
    u32 tiles = 0;
    u32 tile_gpu = 0;
    std::vector<Shape> shapes;
};

inline bool collecting = false;
inline bool key_was_down = false;
inline u32 marks_taken = 0;
inline Report report;
inline ankerl::unordered_dense::set<u32> seen;

inline void List(std::string line) {
    if (report.listed.size() < MAX_LISTED) {
        report.listed.push_back(std::move(line));
    } else {
        ++report.not_listed;
    }
}

// Called for every sampled image view while a report is collecting; each image is looked at once.
inline void CollectSampled(const Tegra::MemoryManager& gpu_memory, u32 index,
                           const ImageBase& image, u64 frame_tick) {
    if (!collecting || !seen.insert(index).second) {
        return;
    }
    ++report.sampled;
    {
        const u32 format = static_cast<u32>(image.info.format);
        const bool gpu = True(image.flags & ImageFlagBits::GpuModified);
        if (format == tile_format) {
            ++report.tiles;
            report.tile_gpu += gpu ? 1 : 0;
        }
        auto& shapes = report.shapes;
        auto it = std::ranges::find_if(shapes, [&](const Shape& s) {
            return s.format == format && s.width == image.info.size.width &&
                   s.height == image.info.size.height && s.levels == image.info.resources.levels &&
                   s.layers == image.info.resources.layers;
        });
        if (it == shapes.end()) {
            shapes.push_back(Shape{format, image.info.size.width, image.info.size.height,
                                   image.info.resources.levels, image.info.resources.layers, 0, 0});
            it = shapes.end() - 1;
        }
        ++it->count;
        it->gpu += gpu ? 1 : 0;
    }
    if (const Mark* const mark = FindMark(index, image)) {
        ++report.stale_marked;
        List(fmt::format("SQUARES_MARK stale {} copy_frame={} age={} kind={} fresh={} | src {}",
                         Describe(index, image), mark->copy_frame, frame_tick - mark->copy_frame,
                         KindName(mark->kind), mark->fresh, mark->source));
    }
    if (True(image.flags & ImageFlagBits::GpuModified)) {
        ++report.gpu_content;
        auto& list = report.gpu_images;
        list.push_back(GpuImage{image.guest_size_bytes, Describe(index, image)});
        std::ranges::sort(list, [](const GpuImage& lhs, const GpuImage& rhs) {
            return lhs.size > rhs.size;
        });
        if (list.size() > MAX_GPU_LISTED) {
            list.pop_back();
        }
        return;
    }
    if (True(image.flags & ImageFlagBits::CpuModified)) {
        ++report.cpu_pending;
        return;
    }
    ++report.clean;
    const auto it = RemapTrace::snapshots.find(index);
    if (it == RemapTrace::snapshots.end() || it->second.gpu_addr != image.gpu_addr ||
        it->second.size != image.guest_size_bytes) {
        ++report.no_snapshot;
        return;
    }
    const RemapTrace::Snapshot& snapshot = it->second;
    const u64 now_hash = RemapTrace::HashGuest(gpu_memory, image.gpu_addr, image.guest_size_bytes);
    report.checked_bytes += image.guest_size_bytes;
    if (now_hash != snapshot.upload_hash) {
        ++report.clean_mismatch;
        List(fmt::format("SQUARES_MARK mismatch {} upload_frame={} age={}", Describe(index, image),
                         snapshot.upload_tick, frame_tick - snapshot.upload_tick));
    }
}

inline bool KeyPressed() {
#ifdef _WIN32
    const bool down = (GetAsyncKeyState(MARK_KEY) & 0x8000) != 0;
#else
    const bool down = false;
#endif
    const bool pressed = down && !key_was_down;
    key_was_down = down;
    return pressed;
}

inline void FinishReport() {
    const Report& r = report;
    LOG_INFO(Render_Vulkan,
             "SQUARES_MARK n={} frame={} sampled={} stale_marked={} gpu_content={} cpu_pending={} "
             "clean={} clean_mismatch={} no_snapshot={} checked_mib={} not_listed={} exp={}",
             r.number, r.frame, r.sampled, r.stale_marked, r.gpu_content, r.cpu_pending, r.clean,
             r.clean_mismatch, r.no_snapshot, r.checked_bytes >> 20, r.not_listed,
             ExperimentName(GetExperiment()));
    for (const std::string& line : r.listed) {
        LOG_INFO(Render_Vulkan, "{} n={}", line, r.number);
    }
    for (const GpuImage& gpu_image : r.gpu_images) {
        LOG_INFO(Render_Vulkan, "SQUARES_MARK gpu {} n={}", gpu_image.line, r.number);
    }
    LOG_INFO(Render_Vulkan,
             "SQUARES_FRAME n={} k={} frame={} sampled={} tiles={} tile_gpu={} gpu_content={} "
             "clean={} cpu_pending={} shapes={}",
             r.number, r.k, r.frame, r.sampled, r.tiles, r.tile_gpu, r.gpu_content, r.clean,
             r.cpu_pending, r.shapes.size());
    std::vector<Shape> shapes = r.shapes;
    std::ranges::sort(shapes, [](const Shape& lhs, const Shape& rhs) {
        return lhs.count > rhs.count;
    });
    size_t shown = 0;
    for (const Shape& s : shapes) {
        if (shown++ == MAX_SHAPES) {
            break;
        }
        LOG_INFO(Render_Vulkan,
                 "SQUARES_SHAPE format={} {}x{} levels={} layers={} count={} gpu={} n={} k={}",
                 s.format, s.width, s.height, s.levels, s.layers, s.count, s.gpu, r.number, r.k);
    }
    LOG_INFO(Render_Vulkan, "SQUARES_MARK end n={} k={}", r.number, r.k);
}

// Called once per frame by the texture cache after it advances frame_tick, so frame_tick is the
// number of the frame about to be drawn.
inline void EndFrame(u64 frame_tick) {
    GetExperiment();
    // Polled every frame; a press while a burst is still reporting is ignored.
    const bool pressed = KeyPressed();
    if (collecting) {
        FinishReport();
        seen.clear();
        const u32 number = report.number;
        const u32 next = report.k + 1;
        report = Report{};
        if (next < BURST_FRAMES) {
            report.number = number;
            report.k = next;
            report.frame = frame_tick;
        } else {
            collecting = false;
        }
    } else if (pressed) {
        collecting = true;
        report.number = ++marks_taken;
        report.frame = frame_tick;
        LOG_INFO(Render_Vulkan, "SQUARES_MARK armed n={} next_frame={} frames={}", report.number,
                 report.frame, BURST_FRAMES);
    }

    static std::chrono::steady_clock::time_point last_log{};
    static std::array<u64, NumCounters> last_values{};
    static size_t last_marks = 0;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_log < std::chrono::seconds(5)) {
        return;
    }
    last_log = now;
    std::array<u64, NumCounters> values{};
    for (size_t i = 0; i < NumCounters; ++i) {
        values[i] = counters[i].load(std::memory_order_relaxed);
    }
    if (values == last_values && marks.size() == last_marks) {
        return;
    }
    last_values = values;
    last_marks = marks.size();
    LOG_INFO(Render_Vulkan,
             "ALIAS_TRACE alias_copies={} join_copies={} stale_alias={} stale_alias_fresh={} "
             "stale_join={} refreshed={} marked_slots={} marks_dropped={} exp={}",
             values[AliasCopies], values[JoinCopies], values[StaleAlias], values[StaleAliasFresh],
             values[StaleJoin], values[Refreshed], marks.size(), values[MarksDropped],
             ExperimentName(GetExperiment()));
}

} // namespace VideoCommon::AliasTrace
#endif
