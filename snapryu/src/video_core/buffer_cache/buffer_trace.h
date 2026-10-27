// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// BUFFER_TRACE / SQUARES_BUF: do the buffers used while terrain squares are on screen hold what
// guest memory holds (TotK NCE, 2026-10-17)?
//
// b51 and b52 showed, on two frames with squares on screen, that every sampled texture either
// equals guest memory or was just drawn by the GPU, and that no newer GPU content went to another
// image. Textures are ruled out; the squares come from buffers or from guest memory itself.
//
// F7 (alias_trace.h) arms a report for the next frame. During that frame every buffer range the
// cache synchronizes for a binding (index, vertex, indirect, uniform, storage, texture buffer,
// transform feedback, the compute ones, DMA, image copies) is recorded with its binding kind. At
// the end of the frame each 4 KiB page of those ranges is classified:
//   gpu               GPU-modified (host content is newer than guest memory; listed by the age of
//                     the last GPU write)
//   download_pending  GPU content on its way to guest memory (committed or async download)
//   cpu_pending       CPU-modified: an upload is still due
//   host_written      written on the host only since its last upload (DMA clear, inline data),
//                     so it may legitimately differ
//   clean             none of the above: the host buffer must equal guest memory byte for byte
// Clean (and host_written) pages are copied from the host buffer to a download staging buffer,
// the GPU is waited on once per batch, and the bytes are compared with guest memory. A clean page
// that differs is a write the cache was never told about: the host buffer is stale.
// Small uniform buffers read straight from guest memory per draw (fast path, UB_KEEP) are only
// counted; they cannot go stale in the cache.
//
// The check runs inside BufferCache::TickFrame under the buffer cache mutex. CPU writes that the
// cache is told about (NCE invalidation, OnCPUWrite) take the same mutex, so they wait for the
// check: a clean page cannot change under it.
// F7 frames hitch (one GPU wait per 63 MiB checked). Diagnostic builds only; release builds
// compile none of this. Used under the buffer cache mutex only.
//
// b54 DISPATCH_TRACE / SQUARES_CS (2026-10-17): b53 found every draw buffer equal to guest memory.
// Under NCE some GPU-made terrain pages (vertex + compute storage) were last written 45-117 s
// before the F7 frame while their neighbours were written in it; under Dynarmic the same ranges
// were rewritten every frame. b54 records who writes GPU-made pages and what the F7 frame
// dispatched:
//   writer       every GPU write mark carries the compute shader (dispatch) or VS/FS pair (draw)
//                that bound the buffer for writing, or DMA, or ObtainBuffer (query cache). Each
//                page keeps its last writer; the F7 report sums the GPU pages it saw per writer
//                and per age.
//   dispatches   every compute dispatch of the F7 frame: shader, grid, block, written storage
//                ranges, and whether it reached the command buffer. A Configure failure or a grid
//                over the device limit returns after the buffers were already marked written.
//   descriptors  storage buffer address/size words are read from guest memory on the CPU. Words
//                the GPU wrote and the cache has not downloaded are stale there; counted.
//   skips        dispatches with no pipeline (before binding) and with an unbuilt pipeline (at
//                record time), and draws with no pipeline, counted since boot.
// A writer that still runs on the F7 frame but left old pages was aimed elsewhere by the game; a
// writer that stopped was not issued or was skipped.
//
// b55 GPU_PAGE_CHECK (2026-10-17): b54 found the same compute work under NCE and Dynarmic. b53
// left out every page with the memory tracker's GPU bit, but CommitAsyncFlushesHigh downloads
// with ForEachDownloadRange(..., false) and PopAsyncBuffers only subtracts gpu_modified_ranges,
// so the bit stays after the GPU content reached guest memory. Those pages are now split:
//   gpu               still GPU-owned: in gpu_modified_ranges or uncommitted (not downloaded)
//   gpu_downloaded    only the tracker bit is left: guest memory already holds the GPU content,
//                     so the host buffer must equal it like a clean page
// gpu_downloaded pages are compared with the clean ones and their differences counted apart
// (gpu_downloaded_mismatch), with the age and writer of the page's last GPU write. Both kinds
// keep feeding the b54 ages and writers; SQUARES_BUF gpu lines tell how much of a range was
// downloaded, and SQUARES_BUF owned_ages gives the age of the GPU bytes not yet downloaded (with
// high GPU accuracy every fence downloads them, so old ones mean a download that never came).
//
// b58 FAST_UB (2026-10-17): b53-b55 only counted the fast-path uniform buffers, so their bytes were
// never compared. The fast path copies guest memory into the Vulkan stream buffer on every draw
// (UB_KEEP may swap in the copy from the push) and the GPU reads that copy, not a cached buffer.
// EDEN_FAST_UB=0 (read once) sends every uniform buffer that would take the fast path by its size
// through the cached path instead: SynchronizeBuffer uploads it and the F7 check compares it with
// guest memory like any other binding (kind uniform). Bindings whose offset breaks the host
// uniform buffer alignment must still use the stream; they are counted apart
// (fast_ub_align_binds). UB_KEEP only acts on the fast path, so with EDEN_FAST_UB=0 it does not
// cover these buffers and polygons may pop as before UB_KEEP.
#pragma once

#if SNAPRYU_DIAGNOSTICS
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <ankerl/unordered_dense.h>
#include <fmt/format.h>
#include "common/common_types.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/texture_cache/alias_trace.h"

