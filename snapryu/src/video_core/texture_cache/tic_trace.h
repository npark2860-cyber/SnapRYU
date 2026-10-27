// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// TIC_TRACE (b70, 2026-10-19): which texture descriptor makes the game sample a terrain tile that
// reads back empty, what that descriptor held over time, and who wrote it?
//
// b69 (runs\B69-DRAW-20261019-052745-257): no draw or clear ever bound a render or depth target at
// the address of an empty tile (2.2-3.0M draws, 2454-2742 targets, none dropped); the targets
// inside an empty tile's memory are a neighbouring tile's mips. The empty tiles sit off the grid
// the game bakes on (0x651799000 sampled, mip 0 baked at 0x65177f000), some with no target within
// 0x40000. The texture cache makes the sampled image from the TIC entry the shader reads
// (VisitImageView: DescriptorTable::Read reads guest memory; CreateImageView: image address = TIC
// address - base layer offset). So the TIC entry in guest memory points where nothing was baked.
// Either the game wrote that address there, or guest memory holds an older entry because the newer
// one went into a buffer the buffer cache has not written back (a DMA copy BufferCache::DMACopy
// took, a shader store), or the bake went to another GPU address mapped to the same memory.
//
// Per TIC slot (table address + index * 32, graphics or compute) that ever gave a tile-format image:
// the distinct entries read there (up to MAX_VALUES: raw words, the image, first and last frame,
// reads) and the number of changes; once a slot is known, every new entry read there is noted,
// whatever image it gives. Every DMA launch and inline-to-memory write into a TIC table seen so far
// is kept in a ring: frame, path, range, whether the buffer cache took the copy, and the TIC address
// guest memory holds at the destination and at the source right after the write.
// F7, for each zero tile and control tile tile_trace.h lists:
//   TIC_SLOT   each slot that gave this image: table, index, the entry read last and the one guest
//              memory holds now
//   TIC_VALUE  the slot's entry history
//   TIC_NEAR   the entries at index -4..+4 now: address, the tile image there and its readback
//   TIC_WRITE  the ring entries that wrote the slot
//   TIC_CPU    draw targets whose memory (device address now) lies in the tile's memory under
//              another GPU address
// and each slot's range goes to TILE_BUF (buffer cache state, id = 0x40000000 + tile id).
// TIC_TRACE gives the totals.
//
// Diagnostic builds only; release builds compile none of this. GPU thread (VisitImageView under
// the texture cache mutex, MaxwellDMA::Launch, AccelerateInlineToMemory, the texture cache
// TickFrame), behind one mutex; taken before DmaTrace::mutex and DrawTargetTrace::mutex, never
// while holding them.
#pragma once

#if SNAPRYU_DIAGNOSTICS
#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <fmt/format.h>
#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/engines/dma_trace.h"
#include "video_core/engines/draw_target_trace.h"
#include "video_core/memory_manager.h"

