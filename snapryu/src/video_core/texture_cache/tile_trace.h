// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// TILE_TRACE (b63): the life of every terrain tile image, and which ones are zero when the
// squares are on screen (TotK NCE, 2026-10-18).
//
// Finding (b61 NCE capture 1, re-examined): the draw that paints a square samples a 260x260
// R8G8_UNORM (PixelFormat 52) tile normal texture whose content is ALL ZEROS; the neighbouring good
// draw has the same inputs except a real tile. A zero normal gives one constant tilted normal, so
// the lighting pass paints one flat tile: the square. The game bakes these tiles on the GPU
// (level 0 by a 260x260 render pass, the mips by 130->1 passes into separate images, joined into
// the tile by alias copies), so the texture cache has to carry the baked content from image to
// image; guest memory holds whatever the game wrote (usually nothing).
//
// b51/b52 could not see a zero tile: both compare only images with GPU writes or GpuModified, and a
// tile re-uploaded from zero guest memory with no GPU write looks "clean" (zeros == zeros).
//
// So this trace keeps, for every image of the tile format, a ring of the events that made or lost
// its content:
//   create          JoinImages made it (counts of overlaps, aliases, bad overlaps, ignored images)
//   upload          RefreshContents uploaded guest memory (zero permille of the uploaded bytes, and
//                   whether the image held GPU content at that moment: a zero upload over GPU content
//                   is the suspected mechanism)
//   target/storage/dma/blit   a GPU write (render target: the view's levels and layers)
//   copy_in/copy_out          texture cache copies (join: JoinImages, alias: SynchronizeAliases)
//   join_alias_skipped        JoinImages did not copy an alias (unsafe copy, samples, no alias)
//   join_no_copy              JoinImages absorbed an overlap without copying (it was not GpuModified)
//   join_delete / join_ignore_delete / gc_delete / unmap_delete   the image is deleted; gc_delete
//                   says whether the content was downloaded first (BadOverlap images are not)
//   remapped        UnmapGPUMemory marked it CpuModified + Remapped
//   cpu_modified    WriteMemory marked it CpuModified (the next use uploads guest memory)
//   download        DownloadMemory wrote its content to guest memory (clears GpuModified)
//   invalidate      PrepareImage(invalidate) dropped CpuModified and GpuModified
// Deleted images keep their ring in a graveyard by address, so the history of the image that held
// the same memory before a zero tile can be shown next to it.
//
// F7 (alias_trace.h arms a report for the next frame): every tile-format image sampled in that
// frame is read back from the GPU at the end of the frame (TickFrame), its level-0 and whole zero
// permille is computed, and guest memory at its address is counted too. Each zero tile is listed
// with its ring, the rings of its aliases and overlaps, of other live tile images over its memory,
// and of the graveyard images over its memory; two non-zero tiles of the same shape follow as
// controls. The first few risky events (a zero upload over GPU content, a GC delete without download
// of GPU content, a join without copy after GPU writes) are logged at once, with the ring.
//
// b64 additions:
//   cpu_modified says which store made it: under NCE the exact fault address, guest pc and
//   instruction word (common/nce_write_note.h), and whether the fault address lies inside the
//   image. Each such event on a tile image holding GPU content is logged as TILE_CPUW at the end
//   of the frame (first 400; WriteMemory may run inside the NCE exception handler).
//   F7 also unswizzles guest memory of each checked tile as an upload would and compares level 0
//   with the GPU readback (guest_l0_zero_permille, match_l0_permille; TILE_COMPARE per tile).
//   Level 0 of each zero tile and of the first control is written to
//   log/tile_dump_<unix seconds>/n<report>_id<id>_<w>x<h>_{gpu,guest}.r8g8 (raw bytes).
//   The containers are allocated once and never destroyed (no static destruction at exit).
//
// Diagnostic builds only; release builds compile none of this. Used under the texture cache mutex
// (GPU thread, and CPU threads in WriteMemory).
#pragma once

#if SNAPRYU_DIAGNOSTICS
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <ankerl/unordered_dense.h>
#include <fmt/format.h>
#include "common/common_types.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/nce_write_note.h"
#include "video_core/memory_manager.h"
#include "video_core/engines/dma_trace.h"
#include "video_core/engines/draw_target_trace.h"
#include "video_core/texture_cache/cpu_write_trace.h"
#include "video_core/texture_cache/tic_trace.h"
#include "video_core/texture_cache/alias_trace.h"
#include "video_core/texture_cache/image_base.h"
#include "video_core/texture_cache/types.h"

