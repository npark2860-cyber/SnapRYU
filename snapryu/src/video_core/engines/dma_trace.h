// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// DMA_TRACE (b68, 2026-10-19): which DMA copies wrote the memory of a terrain tile that reads back
// empty?
//
// b67 (runs\B67-READALL-20261019-014438-592): after a save was reloaded, 22-29 of the 30-38
// terrain tiles sampled at F7 were empty on the GPU and in guest memory, and no GPU write reached
// them after their creation. Each sits exactly 0x4000 below a tile the GPU baked a second earlier
// (0x648008000 sampled, 0x64800c000 baked; the same for seven pairs). A baked tile stays in the
// texture cache; guest memory gets it only when the cache downloads it. A copy that reads the
// baked tile from guest memory before that reads zeros. BufferCache::DMACopy, which takes the 1D
// pitch DMA copy when the buffer cache knows the source or the destination, copies guest memory
// and buffers without asking the texture cache. The other DMA paths read with SafeRead, which
// downloads first.
//
// Every DMA launch whose source or destination spans at least MIN_BYTES is kept in a ring:
// frame, source and destination (GPU and device addresses, sizes), the path MaxwellDMA::Launch
// takes, and for the 1D buffer copy whether the texture cache held GPU content in the source and
// in the destination, and whether the buffer cache takes the copy (the same test DMACopy makes).
// A buffer copy whose source the texture cache held is also sampled after the copy: zero permille
// of its source and destination in guest memory.
// At F7 every zero tile tile_trace.h lists lists the ring entries that wrote into its memory or
// read from it (DMA_TILE), and buffer_cache.h reports the buffer cache state of every zero tile's
// range (TILE_BUF). DMA_TRACE gives the totals since boot.
//
// Diagnostic builds only; release builds compile none of this. GPU thread (MaxwellDMA::Launch,
// AccelerateDMA, the cache TickFrames), behind one mutex.
#pragma once

#if SNAPRYU_DIAGNOSTICS
#include <algorithm>
#include <array>
#include <mutex>
#include <optional>
#include <vector>
#include "common/common_types.h"
#include "common/logging.h"

namespace VideoCommon::DmaTrace {

enum class Path : u8 {
    Clear,          // 1D, remap to constant A: BufferClear + write
    Buffer,         // 1D pitch to pitch: AccelerateDMA::BufferCopy, else a SafeRead copy
    BlockToPitch1D, // 1D, 16-byte swizzled reads (SafeRead)
    PitchToBlock1D,
    PitchToPitch,   // multi-line, CopyBlock per line
    BlockToPitch,   // multi-line, ImageToBuffer, else SafeRead + unswizzle
    PitchToBlock,   // multi-line, BufferToImage, else SafeRead + swizzle
    BlockToBlock,   // multi-line, SafeRead + unswizzle + swizzle
    Count,
};

inline const char* PathName(Path path) {
    static constexpr std::array<const char*, static_cast<size_t>(Path::Count)> names{
        "clear",          "buffer",         "block_to_pitch_1d", "pitch_to_block_1d",
        "pitch_to_pitch", "block_to_pitch", "pitch_to_block",    "block_to_block"};
    const size_t index = static_cast<size_t>(path);
    return index < names.size() ? names[index] : "?";
}

// tex_src / tex_dst / buffer: 0 not checked, 1 no, 2 yes.
struct Entry {
    u64 seq = 0;
    u64 frame = 0;
    u64 src_gpu = 0;
    u64 dst_gpu = 0;
    u64 src_dev = 0; // 0: unmapped
    u64 dst_dev = 0;
    u32 src_size = 0; // 0: no source (clear)
    u32 dst_size = 0;
    u16 src_zero = NOT_SAMPLED; // permille, after the copy
    u16 dst_zero = NOT_SAMPLED;
    Path path = Path::Buffer;
    u8 tex_src = 0;
    u8 tex_dst = 0;
    u8 buffer = 0;