namespace VideoCommon::BufTrace {

enum class Kind : u16 {
    Index = 1 << 0,
    Vertex = 1 << 1,
    Indirect = 1 << 2,
    Uniform = 1 << 3,
    Storage = 1 << 4,
    Texture = 1 << 5,
    Feedback = 1 << 6,
    ComputeUniform = 1 << 7,
    ComputeStorage = 1 << 8,
    ComputeTexture = 1 << 9,
    Dma = 1 << 10,
    ImageCopy = 1 << 11,
    Inline = 1 << 12,
    Other = 1 << 13,
};
inline constexpr size_t NUM_KINDS = 14;
inline constexpr std::array<std::string_view, NUM_KINDS> KIND_NAMES{
    "index",    "vertex",   "indirect", "uniform", "storage",    "texture", "feedback",
    "cuniform", "cstorage", "ctexture", "dma",     "image_copy", "inline",  "other"};

enum class Bucket : u8 {
    Clean,
    Gpu,
    DownloadPending,
    CpuPending,
    HostWritten,
    GpuDownloaded,
    NumBuckets
};

inline constexpr u64 PAGE = 4096;
inline constexpr size_t MAX_RANGES = size_t{1} << 18;
inline constexpr size_t MAX_PAGES = size_t{1} << 21;
inline constexpr u64 MAX_CHECK_BYTES = u64{1} << 30;
inline constexpr u64 BATCH_BYTES = u64{63} << 20; // staging stays within one 64 MiB buffer
inline constexpr size_t MAX_MISMATCH_LINES = 64;
inline constexpr size_t MAX_GPU_LINES = 64;

// Frame stamps are frame + 1; 0 means never.
struct PageInfo {
    u32 upload = 0; // last upload from guest memory
    u32 gpu = 0;    // last GPU write mark
    u32 host = 0;   // host-only write after the last upload (cleared by an upload)
    u32 writer = 0; // writer of the last GPU write mark
};

// b54 writers. Fixed ids: 0 unknown, 1 DMA copy, 2 ObtainBuffer (query cache).
inline constexpr u32 WRITER_UNKNOWN = 0;
inline constexpr u32 WRITER_DMA = 1;
inline constexpr u32 WRITER_OBTAIN = 2;
inline constexpr size_t MAX_WRITERS = size_t{1} << 16;

struct Writer {
    char type = '?'; // C compute, G graphics, D DMA, Q ObtainBuffer
    u64 a = 0;       // compute shader hash, or VS hash
    u64 b = 0;       // FS hash
    u64 first_frame = 0;
    u64 last_frame = 0;
    u64 uses = 0; // dispatches or draws
};

struct WriterKey {
    char type = '?';
    u64 a = 0;
    u64 b = 0;
    bool operator==(const WriterKey&) const = default;
};

struct WriterKeyHash {
    u64 operator()(const WriterKey& key) const noexcept {
        const u64 h = (key.a * 0x9E3779B97F4A7C15ULL) ^
                      ((key.b + static_cast<u8>(key.type)) * 0xC2B2AE3D27D4EB4FULL);
        return h ^ (h >> 29);
    }
};

enum class DispatchStatus : u8 { Recorded, ConfigureFailed, OverMaxDim };
inline constexpr std::array<std::string_view, 3> DISPATCH_STATUS{"ok", "configure_failed",
                                                                 "over_max_dim"};

struct Dispatch {
    u64 hash = 0;
    u32 writer = 0;
    std::array<u32, 3> grid{};
    std::array<u32, 3> block{};
    u32 writes_begin = 0; // into dispatch_writes
    u32 writes = 0;
    u32 writes_dropped = 0;
    u32 desc_reads = 0;
    u32 desc_gpu = 0;
    DispatchStatus status = DispatchStatus::Recorded;
    bool indirect = false;
};

inline constexpr size_t MAX_DISPATCHES = 16384;
inline constexpr size_t MAX_DISPATCH_WRITES = 65536;
inline constexpr size_t MAX_DISPATCH_LINES = 4096;
inline constexpr size_t MAX_SHADER_LINES = 256;
inline constexpr size_t MAX_WRITER_LINES = 64;
inline constexpr size_t WRITES_PER_LINE = 16;

// Age of a GPU page's last write mark at the end of the F7 frame.
enum Age : size_t {
    Age0f,
    AgeUnder1s,
    Age1to10s,
    Age10to30s,
    Age30to60s,
    Age60s,
    AgeNever,
    NUM_AGES
};
inline constexpr std::array<std::string_view, NUM_AGES> AGE_NAMES{
    "0f", "lt1s", "1to10s", "10to30s", "30to60s", "ge60s", "never"};

struct WriterTotals {
    std::array<u64, NUM_AGES> bytes{};
    u32 oldest = 0;
};

struct Range {
    u64 addr = 0;
    u64 size = 0;
    u16 kinds = 0;
};

struct Mismatch {
    u64 addr = 0;
    u64 size = 0;
    u16 kinds = 0;
    u32 buffer = 0;
    u64 buffer_addr = 0;
    u64 diff_bytes = 0;
    u64 first_diff = 0; // offset from addr
    std::string host_bytes;
    std::string guest_bytes;
    u32 upload = 0;
    bool host_written = false;
    bool gpu_downloaded = false;
    u32 gpu = 0;    // last GPU write stamp of the first page
    u32 writer = 0; // its writer
};

struct GpuRange {
    u64 addr = 0;
    u64 size = 0;
    u16 kinds = 0;
    u32 oldest = 0; // oldest GPU write stamp among its pages (0: no record)
    u32 newest = 0;
    u32 oldest_writer = 0;
    u64 downloaded = 0; // bytes of pages whose GPU content is already in guest memory
};

struct KindTotals {
    std::array<u64, static_cast<size_t>(Bucket::NumBuckets)> bytes{};
    u64 mismatch = 0;
    u64 gpu_downloaded_mismatch = 0;
};

struct Report {
    u32 number = 0;
    u64 frame = 0;
    std::array<u64, static_cast<size_t>(Bucket::NumBuckets)> bytes{};
    std::array<KindTotals, NUM_KINDS> kinds{};
    u64 merged_ranges = 0;
    u64 checked = 0;
    u64 unchecked = 0; // over MAX_CHECK_BYTES
    u64 no_buffer = 0;
    u64 mismatch = 0;
    u64 host_written_mismatch = 0;
    u64 gpu_downloaded_mismatch = 0;
    std::vector<Mismatch> mismatches;
    u64 mismatches_not_listed = 0;
    std::vector<GpuRange> gpu_ranges;
    std::array<u64, NUM_AGES> age_bytes{};                          // GPU bytes by write age
    std::array<u64, NUM_AGES> owned_age_bytes{}; // b55: GPU bytes not yet downloaded, by age
    ankerl::unordered_dense::map<u32, WriterTotals> writer_totals; // GPU bytes per last writer
};

inline ankerl::unordered_dense::map<u32, PageInfo> pages;
inline u64 pages_dropped = 0;
inline u16 current_kind = static_cast<u16>(Kind::Other);
inline bool collecting = false;
inline std::vector<Range> ranges;
inline u64 ranges_dropped = 0;
inline u64 fast_binds = 0;
inline u64 fast_bytes = 0;
inline u64 fast_align_binds = 0; // b58: of fast_binds, forced by alignment
inline Report report;
inline std::vector<float> frame_seconds; // [buffer cache frame]: start of that frame, seconds
// addr -> (size, writer) marked this frame
inline ankerl::unordered_dense::map<u64, std::pair<u64, u32>> gpu_seen;
inline u64 gpu_seen_frame = ~u64{0};
inline u64 current_frame = 0; // buffer cache frame_tick, set by Follow

inline std::vector<Writer> writers{Writer{.type = '?'}, Writer{.type = 'D'},
                                    Writer{.type = 'Q'}};
inline ankerl::unordered_dense::map<WriterKey, u32, WriterKeyHash> writer_ids;
inline u64 writers_dropped = 0;
inline u32 current_writer = WRITER_UNKNOWN;
inline u32 saved_writer = WRITER_UNKNOWN;

// F7 frame dispatches.
inline std::vector<Dispatch> dispatches;
inline std::vector<std::pair<u64, u64>> dispatch_writes; // addr, size
inline u64 dispatches_dropped = 0;
inline u64 dispatch_writes_dropped = 0;
inline bool dispatch_open = false;
inline u64 draw_desc_reads = 0;
inline u64 draw_desc_gpu = 0;

// Since boot.
inline u64 total_dispatches = 0;
inline u64 total_configure_failed = 0;
inline u64 total_over_max_dim = 0;
inline u64 total_desc_reads = 0;
inline u64 total_desc_gpu = 0;
inline u64 total_desc_gpu_written = 0;
// Counted outside the buffer cache mutex (before the lock, or on the scheduler worker).
inline std::atomic<u64> null_compute_pipeline{0};
inline std::atomic<u64> unbound_dispatch{0};
inline std::atomic<u64> null_graphics_pipeline{0};

class KindScope {
public:
    explicit KindScope(Kind kind) : previous{current_kind} {
        current_kind = static_cast<u16>(kind);
    }
    ~KindScope() {
        current_kind = previous;
    }
    KindScope(const KindScope&) = delete;
    KindScope& operator=(const KindScope&) = delete;

private:
    u16 previous;
};

inline u32 Stamp(u64 frame) {
    return static_cast<u32>(frame) + 1;
}

inline PageInfo* FindPage(u32 page, bool create) {
    if (const auto it = pages.find(page); it != pages.end()) {
        return &it->second;
    }
    if (!create) {
        return nullptr;
    }
    if (pages.size() >= MAX_PAGES) {
        ++pages_dropped;
        return nullptr;
    }
    return &pages[page];
}

template <typename Func>
inline void ForEachPage(u64 addr, u64 size, Func&& func) {
    if (size == 0) {
        return;
    }
    const u64 last = (addr + size - 1) / PAGE;
    for (u64 page = addr / PAGE; page <= last; ++page) {
        func(static_cast<u32>(page));
    }
}

inline float Seconds(u64 stamp) {
    if (stamp == 0) {
        return 0.0f;
    }
    const u64 frame = stamp - 1;
    return frame < frame_seconds.size() ? frame_seconds[frame] : 0.0f;
}

inline std::string KindText(u16 kinds) {
    std::string text;
    for (size_t i = 0; i < NUM_KINDS; ++i) {
        if ((kinds >> i) & 1) {
            if (!text.empty()) {
                text += ',';
            }
            text += KIND_NAMES[i];
        }
    }
    return text.empty() ? std::string{"none"} : text;
}

inline std::string AgeText(u32 stamp, u64 frame) {
    if (stamp == 0) {
        return "never";
    }
    const u64 then = stamp - 1;
    if (then > frame) {
        return "0";
    }
    return fmt::format("{}f/{:.1f}s", frame - then, Seconds(Stamp(frame)) - Seconds(stamp));
}

inline size_t AgeBucket(u32 stamp) {
    if (stamp == 0) {
        return AgeNever;
    }
    if (stamp - 1 >= current_frame) {
        return Age0f;
    }
    const float age = Seconds(Stamp(current_frame)) - Seconds(stamp);
    return age < 1.0f    ? AgeUnder1s
           : age < 10.0f ? Age1to10s
           : age < 30.0f ? Age10to30s
           : age < 60.0f ? Age30to60s
                         : Age60s;
}

// SynchronizeBuffer: guest memory was just uploaded to the host buffer.
inline void Uploaded(u64 addr, u64 size, u64 frame) {
    ForEachPage(addr, size, [&](u32 page) {
        if (PageInfo* const info = FindPage(page, true)) {
            info->upload = Stamp(frame);
            info->host = 0;
        }
    });
}

// MarkWrittenBuffer, DMA copy into GPU-modified memory. A storage buffer is marked on every draw
// that binds it; the same range by the same writer again in the same frame would write the same
// stamps, so skip it. On the F7 frame the range also goes to the open dispatch.
inline void GpuWrote(u64 addr, u64 size, u64 frame, u32 writer) {
    if (dispatch_open && writer == dispatches.back().writer) {
        Dispatch& dispatch = dispatches.back();
        if (dispatch_writes.size() >= MAX_DISPATCH_WRITES) {
            ++dispatch.writes_dropped;
            ++dispatch_writes_dropped;
        } else {
            dispatch_writes.emplace_back(addr, size);
            ++dispatch.writes;
        }
    }
    if (frame != gpu_seen_frame) {
        gpu_seen.clear();
        gpu_seen_frame = frame;
    }
    const auto [it, inserted] = gpu_seen.try_emplace(addr, size, writer);
    if (!inserted && it->second.first == size && it->second.second == writer) {
        return;
    }
    it->second = {size, writer};
    ForEachPage(addr, size, [&](u32 page) {
        if (PageInfo* const info = FindPage(page, true)) {
            info->gpu = Stamp(frame);
            info->writer = writer;
        }
    });
}

// DMA clear, inline data: the host buffer changed before or without guest memory.
inline void HostWrote(u64 addr, u64 size, u64 frame) {
    ForEachPage(addr, size, [&](u32 page) {
        if (PageInfo* const info = FindPage(page, true)) {
            info->host = Stamp(frame);
        }
    });
}

// b54: id of a compute shader ('C', hash) or a draw's VS/FS pair ('G'); counts one use now.
inline u32 FindWriter(char type, u64 a, u64 b) {
    const WriterKey key{type, a, b};
    if (const auto it = writer_ids.find(key); it != writer_ids.end()) {
        Writer& writer = writers[it->second];
        writer.last_frame = current_frame;
        ++writer.uses;
        return it->second;
    }
    if (writers.size() >= MAX_WRITERS) {
        ++writers_dropped;
        return WRITER_UNKNOWN;
    }
    const u32 id = static_cast<u32>(writers.size());
    writers.push_back(Writer{.type = type,
                             .a = a,
                             .b = b,
                             .first_frame = current_frame,
                             .last_frame = current_frame,
                             .uses = 1});
    writer_ids.emplace(key, id);
    return id;
}

// RasterizerVulkan::DispatchCompute under the buffer cache lock, before Configure binds buffers.
inline void BeginDispatch(u64 hash, const std::array<u32, 3>& grid,
                          const std::array<u32, 3>& block) {
    dispatch_open = false;
    current_writer = FindWriter('C', hash, 0);
    ++total_dispatches;
    if (!collecting) {
        return;
    }
    if (dispatches.size() >= MAX_DISPATCHES) {
        ++dispatches_dropped;
        return;
    }
    dispatches.push_back(Dispatch{.hash = hash,
                                  .writer = current_writer,
                                  .grid = grid,
                                  .block = block,
                                  .writes_begin = static_cast<u32>(dispatch_writes.size())});
    dispatch_open = true;
}

// Where DispatchCompute ends: recorded (direct or indirect), or returned after Configure.
inline void EndDispatch(DispatchStatus status, bool indirect) {
    if (status == DispatchStatus::ConfigureFailed) {
        ++total_configure_failed;
    } else if (status == DispatchStatus::OverMaxDim) {
        ++total_over_max_dim;
    }
    if (dispatch_open) {
        dispatches.back().status = status;
        dispatches.back().indirect = indirect;
        dispatch_open = false;
    }
}

// RasterizerVulkan::PrepareDraw under the buffer cache lock, before Configure binds buffers.
inline void BeginDraw(u64 vs, u64 fs) {
    dispatch_open = false;
    current_writer = FindWriter('G', vs, fs);
}

// ObtainBuffer(MarkAsWritten): query cache writes outside a draw or dispatch.
inline void BeginOther(u32 writer) {
    saved_writer = current_writer;
    current_writer = writer;
}

inline void EndOther() {
    current_writer = saved_writer;
}

// Bind{Graphics,Compute}StorageBuffer: the SSBO address/size words were read from guest memory.
// gpu_made: the GPU wrote those words and they are not downloaded, so the CPU read an old address
// or size.
inline void DescriptorRead(bool gpu_made, bool is_written, bool compute) {
    ++total_desc_reads;
    if (gpu_made) {
        ++total_desc_gpu;
        total_desc_gpu_written += is_written ? 1 : 0;
    }
    if (!collecting) {
        return;
    }
    if (!compute) {
        ++draw_desc_reads;
        draw_desc_gpu += gpu_made ? 1 : 0;
    } else if (dispatch_open) {
        ++dispatches.back().desc_reads;
        dispatches.back().desc_gpu += gpu_made ? 1 : 0;
    }
}

// SynchronizeBuffer on the F7 frame: this range is used by the current binding kind.
inline void Used(u64 addr, u64 size) {
    if (!collecting || size == 0) {
        return;
    }
    if (ranges.size() >= MAX_RANGES) {
        ++ranges_dropped;
        return;
    }
    ranges.push_back(Range{addr, size, current_kind});
}

inline void FastUniform(u64 size, bool alignment) {
    if (collecting) {
        ++fast_binds;
        fast_bytes += size;
        fast_align_binds += alignment ? 1 : 0;
    }
}

// b58: false when EDEN_FAST_UB=0 (see the header). Read once, logged once.
inline bool FastUbAllowed() {
    static const bool allowed = [] {
        const char* const value = std::getenv("EDEN_FAST_UB");
        const bool on = value == nullptr || std::string_view{value} != "0";
        LOG_WARNING(Render_Vulkan, "FAST_UB size_path={} (EDEN_FAST_UB)", on ? "on" : "off");
        return on;
    }();
    return allowed;
}

// Sorted, overlaps and neighbours merged, kinds combined.
inline std::vector<Range> Merged() {
    std::vector<Range> sorted = ranges;
    std::ranges::sort(sorted, [](const Range& lhs, const Range& rhs) { return lhs.addr < rhs.addr; });
    std::vector<Range> merged;
    for (const Range& range : sorted) {
        if (!merged.empty() && range.addr <= merged.back().addr + merged.back().size) {
            Range& last = merged.back();
            last.size = std::max(last.addr + last.size, range.addr + range.size) - last.addr;
            last.kinds |= range.kinds;
        } else {
            merged.push_back(range);
        }
    }
    return merged;
}

// Calls func(start, size, page) for each 4 KiB page piece of a range.
template <typename Func>
inline void ForEachChunk(const Range& range, Func&& func) {
    const u64 end = range.addr + range.size;
    for (u64 start = range.addr; start < end;) {
        const u64 page_end = (start / PAGE + 1) * PAGE;
        const u64 stop = std::min(page_end, end);
        func(start, stop - start, static_cast<u32>(start / PAGE));
        start = stop;
    }
}

inline void AddBytes(Bucket bucket, u16 kinds, u64 size) {
    report.bytes[static_cast<size_t>(bucket)] += size;
    for (size_t i = 0; i < NUM_KINDS; ++i) {
        if ((kinds >> i) & 1) {
            report.kinds[i].bytes[static_cast<size_t>(bucket)] += size;
        }
    }
}

inline void AddGpu(u16 kinds, u64 start, u64 size, u32 page, bool downloaded) {
    AddBytes(downloaded ? Bucket::GpuDownloaded : Bucket::Gpu, kinds, size);
    const u64 downloaded_bytes = downloaded ? size : 0;
    const PageInfo* const info = FindPage(page, false);
    const u32 stamp = info ? info->gpu : 0;
    const u32 writer = info ? info->writer : WRITER_UNKNOWN;
    const size_t age = AgeBucket(stamp);
    report.age_bytes[age] += size;
    if (!downloaded) {
        report.owned_age_bytes[age] += size;
    }
    WriterTotals& totals = report.writer_totals[writer];
    totals.bytes[age] += size;
    if (stamp != 0 && (totals.oldest == 0 || stamp < totals.oldest)) {
        totals.oldest = stamp;
    }
    auto& list = report.gpu_ranges;
    if (!list.empty() && list.back().addr + list.back().size == start) {
        GpuRange& last = list.back();
        last.size += size;
        last.kinds |= kinds;
        last.downloaded += downloaded_bytes;
        if (stamp != 0 && (last.oldest == 0 || stamp < last.oldest)) {
            last.oldest = stamp;
            last.oldest_writer = writer;
        }
        last.newest = std::max(last.newest, stamp);
        return;
    }
    list.push_back(GpuRange{start, size, kinds, stamp, stamp, writer, downloaded_bytes});
}

inline std::string Hex(const u8* data, size_t size) {
    std::string text;
    for (size_t i = 0; i < size; ++i) {
        text += fmt::format("{:02x}", data[i]);
    }
    return text;
}

// Compares one checked piece; host and guest point at size bytes each.
inline void Compare(u64 addr, u64 size, u16 kinds, u32 buffer, u64 buffer_addr, const u8* host,
                    const u8* guest, bool host_written, bool gpu_downloaded) {
    report.checked += size;
    if (std::memcmp(host, guest, size) == 0) {
        return;
    }
    u64 diff = 0;
    u64 first = size;
    for (u64 i = 0; i < size; ++i) {
        if (host[i] != guest[i]) {
            ++diff;
            first = std::min(first, i);
        }
    }
    if (host_written) {
        report.host_written_mismatch += diff;
    } else if (gpu_downloaded) {
        report.gpu_downloaded_mismatch += diff;
        for (size_t i = 0; i < NUM_KINDS; ++i) {
            if ((kinds >> i) & 1) {
                report.kinds[i].gpu_downloaded_mismatch += diff;
            }
        }
    } else {
        report.mismatch += diff;
        for (size_t i = 0; i < NUM_KINDS; ++i) {
            if ((kinds >> i) & 1) {
                report.kinds[i].mismatch += diff;
            }
        }
    }
    const PageInfo* const info = FindPage(static_cast<u32>(addr / PAGE), false);
    auto& list = report.mismatches;
    if (!list.empty()) {
        Mismatch& last = list.back();
        if (last.addr + last.size == addr && last.buffer == buffer &&
            last.host_written == host_written && last.gpu_downloaded == gpu_downloaded) {
            last.size += size;
            last.diff_bytes += diff;
            last.kinds |= kinds;
            return;
        }
    }
    if (list.size() >= MAX_MISMATCH_LINES) {
        ++report.mismatches_not_listed;
        return;
    }
    const size_t shown = static_cast<size_t>(std::min<u64>(16, size - first));
    list.push_back(Mismatch{
        .addr = addr,
        .size = size,
        .kinds = kinds,
        .buffer = buffer,
        .buffer_addr = buffer_addr,
        .diff_bytes = diff,
        .first_diff = first,
        .host_bytes = Hex(host + first, shown),
        .guest_bytes = Hex(guest + first, shown),
        .upload = info ? info->upload : 0,
        .host_written = host_written,
        .gpu_downloaded = gpu_downloaded,
        .gpu = info ? info->gpu : 0,
        .writer = info ? info->writer : WRITER_UNKNOWN,
    });
}

inline std::string WriterText(u32 id) {
    if (id >= writers.size()) {
        return "?";
    }
    const Writer& writer = writers[id];
    switch (writer.type) {
    case 'C':
        return fmt::format("C:{:016x}", writer.a);
    case 'G':
        return fmt::format("G:{:016x}/{:016x}", writer.a, writer.b);
    default:
        return std::string(1, writer.type);
    }
}

// SQUARES_CS: the F7 frame's compute dispatches, and who last wrote the GPU pages it used.
inline void FinishDispatchReport(u64 frame) {
    const Report& r = report;
    u64 ok = 0;
    u64 configure_failed = 0;
    u64 over_max_dim = 0;
    u64 indirect = 0;
    u64 desc_reads = 0;
    u64 desc_gpu = 0;
    for (const Dispatch& d : dispatches) {
        ok += d.status == DispatchStatus::Recorded ? 1 : 0;
        configure_failed += d.status == DispatchStatus::ConfigureFailed ? 1 : 0;
        over_max_dim += d.status == DispatchStatus::OverMaxDim ? 1 : 0;
        indirect += d.indirect ? 1 : 0;
        desc_reads += d.desc_reads;
        desc_gpu += d.desc_gpu;
    }
    LOG_INFO(Render_Vulkan,
             "SQUARES_CS n={} frame={} nce={} dispatches={} ok={} configure_failed={} "
             "over_max_dim={} indirect={} dispatches_dropped={} writes={} writes_dropped={} "
             "desc_reads={} desc_gpu={} draw_desc_reads={} draw_desc_gpu={} writers={} "
             "writers_dropped={} "
             "total_dispatches={} total_configure_failed={} total_over_max_dim={} "
             "total_null_pipeline={} total_unbound={} total_null_draw_pipeline={} "
             "total_desc_reads={} total_desc_gpu={} total_desc_gpu_written={}",
             r.number, frame, Settings::IsNceEnabled() ? 1 : 0, dispatches.size(), ok,
             configure_failed, over_max_dim, indirect,
             dispatches_dropped, dispatch_writes.size(), dispatch_writes_dropped, desc_reads,
             desc_gpu, draw_desc_reads, draw_desc_gpu, writers.size(), writers_dropped,
             total_dispatches, total_configure_failed, total_over_max_dim,
             null_compute_pipeline.load(std::memory_order_relaxed),
             unbound_dispatch.load(std::memory_order_relaxed),
             null_graphics_pipeline.load(std::memory_order_relaxed), total_desc_reads,
             total_desc_gpu, total_desc_gpu_written);
    std::string ages;
    for (size_t i = 0; i < NUM_AGES; ++i) {
        ages += fmt::format(" {}={}", AGE_NAMES[i], r.age_bytes[i]);
    }
    LOG_INFO(Render_Vulkan, "SQUARES_CS ages n={}{}", r.number, ages);

    // Writers of the GPU pages this frame used, most bytes last written 10 s or more ago first.
    ankerl::unordered_dense::map<u32, std::pair<u64, u64>> f7_by_writer; // dispatches, writes
    for (const Dispatch& d : dispatches) {
        auto& [count, writes] = f7_by_writer[d.writer];
        ++count;
        writes += d.writes;
    }
    struct WriterRow {
        u32 id;
        const WriterTotals* totals;
        u64 old;
        u64 total;
    };
    std::vector<WriterRow> writer_rows;
    for (const auto& [id, totals] : r.writer_totals) {
        u64 total = 0;
        for (const u64 bytes : totals.bytes) {
            total += bytes;
        }
        writer_rows.push_back(WriterRow{id, &totals,
                                        totals.bytes[Age10to30s] + totals.bytes[Age30to60s] +
                                            totals.bytes[Age60s],
                                        total});
    }
    std::ranges::sort(writer_rows, [](const WriterRow& lhs, const WriterRow& rhs) {
        return lhs.old != rhs.old ? lhs.old > rhs.old : lhs.total > rhs.total;
    });
    size_t writer_lines = 0;
    for (const WriterRow& row : writer_rows) {
        if (writer_lines == MAX_WRITER_LINES) {
            break;
        }
        ++writer_lines;
        const Writer& writer = row.id < writers.size() ? writers[row.id] : writers[0];
        const bool counted = writer.type == 'C' || writer.type == 'G';
        const auto f7 = f7_by_writer.find(row.id);
        std::string bytes;
        for (size_t i = 0; i < NUM_AGES; ++i) {
            bytes += fmt::format(" {}={}", AGE_NAMES[i], row.totals->bytes[i]);
        }
        LOG_INFO(Render_Vulkan,
                 "SQUARES_CS writer n={} id={} who={} uses={} first_use_age={} last_use_age={} "
                 "f7_dispatches={} f7_writes={} oldest_write_age={}{}",
                 r.number, row.id, WriterText(row.id), writer.uses,
                 counted ? AgeText(Stamp(writer.first_frame), frame) : std::string{"-"},
                 counted ? AgeText(Stamp(writer.last_frame), frame) : std::string{"-"},
                 f7 != f7_by_writer.end() ? f7->second.first : 0,
                 f7 != f7_by_writer.end() ? f7->second.second : 0,
                 AgeText(row.totals->oldest, frame), bytes);
    }

    // Compute shaders of the F7 frame, most bytes marked written first.
    struct ShaderRow {
        u64 hash = 0;
        u32 writer = 0;
        u64 dispatches = 0;
        u64 ok = 0;
        u64 configure_failed = 0;
        u64 over_max_dim = 0;
        u64 groups = 0;
        u64 writes = 0;
        u64 write_bytes = 0;
        u64 desc_gpu = 0;
        size_t first_seq = 0;
        size_t last_seq = 0;
    };
    std::vector<ShaderRow> shader_rows;
    ankerl::unordered_dense::map<u64, size_t> shader_index;
    for (size_t seq = 0; seq < dispatches.size(); ++seq) {
        const Dispatch& d = dispatches[seq];
        const auto [it, inserted] = shader_index.try_emplace(d.hash, shader_rows.size());
        if (inserted) {
            shader_rows.push_back(ShaderRow{.hash = d.hash, .writer = d.writer, .first_seq = seq});
        }
        ShaderRow& row = shader_rows[it->second];
        ++row.dispatches;
        row.ok += d.status == DispatchStatus::Recorded ? 1 : 0;
        row.configure_failed += d.status == DispatchStatus::ConfigureFailed ? 1 : 0;
        row.over_max_dim += d.status == DispatchStatus::OverMaxDim ? 1 : 0;
        row.groups += u64{d.grid[0]} * d.grid[1] * d.grid[2];
        row.writes += d.writes;
        for (u32 i = 0; i < d.writes; ++i) {
            row.write_bytes += dispatch_writes[d.writes_begin + i].second;
        }
        row.desc_gpu += d.desc_gpu;
        row.last_seq = seq;
    }
    std::ranges::sort(shader_rows, [](const ShaderRow& lhs, const ShaderRow& rhs) {
        return lhs.write_bytes != rhs.write_bytes ? lhs.write_bytes > rhs.write_bytes
                                                  : lhs.dispatches > rhs.dispatches;
    });
    size_t shader_lines = 0;
    for (const ShaderRow& row : shader_rows) {
        if (shader_lines == MAX_SHADER_LINES) {
            break;
        }
        ++shader_lines;
        LOG_INFO(Render_Vulkan,
                 "SQUARES_CS shader n={} hash={:016x} writer={} dispatches={} ok={} "
                 "configure_failed={} over_max_dim={} groups={} writes={} write_bytes={} "
                 "desc_gpu={} first_seq={} last_seq={}",
                 r.number, row.hash, row.writer, row.dispatches, row.ok, row.configure_failed,
                 row.over_max_dim, row.groups, row.writes, row.write_bytes, row.desc_gpu,
                 row.first_seq, row.last_seq);
    }

    // Every dispatch in order, with the storage ranges it marked written.
    size_t dispatch_lines = 0;
    for (size_t seq = 0; seq < dispatches.size() && seq < MAX_DISPATCH_LINES; ++seq) {
        const Dispatch& d = dispatches[seq];
        std::string writes;
        for (u32 i = 0; i < d.writes && i < WRITES_PER_LINE; ++i) {
            const auto& [addr, size] = dispatch_writes[d.writes_begin + i];
            writes += fmt::format("{}{:#x}+{:#x}", i == 0 ? "" : ",", addr, size);
        }
        ++dispatch_lines;
        LOG_INFO(Render_Vulkan,
                 "SQUARES_CS dispatch n={} seq={} hash={:016x} writer={} grid={}x{}x{} "
                 "block={}x{}x{} status={} indirect={} desc_reads={} desc_gpu={} writes={} "
                 "writes_not_listed={} writes_dropped={} w={}",
                 r.number, seq, d.hash, d.writer, d.grid[0], d.grid[1], d.grid[2], d.block[0],
                 d.block[1], d.block[2], DISPATCH_STATUS[static_cast<size_t>(d.status)],
                 d.indirect ? 1 : 0, d.desc_reads, d.desc_gpu, d.writes,
                 d.writes > WRITES_PER_LINE ? d.writes - WRITES_PER_LINE : 0, d.writes_dropped,
                 writes.empty() ? std::string{"none"} : writes);
    }
    LOG_INFO(Render_Vulkan,
             "SQUARES_CS end n={} dispatch_lines={} dispatch_not_listed={} shader_lines={} "
             "shader_not_listed={} writer_lines={} writer_not_listed={}",
             r.number, dispatch_lines, dispatches.size() - dispatch_lines, shader_lines,
             shader_rows.size() - shader_lines, writer_lines, writer_rows.size() - writer_lines);
}

inline void FinishReport(u64 frame, float check_ms) {
    Report& r = report;
    const auto b = [&](Bucket bucket) { return r.bytes[static_cast<size_t>(bucket)]; };
    LOG_INFO(Render_Vulkan,
             "SQUARES_BUF n={} frame={} t={:.1f} ranges={} ranges_dropped={} merged={} clean={} "
             "checked={} mismatch={} mismatch_ranges={} host_written={} host_written_mismatch={} "
             "gpu={} gpu_downloaded={} gpu_downloaded_mismatch={} download_pending={} "
             "cpu_pending={} no_buffer={} unchecked={} fast_ub_binds={} fast_ub_bytes={} "
             "fast_ub_align_binds={} "
             "pages_tracked={} pages_dropped={} check_ms={:.0f}",
             r.number, frame, Seconds(Stamp(frame)), ranges.size(), ranges_dropped,
             r.merged_ranges, b(Bucket::Clean), r.checked, r.mismatch,
             r.mismatches.size() + r.mismatches_not_listed, b(Bucket::HostWritten),
             r.host_written_mismatch, b(Bucket::Gpu), b(Bucket::GpuDownloaded),
             r.gpu_downloaded_mismatch, b(Bucket::DownloadPending), b(Bucket::CpuPending),
             r.no_buffer, r.unchecked, fast_binds, fast_bytes, fast_align_binds, pages.size(),
             pages_dropped,
             check_ms);
    for (size_t i = 0; i < NUM_KINDS; ++i) {
        const KindTotals& k = r.kinds[i];
        u64 total = 0;
        for (const u64 value : k.bytes) {
            total += value;
        }
        if (total == 0) {
            continue;
        }
        LOG_INFO(Render_Vulkan,
                 "SQUARES_BUF kind name={} bytes={} clean={} mismatch={} gpu={} gpu_downloaded={} "
                 "gpu_downloaded_mismatch={} download_pending={} cpu_pending={} host_written={} "
                 "n={}",
                 KIND_NAMES[i], total, k.bytes[static_cast<size_t>(Bucket::Clean)], k.mismatch,
                 k.bytes[static_cast<size_t>(Bucket::Gpu)],
                 k.bytes[static_cast<size_t>(Bucket::GpuDownloaded)], k.gpu_downloaded_mismatch,
                 k.bytes[static_cast<size_t>(Bucket::DownloadPending)],
                 k.bytes[static_cast<size_t>(Bucket::CpuPending)],
                 k.bytes[static_cast<size_t>(Bucket::HostWritten)], r.number);
    }
    LOG_INFO(Render_Vulkan,
             "SQUARES_BUF owned_ages n={} 0f={} lt1s={} 1to10s={} 10to30s={} 30to60s={} ge60s={} "
             "never={}",
             r.number, r.owned_age_bytes[Age0f], r.owned_age_bytes[AgeUnder1s],
             r.owned_age_bytes[Age1to10s], r.owned_age_bytes[Age10to30s],
             r.owned_age_bytes[Age30to60s], r.owned_age_bytes[Age60s], r.owned_age_bytes[AgeNever]);
    for (const Mismatch& m : r.mismatches) {
        LOG_INFO(Render_Vulkan,
                 "SQUARES_BUF mismatch addr={:#x} size={:#x} kinds={} buffer={} buffer_addr={:#x} "
                 "diff_bytes={} first_diff=+{:#x} host={} guest={} upload_age={} host_written={} "
                 "gpu_downloaded={} gpu_write_age={} writer={} n={}",
                 m.addr, m.size, KindText(m.kinds), m.buffer, m.buffer_addr, m.diff_bytes,
                 m.first_diff, m.host_bytes, m.guest_bytes, AgeText(m.upload, frame),
                 m.host_written ? 1 : 0, m.gpu_downloaded ? 1 : 0, AgeText(m.gpu, frame),
                 WriterText(m.writer), r.number);
    }
    std::vector<GpuRange> gpu = r.gpu_ranges;
    // Oldest last write first (no record first), larger first on ties.
    std::ranges::sort(gpu, [](const GpuRange& lhs, const GpuRange& rhs) {
        return lhs.oldest != rhs.oldest ? lhs.oldest < rhs.oldest : lhs.size > rhs.size;
    });
    size_t gpu_listed = 0;
    for (const GpuRange& g : gpu) {
        if (gpu_listed == MAX_GPU_LINES) {
            break;
        }
        ++gpu_listed;
        LOG_INFO(Render_Vulkan,
                 "SQUARES_BUF gpu addr={:#x} size={:#x} kinds={} oldest_write_age={} "
                 "newest_write_age={} oldest_writer={} downloaded={:#x} n={}",
                 g.addr, g.size, KindText(g.kinds), AgeText(g.oldest, frame),
                 AgeText(g.newest, frame), WriterText(g.oldest_writer), g.downloaded, r.number);
    }
    LOG_INFO(Render_Vulkan,
             "SQUARES_BUF end n={} mismatch_listed={} mismatch_not_listed={} gpu_listed={} "
             "gpu_not_listed={}",
             r.number, r.mismatches.size(), r.mismatches_not_listed, gpu_listed,
             gpu.size() - gpu_listed);
    FinishDispatchReport(frame);
}

// BufferCache::TickFrame, after TextureCache::TickFrame (AliasTrace::EndFrame) in the same
// frame boundary: start collecting when F7 has just armed the texture report for the next frame.
inline void Follow(u64 frame) {
    static const auto boot = std::chrono::steady_clock::now();
    if (frame_seconds.size() <= frame) {
        frame_seconds.resize(static_cast<size_t>(frame) + 1, 0.0f);
    }
    frame_seconds[frame] =
        std::chrono::duration<float>(std::chrono::steady_clock::now() - boot).count();
    current_frame = frame;
    collecting = AliasTrace::collecting;
    ranges.clear();
    ranges_dropped = 0;
    fast_binds = 0;
    fast_bytes = 0;
    fast_align_binds = 0;
    report = Report{};
    dispatches.clear();
    dispatch_writes.clear();
    dispatches_dropped = 0;
    dispatch_writes_dropped = 0;
    dispatch_open = false;
    draw_desc_reads = 0;
    draw_desc_gpu = 0;
    if (collecting) {
        report.number = AliasTrace::report.number;
    }
}

} // namespace VideoCommon::BufTrace
#endif