namespace VideoCommon::TileTrace {

enum class Kind : u8 {
    Create,
    Upload,
    UploadAsync,
    Write,     // writer says target/storage/dma/blit
    CopyIn,    // writer says join/alias
    CopyOut,
    JoinAliasSkipped,
    JoinNoCopy,
    JoinDelete,
    JoinIgnoreDelete,
    GcDelete,
    UnmapDelete,
    Remapped,
    CpuModified,
    Download,
    Invalidate,
};

inline const char* KindName(Kind kind) {
    switch (kind) {
    case Kind::Create:
        return "create";
    case Kind::Upload:
        return "upload";
    case Kind::UploadAsync:
        return "upload_async";
    case Kind::Write:
        return "write";
    case Kind::CopyIn:
        return "copy_in";
    case Kind::CopyOut:
        return "copy_out";
    case Kind::JoinAliasSkipped:
        return "join_alias_skipped";
    case Kind::JoinNoCopy:
        return "join_no_copy";
    case Kind::JoinDelete:
        return "join_delete";
    case Kind::JoinIgnoreDelete:
        return "join_ignore_delete";
    case Kind::GcDelete:
        return "gc_delete";
    case Kind::UnmapDelete:
        return "unmap_delete";
    case Kind::Remapped:
        return "remapped";
    case Kind::CpuModified:
        return "cpu_modified";
    case Kind::Download:
        return "download";
    case Kind::Invalidate:
        return "invalidate";
    }
    return "?";
}

// Same order as WriteTrace::Writer (write_trace.h includes this header, so the value is passed).
inline const char* WriterName(u8 writer) {
    static constexpr std::array<const char*, 6> names{"target", "storage", "dma",
                                                      "blit",   "join",    "alias"};
    return writer < names.size() ? names[writer] : "?";
}

inline const char* SkipReason(u32 reason) {
    switch (reason) {
    case 1:
        return "unsafe_copy";
    case 2:
        return "samples";
    case 3:
        return "no_alias";
    }
    return "?";
}

inline constexpr u32 DEFAULT_FORMAT = 52; // PixelFormat::R8G8_UNORM (the tile normal format)
inline constexpr size_t HEAD_EVENTS = 8;
inline constexpr size_t TAIL_EVENTS = 40;
inline constexpr size_t MAX_LIVE = size_t{1} << 15;
inline constexpr size_t MAX_GRAVEYARD = 2048;
inline constexpr size_t MAX_PENDING = 1024;
inline constexpr size_t MAX_ZERO_LISTED = 6;
inline constexpr size_t MAX_RELATIONS = 8;
inline constexpr size_t MAX_RELATION_EVENTS = 16;
inline constexpr size_t MAX_CONTROLS = 2;
inline constexpr u32 MAX_WARNINGS = 24;
inline constexpr u32 ZERO_PERMILLE = 999;

struct Event {
    u64 seq = 0;
    u64 frame = 0;
    u64 c = 0;
    u64 d = 0; // cpu_modified: fault device address (0: unknown)
    u64 e = 0; // cpu_modified: guest pc
    u64 tick = 0;
    u32 a = 0;
    u32 b = 0;
    u32 f = 0; // cpu_modified: instruction word
    u32 flags = 0;
    Kind kind = Kind::Create;
    u8 writer = 0;
};

struct Record {
    u32 index = 0;
    GPUVAddr gpu_addr = 0;
    DAddr cpu_addr = 0;
    u32 size = 0;
    u32 format = 0;
    u32 width = 0;
    u32 height = 0;
    s32 levels = 1;
    s32 layers = 1;
    u64 created = 0;         // frame JoinImages made it (0: seen first after creation)
    u64 deleted = 0;         // frame DeleteImage took it (graveyard only)
    u64 first_sampled = 0;
    u64 last_sampled = 0;
    u64 samples = 0;
    u64 events = 0;          // all events, also those the ring no longer holds
    bool gpu_content = false; // a GPU write or copy_in since the last upload
    std::vector<Event> head;
    std::vector<Event> tail; // circular, oldest at tail_pos once full
    size_t tail_pos = 0;
};

enum Counter : size_t {
    Events,
    Uploads,
    UploadsZero,
    UploadsZeroOverGpu,
    GcDeletes,
    GcDeletesNoDownload,
    GcDeletesNoDownloadGpu,
    JoinNoCopies,
    JoinNoCopiesAfterGpu,
    JoinAliasSkips,
    JoinDeletes,
    UnmapDeletes,
    Remaps,
    CpuModifieds,
    Downloads,
    Invalidates,
    Dropped,
    CpuwGpu,        // cpu_modified on a tile image holding GPU content
    CpuwGpuNce,     // ... with an NCE fault note
    CpuwGpuInside,  // ... whose fault address lies inside the image
    CpuwGpuOutside, // ... whose fault address lies outside the image (same page)
    NumCounters,
};

inline std::array<u64, NumCounters> counters{};
inline ankerl::unordered_dense::map<u32, Record>& live =
    *new ankerl::unordered_dense::map<u32, Record>();
inline std::deque<Record>& graveyard = *new std::deque<Record>();
// sampled while a report collects
inline ankerl::unordered_dense::set<u32>& pending = *new ankerl::unordered_dense::set<u32>();
// b66: a press reports several frames; each tile is read back once per press (the first frame of
// the burst that samples it), so later frames list only tiles not seen before.
inline ankerl::unordered_dense::set<u32>& burst_checked = *new ankerl::unordered_dense::set<u32>();
inline u32 burst_number = 0;
inline u64 sequence = 0;
inline u64 current_frame = 0; // frame_tick, set by EndFrame (for hooks that get no frame)
inline u32 warnings_logged = 0;
inline std::vector<float>& frame_seconds = *new std::vector<float>();
inline u32 cpuw_logged = 0;
inline constexpr u32 MAX_CPUW = 400;
inline constexpr size_t CPUW_QUEUE = 64;
inline std::chrono::steady_clock::duration log_interval = std::chrono::seconds(5);

inline void Add(Counter counter, u64 amount = 1) {
    counters[counter] += amount;
}

inline u32 TrackedFormat() {
    static const u32 format = [] {
        const char* const value = std::getenv("EDEN_TILE_TRACE_FORMAT");
        const u32 result = value && *value ? static_cast<u32>(std::strtoul(value, nullptr, 0))
                                           : DEFAULT_FORMAT;
        LOG_INFO(Render_Vulkan, "TILE_TRACE ready b64 format={} key=F7", result);
        LOG_INFO(Render_Vulkan, "TILE_TRACE b66 burst={} readback=once_per_press",
                 AliasTrace::BURST_FRAMES);
        LOG_INFO(Render_Vulkan, "TILE_TRACE b67 readback=all_single_sample");
        LOG_INFO(Render_Vulkan, "TILE_TRACE b68 dma_trace ring={} min_bytes={:#x}",
                 DmaTrace::RING, DmaTrace::MIN_BYTES);
        LOG_INFO(Render_Vulkan, "TILE_TRACE b69 draw_target max_targets={} near={:#x}",
                 DrawTargetTrace::MAX_TARGETS, DrawTargetTrace::NEAR_BYTES);
        LOG_INFO(Render_Vulkan, "TILE_TRACE b70 tic_trace max_slots={} write_ring={} neighbours={}",
                 TicTrace::MAX_SLOTS, TicTrace::WRITE_RING, TicTrace::NEIGHBOURS);
        LOG_INFO(Render_Vulkan, "TILE_TRACE b71 cpu_write_trace page_slots={} tile_ring={} regs={}",
                 CpuWriteTrace::PAGE_SLOTS, CpuWriteTrace::TILE_RING, CpuWriteTrace::REGS);
        AliasTrace::tile_format = result;
        return result;
    }();
    return format;
}

inline bool Tracks(const ImageBase& image) {
    return static_cast<u32>(image.info.format) == TrackedFormat();
}

inline float Seconds(u64 frame) {
    return frame < frame_seconds.size() ? frame_seconds[frame] : 0.0f;
}

inline std::string Age(u64 now, u64 frame) {
    if (frame == 0 || frame > now) {
        return "-";
    }
    return fmt::format("{:.1f}", Seconds(now) - Seconds(frame));
}

inline void Reset(Record& record, u32 index, const ImageBase& image, u64 created) {
    record = Record{};
    record.index = index;
    record.gpu_addr = image.gpu_addr;
    record.cpu_addr = image.cpu_addr;
    record.size = image.guest_size_bytes;
    record.format = static_cast<u32>(image.info.format);
    record.width = image.info.size.width;
    record.height = image.info.size.height;
    record.levels = static_cast<s32>(image.info.resources.levels);
    record.layers = static_cast<s32>(image.info.resources.layers);
    record.created = created;
}

// Record of the tracked image now in this slot. With create, an unknown or replaced slot starts a
// record (creation unknown). Nullptr for untracked formats and, without create, unknown slots.
inline Record* Find(u32 index, const ImageBase& image, bool create) {
    if (!Tracks(image)) {
        return nullptr;
    }
    const auto it = live.find(index);
    if (it != live.end()) {
        Record& record = it->second;
        if (record.gpu_addr == image.gpu_addr && record.size == image.guest_size_bytes) {
            return &record;
        }
        if (!create) {
            return nullptr;
        }
        Reset(record, index, image, 0);
        return &record;
    }
    if (!create) {
        return nullptr;
    }
    if (live.size() >= MAX_LIVE) {
        Add(Dropped);
        return nullptr;
    }
    Record& record = live[index];
    Reset(record, index, image, 0);
    return &record;
}

inline void Push(Record& record, Event event) {
    event.seq = ++sequence;
    ++record.events;
    Add(Events);
    if (record.head.size() < HEAD_EVENTS) {
        record.head.push_back(event);
        return;
    }
    if (record.tail.size() < TAIL_EVENTS) {
        record.tail.push_back(event);
        return;
    }
    record.tail[record.tail_pos] = event;
    record.tail_pos = (record.tail_pos + 1) % TAIL_EVENTS;
}

template <typename Func>
inline void ForEachEvent(const Record& record, Func&& func) {
    for (const Event& event : record.head) {
        func(event);
    }
    const size_t count = record.tail.size();
    for (size_t i = 0; i < count; ++i) {
        func(record.tail[(record.tail_pos + i) % count]);
    }
}

inline Event Make(const ImageBase& image, u64 frame, Kind kind, u32 a, u32 b, u64 c, u8 writer) {
    Event event;
    event.frame = frame;
    event.c = c;
    event.tick = image.modification_tick;
    event.a = a;
    event.b = b;
    event.flags = static_cast<u32>(image.flags);
    event.kind = kind;
    event.writer = writer;
    return event;
}

inline std::string Describe(const Record& r) {
    return fmt::format("id={} gpu={:#x} cpu={:#x} size={:#x} format={} {}x{} levels={} layers={} "
                       "created_frame={} deleted_frame={} samples={} events={} gpu_content={}",
                       r.index, r.gpu_addr, r.cpu_addr, r.size, r.format, r.width, r.height,
                       r.levels, r.layers, r.created, r.deleted, r.samples, r.events,
                       r.gpu_content);
}

inline std::string EventText(const Event& e, u64 now) {
    std::string text = fmt::format("#{} frame={} age_s={} kind={}", e.seq, e.frame,
                                   Age(now, e.frame), KindName(e.kind));
    switch (e.kind) {
    case Kind::Create:
        text += fmt::format(" overlaps={} aliases={} bad_overlaps={} ignored={}", e.a, e.b,
                            e.c & 0xFFFF, e.c >> 16);
        break;
    case Kind::Upload:
        text += fmt::format(" zero_permille={} bytes={} gpu_before={}", e.a, e.b, e.c);
        break;
    case Kind::UploadAsync:
        break;
    case Kind::Write:
        text += fmt::format(" writer={}", WriterName(e.writer));
        if (e.c == ~u64{0}) {
            text += " range=all";
        } else {
            text += fmt::format(" level={} layer={} levels={} layers={}", e.a, e.b, e.c >> 16,
                                e.c & 0xFFFF);
        }
        break;
    case Kind::CopyIn:
        text += fmt::format(" writer={} src_id={} src_gpu={:#x} copies={}", WriterName(e.writer),
                            e.a, e.c, e.b);
        break;
    case Kind::CopyOut:
        text += fmt::format(" writer={} dst_id={} dst_gpu={:#x} copies={}", WriterName(e.writer),
                            e.a, e.c, e.b);
        break;
    case Kind::JoinAliasSkipped:
        text += fmt::format(" reason={} src_id={} src_gpu={:#x}", SkipReason(e.b), e.a, e.c);
        break;
    case Kind::JoinNoCopy:
        text += fmt::format(" src_id={} src_gpu={:#x} src_flags={:#x}", e.a, e.c, e.b);
        break;
    case Kind::JoinDelete:
        text += fmt::format(" new_id={} new_gpu={:#x} copied={}", e.a, e.c, e.b);
        break;
    case Kind::JoinIgnoreDelete:
        text += fmt::format(" new_id={} new_gpu={:#x}", e.a, e.c);
        break;
    case Kind::GcDelete:
        text += fmt::format(" downloaded={} high_priority={} aggressive={}", e.a, e.b & 1,
                            (e.b >> 1) & 1);
        break;
    case Kind::UnmapDelete:
        text += fmt::format(" unmap_cpu={:#x} unmap_size={:#x}", e.c, e.a);
        break;
    case Kind::Remapped:
        text += fmt::format(" range_gpu={:#x} range_size={:#x}", e.c, e.a);
        break;
    case Kind::CpuModified:
        text += fmt::format(" write_cpu={:#x} write_size={:#x}", e.c, e.a);
        if (e.b & 1) {
            text += fmt::format(" source=nce fault_dev={:#x} inside={} pc={:#x} insn={:08x}", e.d,
                                (e.b >> 1) & 1, e.e, e.f);
        } else {
            text += " source=other";
        }
        break;
    case Kind::Download:
    case Kind::Invalidate:
        break;
    }
    text += fmt::format(" flags={:#x} tick={}", e.flags, e.tick);
    return text;
}

inline void LogRecord(const char* tag, const Record& record, u64 now, u32 number,
                      size_t max_events) {
    LOG_INFO(Render_Vulkan, "{} {} n={}", tag, Describe(record), number);
    size_t shown = 0;
    size_t skipped = 0;
    ForEachEvent(record, [&](const Event& event) {
        if (shown < max_events) {
            ++shown;
            LOG_INFO(Render_Vulkan, "TILE_EVENT of={} {} n={}", record.index, EventText(event, now),
                     number);
        } else {
            ++skipped;
        }
    });
    if (skipped != 0 || record.events > shown + skipped) {
        LOG_INFO(Render_Vulkan, "TILE_EVENT of={} not_shown={} n={}", record.index,
                 skipped + (record.events - shown - skipped), number);
    }
}

inline void Warn(const char* what, const Record& record, u64 frame) {
    if (warnings_logged >= MAX_WARNINGS) {
        return;
    }
    ++warnings_logged;
    LOG_WARNING(Render_Vulkan, "TILE_WARN {} frame={} w={}", what, frame, warnings_logged);
    LogRecord("TILE_WARN image", record, frame, warnings_logged, HEAD_EVENTS + TAIL_EVENTS);
}

// ---- hooks ----

inline void Created(u32 index, const ImageBase& image, u64 frame, size_t overlaps, size_t aliases,
                    size_t bad_overlaps, size_t ignored) {
    if (!Tracks(image)) {
        return;
    }
    if (live.size() >= MAX_LIVE && !live.contains(index)) {
        Add(Dropped);
        return;
    }
    Record& record = live[index];
    Reset(record, index, image, frame);
    Push(record, Make(image, frame, Kind::Create, static_cast<u32>(overlaps),
                      static_cast<u32>(aliases),
                      (static_cast<u64>(ignored) << 16) | (bad_overlaps & 0xFFFF), 0));
}

inline void Note(u32 index, const ImageBase& image, u64 frame, Kind kind, u32 a, u32 b, u64 c) {
    Record* const record = Find(index, image, true);
    if (!record) {
        return;
    }
    Push(*record, Make(image, frame, kind, a, b, c, 0));
    switch (kind) {
    case Kind::GcDelete:
        Add(GcDeletes);
        if (a == 0) {
            Add(GcDeletesNoDownload);
            if (True(image.flags & ImageFlagBits::GpuModified) || record->gpu_content) {
                Add(GcDeletesNoDownloadGpu);
                Warn("gc_delete_without_download_of_gpu_content", *record, frame);
            }
        }
        break;
    case Kind::JoinNoCopy:
        Add(JoinNoCopies);
        break;
    case Kind::JoinAliasSkipped:
        Add(JoinAliasSkips);
        break;
    case Kind::JoinDelete:
        Add(JoinDeletes);
        if (b == 0 && record->gpu_content) {
            Add(JoinNoCopiesAfterGpu);
            Warn("join_delete_without_copy_after_gpu_writes", *record, frame);
        }
        break;
    case Kind::UnmapDelete:
        Add(UnmapDeletes);
        break;
    case Kind::Remapped:
        Add(Remaps);
        break;
    case Kind::CpuModified:
        Add(CpuModifieds);
        break;
    case Kind::Download:
        Add(Downloads);
        break;
    case Kind::Invalidate:
        Add(Invalidates);
        break;
    default:
        break;
    }
}

// A cpu_modified on an image holding GPU content, logged at the end of the frame: WriteMemory can
// run inside the NCE exception handler (host access fault path), where nothing may be logged.
struct CpuwEntry {
    Record image; // head/tail left empty
    Event event;
    u64 image_end = 0;
};
inline std::array<CpuwEntry, CPUW_QUEUE>& cpuw_queue = *new std::array<CpuwEntry, CPUW_QUEUE>();
inline size_t cpuw_queued = 0;
inline u64 cpuw_queue_dropped = 0;

// b71: guest memory zero permille through the GPU page table the texture cache last used
// (TicTrace::last_memory; NOT_SAMPLED before the first TIC read).
inline u32 GuestZeroPermille(const Tegra::MemoryManager& gpu_memory, GPUVAddr gpu_addr, u32 size);
inline u32 ZeroNow(u64 gpu_addr, u32 size) {
    const Tegra::MemoryManager* memory = nullptr;
    {
        std::scoped_lock lock{TicTrace::mutex};
        memory = TicTrace::last_memory;
    }
    return memory ? GuestZeroPermille(*memory, gpu_addr, size) : CpuWriteTrace::NOT_SAMPLED;
}

// b71: the tile image over a device address (a live one with GPU content first).
inline CpuWriteTrace::Hit FindDev(u64 dev) {
    CpuWriteTrace::Hit best{};
    const auto take = [&best, dev](const Record& r, bool dead) {
        if (r.cpu_addr == 0 || dev < r.cpu_addr || dev >= r.cpu_addr + r.size) {
            return;
        }
        const bool better = best.image == CpuWriteTrace::NO_IMAGE ||
                            (r.gpu_content && !best.gpu) ||
                            (r.gpu_content == best.gpu && best.dead && !dead);
        if (better) {
            best = CpuWriteTrace::Hit{r.index, static_cast<u32>(dev - r.cpu_addr), r.width,
                                      r.height, r.gpu_content, dead};
        }
    };
    for (const auto& [index, r] : live) {
        take(r, false);
    }
    for (const Record& r : graveyard) {
        take(r, true);
    }
    return best;
}

// WriteMemory is about to mark the image CpuModified. Under NCE the store that faulted is in
// Common::NceWriteNote::current (set by RunThread around InvalidateNCE, which invalidates the whole
// page): its device address is the page of this write plus the fault's page offset.
inline void CpuWrite(u32 index, const ImageBase& image, u64 frame, DAddr cpu_addr, size_t size) {
    Record* const record = Find(index, image, true);
    if (!record) {
        return;
    }
    const Common::NceWriteNote::Note note = Common::NceWriteNote::current;
    const bool nce = note.valid && size == 0x1000 && (cpu_addr & 0xFFF) == 0;
    const u64 fault_dev = nce ? (cpu_addr & ~u64{0xFFF}) + (note.fault & 0xFFF) : 0;
    const bool inside = nce && fault_dev >= image.cpu_addr && fault_dev < image.cpu_addr_end;
    const bool gpu = True(image.flags & ImageFlagBits::GpuModified) || record->gpu_content;
    // b71: every cpu_modified with the store's guest state and what its registers pointed at
    // (cpu_write_trace.h).
    {
        CpuWriteTrace::TileWrite& tile_write =
            CpuWriteTrace::NoteTile(index, record->gpu_addr, record->cpu_addr, record->size, gpu,
                                    cpu_addr, size, note, frame);
        u64 span_gpu = 0;
        u32 span_bytes = 0;
        if (CpuWriteTrace::ImageSpan(tile_write, span_gpu, span_bytes)) {
            tile_write.before_zero = static_cast<u16>(ZeroNow(span_gpu, span_bytes));
        }
        for (size_t i = 0; i < CpuWriteTrace::REGS; ++i) {
            u64 dev = 0;
            if (CpuWriteTrace::Estimate(note, cpu_addr, i, dev)) {
                tile_write.hits[i] = FindDev(dev);
            }
        }
    }
    Event event = Make(image, frame, Kind::CpuModified, static_cast<u32>(size),
                       (nce ? 1u : 0u) | (inside ? 2u : 0u) | (gpu ? 4u : 0u), cpu_addr, 0);
    event.d = fault_dev;
    event.e = nce ? note.pc : 0;
    event.f = nce ? note.insn : 0;
    Push(*record, event);
    Add(CpuModifieds);
    if (!gpu) {
        return;
    }
    Add(CpuwGpu);
    if (nce) {
        Add(CpuwGpuNce);
        Add(inside ? CpuwGpuInside : CpuwGpuOutside);
    }
    if (cpuw_logged + cpuw_queued >= MAX_CPUW) {
        return;
    }
    if (cpuw_queued == CPUW_QUEUE) {
        ++cpuw_queue_dropped;
        return;
    }
    CpuwEntry& entry = cpuw_queue[cpuw_queued++];
    entry.image.index = record->index;
    entry.image.gpu_addr = record->gpu_addr;
    entry.image.cpu_addr = record->cpu_addr;
    entry.image.size = record->size;
    entry.image.format = record->format;
    entry.image.width = record->width;
    entry.image.height = record->height;
    entry.image.levels = record->levels;
    entry.image.created = record->created;
    entry.image.samples = record->samples;
    entry.event = record->tail.empty() ? record->head.back()
                                       : record->tail[(record->tail_pos + record->tail.size() - 1) %
                                                      record->tail.size()];
    entry.image_end = image.cpu_addr_end;
}

inline void FlushCpuWrites(u64 now) {
    for (size_t i = 0; i < cpuw_queued; ++i) {
        const CpuwEntry& entry = cpuw_queue[i];
        const Record& r = entry.image;
        const Event& e = entry.event;
        ++cpuw_logged;
        if ((e.b & 1) == 0) {
            LOG_WARNING(Render_Vulkan,
                        "TILE_CPUW id={} gpu={:#x} cpu={:#x} size={:#x} format={} {}x{} levels={} "
                        "created_frame={} samples={} write_cpu={:#x} write_size={:#x} source=other "
                        "flags={:#x} frame={} seq={} age_s={} w={}",
                        r.index, r.gpu_addr, r.cpu_addr, r.size, r.format, r.width, r.height,
                        r.levels, r.created, r.samples, e.c, e.a, e.flags, e.frame, e.seq,
                        Age(now, e.frame), cpuw_logged);
            continue;
        }
        const s64 image_off = static_cast<s64>(e.d - r.cpu_addr);
        LOG_WARNING(Render_Vulkan,
                    "TILE_CPUW id={} gpu={:#x} cpu={:#x} size={:#x} format={} {}x{} levels={} "
                    "created_frame={} samples={} write_cpu={:#x} write_size={:#x} source=nce "
                    "fault_dev={:#x} page_off={:#x} image_off={} inside={} after_end={} "
                    "image_end={:#x} pc={:#x} insn={:08x} flags={:#x} frame={} seq={} age_s={} "
                    "w={}",
                    r.index, r.gpu_addr, r.cpu_addr, r.size, r.format, r.width, r.height, r.levels,
                    r.created, r.samples, e.c, e.a, e.d, e.d & 0xFFF, image_off, (e.b >> 1) & 1,
                    e.d >= entry.image_end ? 1 : 0, entry.image_end, e.e, e.f, e.flags, e.frame,
                    e.seq, Age(now, e.frame), cpuw_logged);
    }
    cpuw_queued = 0;
    if (cpuw_queue_dropped != 0) {
        LOG_WARNING(Render_Vulkan, "TILE_CPUW queue_dropped={} frame={}", cpuw_queue_dropped, now);
        cpuw_queue_dropped = 0;
    }
}

inline u32 ZeroPermille(std::span<const u8> bytes) {
    if (bytes.empty()) {
        return 0;
    }
    const auto zeros = static_cast<u64>(std::ranges::count(bytes, u8{0}));
    return static_cast<u32>(zeros * 1000 / bytes.size());
}

// RefreshContents uploaded `bytes` (the staging span, MapSizeBytes long) from guest memory.
// gpu_before: the image held GpuModified content when the upload began.
inline void Uploaded(u32 index, const ImageBase& image, u64 frame, std::span<const u8> bytes,
                     bool gpu_before) {
    Record* const record = Find(index, image, true);
    if (!record) {
        return;
    }
    const u32 permille = ZeroPermille(bytes);
    Add(Uploads);
    Push(*record, Make(image, frame, Kind::Upload, permille, static_cast<u32>(bytes.size()),
                       gpu_before ? 1 : 0, 0));
    const bool had_gpu = gpu_before || record->gpu_content;
    record->gpu_content = false;
    if (permille >= ZERO_PERMILLE) {
        Add(UploadsZero);
        if (had_gpu) {
            Add(UploadsZeroOverGpu);
            Warn("zero_upload_over_gpu_content", *record, frame);
        }
    }
}

inline void Wrote(u32 index, const ImageBase& image, u64 frame, u8 writer,
                  const std::optional<SubresourceRange>& range) {
    Record* const record = Find(index, image, true);
    if (!record) {
        return;
    }
    record->gpu_content = true;
    const bool ranged = range && (writer == 0 || writer == 2); // target or dma
    const u32 level = ranged ? static_cast<u32>(range->base.level) : 0;
    const u32 layer = ranged ? static_cast<u32>(range->base.layer) : 0;
    const u64 extent = ranged ? (static_cast<u64>(range->extent.levels) << 16) |
                                    static_cast<u64>(range->extent.layers & 0xFFFF)
                              : ~u64{0};
    Push(*record, Make(image, frame, Kind::Write, level, layer, extent, writer));
}

inline void Copied(u32 dst_index, const ImageBase& dst, u32 src_index, const ImageBase& src,
                   size_t copies, u8 writer) {
    const u64 frame = current_frame;
    if (Record* const record = Find(dst_index, dst, true)) {
        record->gpu_content = true;
        Push(*record, Make(dst, frame, Kind::CopyIn, src_index, static_cast<u32>(copies),
                           src.gpu_addr, writer));
    }
    if (Record* const record = Find(src_index, src, false)) {
        Push(*record, Make(src, frame, Kind::CopyOut, dst_index, static_cast<u32>(copies),
                           dst.gpu_addr, writer));
    }
}

inline void Sampled(u32 index, const ImageBase& image, u64 frame) {
    Record* const record = Find(index, image, true);
    if (!record) {
        return;
    }
    if (record->first_sampled == 0) {
        record->first_sampled = frame;
    }
    record->last_sampled = frame;
    ++record->samples;
    if (AliasTrace::collecting && pending.size() < MAX_PENDING) {
        if (burst_number != AliasTrace::report.number) {
            burst_number = AliasTrace::report.number;
            burst_checked.clear();
        }
        if (burst_checked.insert(index).second) {
            pending.insert(index);
        }
    }
}

// DeleteImage: the record moves to the graveyard (the reason was noted just before).
inline void Forget(u32 index, const ImageBase& image, u64 frame) {
    pending.erase(index);
    burst_checked.erase(index);
    const auto it = live.find(index);
    if (it == live.end()) {
        return;
    }
    if (it->second.gpu_addr == image.gpu_addr && it->second.size == image.guest_size_bytes) {
        it->second.deleted = frame;
        graveyard.push_back(std::move(it->second));
        if (graveyard.size() > MAX_GRAVEYARD) {
            graveyard.pop_front();
        }
    }
    live.erase(it);
}

// ---- F7 report ----

struct Result {
    u32 index = 0;
    u32 gpu_zero_l0 = 0;
    u32 gpu_zero_all = 0;
    u32 guest_zero = 0;
    u32 guest_zero_l0 = 1000; // level 0 of guest memory unswizzled (1000: not computed)
    u32 match_l0 = 0;         // level-0 bytes equal in the GPU readback and in guest memory
    bool compared = false;
    bool downloadable = true;
    bool flagged = true; // b67: IsDownloadable (GpuModified, not CpuModified); false = read anyway
    std::string description;
    std::vector<u32> aliases;
    std::vector<u32> overlaps;
    std::vector<u8> gpu_l0;   // kept for zero tiles and a few controls (dumps)
    std::vector<u8> guest_l0;
};

inline constexpr size_t MAX_KEPT_CONTROLS = 8;
inline size_t kept_controls = 0;
inline std::filesystem::path& dump_dir = *new std::filesystem::path();

inline std::vector<Result>& results = *new std::vector<Result>();

// Zero permille of guest memory, page by page; an unmapped page reads as zeros (as an upload of it
// would), as in RemapTrace::HashGuest.
inline u32 GuestZeroPermille(const Tegra::MemoryManager& gpu_memory, GPUVAddr gpu_addr, u32 size) {
    constexpr u64 PAGE_SIZE = 4096;
    if (size == 0) {
        return 0;
    }
    u64 zeros = 0;
    u64 done = 0;
    while (done < size) {
        const GPUVAddr addr = gpu_addr + done;
        const u64 chunk = (std::min)(PAGE_SIZE - (addr & (PAGE_SIZE - 1)), u64{size} - done);
        const u8* const source = gpu_memory.GetPointer(addr);
        if (source) {
            zeros += static_cast<u64>(
                std::count(source, source + chunk, u8{0}));
        } else {
            zeros += chunk;
        }
        done += chunk;
    }
    return static_cast<u32>(zeros * 1000 / size);
}

inline Result& NewResult(u32 index, const ImageBase& image) {
    results.push_back(Result{});
    Result& result = results.back();
    result.index = index;
    result.description = AliasTrace::Describe(index, image);
    for (const AliasedImage& alias : image.aliased_images) {
        result.aliases.push_back(alias.id.index);
    }
    for (const ImageId overlap : image.overlapping_images) {
        result.overlaps.push_back(overlap.index);
    }
    return result;
}

inline void NotDownloadable(u32 index, const ImageBase& image) {
    NewResult(index, image).downloadable = false;
}

// Level-0 bytes of `data` laid out by `copies` (all layers of level 0, in copy order).
inline std::vector<u8> LevelZero(std::span<const u8> data,
                                 std::span<const BufferImageCopy> copies) {
    std::vector<u8> level;
    for (const BufferImageCopy& copy : copies) {
        if (copy.image_subresource.base_level != 0 || copy.buffer_offset >= data.size()) {
            continue;
        }
        const size_t end = (std::min)(copy.buffer_offset + copy.buffer_size, data.size());
        level.insert(level.end(), data.begin() + copy.buffer_offset, data.begin() + end);
    }
    return level;
}

// The image was read back into `data` with `copies` (FullDownloadCopies). `guest` is guest memory
// unswizzled by `guest_copies` (UnswizzleImage, as an upload would see it); empty when the image is
// converted on upload (no byte-comparable layout).
inline void Checked(u32 index, const ImageBase& image, std::span<const u8> data,
                    std::span<const BufferImageCopy> copies, std::span<const u8> guest,
                    std::span<const BufferImageCopy> guest_copies,
                    const Tegra::MemoryManager& gpu_memory, bool flagged) {
    Result& result = NewResult(index, image);
    result.flagged = flagged;
    std::vector<u8> gpu_l0 = LevelZero(data, copies);
    result.gpu_zero_l0 = ZeroPermille(gpu_l0);
    result.gpu_zero_all = ZeroPermille(data);
    result.guest_zero = GuestZeroPermille(gpu_memory, image.gpu_addr, image.guest_size_bytes);
    if (guest.empty()) {
        return;
    }
    std::vector<u8> guest_l0 = LevelZero(guest, guest_copies);
    result.compared = true;
    result.guest_zero_l0 = ZeroPermille(guest_l0);
    const size_t common = (std::min)(gpu_l0.size(), guest_l0.size());
    size_t equal = 0;
    for (size_t i = 0; i < common; ++i) {
        equal += gpu_l0[i] == guest_l0[i];
    }
    const size_t total = (std::max)(gpu_l0.size(), guest_l0.size());
    result.match_l0 = total ? static_cast<u32>(u64{equal} * 1000 / total) : 0;
    const bool zero = result.gpu_zero_l0 >= ZERO_PERMILLE;
    if (zero || kept_controls < MAX_KEPT_CONTROLS) {
        kept_controls += zero ? 0 : 1;
        result.gpu_l0 = std::move(gpu_l0);
        result.guest_l0 = std::move(guest_l0);
    }
}

inline std::string CompareText(const Result& result) {
    if (!result.compared) {
        return "guest_l0_zero_permille=- match_l0_permille=-";
    }
    return fmt::format("guest_l0_zero_permille={} match_l0_permille={}", result.guest_zero_l0,
                       result.match_l0);
}

// log/tile_dump_<unix seconds of the first dump>/n<report>_id<id>_<w>x<h>_{gpu,guest}.r8g8
inline void Dump(const Result& result, const Record& record, const char* what, u32 number) {
    if (result.gpu_l0.empty() && result.guest_l0.empty()) {
        return;
    }
    std::error_code error;
    if (dump_dir.empty()) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
        dump_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) /
                   fmt::format("tile_dump_{}", seconds);
    }
    std::filesystem::create_directories(dump_dir, error);
    const auto write = [&](const std::vector<u8>& bytes, const char* side) {
        const std::filesystem::path path =
            dump_dir / fmt::format("n{}_id{}_{}x{}_{}.r8g8", number, record.index, record.width,
                                   record.height, side);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        LOG_INFO(Render_Vulkan, "TILE_DUMP {} id={} side={} bytes={} ok={} path={} n={}", what,
                 record.index, side, bytes.size(), file.good() ? 1 : 0, path.string(), number);
    };
    write(result.gpu_l0, "gpu");
    write(result.guest_l0, "guest");
}