namespace VideoCommon::TicTrace {

using Raw = std::array<u64, 4>;

inline constexpr u64 ENTRY_BYTES = 32; // sizeof(Tegra::Texture::TICEntry)
inline constexpr size_t MAX_VALUES = 6;
inline constexpr size_t MAX_SLOTS = size_t{1} << 14;
inline constexpr size_t MAX_TABLES = 16;
inline constexpr size_t WRITE_RING = size_t{1} << 14;
inline constexpr size_t MAX_SLOTS_LISTED = 4;
inline constexpr size_t MAX_WRITES_LISTED = 8;
inline constexpr size_t MAX_CPU_LISTED = 8;
inline constexpr u32 NEIGHBOURS = 4;
inline constexpr u32 SLOT_RANGE_ID = 0x40000000;
inline constexpr u32 NO_IMAGE = 0xffffffff;

// TICEntry::Address(): address_low is bits 32..63 of word 0, address_high bits 0..15 of word 1.
inline u64 Address(const Raw& raw) {
    return ((raw[1] & 0xffff) << 32) | (raw[0] >> 32);
}

inline std::string RawText(const Raw& raw) {
    return fmt::format("{:016x}:{:016x}:{:016x}:{:016x}", raw[0], raw[1], raw[2], raw[3]);
}

inline std::string Offset(u64 addr, u64 base) {
    return addr >= base ? fmt::format("+{:#x}", addr - base) : fmt::format("-{:#x}", base - addr);
}

// One entry as guest memory holds it now (TIC entries are 32-byte aligned: never across a page).
inline bool ReadNow(const Tegra::MemoryManager* memory, u64 gpu, Raw& raw) {
    raw = Raw{};
    const u8* const bytes = memory ? memory->GetPointer(gpu) : nullptr;
    if (!bytes) {
        return false;
    }
    std::memcpy(raw.data(), bytes, sizeof(Raw));
    return true;
}

struct Value {
    Raw raw{};
    u32 image = NO_IMAGE; // index of the image the view gave (NO_IMAGE: null view or buffer)
    u64 image_gpu = 0;
    bool tile = false;    // a tile-format image
    u64 first_frame = 0;
    u64 last_frame = 0;
    u64 reads = 0;
};

struct Slot {
    u64 gpu = 0;
    u64 table = 0;
    u32 index = 0;
    u32 limit = 0;
    bool compute = false;
    const Tegra::MemoryManager* memory = nullptr;
    std::array<Value, MAX_VALUES> values{};
    size_t used = 0;
    size_t current = 0;
    u64 changes = 0;        // reads that found another entry than the read before
    u64 dropped_values = 0; // distinct entries beyond MAX_VALUES (the oldest after the first go)
    u64 first_frame = 0;
};

struct Table {
    u64 base = 0;
    u32 limit = 0;
    bool compute = false;
    u64 first_frame = 0;
    u64 last_frame = 0;
};

enum class Source : u8 { Dma, Inline };

struct Write {
    u64 seq = 0;
    u64 frame = 0;
    u64 dst = 0;
    u64 src = 0;
    u32 dst_size = 0;
    u32 src_size = 0;
    Source source = Source::Dma;
    u8 path = 0;    // DmaTrace::Path
    u8 buffer = 0;  // DmaTrace tri-state: did the buffer cache take the copy
    u8 tex_src = 0; // ... did the texture cache hold GPU content in the source
    bool dst_read = false;
    bool src_read = false;
    u64 slot = 0;     // first whole entry the write covers
    u64 dst_addr = 0; // TIC address guest memory holds there right after the write
    u64 src_addr = 0; // ... and at the matching source entry
};

inline std::mutex mutex;
inline std::unordered_map<u64, Slot>& slots = *new std::unordered_map<u64, Slot>();
inline std::vector<Table>& tables = *new std::vector<Table>();
inline std::vector<Write>& writes = *new std::vector<Write>(WRITE_RING);
inline size_t head = 0;
inline u64 kept = 0;
inline u64 notes = 0;
inline u64 new_reads = 0;
inline u64 changes = 0;
inline u64 dropped_slots = 0;
inline u64 dropped_tables = 0;
inline u64 writes_dma = 0;
inline u64 writes_inline = 0;
inline u64 writes_buffer_taken = 0;
inline u64 frame = 0; // texture cache frame_tick, set by TileTrace::EndFrame
inline const Tegra::MemoryManager* last_memory = nullptr;

inline u64 Key(u64 gpu, bool compute) {
    return (gpu << 1) | (compute ? 1U : 0U);
}

inline bool Overlaps(u64 addr, u64 size, u64 range, u64 range_size) {
    return size != 0 && range_size != 0 && addr < range + range_size && range < addr + size;
}

inline void SeeTable(u64 base, u32 limit, bool compute, u64 now) {
    for (Table& t : tables) {
        if (t.base == base && t.compute == compute) {
            t.limit = (std::max)(t.limit, limit);
            t.last_frame = now;
            return;
        }
    }
    if (tables.size() >= MAX_TABLES) {
        ++dropped_tables;
        return;
    }
    tables.push_back(Table{base, limit, compute, now, now});
}

// VisitImageView, after the view is known: `tile` when it gives a tile-format image, `is_new`
// when DescriptorTable::Read found a new entry. Only tile reads and new entries are noted.
inline void Note(const Tegra::MemoryManager& memory, u64 table, u32 limit, u32 index, bool compute,
                 const Raw& raw, bool is_new, bool tile, u32 image, u64 image_gpu, u64 now) {
    if (!tile && !is_new) {
        return;
    }
    const u64 gpu = table + u64{index} * ENTRY_BYTES;
    std::scoped_lock lock{mutex};
    ++notes;
    new_reads += is_new ? 1 : 0;
    last_memory = &memory;
    auto it = slots.find(Key(gpu, compute));
    if (it == slots.end()) {
        if (!tile) {
            return;
        }
        if (slots.size() >= MAX_SLOTS) {
            ++dropped_slots;
            return;
        }
        it = slots.emplace(Key(gpu, compute), Slot{}).first;
        Slot& fresh = it->second;
        fresh.gpu = gpu;
        fresh.table = table;
        fresh.index = index;
        fresh.compute = compute;
        fresh.first_frame = now;
        SeeTable(table, limit, compute, now);
    }
    Slot& slot = it->second;
    slot.limit = limit;
    slot.memory = &memory;
    if (slot.used != 0) {
        Value& current = slot.values[slot.current];
        if (current.raw == raw && current.image == image && current.image_gpu == image_gpu) {
            current.last_frame = now;
            ++current.reads;
            return;
        }
        ++slot.changes;
        ++changes;
    }
    for (size_t i = 0; i < slot.used; ++i) {
        Value& value = slot.values[i];
        if (value.raw == raw && value.image == image && value.image_gpu == image_gpu) {
            value.last_frame = now;
            ++value.reads;
            slot.current = i;
            return;
        }
    }
    if (slot.used == MAX_VALUES) {
        // Keep the first entry; the oldest of the others goes.
        std::move(slot.values.begin() + 2, slot.values.end(), slot.values.begin() + 1);
        --slot.used;
        ++slot.dropped_values;
    }
    slot.values[slot.used] = Value{raw, image, image_gpu, tile, now, now, 1};
    slot.current = slot.used++;
}

// The first whole entry of a TIC table seen so far that [dst, dst + size) covers.
inline std::optional<u64> CoveredSlot(u64 dst, u64 size) {
    for (const Table& t : tables) {
        const u64 bytes = (u64{t.limit} + 1) * ENTRY_BYTES;
        if (!Overlaps(dst, size, t.base, bytes)) {
            continue;
        }
        const u64 start = (std::max)(dst, t.base);
        const u64 containing = t.base + (start - t.base) / ENTRY_BYTES * ENTRY_BYTES;
        const u64 whole = containing == start ? containing : containing + ENTRY_BYTES;
        return whole + ENTRY_BYTES <= dst + size ? whole : containing;
    }
    return std::nullopt;
}

inline void NoteWrite(const Tegra::MemoryManager& memory, Source source, u8 path, u64 dst,
                      u64 dst_size, u64 src, u64 src_size, u8 buffer, u8 tex_src) {
    std::scoped_lock lock{mutex};
    const std::optional<u64> slot = CoveredSlot(dst, dst_size);
    if (!slot) {
        return;
    }
    Write w{};
    w.seq = ++kept;
    w.frame = frame;
    w.source = source;
    w.path = path;
    w.dst = dst;
    w.src = src;
    w.dst_size = static_cast<u32>((std::min)(dst_size, u64{0xffffffff}));
    w.src_size = static_cast<u32>((std::min)(src_size, u64{0xffffffff}));
    w.buffer = buffer;
    w.tex_src = tex_src;
    w.slot = *slot;
    Raw raw{};
    w.dst_read = ReadNow(&memory, *slot, raw);
    w.dst_addr = Address(raw);
    if (src_size != 0 && *slot >= dst) {
        w.src_read = ReadNow(&memory, src + (*slot - dst), raw);
        w.src_addr = Address(raw);
    }
    writes_dma += source == Source::Dma ? 1 : 0;
    writes_inline += source == Source::Inline ? 1 : 0;
    writes_buffer_taken += buffer == 2 ? 1 : 0;
    writes[head] = w;
    head = (head + 1) % WRITE_RING;
}

// MaxwellDMA::Launch, after the copy and before DmaTrace::End.
inline void NoteDma(const Tegra::MemoryManager& memory) {
    DmaTrace::Entry e;
    {
        std::scoped_lock lock{DmaTrace::mutex};
        if (!DmaTrace::open) {
            return;
        }
        e = DmaTrace::current;
    }
    NoteWrite(memory, Source::Dma, static_cast<u8>(e.path), e.dst_gpu, e.dst_size, e.src_gpu,
              e.src_size, e.buffer, e.tex_src);
}

// RasterizerVulkan::AccelerateInlineToMemory, after the write.
inline void NoteInline(const Tegra::MemoryManager& memory, u64 dst, u64 size) {
    NoteWrite(memory, Source::Inline, 0, dst, size, 0, 0, 0, 0);
}

template <typename Func>
inline void ForEachWrite(Func&& func) {
    const size_t count = static_cast<size_t>((std::min)(kept, u64{WRITE_RING}));
    for (size_t i = 0; i < count; ++i) {
        if (!func(writes[(head + WRITE_RING - 1 - i) % WRITE_RING])) {
            return;
        }
    }
}

inline bool Gave(const Slot& slot, u32 id, u64 gpu) {
    for (size_t i = 0; i < slot.used; ++i) {
        if (slot.values[i].image == id && slot.values[i].image_gpu == gpu) {
            return true;
        }
    }
    return false;
}

inline std::vector<const Slot*> SlotsOf(u32 id, u64 gpu) {
    std::vector<const Slot*> found;
    for (const auto& [key, slot] : slots) {
        if (Gave(slot, id, gpu)) {
            found.push_back(&slot);
        }
    }
    std::ranges::sort(found, [](const Slot* a, const Slot* b) {
        return a->gpu != b->gpu ? a->gpu < b->gpu : a->compute < b->compute;
    });
    return found;
}

// F7, for a zero tile (what = "zero") or a control (what = "good"). `describe(gpu)` names the
// tile image at a GPU address and its readback (tile_trace.h).
template <typename AgeText, typename Describe>
inline void ListFor(const char* what, u32 id, u64 gpu, u64 cpu, u64 size, u64 now, AgeText&& age,
                    Describe&& describe, u32 number) {
    std::scoped_lock lock{mutex};
    const std::vector<const Slot*> found = SlotsOf(id, gpu);
    size_t listed = 0;
    size_t write_hits = 0;
    for (const Slot* slot : found) {
        if (listed == MAX_SLOTS_LISTED) {
            break;
        }
        ++listed;
        const Value& read = slot->values[slot->current];
        Raw now_raw{};
        const bool mapped = ReadNow(slot->memory, slot->gpu, now_raw);
        const std::optional<DAddr> dev =
            slot->memory ? slot->memory->GpuToCpuAddress(slot->gpu) : std::nullopt;
        LOG_INFO(Render_Vulkan,
                 "TIC_SLOT what={} id={} gpu={:#x} slot={:#x} dev={:#x} table={:#x} index={} "
                 "limit={} compute={} values={} changes={} dropped_values={} first_frame={} "
                 "first_age_s={} read_addr={:#x} read_offset={} read_image={} read_last_frame={} "
                 "now_mapped={} now_addr={:#x} now_offset={} now_same={} read_raw={} now_raw={} n={}",
                 what, id, gpu, slot->gpu, dev.value_or(0), slot->table, slot->index, slot->limit,
                 slot->compute, slot->used, slot->changes, slot->dropped_values, slot->first_frame,
                 age(now, slot->first_frame), Address(read.raw), Offset(Address(read.raw), gpu),
                 read.image == NO_IMAGE ? -1 : static_cast<s64>(read.image), read.last_frame, mapped,
                 Address(now_raw), Offset(Address(now_raw), gpu), now_raw == read.raw,
                 RawText(read.raw), RawText(now_raw), number);
        for (size_t i = 0; i < slot->used; ++i) {
            const Value& v = slot->values[i];
            LOG_INFO(Render_Vulkan,
                     "TIC_VALUE what={} id={} slot={:#x} i={} current={} addr={:#x} offset={} "
                     "image={} image_gpu={:#x} tile={} first_frame={} last_frame={} last_age_s={} "
                     "reads={} raw={} n={}",
                     what, id, slot->gpu, i, i == slot->current, Address(v.raw),
                     Offset(Address(v.raw), gpu),
                     v.image == NO_IMAGE ? -1 : static_cast<s64>(v.image), v.image_gpu, v.tile,
                     v.first_frame, v.last_frame, age(now, v.last_frame), v.reads, RawText(v.raw),
                     number);
        }
        for (s64 delta = -s64{NEIGHBOURS}; delta <= s64{NEIGHBOURS}; ++delta) {
            const s64 other = static_cast<s64>(slot->index) + delta;
            if (delta == 0 || other < 0 || other > static_cast<s64>(slot->limit)) {
                continue;
            }
            Raw raw{};
            const u64 at = slot->table + static_cast<u64>(other) * ENTRY_BYTES;
            const bool ok = ReadNow(slot->memory, at, raw);
            LOG_INFO(Render_Vulkan,
                     "TIC_NEAR what={} id={} slot={:#x} index={} delta={} mapped={} addr={:#x} "
                     "offset={} {} raw={} n={}",
                     what, id, slot->gpu, other, delta, ok, Address(raw), Offset(Address(raw), gpu),
                     describe(Address(raw)), RawText(raw), number);
        }
        size_t write_listed = 0;
        ForEachWrite([&](const Write& w) {
            if (!Overlaps(w.dst, w.dst_size, slot->gpu, ENTRY_BYTES)) {
                return true;
            }
            ++write_hits;
            if (write_listed < MAX_WRITES_LISTED) {
                ++write_listed;
                LOG_INFO(Render_Vulkan,
                         "TIC_WRITE what={} id={} slot={:#x} #{} frame={} age_s={} source={} "
                         "path={} dst={:#x}+{:#x} src={:#x}+{:#x} buffer_took={} tex_src_gpu={} "
                         "entry={:#x} dst_read={} dst_addr={:#x} src_read={} src_addr={:#x} n={}",
                         what, id, slot->gpu, w.seq, w.frame, age(now, w.frame),
                         w.source == Source::Dma ? "dma" : "inline",
                         w.source == Source::Dma
                             ? DmaTrace::PathName(static_cast<DmaTrace::Path>(w.path))
                             : "-",
                         w.dst, w.dst_size, w.src, w.src_size, DmaTrace::Tri(w.buffer),
                         DmaTrace::Tri(w.tex_src), w.slot, w.dst_read, w.dst_addr, w.src_read,
                         w.src_addr, number);
            }
            return true;
        });
        std::scoped_lock dma_lock{DmaTrace::mutex};
        DmaTrace::zero_ranges.push_back(DmaTrace::ZeroRange{SLOT_RANGE_ID + id, number, slot->gpu,
                                                            dev.value_or(0), ENTRY_BYTES});
    }
    // Draw targets whose memory lies in this tile's memory under another GPU address.
    size_t cpu_hits = 0;
    if (last_memory && cpu != 0) {
        std::scoped_lock draw_lock{DrawTargetTrace::mutex};
        size_t cpu_listed = 0;
        for (const auto& [key, t] : DrawTargetTrace::targets) {
            if (Overlaps(t.gpu, 1, gpu, size)) {
                continue;
            }
            const std::optional<DAddr> dev = last_memory->GpuToCpuAddress(t.gpu);
            if (!dev || !Overlaps(*dev, 1, cpu, size)) {
                continue;
            }
            ++cpu_hits;
            if (cpu_listed < MAX_CPU_LISTED) {
                ++cpu_listed;
                LOG_INFO(Render_Vulkan,
                         "TIC_CPU what={} id={} target={:#x} dev={:#x} offset={} zeta={} {}x{} "
                         "format={} first_frame={} last_frame={} last_age_s={} ran={} skipped={} "
                         "n={}",
                         what, id, t.gpu, *dev, Offset(*dev, cpu), t.zeta, t.width, t.height,
                         t.format, t.first_frame, t.last_frame, age(now, t.last_frame),
                         t.Total(DrawTargetTrace::Outcome::Ran), t.Skipped(), number);
            }
        }
    }
    LOG_INFO(Render_Vulkan,
             "TIC_TILE end what={} id={} gpu={:#x} cpu={:#x} slots={} listed={} writes={} "
             "cpu_targets={} n={}",
             what, id, gpu, cpu, found.size(), listed, write_hits, cpu_hits, number);
}

// F7, every zero tile.
struct TileCount {
    size_t tiles = 0;
    size_t with_slot = 0;
    size_t now_differs = 0;  // a slot whose entry in guest memory now is not the one read last
    size_t written = 0;      // a slot a kept DMA or inline write covered
    size_t written_buffer_taken = 0; // ... by a DMA copy the buffer cache took
};

inline void CountFor(TileCount& count, u32 id, u64 gpu) {
    std::scoped_lock lock{mutex};
    ++count.tiles;
    const std::vector<const Slot*> found = SlotsOf(id, gpu);
    bool differs = false;
    bool written = false;
    bool taken = false;
    for (const Slot* slot : found) {
        Raw now_raw{};
        ReadNow(slot->memory, slot->gpu, now_raw);
        differs = differs || now_raw != slot->values[slot->current].raw;
        ForEachWrite([&](const Write& w) {
            if (Overlaps(w.dst, w.dst_size, slot->gpu, ENTRY_BYTES)) {
                written = true;
                taken = taken || w.buffer == 2;
            }
            return true;
        });
    }
    count.with_slot += found.empty() ? 0 : 1;
    count.now_differs += differs ? 1 : 0;
    count.written += written ? 1 : 0;
    count.written_buffer_taken += taken ? 1 : 0;
}

inline void Summary(const TileCount& count, u32 number) {
    std::scoped_lock lock{mutex};
    LOG_INFO(Render_Vulkan,
             "TIC_TRACE n={} slots={} dropped_slots={} tables={} dropped_tables={} notes={} "
             "new_reads={} changes={} writes={} writes_dma={} writes_inline={} "
             "writes_buffer_taken={} ring={} frame={} zero_tiles={} zero_tiles_slot={} "
             "zero_tiles_no_slot={} zero_tiles_now_differs={} zero_tiles_written={} "
             "zero_tiles_written_buffer_taken={}",
             number, slots.size(), dropped_slots, tables.size(), dropped_tables, notes, new_reads,
             changes, kept, writes_dma, writes_inline, writes_buffer_taken, WRITE_RING, frame,
             count.tiles, count.with_slot, count.tiles - count.with_slot, count.now_differs,
             count.written, count.written_buffer_taken);
    for (const Table& t : tables) {
        LOG_INFO(Render_Vulkan,
                 "TIC_TABLE base={:#x} limit={} compute={} first_frame={} last_frame={} n={}",
                 t.base, t.limit, t.compute, t.first_frame, t.last_frame, number);
    }
}

} // namespace VideoCommon::TicTrace
#endif
