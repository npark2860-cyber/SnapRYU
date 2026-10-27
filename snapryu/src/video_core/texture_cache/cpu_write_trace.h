// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// CPUW_TRACE (b71): who writes the memory a zero terrain tile is made from (TotK NCE, 2026-10-19).
//
// b70 (runs\B70-TIC-*): every zero tile is sampled through one TIC entry the game wrote itself
// (TIC_GAME 3/3), and one of them (id 321, cpu 0xf6e1000) sits inside a tile baked 24 frames before
// (id 1804 at +0x16000). Three frames before 321 was made, stores began exactly at 0xf6e1000 and
// went through 1804 and its mips (TILE_CPUW source=other, frame 1685). Those were NCE stores on the
// write-back path (GPU_WRITEBACK addr=0xf6e1000 in the same run), whose note was lost on the GPU
// thread. Under NCE the game reads guest memory natively: a read of a tile whose content only the
// GPU has sees zeros (JIT asks the GPU first, Memory::HandleRasterizerDownload). A CPU copy of a
// baked tile would then copy zeros (hypothesis; not shown yet).
//
// This file keeps, with the store's guest state (common/nce_write_note.h):
//   pages  the last PAGE_SLOTS noted NCE stores per device page (GPU::InvalidateRegion,
//          CPU thread), also pages no tile image covers;
//   tiles  a ring of every cpu_modified on a tile image (TileTrace::CpuWrite, texture cache lock),
//          with what x0..x7 pointed at then (a tile image, its offset, whether it held GPU content)
//          and the zero permille of the written bytes before the store and at the end of the frame.
// At F7 each zero tile lists the writes into its memory (CPUW_TILE, CPUW_PAGE, CPUW_REG).
//
// Register targets are device addresses estimated as page + (x - fault page): exact only when
// x lies in the same contiguous mapping as the fault address (contig=1 says the host pointers agree).
//
// Diagnostic builds only; release builds compile none of this.
#pragma once

#if SNAPRYU_DIAGNOSTICS
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <ankerl/unordered_dense.h>
#include <fmt/format.h>
#include "common/common_types.h"
#include "common/logging.h"
#include "common/nce_write_note.h"