inline bool Overlaps(const Record& record, GPUVAddr gpu_addr, u32 size) {
    return record.gpu_addr < gpu_addr + size && gpu_addr < record.gpu_addr + record.size;
}

inline void Relate(const char* relation, const Record& record, u64 now, u32 number, u32 zero_index,
                   size_t& shown) {
    if (shown >= MAX_RELATIONS) {
        return;
    }
    ++shown;
    LOG_INFO(Render_Vulkan, "TILE_REL zero_id={} relation={} n={}", zero_index, relation, number);
    LogRecord("TILE_REL image", record, now, number, MAX_RELATION_EVENTS);
}

// b70: the tile image at a GPU address (TIC_NEAR) and its readback in this report.
inline std::string DescribeAt(u64 gpu) {
    for (const auto& [index, r] : live) {
        if (r.gpu_addr != gpu) {
            continue;
        }
        const u32 image_index = index;
        const auto it = std::ranges::find_if(
            results, [image_index](const Result& x) { return x.index == image_index; });
        const char* const readback = it == results.end() ? "-"
                                     : !it->downloadable ? "not_downloadable"
                                     : it->gpu_zero_l0 >= ZERO_PERMILLE ? "zero"
                                                                        : "good";
        return fmt::format("image={} {}x{} levels={} gpu_content={} readback={}", image_index,
                           r.width, r.height, r.levels, r.gpu_content, readback);
    }
    return "image=- readback=-";
}

