// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// GPU_LOSS (b65): every time the texture cache loses image content that only the GPU holds, and
// which path caused it (TotK NCE terrain squares, 2026-10-18).
//
// b64 proved one case: an NCE store just past a terrain tile invalidates the whole 4 KiB page,
// WriteMemory marks the GPU-baked tile CpuModified, and the next RefreshContents uploads guest
// memory (zeros) over it. b65 makes GPU::InvalidateRegion write such images back to guest memory
// before a preserving (NCE) invalidation. This trace checks that rule from the other side: it
// does not look at one path, it looks at the moment content is lost, whatever path led there.
//
// "GPU-only content": GpuModified, and modification_tick changed since the image was last
// uploaded from guest memory (GpuModified alone stays set after uploads). DownloadMemory clears
// GpuModified, so written-back images do not count.
//
// Losses (what=):
//   upload_over_gpu   RefreshContents uploads guest memory over GPU-only content. source= is the
//                     path whose WriteMemory (or remap) marked the image CpuModified.
//   gc_drop           the garbage collector deletes it without a download (BadOverlap/MSAA)
//   join_ignore_drop  JoinImages deletes an ignored overlap with GPU writes
//   unmap_drop        UnmapMemory deletes it
// Counted only: mark_over_gpu, a mark of an image holding GPU-only content, per source (the
// precursor of upload_over_gpu; with the b65 rule NCE marks should come only after a write-back).
//
// Sources: the rasterizer entry point that called WriteMemory (outer caller wins), remap for
// UnmapGPUMemory. nce_handler, nce_fast and nce_writeback were the b65 write-back paths (removed
// in b74); their counters stay 0.
//
// Logs: GPU_LOSS per loss (Warning, first 300, then powers of two), GPU_LOSS_COUNTS every 5 s when
// something changed, with the write-back counters of gpu.cpp (fast/slow/handler, wait).
//
// Marks are stored in a fixed table (WriteMemory may run inside the NCE exception handler: no
// allocation, no logging there). Used under the texture cache mutex, except the atomics.
// Diagnostic builds only.
#pragma once

#if SNAPRYU_DIAGNOSTICS
#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <fmt/format.h>
#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/texture_cache/image_base.h"