namespace VideoCommon::CpuWriteTrace {

using Common::NceWriteNote::Note;
using Common::NceWriteNote::REGS;

inline constexpr size_t PAGE_SLOTS = 2;
inline constexpr size_t MAX_PAGES = size_t{1} << 18;
inline constexpr size_t TILE_RING = size_t{1} << 12;
inline constexpr size_t MAX_TILE_LISTED = 16;
inline constexpr size_t MAX_PAGE_LISTED = 24;
inline constexpr u64 PAGE = 0x1000;
inline constexpr u64 NEAR_VA = u64{1} << 28; // estimate register targets within 256 MiB only
inline constexpr u16 NOT_SAMPLED = 0xffff;
inline constexpr u32 NO_IMAGE = 0xffffffff;

struct Write {
    Note note{};
    u64 page = 0;  // device address of the invalidated page
    u64 frame = 0; // texture cache frame_tick (pages: the last one EndFrame set)
};

struct PageWrites {
    std::array<Write, PAGE_SLOTS> slots{};
    u64 count = 0;
};

// What a register pointed at when the store ran.
struct Hit {
    u32 image = NO_IMAGE;
    u32 offset = 0;
    u32 width = 0;
    u32 height = 0;
    bool gpu = false;  // the image held GPU content
    bool dead = false; // graveyard record
};

struct TileWrite {
    Write write{};
    u32 image = NO_IMAGE;
    u64 image_gpu = 0;
    u64 image_cpu = 0;
    u32 image_size = 0;
    u32 size = 0; // invalidated bytes
    bool gpu_content = false;
    u16 before_zero = NOT_SAMPLED; // zero permille of image bytes in the page, before the store
    u16 after_zero = NOT_SAMPLED;  // ... at the end of that frame
    std::array<Hit, REGS> hits{};
};

// pages: CPU threads (GPU::InvalidateRegion; empty notes, as in the exception handler, are skipped)
inline std::mutex mutex;
inline std::atomic<u64> frame{0};
inline ankerl::unordered_dense::map<u64, PageWrites>& pages =
    *new ankerl::unordered_dense::map<u64, PageWrites>();
inline u64 page_notes = 0;
inline u64 page_dropped = 0;
// tiles: under the texture cache mutex (TileTrace)
inline std::array<TileWrite, TILE_RING>& tiles = *new std::array<TileWrite, TILE_RING>();
inline u64 tile_writes = 0;
inline u64 tile_nce = 0;
inline u64 tile_carried = 0;
inline u64 tile_other = 0;
inline u64 sampled = 0; // tiles[] entries before this index have after_zero set (or were skipped)

inline bool IsNce(const Note& note, u64 page, u64 size) {
    return note.valid && size == PAGE && (page & (PAGE - 1)) == 0;
}

// A noted NCE store is about to change device page `page`.
inline void NotePage(u64 page, u64 size, const Note& note) {
    if (!IsNce(note, page, size)) {
        return;
    }
    std::scoped_lock lock{mutex};
    ++page_notes;
    auto it = pages.find(page);
    if (it == pages.end()) {
        if (pages.size() >= MAX_PAGES) {
            ++page_dropped;
            return;
        }
        it = pages.emplace(page, PageWrites{}).first;
    }
    PageWrites& p = it->second;
    p.slots[p.count % PAGE_SLOTS] = Write{note, page, frame.load(std::memory_order_relaxed)};
    ++p.count;
}

// TileTrace::CpuWrite: the texture cache marks a tile image CpuModified for [page, page + size).
inline TileWrite& NoteTile(u32 image, u64 image_gpu, u64 image_cpu, u32 image_size,
                           bool gpu_content, u64 page, u64 size, const Note& note, u64 now) {
    TileWrite& t = tiles[tile_writes % TILE_RING];
    ++tile_writes;
    t = TileWrite{};
    t.write = Write{note, page, now};
    t.image = image;
    t.image_gpu = image_gpu;
    t.image_cpu = image_cpu;
    t.image_size = image_size;
    t.size = static_cast<u32>(size);
    t.gpu_content = gpu_content;
    if (!IsNce(note, page, size)) {
        ++tile_other;
    } else if (note.carried) {
        ++tile_carried;
    } else {
        ++tile_nce;
    }
    return t;
}

// Device bytes of the image inside the written range, as a GPU range.
inline bool ImageSpan(const TileWrite& t, u64& gpu, u32& bytes) {
    const u64 begin = (std::max)(t.write.page, t.image_cpu);
    const u64 end = (std::min)(t.write.page + t.size, t.image_cpu + t.image_size);
    if (begin >= end) {
        return false;
    }
    gpu = t.image_gpu + (begin - t.image_cpu);
    bytes = static_cast<u32>(end - begin);
    return true;
}

// End of frame `now`: the zero permille of the written image bytes of earlier frames' writes.
template <typename ZeroPermille>
inline void SampleAfter(u64 now, ZeroPermille&& zero_permille) {
    if (tile_writes - sampled > TILE_RING) {
        sampled = tile_writes - TILE_RING;
    }
    for (; sampled < tile_writes; ++sampled) {
        TileWrite& t = tiles[sampled % TILE_RING];
        if (t.write.frame >= now) {
            break;
        }
        u64 gpu = 0;
        u32 bytes = 0;
        if (ImageSpan(t, gpu, bytes)) {
            t.after_zero = static_cast<u16>(zero_permille(gpu, bytes));
        }
    }
}

inline std::string Hex(u64 value) {
    return fmt::format("{:#x}", value);
}

inline std::string NoteText(const Note& note) {
    if (!note.valid) {
        return "note=none";
    }
    std::string text = fmt::format(
        "pc={:#x} insn={:08x} fault={:#x} carried={} seq={} thread={} lr={:#x} fp={:#x} sp={:#x} "
        "store={} data={:#x}:{:#x}:{:#x}:{:#x} dst_zeros={}/{}",
        note.pc, note.insn, note.fault, note.carried ? 1 : 0, note.seq, note.thread, note.lr,
        note.fp, note.sp, Common::NceWriteNote::StoreName(note.store), note.data[0], note.data[1],
        note.data[2], note.data[3], note.dst_zeros, note.dst_bytes);
    for (size_t i = 0; i < REGS; ++i) {
        text += fmt::format(" x{}={:#x}:{}/{}:{}", i, note.x[i], note.zeros[i], note.bytes[i],
                            note.contig[i]);
    }
    text += fmt::format(" frames={:#x},{:#x},{:#x},{:#x}", note.frames[0], note.frames[1],
                        note.frames[2], note.frames[3]);
    return text;
}

// Device address a register pointed at, estimated from the fault (false: not estimated).
inline bool Estimate(const Note& note, u64 page, size_t reg, u64& dev) {
    if (!note.valid || !note.filled || note.contig[reg] == 0) {
        return false;
    }
    const u64 fault_page = note.fault & ~(PAGE - 1);
    const u64 x = note.x[reg];
    const u64 distance = x > fault_page ? x - fault_page : fault_page - x;
    if (distance >= NEAR_VA) {
        return false;
    }
    dev = page + (x - fault_page);
    return true;
}

inline std::string HitText(const Hit& hit) {
    if (hit.image == NO_IMAGE) {
        return "-";
    }
    return fmt::format("img{}@+{:#x}:{}x{}:gpu{}{}", hit.image, hit.offset, hit.width,
                       hit.height, hit.gpu ? 1 : 0, hit.dead ? ":dead" : "");
}

inline bool Overlaps(u64 a, u64 a_size, u64 b, u64 b_size) {
    return a < b + b_size && b < a + a_size;
}

// F7: the writes into one zero tile's memory [cpu, cpu + size). find(dev) -> Hit is TileTrace's
// record lookup (now), zero_permille(gpu, bytes) reads guest memory now. Under the texture cache
// mutex.
template <typename Find, typename ZeroPermille, typename AgeText>
inline void ListFor(u32 id, u64 cpu, u64 size, u64 now, Find&& find,
                    ZeroPermille&& zero_permille, AgeText&& age, u32 number) {
    if (cpu == 0 || size == 0) {
        LOG_INFO(Render_Vulkan, "CPUW_TILE end id={} tile_writes=0 page_writes=0 no_cpu n={}", id,
                 number);
        return;
    }
    const auto log_regs = [&](const char* kind, const Note& note, u64 page,
                              const std::array<Hit, REGS>* hits) {
        for (size_t i = 0; i < REGS; ++i) {
            u64 dev = 0;
            if (!Estimate(note, page, i, dev)) {
                continue;
            }
            const Hit hit = hits ? (*hits)[i] : find(dev);
            const bool in_zero = dev >= cpu && dev < cpu + size;
            if (hit.image == NO_IMAGE && !in_zero) {
                continue;
            }
            LOG_INFO(Render_Vulkan,
                     "CPUW_REG id={} from={} seq={} reg=x{} va={:#x} dev={:#x} zero_off={} "
                     "zeros={}/{} hit={} when={} n={}",
                     id, kind, note.seq, i, note.x[i], dev,
                     in_zero ? Hex(dev - cpu) : std::string{"-"}, note.zeros[i], note.bytes[i],
                     HitText(hit), hits ? "write" : "now", number);
        }
    };
    // tile image writes over the zero tile's memory, oldest first
    size_t tile_hits = 0;
    size_t tile_listed = 0;
    const u64 first = tile_writes > TILE_RING ? tile_writes - TILE_RING : 0;
    for (u64 i = first; i < tile_writes; ++i) {
        const TileWrite& t = tiles[i % TILE_RING];
        if (!Overlaps(t.write.page, t.size, cpu, size)) {
            continue;
        }
        ++tile_hits;
        if (tile_listed == MAX_TILE_LISTED) {
            continue;
        }
        ++tile_listed;
        u64 gpu = 0;
        u32 bytes = 0;
        const u32 now_zero = ImageSpan(t, gpu, bytes) ? zero_permille(gpu, bytes) : NOT_SAMPLED;
        const Note& note = t.write.note;
        const char* const source = !IsNce(note, t.write.page, t.size) ? "other"
                                   : note.carried                    ? "carried"
                                                                     : "nce";
        LOG_INFO(Render_Vulkan,
                 "CPUW_TILE id={} #{} frame={} age_s={} image={} image_gpu={:#x} image_cpu={:#x} "
                 "image_size={:#x} page={:#x} zero_off={} gpu_content={} source={} "
                 "before_zero={} after_zero={} now_zero={} {} n={}",
                 id, i, t.write.frame, age(now, t.write.frame), t.image, t.image_gpu,
                 t.image_cpu, t.image_size, t.write.page,
                 t.write.page >= cpu ? Hex(t.write.page - cpu)
                                     : "-" + Hex(cpu - t.write.page),
                 t.gpu_content ? 1 : 0, source, t.before_zero, t.after_zero, now_zero,
                 NoteText(note), number);
        log_regs("tile", note, t.write.page, &t.hits);
    }
    // noted NCE stores per page (also pages no tile image covered)
    size_t page_hits = 0;
    size_t page_listed = 0;
    size_t pages_with = 0;
    {
        std::scoped_lock lock{mutex};
        for (u64 page = cpu & ~(PAGE - 1); page < cpu + size; page += PAGE) {
            const auto it = pages.find(page);
            if (it == pages.end()) {
                continue;
            }
            ++pages_with;
            const PageWrites& p = it->second;
            const u64 kept = (std::min<u64>)(p.count, PAGE_SLOTS);
            for (u64 k = p.count - kept; k < p.count; ++k) {
                const Write& w = p.slots[k % PAGE_SLOTS];
                ++page_hits;
                if (page_listed == MAX_PAGE_LISTED) {
                    continue;
                }
                ++page_listed;
                LOG_INFO(Render_Vulkan,
                         "CPUW_PAGE id={} page={:#x} zero_off={} write={}/{} frame={} age_s={} {} "
                         "n={}",
                         id, page,
                         page >= cpu ? Hex(page - cpu) : "-" + Hex(cpu - page), k + 1, p.count,
                         w.frame, age(now, w.frame), NoteText(w.note), number);
                log_regs("page", w.note, page, nullptr);
            }
        }
    }
    LOG_INFO(Render_Vulkan,
             "CPUW_TILE end id={} tile_writes={} tile_listed={} pages_with_writes={} "
             "page_writes={} page_listed={} n={}",
             id, tile_hits, tile_listed, pages_with, page_hits, page_listed, number);
}

inline void Summary(u32 number) {
    size_t page_count = 0;
    u64 notes = 0;
    u64 dropped = 0;
    {
        std::scoped_lock lock{mutex};
        page_count = pages.size();
        notes = page_notes;
        dropped = page_dropped;
    }
    LOG_INFO(Render_Vulkan,
             "CPUW_TRACE n={} page_notes={} pages={} page_dropped={} tile_writes={} tile_nce={} "
             "tile_carried={} tile_other={} ring={} notes_filled={}",
             number, notes, page_count, dropped, tile_writes, tile_nce, tile_carried, tile_other,
             TILE_RING, Common::NceWriteNote::sequence.load(std::memory_order_relaxed));
}

} // namespace VideoCommon::CpuWriteTrace
#endif