// b70: tile images over this tile's memory (device address) under another GPU address.
inline void CpuAliases(u32 zero_index, const Record& record, u64 now, u32 number) {
    if (record.cpu_addr == 0) {
        return;
    }
    size_t hits = 0;
    const auto check = [&](const char* kind, const Record& other) {
        if (other.index == zero_index || other.cpu_addr == 0 ||
            Overlaps(other, record.gpu_addr, record.size) ||
            !(other.cpu_addr < record.cpu_addr + record.size &&
              record.cpu_addr < other.cpu_addr + other.size)) {
            return;
        }
        if (++hits > MAX_RELATIONS) {
            return;
        }
        LOG_INFO(Render_Vulkan,
                 "TIC_CPU what=zero id={} kind={} other={} gpu={:#x} cpu={:#x} offset={} size={:#x} "
                 "{}x{} levels={} gpu_content={} created={} deleted={} last_sampled_age_s={} n={}",
                 zero_index, kind, other.index, other.gpu_addr, other.cpu_addr,
                 TicTrace::Offset(other.cpu_addr, record.cpu_addr), other.size, other.width,
                 other.height, other.levels, other.gpu_content, other.created, other.deleted,
                 Age(now, other.last_sampled), number);
    };
    for (const auto& [index, other] : live) {
        check("live_image", other);
    }
    for (const Record& other : graveyard) {
        check("graveyard_image", other);
    }
    LOG_INFO(Render_Vulkan, "TIC_CPU end id={} images={} n={}", zero_index, hits, number);
}