    static constexpr u16 NOT_SAMPLED = 0xffff;
};

struct ZeroRange {
    u32 id = 0;
    u32 number = 0;
    u64 gpu = 0;
    u64 dev = 0;
    u64 size = 0;
};

inline constexpr size_t RING = size_t{1} << 19;
inline constexpr u64 MIN_BYTES = 0x100;
inline constexpr u64 MAX_SAMPLE = u64{1} << 20;
inline constexpr size_t MAX_DST_LISTED = 12;
inline constexpr size_t MAX_SRC_LISTED = 6;
inline constexpr size_t MAX_STALE_LISTED = 8;

inline std::mutex mutex;
inline std::vector<Entry>& ring = *new std::vector<Entry>(RING);
inline std::vector<ZeroRange>& zero_ranges = *new std::vector<ZeroRange>();
inline size_t head = 0;  // next slot
inline u64 kept = 0;     // entries ever written to the ring
inline u64 launches = 0;
inline u64 small_launches = 0; // under MIN_BYTES, not kept
inline std::array<u64, static_cast<size_t>(Path::Count)> path_counts{};
inline u64 buffer_taken = 0;     // BufferCopy taken by the buffer cache
inline u64 buffer_refused = 0;   // fell back to the SafeRead copy
inline u64 taken_tex_src = 0;    // taken while the texture cache held GPU content in the source
inline u64 taken_tex_dst = 0;    // taken while the texture cache held GPU content in the destination
inline u64 taken_tex_src_zero_dst = 0; // ... and the destination read back zero after the copy
inline u64 frame = 0;            // texture cache frame_tick, set by TileTrace::EndFrame
inline Entry current{};
inline bool open = false;

inline bool Overlaps(u64 addr, u64 size, u64 range, u64 range_size) {
    return size != 0 && range_size != 0 && addr < range + range_size && range < addr + size;
}

// MaxwellDMA::Launch, before it copies.
inline void Begin(Path path, u64 src_gpu, u64 src_size, std::optional<u64> src_dev, u64 dst_gpu,
                  u64 dst_size, std::optional<u64> dst_dev) {
    std::scoped_lock lock{mutex};
    ++launches;
    ++path_counts[static_cast<size_t>(path)];
    current = Entry{};
    current.frame = frame;
    current.path = path;
    current.src_gpu = src_gpu;
    current.dst_gpu = dst_gpu;
    current.src_size = static_cast<u32>((std::min)(src_size, u64{0xffffffff}));
    current.dst_size = static_cast<u32>((std::min)(dst_size, u64{0xffffffff}));
    current.src_dev = src_dev.value_or(0);
    current.dst_dev = dst_dev.value_or(0);
    open = true;
}

// AccelerateDMA::BufferCopy, before BufferCache::DMACopy: the texture cache state of both ranges
// and the buffer cache's own test (both addresses mapped, source or destination registered).
template <typename TextureCache, typename BufferCache>
inline void BeforeBufferCopy(TextureCache& texture_cache, BufferCache& buffer_cache, u64 amount) {
    u64 src = 0;
    u64 dst = 0;
    {
        std::scoped_lock lock{mutex};
        if (!open) {
            return;
        }
        src = current.src_dev;
        dst = current.dst_dev;
    }
    u8 tex_src = 0;
    u8 tex_dst = 0;
    u8 buffer = 1;
    if (src != 0 && dst != 0) {
        {
            std::scoped_lock lock{texture_cache.mutex};
            tex_src = texture_cache.IsRegionGpuModified(src, amount) ? 2 : 1;
            tex_dst = texture_cache.IsRegionGpuModified(dst, amount) ? 2 : 1;
        }
        std::scoped_lock lock{buffer_cache.mutex};
        buffer = buffer_cache.IsRegionRegistered(src, amount) ||
                         buffer_cache.IsRegionRegistered(dst, amount)
                     ? 2
                     : 1;
    }
    std::scoped_lock lock{mutex};
    current.tex_src = tex_src;
    current.tex_dst = tex_dst;
    current.buffer = buffer;
    if (buffer == 2) {
        ++buffer_taken;
        taken_tex_src += tex_src == 2 ? 1 : 0;
        taken_tex_dst += tex_dst == 2 ? 1 : 0;
    } else {
        ++buffer_refused;
    }
}

// Zero permille of guest memory, page by page; an unmapped page counts as zeros.
template <typename GpuMemory>
inline u16 ZeroPermille(const GpuMemory& gpu_memory, u64 gpu_addr, u64 size) {
    constexpr u64 PAGE = 4096;
    if (size == 0) {
        return Entry::NOT_SAMPLED;
    }
    u64 zeros = 0;
    for (u64 done = 0; done < size;) {
        const u64 addr = gpu_addr + done;
        const u64 chunk = (std::min)(PAGE - (addr & (PAGE - 1)), size - done);
        const u8* const bytes = gpu_memory.GetPointer(addr);
        zeros += bytes ? static_cast<u64>(std::count(bytes, bytes + chunk, u8{0})) : chunk;
        done += chunk;
    }
    return static_cast<u16>(zeros * 1000 / size);
}

// MaxwellDMA::Launch, after it copied.
template <typename GpuMemory>
inline void End(const GpuMemory& gpu_memory) {
    std::scoped_lock lock{mutex};
    if (!open) {
        return;
    }
    open = false;
    Entry& e = current;
    if (e.buffer == 2 && e.tex_src == 2) {
        e.src_zero = ZeroPermille(gpu_memory, e.src_gpu, (std::min)(u64{e.src_size}, MAX_SAMPLE));
        e.dst_zero = ZeroPermille(gpu_memory, e.dst_gpu, (std::min)(u64{e.dst_size}, MAX_SAMPLE));
        taken_tex_src_zero_dst += e.dst_zero >= 999 ? 1 : 0;
    }
    if ((std::max)(e.src_size, e.dst_size) < MIN_BYTES && e.tex_src != 2) {
        ++small_launches;
        return;
    }
    e.seq = ++kept;
    ring[head] = e;
    head = (head + 1) % RING;
}

template <typename Func>
inline void ForEachNewest(Func&& func) {
    const size_t count = static_cast<size_t>((std::min)(kept, u64{RING}));
    for (size_t i = 0; i < count; ++i) {
        const Entry& e = ring[(head + RING - 1 - i) % RING];
        if (!func(e)) {
            return;
        }
    }
}

inline const char* Tri(u8 value) {
    return value == 2 ? "yes" : value == 1 ? "no" : "-";
}

template <typename AgeText>
inline void LogEntry(const char* tag, const Entry& e, u64 now, AgeText&& age, u32 id, u32 number) {
    LOG_INFO(Render_Vulkan,
             "{} id={} #{} frame={} age_s={} path={} src={:#x}+{:#x} src_dev={:#x} "
             "dst={:#x}+{:#x} dst_dev={:#x} tex_src_gpu={} tex_dst_gpu={} buffer_took={} "
             "src_zero_permille={} dst_zero_permille={} n={}",
             tag, id, e.seq, e.frame, age(now, e.frame), PathName(e.path), e.src_gpu, e.src_size,
             e.src_dev, e.dst_gpu, e.dst_size, e.dst_dev, Tri(e.tex_src), Tri(e.tex_dst),
             Tri(e.buffer), e.src_zero == Entry::NOT_SAMPLED ? -1 : static_cast<int>(e.src_zero),
             e.dst_zero == Entry::NOT_SAMPLED ? -1 : static_cast<int>(e.dst_zero), number);
}

// F7, for a listed zero tile: the newest ring entries that wrote into its memory or read from it.
template <typename AgeText>
inline void ListFor(u32 id, u64 gpu, u64 size, u64 now, AgeText&& age, u32 number) {
    std::scoped_lock lock{mutex};
    size_t dst_hits = 0;
    size_t src_hits = 0;
    size_t dst_listed = 0;
    size_t src_listed = 0;
    ForEachNewest([&](const Entry& e) {
        if (Overlaps(e.dst_gpu, e.dst_size, gpu, size)) {
            ++dst_hits;
            if (dst_listed < MAX_DST_LISTED) {
                ++dst_listed;
                LogEntry("DMA_TILE hit=dst", e, now, age, id, number);
            }
        }
        if (Overlaps(e.src_gpu, e.src_size, gpu, size)) {
            ++src_hits;
            if (src_listed < MAX_SRC_LISTED) {
                ++src_listed;
                LogEntry("DMA_TILE hit=src", e, now, age, id, number);
            }
        }
        return true;
    });
    LOG_INFO(Render_Vulkan, "DMA_TILE end id={} dst_hits={} src_hits={} n={}", id, dst_hits,
             src_hits, number);
}

// F7, every zero tile: counted here, its range kept for buffer_cache.h (TILE_BUF).
struct TileCount {
    size_t tiles = 0;
    size_t with_dst = 0;
    size_t with_dst_taken_tex_src = 0; // a buffer copy the buffer cache took from texture GPU content
    size_t with_src = 0;
};

inline void CountFor(TileCount& count, u32 id, u64 gpu, u64 dev, u64 size, u32 number) {
    std::scoped_lock lock{mutex};
    ++count.tiles;
    bool dst = false;
    bool stale = false;
    bool src = false;
    ForEachNewest([&](const Entry& e) {
        if (Overlaps(e.dst_gpu, e.dst_size, gpu, size)) {
            dst = true;
            stale = stale || (e.buffer == 2 && e.tex_src == 2);
        }
        src = src || Overlaps(e.src_gpu, e.src_size, gpu, size);
        return true;
    });
    count.with_dst += dst ? 1 : 0;
    count.with_dst_taken_tex_src += stale ? 1 : 0;
    count.with_src += src ? 1 : 0;
    zero_ranges.push_back(ZeroRange{id, number, gpu, dev, size});
}

template <typename AgeText>
inline void Summary(const TileCount& count, u64 now, AgeText&& age, u32 number) {
    std::scoped_lock lock{mutex};
    u64 oldest = 0;
    if (kept != 0) {
        oldest = ring[kept >= RING ? head : 0].frame;
    }
    LOG_INFO(Render_Vulkan,
             "DMA_TRACE n={} launches={} kept={} small={} ring={} oldest_frame={} oldest_age_s={} "
             "clear={} buffer={} block_to_pitch_1d={} pitch_to_block_1d={} pitch_to_pitch={} "
             "block_to_pitch={} pitch_to_block={} block_to_block={} buffer_taken={} "
             "buffer_refused={} taken_tex_src_gpu={} taken_tex_dst_gpu={} "
             "taken_tex_src_gpu_dst_zero={} zero_tiles={} zero_tiles_dma_dst={} "
             "zero_tiles_dma_dst_taken_tex_src_gpu={} zero_tiles_dma_src={}",
             number, launches, kept, small_launches, RING, oldest, age(now, oldest), path_counts[0],
             path_counts[1], path_counts[2], path_counts[3], path_counts[4], path_counts[5],
             path_counts[6], path_counts[7], buffer_taken, buffer_refused, taken_tex_src,
             taken_tex_dst, taken_tex_src_zero_dst, count.tiles, count.with_dst,
             count.with_dst_taken_tex_src, count.with_src);
    size_t listed = 0;
    ForEachNewest([&](const Entry& e) {
        if (e.buffer == 2 && e.tex_src == 2) {
            LogEntry("DMA_STALE", e, now, age, 0, number);
            ++listed;
        }
        return listed < MAX_STALE_LISTED;
    });
}

// buffer_cache.h, in the TickFrame after the F7 report (the texture cache ticks first).
inline std::vector<ZeroRange> TakeZeroRanges() {
    std::scoped_lock lock{mutex};
    std::vector<ZeroRange> taken;
    taken.swap(zero_ranges);
    return taken;
}

} // namespace VideoCommon::DmaTrace
#endif