namespace VideoCommon::GpuLossTrace {

enum class Source : u8 {
    Other,
    CacheInvalidation,
    NceFast,
    NceWriteBack,
    NceHandler,
    CpuWrite,
    InnerInvalidation,
    InvalidateRegion,
    InlineMemory,
    Remap,
    Count,
};

enum class What : u8 {
    UploadOverGpu,
    GcDrop,
    JoinIgnoreDrop,
    UnmapDrop,
    MarkOverGpu,
    Count,
};

constexpr size_t NumSources = static_cast<size_t>(Source::Count);
constexpr size_t NumWhats = static_cast<size_t>(What::Count);

constexpr const char* SourceName(Source source) {
    switch (source) {
    case Source::Other:
        return "other";
    case Source::CacheInvalidation:
        return "cache_invalidation";
    case Source::NceFast:
        return "nce_fast";
    case Source::NceWriteBack:
        return "nce_writeback";
    case Source::NceHandler:
        return "nce_handler";
    case Source::CpuWrite:
        return "cpu_write";
    case Source::InnerInvalidation:
        return "inner_invalidation";
    case Source::InvalidateRegion:
        return "invalidate_region";
    case Source::InlineMemory:
        return "inline_memory";
    case Source::Remap:
        return "remap";
    default:
        return "?";
    }
}

constexpr const char* WhatName(What what) {
    switch (what) {
    case What::UploadOverGpu:
        return "upload_over_gpu";
    case What::GcDrop:
        return "gc_drop";
    case What::JoinIgnoreDrop:
        return "join_ignore_drop";
    case What::UnmapDrop:
        return "unmap_drop";
    case What::MarkOverGpu:
        return "mark_over_gpu";
    default:
        return "?";
    }
}

// The rasterizer entry point running on this thread.
inline thread_local Source source = Source::Other;

struct Scope {
    explicit Scope(Source tag) noexcept : previous{source} {
        if (source == Source::Other) {
            source = tag;
        }
    }
    ~Scope() {
        source = previous;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    Source previous;
};

// gpu.cpp write-back rule (CPU threads).
inline std::atomic<u64> nce_fast{0};
inline std::atomic<u64> nce_slow{0};
inline std::atomic<u64> nce_handler{0};
inline std::atomic<u64> nce_wait_us{0};

struct Entry {
    u32 index = ~0U;
    GPUVAddr gpu_addr = 0;
    u64 synced_tick = 0;
    u64 mark_addr = 0;
    u64 mark_size = 0;
    u64 mark_frame = 0;
    Source mark_source = Source::Other;
    bool marked = false;
};

constexpr size_t TableSize = 16384;
inline std::array<Entry, TableSize> table{};
inline std::array<std::array<u64, NumSources>, NumWhats> counts{};
inline u64 collisions = 0;
inline u64 losses = 0;
inline u64 logged = 0;
// b66: losses of terrain tiles are logged up to MAX_TILE_LOGGED whatever the overall count (b65
// logged only the first 300 losses of all formats, then powers of two).
inline constexpr u32 TILE_FORMAT = 52; // PixelFormat R8G8_UNORM, as TileTrace::DEFAULT_FORMAT
inline constexpr u64 MAX_TILE_LOGGED = 2000;
inline u64 tile_losses = 0;

inline Entry& Lookup(u32 index, const ImageBase& image, bool& known) {
    Entry& entry = table[index & (TableSize - 1)];
    known = entry.index == index && entry.gpu_addr == image.gpu_addr;
    if (!known) {
        if (entry.index != ~0U) {
            ++collisions;
        }
        entry = Entry{};
        entry.index = index;
        entry.gpu_addr = image.gpu_addr;
    }
    return entry;
}

inline bool GpuOnly(const ImageBase& image, const Entry& entry) {
    return True(image.flags & ImageFlagBits::GpuModified) &&
           image.modification_tick != entry.synced_tick;
}

inline void Loss(What what, Source from, u32 index, const ImageBase& image, const Entry& entry,
                 bool known, u64 frame) {
    ++counts[static_cast<size_t>(what)][static_cast<size_t>(from)];
    const u64 n = ++losses;
    const bool tile = static_cast<u32>(image.info.format) == TILE_FORMAT;
    tile_losses += tile ? 1 : 0;
    if (!(tile && tile_losses <= MAX_TILE_LOGGED) && n > 300 && (n & (n - 1)) != 0) {
        return;
    }
    ++logged;
    LOG_WARNING(Render_Vulkan,
                "GPU_LOSS what={} source={} id={} format={} {}x{}x{} levels={} layers={} "
                "samples={} size={:#x} gpu={:#x} cpu={:#x} flags={:#x} tick={} synced_tick={} "
                "mark_addr={:#x} mark_size={:#x} mark_frame={} frame={} known={} n={}",
                WhatName(what), SourceName(from), index, static_cast<u32>(image.info.format),
                image.info.size.width, image.info.size.height, image.info.size.depth,
                image.info.resources.levels, image.info.resources.layers, image.info.num_samples,
                image.guest_size_bytes, image.gpu_addr, image.cpu_addr,
                static_cast<u32>(image.flags), image.modification_tick, entry.synced_tick,
                entry.mark_addr, entry.mark_size, entry.mark_frame, frame, known ? 1 : 0, n);
}

// WriteMemory (or UnmapGPUMemory) is about to mark the image CpuModified. May run inside the NCE
// exception handler: store only.
inline void Marked(u32 index, const ImageBase& image, u64 addr, u64 size, u64 frame,
                   Source from = Source::Other) {
    if (from == Source::Other) {
        from = source;
    }
    bool known = false;
    Entry& entry = Lookup(index, image, known);
    if (GpuOnly(image, entry)) {
        ++counts[static_cast<size_t>(What::MarkOverGpu)][static_cast<size_t>(from)];
    }
    entry.mark_addr = addr;
    entry.mark_size = size;
    entry.mark_frame = frame;
    entry.mark_source = from;
    entry.marked = true;
}

// RefreshContents cleared CpuModified and uploads guest memory now.
inline void Refreshed(u32 index, const ImageBase& image, u64 frame) {
    bool known = false;
    Entry& entry = Lookup(index, image, known);
    if (GpuOnly(image, entry)) {
        Loss(What::UploadOverGpu, entry.marked ? entry.mark_source : Source::Other, index, image,
             entry, known, frame);
    }
    entry.synced_tick = image.modification_tick;
    entry.marked = false;
}

// The image is deleted; what says by which path. A CpuModified image already lost its GPU
// content at its mark (or is about to be re-uploaded anyway), so only clean ones count here.
inline void Dropped(u32 index, const ImageBase& image, What what, u64 frame) {
    if (True(image.flags & ImageFlagBits::CpuModified)) {
        return;
    }
    bool known = false;
    Entry& entry = Lookup(index, image, known);
    if (GpuOnly(image, entry)) {
        Loss(what, Source::Other, index, image, entry, known, frame);
    }
}

inline void Forget(u32 index) {
    Entry& entry = table[index & (TableSize - 1)];
    if (entry.index == index) {
        entry = Entry{};
    }
}

// Called once per frame by the texture cache.
inline void EndFrame(u64 frame) {
    static bool ready = false;
    if (!ready) {
        ready = true;
        LOG_INFO(Render_Vulkan, "GPU_LOSS_TRACE ready table={} rule=writeback_before_nce_store",
                 TableSize);
    }
    static std::chrono::steady_clock::time_point last_log{};
    static std::array<std::array<u64, NumSources>, NumWhats> last_counts{};
    static u64 last_fast = 0;
    static u64 last_slow = 0;
    static u64 last_handler = 0;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_log < std::chrono::seconds(5)) {
        return;
    }
    last_log = now;
    const u64 fast = nce_fast.load(std::memory_order_relaxed);
    const u64 slow = nce_slow.load(std::memory_order_relaxed);
    const u64 handler = nce_handler.load(std::memory_order_relaxed);
    if (counts == last_counts && fast == last_fast && slow == last_slow &&
        handler == last_handler) {
        return;
    }
    last_counts = counts;
    last_fast = fast;
    last_slow = slow;
    last_handler = handler;
    std::string text;
    for (size_t what = 0; what < NumWhats; ++what) {
        std::string sources;
        for (size_t from = 0; from < NumSources; ++from) {
            if (counts[what][from] == 0) {
                continue;
            }
            sources += fmt::format("{}{}={}", sources.empty() ? "" : ",",
                                   SourceName(static_cast<Source>(from)), counts[what][from]);
        }
        text += fmt::format(" {}=[{}]", WhatName(static_cast<What>(what)), sources);
    }
    LOG_INFO(Render_Vulkan,
             "GPU_LOSS_COUNTS{} losses={} logged={} nce_fast={} nce_slow={} nce_handler={} "
             "nce_wait_ms={} collisions={} frame={} tile_losses={}",
             text, losses, logged, fast, slow, handler,
             nce_wait_us.load(std::memory_order_relaxed) / 1000, collisions, frame, tile_losses);
}

} // namespace VideoCommon::GpuLossTrace
#endif