inline void ListZero(const Result& result, const Record& record, u64 now, u32 number) {
    LOG_INFO(Render_Vulkan,
             "TILE_ZERO {} gpu_zero_l0_permille={} gpu_zero_all_permille={} guest_zero_permille={} "
             "{} first_sampled_age_s={} last_sampled_age_s={} n={}",
             result.description, result.gpu_zero_l0, result.gpu_zero_all, result.guest_zero,
             CompareText(result), Age(now, record.first_sampled), Age(now, record.last_sampled),
             number);
    LogRecord("TILE_ZERO image", record, now, number, HEAD_EVENTS + TAIL_EVENTS);
    Dump(result, record, "zero", number);
    // b68: the DMA copies that wrote into this tile's memory or read from it (dma_trace.h).
    DmaTrace::ListFor(result.index, record.gpu_addr, record.size, now, Age, number);
    // b69: the draw and clear targets inside this tile's memory and near it (draw_target_trace.h).
    DrawTargetTrace::ListFor(result.index, record.gpu_addr, record.size, now, Age, number);
    // b70: the TIC slots it is sampled through, their entries and writers (tic_trace.h), and the
    // tile images over its memory under another GPU address.
    TicTrace::ListFor("zero", result.index, record.gpu_addr, record.cpu_addr, record.size, now, Age,
                      DescribeAt, number);
    CpuAliases(result.index, record, now, number);
    // b71: the writes into its memory, with the stores' guest state (cpu_write_trace.h).
    CpuWriteTrace::ListFor(result.index, record.cpu_addr, record.size, now, FindDev, ZeroNow, Age,
                           number);
    size_t shown = 0;
    ankerl::unordered_dense::set<u32> seen;
    seen.insert(result.index);
    for (const u32 index : result.aliases) {
        const auto it = live.find(index);
        if (it != live.end() && seen.insert(index).second) {
            Relate("alias", it->second, now, number, result.index, shown);
        }
    }
    for (const u32 index : result.overlaps) {
        const auto it = live.find(index);
        if (it != live.end() && seen.insert(index).second) {
            Relate("bad_overlap", it->second, now, number, result.index, shown);
        }
    }
    for (const auto& [index, other] : live) {
        if (Overlaps(other, record.gpu_addr, record.size) && seen.insert(index).second) {
            Relate("live_same_memory", other, now, number, result.index, shown);
        }
    }
    size_t graves = 0;
    for (auto it = graveyard.rbegin(); it != graveyard.rend() && graves < MAX_RELATIONS; ++it) {
        if (Overlaps(*it, record.gpu_addr, record.size)) {
            ++graves;
            Relate("graveyard_same_memory", *it, now, number, result.index, shown);
        }
    }
    LOG_INFO(Render_Vulkan, "TILE_ZERO end id={} relations={} n={}", result.index, shown, number);
}

inline void FinishReport(u64 now, u32 number) {
    size_t zero_l0 = 0;
    size_t zero_all = 0;
    size_t guest_zero = 0;
    size_t not_downloadable = 0;
    size_t unflagged = 0;
    size_t unflagged_zero_l0 = 0;
    size_t unflagged_guest_zero = 0;
    for (const Result& result : results) {
        if (!result.downloadable) {
            ++not_downloadable;
            continue;
        }
        zero_l0 += result.gpu_zero_l0 >= ZERO_PERMILLE;
        zero_all += result.gpu_zero_all >= ZERO_PERMILLE;
        guest_zero += result.guest_zero >= ZERO_PERMILLE;
        if (!result.flagged) {
            ++unflagged;
            unflagged_zero_l0 += result.gpu_zero_l0 >= ZERO_PERMILLE;
            unflagged_guest_zero += result.guest_zero >= ZERO_PERMILLE;
        }
    }
    LOG_INFO(Render_Vulkan,
             "TILE_READBACK n={} frame={} t={:.1f} sampled_tracked={} checked={} zero_l0={} "
             "zero_all={} guest_zero={} not_downloadable={} live={} graveyard={} unflagged={} "
             "unflagged_zero_l0={} unflagged_guest_zero={}",
             number, now, Seconds(now), pending.size(), results.size() - not_downloadable, zero_l0,
             zero_all, guest_zero, not_downloadable, live.size(), graveyard.size(), unflagged,
             unflagged_zero_l0, unflagged_guest_zero);
    // Per-shape summary, so a zero tile can be compared with the others of its kind.
    struct Shape {
        u32 width, height;
        s32 levels;
        size_t count = 0, zero = 0, guest_zero = 0;
    };
    std::vector<Shape> shapes;
    for (const Result& result : results) {
        const auto it = live.find(result.index);
        if (it == live.end() || !result.downloadable) {
            continue;
        }
        const Record& r = it->second;
        auto shape = std::ranges::find_if(shapes, [&r](const Shape& s) {
            return s.width == r.width && s.height == r.height && s.levels == r.levels;
        });
        if (shape == shapes.end()) {
            shapes.push_back(Shape{r.width, r.height, r.levels});
            shape = shapes.end() - 1;
        }
        ++shape->count;
        shape->zero += result.gpu_zero_l0 >= ZERO_PERMILLE;
        shape->guest_zero += result.guest_zero >= ZERO_PERMILLE;
    }
    for (const Shape& s : shapes) {
        LOG_INFO(Render_Vulkan,
                 "TILE_READBACK shape {}x{} levels={} count={} zero_l0={} guest_zero={} n={}",
                 s.width, s.height, s.levels, s.count, s.zero, s.guest_zero, number);
    }
    // b68: DMA copies into every zero tile (DMA_TRACE); its range goes to TILE_BUF.
    DmaTrace::TileCount dma_count;
    DrawTargetTrace::TileCount draw_count;
    TicTrace::TileCount tic_count;
    for (const Result& result : results) {
        if (!result.downloadable || result.gpu_zero_l0 < ZERO_PERMILLE) {
            continue;
        }
        const auto it = live.find(result.index);
        if (it == live.end()) {
            continue;
        }
        const Record& r = it->second;
        DmaTrace::CountFor(dma_count, result.index, r.gpu_addr, r.cpu_addr, r.size, number);
        DrawTargetTrace::CountFor(draw_count, r.gpu_addr, r.size);
        TicTrace::CountFor(tic_count, result.index, r.gpu_addr);
    }
    DmaTrace::Summary(dma_count, now, Age, number);
    DrawTargetTrace::Summary(draw_count, number);
    TicTrace::Summary(tic_count, number);
    CpuWriteTrace::Summary(number);
    size_t listed = 0;
    const Record* first_zero = nullptr;
    for (const Result& result : results) {
        if (!result.downloadable || result.gpu_zero_l0 < ZERO_PERMILLE) {
            continue;
        }
        const auto it = live.find(result.index);
        if (it == live.end()) {
            continue;
        }
        if (!first_zero) {
            first_zero = &it->second;
        }
        if (listed == MAX_ZERO_LISTED) {
            break;
        }
        ++listed;
        ListZero(result, it->second, now, number);
    }
    size_t controls = 0;
    for (const Result& result : results) {
        if (controls == MAX_CONTROLS || !result.downloadable ||
            result.gpu_zero_l0 >= ZERO_PERMILLE) {
            continue;
        }
        const auto it = live.find(result.index);
        if (it == live.end()) {
            continue;
        }
        const Record& r = it->second;
        if (first_zero && (r.width != first_zero->width || r.height != first_zero->height ||
                           r.levels != first_zero->levels)) {
            continue;
        }
        ++controls;
        LOG_INFO(Render_Vulkan,
                 "TILE_GOOD {} gpu_zero_l0_permille={} gpu_zero_all_permille={} "
                 "guest_zero_permille={} {} n={}",
                 result.description, result.gpu_zero_l0, result.gpu_zero_all, result.guest_zero,
                 CompareText(result), number);
        LogRecord("TILE_GOOD image", r, now, number, HEAD_EVENTS + TAIL_EVENTS);
        // b70: the TIC slots of a good tile, for comparison.
        TicTrace::ListFor("good", result.index, r.gpu_addr, r.cpu_addr, r.size, now, Age,
                          DescribeAt, number);
        if (controls == 1) {
            Dump(result, r, "good", number);
        }
    }
    LOG_INFO(Render_Vulkan, "TILE_READBACK end n={} zero_listed={} zero_not_listed={} controls={}",
             number, listed, zero_l0 - listed, controls);
    // Guest-vs-GPU level 0 over every checked tile: does guest memory hold the baked content?
    size_t compared = 0, match_full = 0, match_none = 0;
    for (const Result& result : results) {
        if (!result.downloadable || !result.compared) {
            continue;
        }
        ++compared;
        match_full += result.match_l0 >= 999;
        match_none += result.match_l0 < 100;
    }
    LOG_INFO(Render_Vulkan,
             "TILE_COMPARE n={} compared={} match_l0_ge999={} match_l0_lt100={}", number, compared,
             match_full, match_none);
    for (const Result& result : results) {
        if (!result.downloadable || !result.compared) {
            continue;
        }
        LOG_INFO(Render_Vulkan,
                 "TILE_COMPARE id={} gpu_zero_l0_permille={} {} guest_zero_permille={} n={}",
                 result.index, result.gpu_zero_l0, CompareText(result), result.guest_zero, number);
    }
    results.clear();
    pending.clear();
    kept_controls = 0;
}

// Called once per frame by the texture cache after it advances frame_tick.
inline void EndFrame(u64 frame_tick) {
    static const auto boot = std::chrono::steady_clock::now();
    TrackedFormat();
    current_frame = frame_tick;
    {
        std::scoped_lock lock{DmaTrace::mutex};
        DmaTrace::frame = frame_tick;
    }
    {
        std::scoped_lock lock{DrawTargetTrace::mutex};
        DrawTargetTrace::frame = frame_tick;
    }
    {
        std::scoped_lock lock{TicTrace::mutex};
        TicTrace::frame = frame_tick;
    }
    if (frame_seconds.size() <= frame_tick) {
        frame_seconds.resize(static_cast<size_t>(frame_tick) + 1, 0.0f);
    }
    frame_seconds[frame_tick] =
        std::chrono::duration<float>(std::chrono::steady_clock::now() - boot).count();
    FlushCpuWrites(frame_tick);
    // b71: the zero permille of earlier frames' written tile bytes, after the stores ran.
    CpuWriteTrace::frame.store(frame_tick, std::memory_order_relaxed);
    CpuWriteTrace::SampleAfter(frame_tick, ZeroNow);
    if (!AliasTrace::collecting) {
        pending.clear();
    }

    static std::chrono::steady_clock::time_point last_log{};
    static std::array<u64, NumCounters> last_values{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last_log < log_interval) {
        return;
    }
    last_log = now;
    if (counters == last_values) {
        return;
    }
    last_values = counters;
    LOG_INFO(Render_Vulkan,
             "TILE_TRACE live={} graveyard={} events={} uploads={} uploads_zero={} "
             "uploads_zero_over_gpu={} gc_deletes={} gc_no_download={} gc_no_download_gpu={} "
             "join_no_copies={} join_no_copies_after_gpu={} join_alias_skips={} join_deletes={} "
             "unmap_deletes={} remaps={} cpu_modified={} downloads={} invalidates={} dropped={} "
             "warnings={} cpuw_gpu={} cpuw_gpu_nce={} cpuw_gpu_inside={} cpuw_gpu_outside={} "
             "cpuw_logged={}",
             live.size(), graveyard.size(), counters[Events], counters[Uploads],
             counters[UploadsZero], counters[UploadsZeroOverGpu], counters[GcDeletes],
             counters[GcDeletesNoDownload], counters[GcDeletesNoDownloadGpu],
             counters[JoinNoCopies], counters[JoinNoCopiesAfterGpu], counters[JoinAliasSkips],
             counters[JoinDeletes], counters[UnmapDeletes], counters[Remaps],
             counters[CpuModifieds], counters[Downloads], counters[Invalidates],
             counters[Dropped], warnings_logged, counters[CpuwGpu], counters[CpuwGpuNce],
             counters[CpuwGpuInside], counters[CpuwGpuOutside], cpuw_logged);
}

} // namespace VideoCommon::TileTrace
#endif
