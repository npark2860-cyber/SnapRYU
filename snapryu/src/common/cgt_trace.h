// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// CGT: CPU-GPU value-link trace (diagnostic).
//
// EDEN_CGT=1 turns it on. Any other value, or none, leaves every hook at one static bool check.
//
// Every hook appends one entry to a byte ring (EDEN_CGT_RING_MB, default 384). An entry is a
// 32-byte EntryHeader, nfield u64 fields, then plen payload bytes padded to 8. A Pad entry
// (type 0, header len only) fills the space before a wrap. seq comes from one counter for entries
// and for operation begin markers (BeginOp), so begin/end pairs of two threads can be checked for
// overlap. Field lists per type are below and in tools/cgt_decode.py.
//
// The ring is written to <log dir>/cgt-<run>-f<frame>-<n>.bin when
//   - the user takes a screenshot (RendererBase::RequestScreenshot): frames
//     [mark - EDEN_CGT_FRAMES (90) - 16, mark + 10],
//   - an ABA candidate is found: frames [mark - 3 - 16, mark + 3], at most EDEN_CGT_ABA_DUMPS
//     (4) per minute and EDEN_CGT_ABA_TOTAL (16) per session; 0 turns these dumps off.
// The 16 extra frames hold the payloads that deduplicated entries refer to (a payload is
// repeated at least every 16 frames). File: FileHeader, sticky entries (config, threads, stream
// buffer), live buffer table, then the ring bytes from the first kept frame.
//
// EDEN_CGT_WATCH_FS (with EDEN_CGT=1): comma list of fragment shader hashes (hex). The pages of the fast
// uniform buffers bound by draws of those shaders are marked cached once (DeviceMemoryManager
// UpdatePagesCachedCount +1, never released, at most WATCH_PAGES_MAX pages), so CPU writes to them take
// the rasterizer write path (with EDEN_NCE_WRITE_STEP=1 under NCE: one CpuStore entry per store, with
// the bytes). WatchPage records each page when it is marked. Submit (guest SubmitGPFIFO, before the
// push) and GpEntry (GPU thread, each GPFIFO entry before its commands run, with a hash of its words)
// place the draws' command lists in time.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/cityhash.h"
#include "common/common_types.h"
#include "common/fs/file.h"
#include "common/fs/path_util.h"
#include "common/logging.h"

namespace Common::CGT {

enum class Type : u8 {
    Pad = 0,
    Config = 1,          // [version, ring_bytes, before, after, prefix, run_id] text
    Frame = 2,           // [frame, tick, known_tick]
    CpuStore = 3,        // [va, daddr, size, op, mode, core] CPU_STORE_DATA
    Collect = 4,         // [va, daddr, size, mode, core] (HandleRasterizerWrite outside a store)
    Gather = 5,          // [gather_id, site, daddr, size] GUEST_READ <= 64 bytes
    NceFault = 6,        // [page, size, fault_addr, pc, insn, has_ctx]
    RegionInvalidate = 7, // [daddr, size, preserve, site]
    BcWriteMemory = 8,   // [daddr, size, was_gpu_modified, site, gather_site]
    UploadSrc = 9,       // [op, buffer, dst_offset, daddr, size, staging, staging_offset, hash, ref]
    DownloadPublish = 10, // [op, daddr, size] first 64 bytes
    BufferCreate = 11,   // [handle, daddr, size, serial]
    BufferDelete = 12,   // [handle, daddr, size, serial]
    GpuWriteMark = 13,   // [handle, daddr, size, tick, serial]
    StagingRequest = 14, // [handle, offset, size, kind, usage, deferred, tick, known_tick]
    UbFast = 15,         // [op, slot, binding, daddr, size, handle, offset, staging_seq,
                         //  alignment_stream, hash, ref] STREAM_WRITTEN
    UbClassic = 16,      // [slot, binding, daddr, size, handle, offset, serial, bits]
    Ssbo = 17,           // [slot, daddr, size, handle, offset, serial, written]
    IndexBuffer = 18,    // [daddr, size, handle, offset, serial, inline, first, count, format_bytes]
    VertexBuffer = 19,   // [index, daddr, size, handle, offset, serial, stride]
    DrawOpen = 20,       // [draw_id, frame]
    DrawKey = 21,        // [draw_id, vs, fs, framebuffer, extent, indexed, topology, count, first,
                         //  base_vertex, base_instance, instances, tick, occurrence, aba_key]
    DrawNote = 23,       // [kind, count, instances, execute, vs_pc, fs_pc]
    CondDecision = 24,   // [gpu_va, mode, override, execute, words_read] 4 words
    CondWait = 25,       // [op, daddr, mode, bits, wait_ns] 8 words before, 8 words after
    SyncPath = 26,       // [code, generation]
    FenceWait = 27,      // [site, require_completion, callback_done, wait_ns]
    SnapshotReq = 28,    // [snap_id, draw_id, tick, regions, dst_offset, total] SnapRegion table
    SnapshotData = 29,   // [snap_id, region, draw_id, size, tick, coherent, hash, ref] GPU_SNAPSHOT
    Mark = 30,           // [reason, frame]
    AbaCandidate = 31,   // [draw_id, aba_key, slot, row, value_a, value_b, frame]
    ThreadInfo = 33,     // [id, os_hash]
    StreamInfo = 34,     // [handle, size, coherent, regions]
    GatherBegin = 35,    // [gather_id, site]
    GatherEnd = 36,      // [gather_id, site, ranges, bytes]
    IndirectBuffer = 37, // [which, daddr, size, handle, offset, serial]
    GpuOwnedSkip = 38,   // [handle, daddr, size]
    BufferLive = 39,     // [handle, daddr, size, serial]
    SnapshotSkip = 40,   // [draw_id, reason, total]
    CbData = 41,         // [gpu_va, daddr, size, op] bytes
    ReportRequest = 42,  // [gpu_va, daddr, query_type, payload, flags, location]
    ReportPublish = 43,  // [op, daddr, size, flags, site, location, request] bytes written
    WatchPage = 44,      // [page, size, draw_id, fs, frame] size 0: page limit reached, not marked
    Submit = 45,         // [channel, lists, first_addr, first_words, total_words, fence_id,
                         //  fence_value, flags] guest SubmitGPFIFO, before the entries are pushed
    GpEntry = 46,        // [addr, words, hash, sync, index, lists] GPU thread, before the commands run
};

namespace Flag {
inline constexpr u32 Truncated = 1U << 0;
inline constexpr u32 Dedup = 1U << 2; // payload omitted: field "ref" is the seq holding the bytes
inline constexpr u32 Zero = 1U << 3;  // ZeroBlock store, no payload
inline constexpr u32 NoNotify = 1U << 4;
inline constexpr u32 Exclusive = 1U << 5;
inline constexpr u32 ExclusiveFailed = 1U << 6;
inline constexpr u32 Block = 1U << 7;
} // namespace Flag

// Thread context of a cache write (BcWriteMemory site).
enum class Site : u8 { None = 0, CpuWrite = 1, Gather = 2, Nce = 3, FlushCaching = 4 };
// Caller of GPU::InvalidateGPUCache (deferred CPU write application).
enum class GatherSite : u8 {
    Unknown = 0,
    FenceSignal = 1,      // fence_manager.h SignalFenceImpl end
    WaitRefresh = 2,      // fence_manager.h WaitPendingFences after the forced wait
    TextureDataCache = 3, // maxwell_3d.cpp invalidate_texture_data_cache
    MemOpB = 4,           // puller.cpp MemOpB
};
// HandleRasterizerWrite outcome.
enum class RasterMode : u8 {
    None = 0,       // no device mapping
    SamePage = 1,   // collected through the same-page shortcut
    Collected = 2,  // OnCPUWrite true (GPU-modified): collected, applied at the next gather
    Immediate = 3,  // OnCPUWrite false: caches invalidated now
    Skipped = 4,    // address or size 0
};
enum class BindKind : u8 { UbFast = 1, UbClassic = 2, Ssbo = 3, Vertex = 4, Index = 5, Indirect = 6 };

struct EntryHeader {
    u32 len; // whole entry, multiple of 8
    u8 type;
    u8 thread;
    u8 nfield;
    u8 reserved;
    u32 flags;
    u32 plen;
    u64 seq;
    u64 t_ns;
};
static_assert(sizeof(EntryHeader) == 32);

struct FileHeader {
    std::array<char, 4> magic; // "CGT1"
    u32 version;
    u32 header_bytes;
    u32 reason; // 1 user mark, 2 ABA candidate
    u64 mark_frame;
    u64 first_frame_requested;
    u64 first_frame_actual;
    u64 last_frame;
    u64 lost_first_seq; // both 0 when nothing in the requested window was overwritten
    u64 lost_last_seq;
    u64 sticky_bytes;
    u64 live_bytes;
    u64 stream_bytes;
    u64 ring_bytes;
    u64 prefix_frames;
    u64 run_id;
};
static_assert(sizeof(FileHeader) == 112);

struct SnapRegion {
    u64 kind_slot; // kind << 16 | stage << 8 | index
    u64 src;
    u64 src_offset;
    u64 size;
    u64 dst_offset;
    u64 daddr;
};

struct DrawBinding {
    BindKind kind;
    u32 stage;
    u32 index;
    u64 handle;
    u64 offset;
    u64 size;
    u64 daddr;
};

[[nodiscard]] inline bool Enabled() noexcept {
#if SNAPRYU_DIAGNOSTICS
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_CGT");
        return value != nullptr && std::string_view{value} == "1";
    }();
    return enabled;
#else
    return false;
#endif
}

#if SNAPRYU_DIAGNOSTICS
namespace Detail {
inline thread_local u8 tl_thread = 0;
inline thread_local u8 tl_site = 0;
inline thread_local u8 tl_gather_site = 0;
inline thread_local u64 tl_store_op = 0;
inline thread_local u64 tl_raster_daddr = 0;
inline thread_local u64 tl_raster_mode = 0;
inline thread_local u64 tl_raster_core = 0;
inline thread_local bool tl_nce_valid = false;
inline thread_local u64 tl_nce_fault = 0;
inline thread_local u64 tl_nce_pc = 0;
inline thread_local u64 tl_nce_insn = 0;
inline thread_local u64 tl_staging_handle = 0;
inline thread_local u64 tl_staging_offset = 0;
inline thread_local u64 tl_staging_seq = 0;
inline thread_local u64 tl_gather_id = 0;
inline thread_local u64 tl_gather_ranges = 0;
inline thread_local u64 tl_gather_bytes = 0;

[[nodiscard]] inline u64 EnvU64(const char* name, u64 fallback) {
    const char* const value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 0);
    return end != nullptr && *end == '\0' ? static_cast<u64>(parsed) : fallback;
}

[[nodiscard]] constexpr u64 Mix(u64 h, u64 v) noexcept {
    h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    return h * 0xFF51AFD7ED558CCDULL;
}

[[nodiscard]] inline u64 Hash(std::span<const u8> bytes) {
    return Common::CityHash64(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

[[nodiscard]] constexpr u64 AlignUp8(u64 value) noexcept {
    return (value + 7) & ~u64{7};
}
} // namespace Detail

[[nodiscard]] inline u64 NowNs() noexcept {
#if SNAPRYU_DIAGNOSTICS
    static const auto start = std::chrono::steady_clock::now();
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count());
#else
    return 0;
#endif
}

template <typename H>
[[nodiscard]] inline u64 HandleBits(H handle) noexcept {
    if constexpr (std::is_pointer_v<H>) {
        return static_cast<u64>(reinterpret_cast<std::uintptr_t>(handle));
    } else {
        return static_cast<u64>(handle);
    }
}

#if SNAPRYU_DIAGNOSTICS
class SiteScope {
public:
    explicit SiteScope(Site site) noexcept : previous{Detail::tl_site} {
        Detail::tl_site = static_cast<u8>(site);
    }
    ~SiteScope() {
        Detail::tl_site = previous;
    }
    SiteScope(const SiteScope&) = delete;
    SiteScope& operator=(const SiteScope&) = delete;

private:
    u8 previous;
};
#else
class SiteScope {
public:
    explicit SiteScope(Site) noexcept {}
    SiteScope(const SiteScope&) = delete;
    SiteScope& operator=(const SiteScope&) = delete;
};
#endif

#if SNAPRYU_DIAGNOSTICS
class GatherSiteScope {
public:
    explicit GatherSiteScope(GatherSite site) noexcept : previous{Detail::tl_gather_site} {
        Detail::tl_gather_site = static_cast<u8>(site);
    }
    ~GatherSiteScope() {
        Detail::tl_gather_site = previous;
    }
    GatherSiteScope(const GatherSiteScope&) = delete;
    GatherSiteScope& operator=(const GatherSiteScope&) = delete;

private:
    u8 previous;
};
#else
class GatherSiteScope {
public:
    explicit GatherSiteScope(GatherSite) noexcept {}
    GatherSiteScope(const GatherSiteScope&) = delete;
    GatherSiteScope& operator=(const GatherSiteScope&) = delete;
};
#endif

class State {
public:
    static constexpr u64 PREFIX_FRAMES = 16; // a deduplicated payload repeats at least this often
    static constexpr u32 STORE_PAYLOAD_MAX = 4096;
    static constexpr u32 UB_PAYLOAD_MAX = 64 * 1024;
    static constexpr u32 GUEST_READ_MAX = 64;
    static constexpr u32 ABA_ROWS = 64; // first 1 KiB of each fast uniform buffer
    static constexpr size_t ABA_ROW_LIMIT = 1U << 20;
    static constexpr size_t DEDUP_LIMIT = 1U << 18;

    State() {
        const u64 ring_mb = std::clamp<u64>(Detail::EnvU64("EDEN_CGT_RING_MB", 384), 64, 4096);
        cap = ring_mb << 20;
        ring.reset(new u8[cap]);
        before = std::clamp<u64>(Detail::EnvU64("EDEN_CGT_FRAMES", 90), 1, 1000);
        snap_max = std::clamp<u64>(Detail::EnvU64("EDEN_CGT_SNAP_MAX", 16384), 256, 1U << 20);
        snap_bytes =
            std::clamp<u64>(Detail::EnvU64("EDEN_CGT_SNAP_MB", 64), 4, 1024) << 20;
        aba_per_minute = std::min<u64>(Detail::EnvU64("EDEN_CGT_ABA_DUMPS", 4), 60);
        aba_total_limit = std::min<u64>(Detail::EnvU64("EDEN_CGT_ABA_TOTAL", 16), 1000);
        run_id = static_cast<u64>(std::time(nullptr));
        ParseSnapshotList();
        ParseWatchList();
        LOG_WARNING(Render_Vulkan,
                    "CGT_CONFIG enabled=1 ring_mb={} frames={} after={} prefix={} run={} "
                    "snapshot_pairs={} snap_max={} snap_mb={} aba_dumps_per_minute={} "
                    "aba_dumps_total={} reports=1",
                    ring_mb, before, AFTER_USER, PREFIX_FRAMES, run_id, snapshot_pairs.size(),
                    snap_max, snap_bytes >> 20, aba_per_minute, aba_total_limit);
        std::string watch_text;
        for (const u64 fs : watch_fs) {
            watch_text += fmt::format("{}{:016x}", watch_text.empty() ? "" : ",", fs);
        }
        LOG_WARNING(Render_Vulkan, "CGT_WATCH fs={} pages_max={}",
                    watch_text.empty() ? std::string{"-"} : watch_text, WATCH_PAGES_MAX);
    }

    // ---- ring ----

    u64 Emit(Type type, u32 flags, std::initializer_list<u64> fields, const void* payload = nullptr,
             u32 plen = 0) {
        const u8 thread = ThreadId();
        std::scoped_lock lk{mutex};
        return EmitLocked(thread, type, flags, fields.begin(), static_cast<u32>(fields.size()),
                          payload, plen, nullptr);
    }

    u64 NextSeq() noexcept {
        return seq_counter.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    void Sticky(Type type, std::initializer_list<u64> fields, const void* payload = nullptr,
                u32 plen = 0) {
        const u8 thread = ThreadId();
        std::scoped_lock lk{mutex};
        AppendEntry(sticky, thread, type, 0, NextSeq(), fields.begin(),
                    static_cast<u32>(fields.size()), payload, plen);
    }

    // ---- frames and dumps (frame on the GPU thread, marks from any thread) ----

    void Frame(u64 tick, u64 known_tick) {
        const u8 thread = ThreadId();
        {
            std::scoped_lock lk{aux_mutex};
            occurrence.clear();
        }
        std::scoped_lock lk{mutex};
        ++frame;
        u64 pos = 0;
        const u64 fields[] = {frame, tick, known_tick};
        const u64 seq = EmitLocked(thread, Type::Frame, 0, fields, 3, nullptr, 0, &pos);
        frames.push_back({frame, pos, seq});
        while (frames.size() > 8192) {
            frames.pop_front();
        }
        if (dump.pending && frame >= dump.end) {
            DumpLocked();
        }
        const u64 now = NowNs();
        if (now - last_stats_ns >= 60'000'000'000ULL) {
            last_stats_ns = now;
            LogStatsLocked();
        }
    }

    void Mark(u32 reason) {
        const u8 thread = ThreadId();
        std::scoped_lock lk{mutex};
        const u64 fields[] = {reason, frame};
        EmitLocked(thread, Type::Mark, 0, fields, 2, nullptr, 0, nullptr);
        ScheduleDumpLocked(reason, reason == 1 ? before : AFTER_ABA,
                           reason == 1 ? AFTER_USER : AFTER_ABA);
    }

    // ---- CPU stores ----

    u64 BeginStore() noexcept {
        const u64 op = NextSeq();
        Detail::tl_store_op = op;
        Detail::tl_raster_daddr = 0;
        Detail::tl_raster_mode = 0;
        Detail::tl_raster_core = 0;
        return op;
    }

    void EndStore(u64 op, u64 va, const void* data, u64 size, u32 flags) {
        Detail::tl_store_op = 0;
        u32 plen = 0;
        if (data != nullptr) {
            plen = static_cast<u32>(std::min<u64>(size, STORE_PAYLOAD_MAX));
            if (plen < size) {
                flags |= Flag::Truncated;
            }
        }
        Emit(Type::CpuStore, flags,
             {va, Detail::tl_raster_daddr, size, op, Detail::tl_raster_mode,
              Detail::tl_raster_core},
             data, plen);
    }

    void RasterWrite(u64 va, u64 daddr, u64 size, RasterMode mode, u64 core) {
        if (Detail::tl_store_op != 0) {
            if (Detail::tl_raster_mode == 0) {
                Detail::tl_raster_daddr = daddr;
                Detail::tl_raster_mode = static_cast<u64>(mode);
                Detail::tl_raster_core = core;
            }
            return;
        }
        Emit(Type::Collect, 0, {va, daddr, size, static_cast<u64>(mode), core});
    }

    // ---- GPU thread binding context ----

    u64 DrawOpen() {
        draw_bindings.clear();
        aba_pending.clear();
        draw_open = true;
        draw_id = ++draw_counter;
        Emit(Type::DrawOpen, 0, {draw_id, frame});
        return draw_id;
    }

    void DrawFinish() {
        draw_open = false;
    }

    [[nodiscard]] const std::vector<DrawBinding>& Bindings() const noexcept {
        return draw_bindings;
    }

    void AddBinding(BindKind kind, u32 stage, u32 index, u64 handle, u64 offset, u64 size,
                    u64 daddr) {
        if (draw_open) {
            draw_bindings.push_back({kind, stage, index, handle, offset, size, daddr});
        }
    }

    u64 Serial(u64 handle) {
        std::scoped_lock lk{mutex};
        const auto it = live.find(handle);
        return it != live.end() ? it->second.serial : 0;
    }

    void BufferCreate(u64 handle, u64 daddr, u64 size) {
        const u8 thread = ThreadId();
        std::scoped_lock lk{mutex};
        const u64 serial = ++buffer_serial;
        live[handle] = LiveBuffer{daddr, size, serial};
        const u64 fields[] = {handle, daddr, size, serial};
        EmitLocked(thread, Type::BufferCreate, 0, fields, 4, nullptr, 0, nullptr);
    }

    void BufferDelete(u64 handle, u64 daddr, u64 size) {
        const u8 thread = ThreadId();
        std::scoped_lock lk{mutex};
        u64 serial = 0;
        if (const auto it = live.find(handle); it != live.end()) {
            serial = it->second.serial;
            live.erase(it);
        }
        const u64 fields[] = {handle, daddr, size, serial};
        EmitLocked(thread, Type::BufferDelete, 0, fields, 4, nullptr, 0, nullptr);
    }

    // Emits an entry whose payload is replaced by a reference when the same key carried the same
    // bytes within the last PREFIX_FRAMES frames. fields must end with {hash, ref} placeholders;
    // they are filled here.
    u64 EmitDedup(Type type, u32 flags, u64 key, std::vector<u64> fields, std::span<const u8> bytes,
                  u32 max_bytes) {
        const u32 plen = static_cast<u32>(std::min<size_t>(bytes.size(), max_bytes));
        if (plen < bytes.size()) {
            flags |= Flag::Truncated;
        }
        const u64 hash = Detail::Hash(bytes.first(plen));
        std::scoped_lock aux{aux_mutex};
        auto& last = dedup[key];
        const bool reuse = last.seq != 0 && last.hash == hash && last.frame + PREFIX_FRAMES > frame;
        fields[fields.size() - 2] = hash;
        fields[fields.size() - 1] = reuse ? last.seq : 0;
        const u8 thread = ThreadId();
        std::scoped_lock lk{mutex};
        const u64 seq = EmitLocked(thread, type, reuse ? (flags | Flag::Dedup) : flags,
                                   fields.data(), static_cast<u32>(fields.size()),
                                   reuse ? nullptr : bytes.data(), reuse ? 0 : plen, nullptr);
        if (reuse) {
            ++dedup_hits;
        } else {
            last = DedupState{hash, seq, frame};
            if (dedup.size() > DEDUP_LIMIT) {
                dedup.clear();
            }
        }
        return hash;
    }

    void UbFast(u64 op, u32 stage, u32 index, u32 binding, u64 daddr, std::span<const u8> bytes,
                bool alignment_stream) {
        const u64 handle = Detail::tl_staging_handle;
        const u64 offset = Detail::tl_staging_offset;
        const u64 staging_seq = Detail::tl_staging_seq;
        const u64 slot = (u64{stage} << 8) | index;
        AddBinding(BindKind::UbFast, stage, index, handle, offset, bytes.size(), daddr);
        const u64 key = Detail::Mix(Detail::Mix(Detail::Mix(1, slot), daddr), bytes.size());
        const u64 hash = EmitDedup(Type::UbFast, 0, key,
                                   {op, slot, binding, daddr, bytes.size(), handle, offset,
                                    staging_seq, alignment_stream ? 1ULL : 0ULL, 0, 0},
                                   bytes, UB_PAYLOAD_MAX);
        if (!draw_open) {
            return;
        }
        AbaPending pending{};
        pending.slot = slot;
        pending.content = hash;
        pending.rows = static_cast<u32>(std::min<size_t>(bytes.size() / 16, ABA_ROWS));
        for (u32 row = 0; row < pending.rows; ++row) {
            u64 lo{};
            u64 hi{};
            std::memcpy(&lo, bytes.data() + row * 16, 8);
            std::memcpy(&hi, bytes.data() + row * 16 + 8, 8);
            pending.values[row] = Detail::Mix(lo, hi);
        }
        aba_pending.push_back(pending);
    }

    // ---- draw key and ABA (GPU thread) ----

    void DrawKey(u64 id, u64 vs, u64 fs, u64 framebuffer, u64 extent, u64 indexed, u64 topology,
                 u64 count, u64 first, u64 base_vertex, u64 base_instance, u64 instances,
                 u64 tick) {
        u64 base = Detail::Mix(vs, fs);
        for (const u64 value :
             {extent, indexed, topology, count, first, base_vertex, base_instance, instances}) {
            base = Detail::Mix(base, value);
        }
        std::vector<std::array<u64, 7>> candidates;
        u64 occ = 0;
        u64 aba_key = 0;
        {
            std::scoped_lock aux{aux_mutex};
            occ = ++occurrence[base];
            aba_key = Detail::Mix(base, occ);
            for (const AbaPending& pending : aba_pending) {
                CheckAbaLocked(id, aba_key, pending, candidates);
            }
        }
        Emit(Type::DrawKey, 0,
             {id, vs, fs, framebuffer, extent, indexed, topology, count, first, base_vertex,
              base_instance, instances, tick, occ, aba_key});
        if (candidates.empty()) {
            return;
        }
        const u8 thread = ThreadId();
        std::scoped_lock lk{mutex};
        for (const auto& c : candidates) {
            EmitLocked(thread, Type::AbaCandidate, 0, c.data(), 7, nullptr, 0, nullptr);
        }
        aba_candidates += candidates.size();
        const u64 total = aba_candidates;
        if (total <= 8 || (total & (total - 1)) == 0) {
            LOG_WARNING(Render_Vulkan,
                        "CGT_ABA candidates={} frame={} draw={} vs={:016x} fs={:016x} slot={:#x} "
                        "row={}",
                        total, frame, id, vs, fs, candidates.front()[2], candidates.front()[3]);
        }
        const u64 now = NowNs();
        while (!aba_dump_times.empty() && now - aba_dump_times.front() > 60'000'000'000ULL) {
            aba_dump_times.pop_front();
        }
        if (aba_dump_times.size() < aba_per_minute && aba_dumps < aba_total_limit &&
            !dump.pending) {
            aba_dump_times.push_back(now);
            ++aba_dumps;
            const u64 fields[] = {2, frame};
            EmitLocked(thread, Type::Mark, 0, fields, 2, nullptr, 0, nullptr);
            ScheduleDumpLocked(2, AFTER_ABA, AFTER_ABA);
        }
    }

    // ---- watched uniform buffer pages (EDEN_CGT_WATCH_FS) ----

    [[nodiscard]] bool WatchSelected(u64 fs) const noexcept {
        return std::find(watch_fs.begin(), watch_fs.end(), fs) != watch_fs.end();
    }

    // GPU thread only. True the first time a page is seen while below the limit: the caller then
    // marks the page cached so CPU writes to it go through the rasterizer write path.
    bool WatchPage(u64 page, u64 id, u64 fs) {
        if (watch_pages.contains(page)) {
            return false;
        }
        if (watch_pages.size() >= WATCH_PAGES_MAX) {
            if (!watch_full) {
                watch_full = true;
                Emit(Type::WatchPage, 0, {page, 0, id, fs, frame});
            }
            return false;
        }
        watch_pages.insert(page);
        Emit(Type::WatchPage, 0, {page, WATCH_PAGE_BYTES, id, fs, frame});
        return true;
    }

    // ---- snapshots ----

    [[nodiscard]] bool SnapshotSelected(u64 vs, u64 fs) const noexcept {
        for (const auto& [want_vs, want_fs] : snapshot_pairs) {
            if (want_vs == vs && (want_fs == ~u64{0} || want_fs == fs)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] u64 SnapshotMaxRegion() const noexcept {
        return snap_max;
    }

    [[nodiscard]] u64 SnapshotBufferBytes() const noexcept {
        return snap_bytes;
    }

    u64 NextSnapshotId() noexcept {
        return ++snapshot_counter;
    }

    void SnapshotData(u64 snap_id, u64 region, u64 id, const SnapRegion& r, u64 tick, bool coherent,
                      std::span<const u8> bytes) {
        const u64 key = Detail::Mix(Detail::Mix(Detail::Mix(Detail::Mix(2, r.kind_slot), r.src),
                                                r.src_offset),
                                    r.size);
        EmitDedup(Type::SnapshotData, 0, key,
                  {snap_id, region, id, bytes.size(), tick, coherent ? 1ULL : 0ULL, 0, 0}, bytes,
                  static_cast<u32>(snap_max));
    }

    void Config(std::string_view text) {
        const u8 thread = ThreadId();
        std::scoped_lock lk{mutex};
        const u64 fields[] = {1, cap, before, AFTER_USER, PREFIX_FRAMES, run_id};
        AppendEntry(sticky, thread, Type::Config, 0, NextSeq(), fields, 6, text.data(),
                    static_cast<u32>(text.size()));
    }

private:
    static constexpr u64 AFTER_USER = 10;
    static constexpr u64 WATCH_PAGES_MAX = 256;
    static constexpr u64 WATCH_PAGE_BYTES = 4096;
    static constexpr u64 AFTER_ABA = 3;

    struct FrameMark {
        u64 frame;
        u64 pos;
        u64 seq;
    };
    struct LiveBuffer {
        u64 daddr;
        u64 size;
        u64 serial;
    };
    struct DedupState {
        u64 hash;
        u64 seq;
        u64 frame;
    };
    struct RowState {
        u64 cur;
        u64 prev;
        u32 cur_count;
        u32 prev_count;
    };
    struct AbaState {
        u64 content = 0;
        u32 repeat = 0;
        std::vector<RowState> rows;
    };
    struct AbaPending {
        u64 slot;
        u64 content;
        u32 rows;
        std::array<u64, ABA_ROWS> values;
    };
    struct DumpRequest {
        bool pending = false;
        u32 reason = 0;
        u64 first = 0;
        u64 end = 0;
        u64 mark_frame = 0;
    };

    u8 ThreadId() {
        if (Detail::tl_thread == 0) {
            const u32 id = next_thread.fetch_add(1, std::memory_order_relaxed) + 1;
            Detail::tl_thread = static_cast<u8>(std::min<u32>(id, 255));
            const u64 os = std::hash<std::thread::id>{}(std::this_thread::get_id());
            const u64 fields[] = {id, os};
            std::scoped_lock lk{mutex};
            AppendEntry(sticky, Detail::tl_thread, Type::ThreadInfo, 0, NextSeq(), fields, 2,
                        nullptr, 0);
        }
        return Detail::tl_thread;
    }

    static void AppendEntry(std::vector<u8>& out, u8 thread, Type type, u32 flags, u64 seq,
                            const u64* fields, u32 nfield, const void* payload, u32 plen) {
        const u64 body = sizeof(EntryHeader) + u64{nfield} * 8 + Detail::AlignUp8(plen);
        const size_t at = out.size();
        out.resize(at + body);
        WriteEntry(out.data() + at, body, thread, type, flags, seq, fields, nfield, payload, plen);
    }

    static void WriteEntry(u8* dst, u64 body, u8 thread, Type type, u32 flags, u64 seq,
                           const u64* fields, u32 nfield, const void* payload, u32 plen) {
        const EntryHeader header{
            .len = static_cast<u32>(body),
            .type = static_cast<u8>(type),
            .thread = thread,
            .nfield = static_cast<u8>(nfield),
            .reserved = 0,
            .flags = flags,
            .plen = plen,
            .seq = seq,
            .t_ns = NowNs(),
        };
        std::memcpy(dst, &header, sizeof(header));
        if (nfield != 0) {
            std::memcpy(dst + sizeof(header), fields, size_t{nfield} * 8);
        }
        u8* const p = dst + sizeof(header) + size_t{nfield} * 8;
        if (plen != 0) {
            std::memcpy(p, payload, plen);
        }
        const u64 padded = Detail::AlignUp8(plen);
        if (padded != plen) {
            std::memset(p + plen, 0, padded - plen);
        }
    }

    u64 EmitLocked(u8 thread, Type type, u32 flags, const u64* fields, u32 nfield,
                   const void* payload, u32 plen, u64* out_pos) {
        const u64 body = sizeof(EntryHeader) + u64{nfield} * 8 + Detail::AlignUp8(plen);
        u64 off = write_pos % cap;
        if (off + body > cap) {
            const u32 pad = static_cast<u32>(cap - off);
            std::memcpy(&ring[off], &pad, sizeof(pad));
            ring[off + 4] = static_cast<u8>(Type::Pad);
            write_pos += pad;
            off = 0;
        }
        const u64 seq = NextSeq();
        WriteEntry(&ring[off], body, thread, type, flags, seq, fields, nfield, payload, plen);
        if (out_pos != nullptr) {
            *out_pos = write_pos;
        }
        write_pos += body;
        ++records;
        return seq;
    }

    void ScheduleDumpLocked(u32 reason, u64 before_frames, u64 after_frames) {
        const u64 back = before_frames + PREFIX_FRAMES;
        const u64 first = frame > back ? frame - back : 1;
        const u64 end = frame + after_frames;
        if (!dump.pending) {
            dump = DumpRequest{true, reason, first, end, frame};
        } else {
            dump.first = std::min(dump.first, first);
            dump.end = std::max(dump.end, end);
            if (reason == 1) {
                dump.reason = 1;
                dump.mark_frame = frame;
            }
        }
        LOG_WARNING(Render_Vulkan, "CGT_MARK reason={} frame={} dump_frames={}..{}", reason, frame,
                    dump.first, dump.end);
    }

    void DumpLocked() {
        const DumpRequest request = dump;
        dump = DumpRequest{};
        const u64 oldest = write_pos > cap ? write_pos - cap : 0;
        u64 requested_seq = 0;
        const FrameMark* start = nullptr;
        for (const FrameMark& mark : frames) {
            if (mark.frame == request.first) {
                requested_seq = mark.seq;
            }
            if (mark.frame >= request.first && mark.pos >= oldest) {
                start = &mark;
                break;
            }
        }
        if (start == nullptr) {
            ++failed_dumps;
            LOG_ERROR(Render_Vulkan, "CGT_DUMP failed: no frame of {}..{} left in the ring",
                      request.first, request.end);
            return;
        }
        FileHeader header{};
        header.magic = {'C', 'G', 'T', '1'};
        header.version = 1;
        header.header_bytes = sizeof(FileHeader);
        header.reason = request.reason;
        header.mark_frame = request.mark_frame;
        header.first_frame_requested = request.first;
        header.first_frame_actual = start->frame;
        header.last_frame = frame;
        if (start->frame > request.first) {
            header.lost_first_seq = requested_seq != 0 ? requested_seq : 1;
            header.lost_last_seq = start->seq - 1;
            ++lossy_dumps;
        }
        std::vector<u8> live_bytes;
        for (const auto& [handle, buffer] : live) {
            const u64 fields[] = {handle, buffer.daddr, buffer.size, buffer.serial};
            AppendEntry(live_bytes, 0, Type::BufferLive, 0, 0, fields, 4, nullptr, 0);
        }
        header.sticky_bytes = sticky.size();
        header.live_bytes = live_bytes.size();
        header.stream_bytes = write_pos - start->pos;
        header.ring_bytes = cap;
        header.prefix_frames = PREFIX_FRAMES;
        header.run_id = run_id;
        const std::string name =
            fmt::format("cgt-{}-f{}-{}.bin", run_id, request.mark_frame, ++dump_count);
        const auto path = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / name;
        Common::FS::IOFile file{path, Common::FS::FileAccessMode::Write,
                                Common::FS::FileType::BinaryFile};
        if (!file.IsOpen()) {
            ++failed_dumps;
            LOG_ERROR(Render_Vulkan, "CGT_DUMP failed to open {}", name);
            return;
        }
        const auto write = [&file](const void* data, size_t size) {
            return file.WriteSpan(std::span<const u8>(static_cast<const u8*>(data), size)) == size;
        };
        bool ok = write(&header, sizeof(header)) && write(sticky.data(), sticky.size()) &&
                  write(live_bytes.data(), live_bytes.size());
        const u64 begin = start->pos % cap;
        const u64 end = write_pos % cap;
        if (header.stream_bytes != 0) {
            if (begin < end) {
                ok = ok && write(&ring[begin], end - begin);
            } else {
                ok = ok && write(&ring[begin], cap - begin) && write(&ring[0], end);
            }
        }
        LOG_WARNING(Render_Vulkan,
                    "CGT_DUMP file={} ok={} reason={} mark_frame={} frames={}..{} requested_first={} "
                    "bytes={} lost_seq={}..{}",
                    name, ok, request.reason, request.mark_frame, start->frame, frame,
                    request.first, header.stream_bytes, header.lost_first_seq,
                    header.lost_last_seq);
    }

    void LogStatsLocked() {
        const u64 oldest = write_pos > cap ? write_pos - cap : 0;
        u64 ring_frames = 0;
        for (const FrameMark& mark : frames) {
            if (mark.pos >= oldest) {
                ++ring_frames;
            }
        }
        LOG_WARNING(Render_Vulkan,
                    "CGT_STATS frame={} records={} written_mb={} ring_frames={} dumps={} "
                    "lossy_dumps={} failed_dumps={} aba={} dedup_hits={} snapshots={}",
                    frame, records, write_pos >> 20, ring_frames, dump_count, lossy_dumps,
                    failed_dumps, aba_candidates, dedup_hits,
                    snapshot_counter.load(std::memory_order_relaxed));
    }

    // A A B A on a 16-byte row (A seen at least twice, B once) marks a candidate.
    void CheckAbaLocked(u64 id, u64 aba_key, const AbaPending& pending,
                        std::vector<std::array<u64, 7>>& out) {
        const u64 key = Detail::Mix(aba_key, pending.slot);
        auto it = aba.find(key);
        if (it == aba.end()) {
            if (aba_rows + pending.rows > ABA_ROW_LIMIT) {
                return;
            }
            AbaState state;
            state.content = pending.content;
            state.repeat = 0;
            state.rows.resize(pending.rows);
            for (u32 row = 0; row < pending.rows; ++row) {
                state.rows[row] = RowState{pending.values[row], 0, 1, 0};
            }
            aba_rows += pending.rows;
            aba.emplace(key, std::move(state));
            return;
        }
        AbaState& state = it->second;
        if (state.content == pending.content) {
            ++state.repeat;
            return;
        }
        const u32 rows = std::min<u32>(pending.rows, static_cast<u32>(state.rows.size()));
        for (u32 row = 0; row < rows; ++row) {
            RowState& r = state.rows[row];
            const u32 cur_total = r.cur_count + state.repeat;
            const u64 value = pending.values[row];
            if (value == r.cur) {
                r.cur_count = cur_total + 1;
                continue;
            }
            if (value == r.prev && r.prev_count >= 2 && cur_total == 1) {
                out.push_back({id, aba_key, pending.slot, row, r.prev, r.cur, frame});
            }
            r.prev = r.cur;
            r.prev_count = cur_total;
            r.cur = value;
            r.cur_count = 1;
        }
        for (u32 row = rows; row < state.rows.size(); ++row) {
            state.rows[row].cur_count += state.repeat + 1;
        }
        state.content = pending.content;
        state.repeat = 0;
    }

    void ParseWatchList() {
        const char* const value = std::getenv("EDEN_CGT_WATCH_FS");
        if (value == nullptr) {
            return;
        }
        std::string_view rest{value};
        while (!rest.empty()) {
            const size_t comma = rest.find(',');
            const std::string text{rest.substr(0, comma)};
            if (!text.empty()) {
                char* end = nullptr;
                const u64 fs = std::strtoull(text.c_str(), &end, 16);
                if (end != text.c_str() && *end == '\0') {
                    watch_fs.push_back(fs);
                }
            }
            if (comma == std::string_view::npos) {
                break;
            }
            rest.remove_prefix(comma + 1);
        }
    }

    void ParseSnapshotList() {
        const char* const value = std::getenv("EDEN_CGT_SNAPSHOT");
        if (value == nullptr) {
            return;
        }
        std::string_view rest{value};
        while (!rest.empty()) {
            const size_t comma = rest.find(',');
            const std::string_view token = rest.substr(0, comma);
            const size_t colon = token.find(':');
            if (colon != std::string_view::npos) {
                const std::string vs_text{token.substr(0, colon)};
                const std::string fs_text{token.substr(colon + 1)};
                char* end = nullptr;
                const u64 vs = std::strtoull(vs_text.c_str(), &end, 16);
                u64 fs = ~u64{0};
                if (fs_text != "*") {
                    fs = std::strtoull(fs_text.c_str(), &end, 16);
                }
                snapshot_pairs.emplace_back(vs, fs);
            }
            if (comma == std::string_view::npos) {
                break;
            }
            rest.remove_prefix(comma + 1);
        }
    }

    std::mutex mutex;     // ring, sticky, live, frames, dump
    std::mutex aux_mutex; // dedup, ABA, occurrence; taken before mutex, never after
    std::unique_ptr<u8[]> ring;
    u64 cap = 0;
    u64 write_pos = 0;
    std::atomic<u64> seq_counter{0};
    std::atomic<u32> next_thread{0};
    std::vector<u8> sticky;
    std::deque<FrameMark> frames;
    DumpRequest dump;
    u64 before = 90;
    u64 snap_max = 16384;
    u64 snap_bytes = 64ULL << 20;
    u64 run_id = 0;
    u64 records = 0;
    u64 dump_count = 0;
    u64 lossy_dumps = 0;
    u64 failed_dumps = 0;
    u64 last_stats_ns = 0;
    u64 aba_candidates = 0;
    u64 dedup_hits = 0;
    u64 buffer_serial = 0;
    std::atomic<u64> snapshot_counter{0};
    std::deque<u64> aba_dump_times;
    u64 aba_per_minute = 4;
    u64 aba_total_limit = 16;
    u64 aba_dumps = 0;
    std::unordered_map<u64, LiveBuffer> live;
    std::unordered_map<u64, DedupState> dedup;
    std::unordered_map<u64, u32> occurrence;
    std::unordered_map<u64, AbaState> aba;
    size_t aba_rows = 0;
    u64 frame = 0; // written by Frame (GPU thread) under mutex; read unlocked only there
    std::vector<std::pair<u64, u64>> snapshot_pairs;
    std::vector<u64> watch_fs;
    // GPU thread only
    std::unordered_set<u64> watch_pages;
    bool watch_full = false;
    std::vector<DrawBinding> draw_bindings;
    std::vector<AbaPending> aba_pending;
    bool draw_open = false;
    u64 draw_id = 0;
    u64 draw_counter = 0;
};

// Never destroyed: hooks can run during shutdown.
[[nodiscard]] inline State& Get() {
    static State* const state = new State();
    return *state;
}

// ---- hook entry points (callers check Enabled() first) ----

inline void Config(std::string_view text) {
    Get().Config(text);
    LOG_WARNING(Render_Vulkan, "CGT_CONFIG {}", text);
}

inline void Frame(u64 tick, u64 known_tick) {
    Get().Frame(tick, known_tick);
}

inline void Mark(u32 reason) {
    Get().Mark(reason);
}

[[nodiscard]] inline u64 BeginOp() {
    return Get().NextSeq();
}

[[nodiscard]] inline u64 BeginStore() {
    return Get().BeginStore();
}

inline void EndStore(u64 op, u64 va, const void* data, u64 size, u32 flags) {
    Get().EndStore(op, va, data, size, flags);
}

inline void RasterWrite(u64 va, u64 daddr, u64 size, RasterMode mode, u64 core) {
    Get().RasterWrite(va, daddr, size, mode, core);
}

inline void SetNceFault(u64 fault_addr, u64 pc, u32 insn) {
    Detail::tl_nce_valid = true;
    Detail::tl_nce_fault = fault_addr;
    Detail::tl_nce_pc = pc;
    Detail::tl_nce_insn = insn;
}

inline void NceInvalidate(u64 page, u64 size) {
    const bool has_ctx = Detail::tl_nce_valid;
    Detail::tl_nce_valid = false;
    Get().Emit(Type::NceFault, 0,
               {page, size, has_ctx ? Detail::tl_nce_fault : 0, has_ctx ? Detail::tl_nce_pc : 0,
                has_ctx ? Detail::tl_nce_insn : 0, has_ctx ? 1ULL : 0ULL});
}

inline void RegionInvalidate(u64 daddr, u64 size, bool preserve) {
    Get().Emit(Type::RegionInvalidate, 0, {daddr, size, preserve ? 1ULL : 0ULL, Detail::tl_site});
}

inline void BcWriteMemory(u64 daddr, u64 size, bool was_gpu_modified) {
    Get().Emit(Type::BcWriteMemory, 0,
               {daddr, size, was_gpu_modified ? 1ULL : 0ULL, Detail::tl_site,
                Detail::tl_gather_site});
}

[[nodiscard]] inline u64 GatherBegin() {
    auto& s = Get();
    const u64 id = s.NextSeq();
    Detail::tl_gather_id = id;
    Detail::tl_gather_ranges = 0;
    Detail::tl_gather_bytes = 0;
    s.Emit(Type::GatherBegin, 0, {id, Detail::tl_gather_site});
    return id;
}

// read(dst, size) copies guest bytes at the range start.
template <typename Read>
inline void GatherRange(u64 id, u64 daddr, u64 size, Read&& read) {
    ++Detail::tl_gather_ranges;
    Detail::tl_gather_bytes += size;
    std::array<u8, State::GUEST_READ_MAX> bytes{};
    const u32 n = static_cast<u32>(std::min<u64>(size, State::GUEST_READ_MAX));
    read(bytes.data(), size_t{n});
    Get().Emit(Type::Gather, n < size ? Flag::Truncated : 0, {id, Detail::tl_gather_site, daddr, size},
               bytes.data(), n);
}

inline void GatherEnd(u64 id) {
    Get().Emit(Type::GatherEnd, 0,
               {id, Detail::tl_gather_site, Detail::tl_gather_ranges, Detail::tl_gather_bytes});
}

inline void UploadSrc(u64 op, u64 buffer, u64 dst_offset, u64 daddr, u64 size, u64 staging,
                      u64 staging_offset, const u8* bytes) {
    const u64 key = Detail::Mix(Detail::Mix(Detail::Mix(3, buffer), dst_offset), size);
    Get().EmitDedup(Type::UploadSrc, 0, key,
                    {op, buffer, dst_offset, daddr, size, staging, staging_offset, 0, 0},
                    std::span<const u8>(bytes, size), State::STORE_PAYLOAD_MAX);
}

inline void DownloadPublish(u64 op, u64 daddr, u64 size, const u8* bytes) {
    const u32 n = static_cast<u32>(std::min<u64>(size, State::GUEST_READ_MAX));
    Get().Emit(Type::DownloadPublish, n < size ? Flag::Truncated : 0, {op, daddr, size}, bytes, n);
}

inline void CbData(u64 gpu_va, u64 daddr, const void* bytes, u64 size) {
    auto& s = Get();
    const u64 op = s.NextSeq();
    const u32 n = static_cast<u32>(std::min<u64>(size, State::STORE_PAYLOAD_MAX));
    s.Emit(Type::CbData, n < size ? Flag::Truncated : 0, {gpu_va, daddr, size, op}, bytes, n);
}

inline void BufferCreate(u64 handle, u64 daddr, u64 size) {
    Get().BufferCreate(handle, daddr, size);
}

inline void BufferDelete(u64 handle, u64 daddr, u64 size) {
    Get().BufferDelete(handle, daddr, size);
}

inline void GpuWriteMark(u64 handle, u64 daddr, u64 size, u64 tick) {
    auto& s = Get();
    s.Emit(Type::GpuWriteMark, 0, {handle, daddr, size, tick, s.Serial(handle)});
}

inline void GpuOwnedSkip(u64 handle, u64 daddr, u64 size) {
    Get().Emit(Type::GpuOwnedSkip, 0, {handle, daddr, size});
}

inline void StagingRequest(u64 handle, u64 offset, u64 size, u64 kind, u64 usage, bool deferred,
                           u64 tick, u64 known_tick) {
    const u64 seq = Get().Emit(Type::StagingRequest, 0,
                               {handle, offset, size, kind, usage, deferred ? 1ULL : 0ULL, tick,
                                known_tick});
    Detail::tl_staging_handle = handle;
    Detail::tl_staging_offset = offset;
    Detail::tl_staging_seq = seq;
}

inline void StreamInfo(u64 handle, u64 size, bool coherent, u64 regions) {
    Get().Sticky(Type::StreamInfo, {handle, size, coherent ? 1ULL : 0ULL, regions});
    LOG_WARNING(Render_Vulkan, "CGT_STREAM handle={:#x} size={} coherent={} regions={}", handle,
                size, coherent, regions);
}

inline void UbFast(u64 op, u32 stage, u32 index, u32 binding, u64 daddr, std::span<const u8> bytes,
                   bool alignment_stream) {
    Get().UbFast(op, stage, index, binding, daddr, bytes, alignment_stream);
}

inline void Submit(u64 channel, u64 lists, u64 first_addr, u64 first_words, u64 total_words,
                   u64 fence_id, u64 fence_value, u64 flags) {
    Get().Emit(Type::Submit, 0,
               {channel, lists, first_addr, first_words, total_words, fence_id, fence_value, flags});
}

inline void GpEntry(u64 addr, u64 words, u64 hash, bool sync, u64 index, u64 lists) {
    Get().Emit(Type::GpEntry, 0, {addr, words, hash, sync ? 1ULL : 0ULL, index, lists});
}

inline void UbClassic(u32 stage, u32 index, u32 binding, u64 daddr, u64 size, u64 handle,
                      u64 offset, bool gpu_modified, bool needs_bind, bool synced) {
    auto& s = Get();
    s.AddBinding(BindKind::UbClassic, stage, index, handle, offset, size, daddr);
    const u64 bits = (gpu_modified ? 1ULL : 0ULL) | (needs_bind ? 2ULL : 0ULL) | (synced ? 4ULL : 0ULL);
    s.Emit(Type::UbClassic, 0,
           {(u64{stage} << 8) | index, binding, daddr, size, handle, offset, s.Serial(handle), bits});
}

inline void Ssbo(u32 stage, u32 index, u64 daddr, u64 size, u64 handle, u64 offset, bool written) {
    auto& s = Get();
    s.AddBinding(BindKind::Ssbo, stage, index, handle, offset, size, daddr);
    s.Emit(Type::Ssbo, 0,
           {(u64{stage} << 8) | index, daddr, size, handle, offset, s.Serial(handle),
            written ? 1ULL : 0ULL});
}

inline void IndexBuffer(u64 daddr, u64 size, u64 handle, u64 offset, bool is_inline, u64 first,
                        u64 count, u64 format_bytes) {
    auto& s = Get();
    s.AddBinding(BindKind::Index, 0, 0, handle, offset, size, daddr);
    s.Emit(Type::IndexBuffer, 0,
           {daddr, size, handle, offset, s.Serial(handle), is_inline ? 1ULL : 0ULL, first, count,
            format_bytes});
}

inline void VertexBuffer(u32 index, u64 daddr, u64 size, u64 handle, u64 offset, u64 stride) {
    auto& s = Get();
    s.AddBinding(BindKind::Vertex, 0, index, handle, offset, size, daddr);
    s.Emit(Type::VertexBuffer, 0, {index, daddr, size, handle, offset, s.Serial(handle), stride});
}

inline void IndirectBuffer(u32 which, u64 daddr, u64 size, u64 handle, u64 offset) {
    auto& s = Get();
    s.AddBinding(BindKind::Indirect, 0, which, handle, offset, size, daddr);
    s.Emit(Type::IndirectBuffer, 0, {which, daddr, size, handle, offset, s.Serial(handle)});
}

[[nodiscard]] inline u64 DrawOpen() {
    return Get().DrawOpen();
}

inline void DrawFinish() {
    Get().DrawFinish();
}

inline void DrawNote(u32 kind, u64 count, u32 instances, bool execute, u64 vs_pc, u64 fs_pc) {
    Get().Emit(Type::DrawNote, 0, {kind, count, instances, execute ? 1ULL : 0ULL, vs_pc, fs_pc});
}

inline void CondDecision(u64 gpu_va, u32 mode, u32 override_mode, bool execute, bool words_read,
                         const std::array<u32, 4>& words) {
    Get().Emit(Type::CondDecision, 0,
               {gpu_va, mode, override_mode, execute ? 1ULL : 0ULL, words_read ? 1ULL : 0ULL},
               words.data(), static_cast<u32>(sizeof(words)));
}

// bits: tracked | pending << 1 | gpu_modified << 2 | waited << 3 | audit << 4
inline void CondWait(u64 op, u64 daddr, u32 mode, u32 bits, u64 wait_ns,
                     const std::array<u32, 8>& before_words,
                     const std::array<u32, 8>& after_words) {
    std::array<u32, 16> words{};
    std::copy(before_words.begin(), before_words.end(), words.begin());
    std::copy(after_words.begin(), after_words.end(), words.begin() + 8);
    Get().Emit(Type::CondWait, 0, {op, daddr, mode, bits, wait_ns}, words.data(),
               static_cast<u32>(sizeof(words)));
}

// A guest report (query result or semaphore release) asked for by the command stream, on the GPU
// thread in command order. location: the query cache's QueryLocation (stream << 27 | query id), a
// slot that is reused once freed. Returns the entry's seq, the request id ReportPublish names.
namespace Detail {
struct ReportRequests {
    std::mutex mutex;
    std::unordered_map<u32, u64> by_location; // location -> seq of the last ReportRequest
};
[[nodiscard]] inline ReportRequests& Requests() {
    static ReportRequests requests;
    return requests;
}
} // namespace Detail

[[nodiscard]] inline u64 ReportRequest(u64 gpu_va, u64 daddr, u32 query_type, u32 payload,
                                       u32 flags, u32 location) {
    const u64 seq = Get().Emit(Type::ReportRequest, 0,
                               {gpu_va, daddr, query_type, payload, flags, location});
    auto& requests = Detail::Requests();
    std::scoped_lock lock{requests.mutex};
    requests.by_location[location] = seq;
    return seq;
}

// The request id of the last ReportRequest that took this location (0: none). For
// SemiFlushQueryDirty: it reaches a location only through cached_queries, under cache_mutex, and
// UnregisterPending frees a location under the same mutex after taking it out of cached_queries;
// a location is reused only after it is freed. So while SemiFlushQueryDirty writes, the last
// request of its location is the request of the query it writes.
[[nodiscard]] inline u64 ReportRequestOf(u32 location) {
    auto& requests = Detail::Requests();
    std::scoped_lock lock{requests.mutex};
    const auto it = requests.by_location.find(location);
    return it != requests.by_location.end() ? it->second : 0;
}

// Bytes of a report written to guest memory. op: BeginOp() taken before the first write, so
// [op, seq] spans the writes (overlap with another [op, seq] to the same bytes means order
// unknown). site: 0 deferred operation, 1 payload written at once, 2 SemiFlushQueryDirty (partial:
// it may come before or after the final write), 3 dropped (invalidated), 4 dropped (value not
// synchronized). request: the ReportRequest seq (site 2: ReportRequestOf(location)). bytes: the
// local copy the write used, in guest memory order (result at +0, timestamp at +8 when size is
// 16). Dropped: op 0, size 0, no bytes.
inline void ReportPublish(u64 op, u64 daddr, u32 size, u32 flags, u32 site, u32 location,
                          u64 request, const void* bytes) {
    Get().Emit(Type::ReportPublish, 0, {op, daddr, size, flags, site, location, request}, bytes,
               bytes != nullptr ? size : 0);
}

// code: 1 no new report, 2 completion reused, 3 forced release (real wait)
inline void SyncPath(u32 code, u64 generation) {
    Get().Emit(Type::SyncPath, 0, {code, generation});
}

inline void FenceWait(u32 site, bool require_completion, bool callback_done, u64 wait_ns) {
    Get().Emit(Type::FenceWait, 0,
               {site, require_completion ? 1ULL : 0ULL, callback_done ? 1ULL : 0ULL, wait_ns});
}

#else
// Release hooks are definition-level no-ops, including calls stored in deferred callbacks.
// Do not construct a recorder or evaluate readback callbacks, even if an opaque request id is nonzero.
[[nodiscard]] inline constexpr u64 NowNs() noexcept { return 0; }
template <typename H>
[[nodiscard]] inline u64 HandleBits(H handle) noexcept {
    if constexpr (std::is_pointer_v<H>) {
        return static_cast<u64>(reinterpret_cast<std::uintptr_t>(handle));
    } else {
        return static_cast<u64>(handle);
    }
}

class SiteScope {
public:
    explicit SiteScope(Site) noexcept {}
    SiteScope(const SiteScope&) = delete;
    SiteScope& operator=(const SiteScope&) = delete;
};
class GatherSiteScope {
public:
    explicit GatherSiteScope(GatherSite) noexcept {}
    GatherSiteScope(const GatherSiteScope&) = delete;
    GatherSiteScope& operator=(const GatherSiteScope&) = delete;
};

inline void Config(std::string_view) {}
inline void Frame(u64, u64) {}
inline void Mark(u32) {}
[[nodiscard]] inline u64 BeginOp() { return 0; }
[[nodiscard]] inline u64 BeginStore() { return 0; }
inline void EndStore(u64, u64, const void*, u64, u32) {}
inline void RasterWrite(u64, u64, u64, RasterMode, u64) {}
inline void SetNceFault(u64, u64, u32) {}
inline void NceInvalidate(u64, u64) {}
inline void RegionInvalidate(u64, u64, bool) {}
inline void BcWriteMemory(u64, u64, bool) {}
[[nodiscard]] inline u64 GatherBegin() { return 0; }
template <typename Read>
inline void GatherRange(u64, u64, u64, Read&&) {}
inline void GatherEnd(u64) {}
inline void UploadSrc(u64, u64, u64, u64, u64, u64, u64, const u8*) {}
inline void DownloadPublish(u64, u64, u64, const u8*) {}
inline void CbData(u64, u64, const void*, u64) {}
inline void BufferCreate(u64, u64, u64) {}
inline void BufferDelete(u64, u64, u64) {}
inline void GpuWriteMark(u64, u64, u64, u64) {}
inline void GpuOwnedSkip(u64, u64, u64) {}
inline void StagingRequest(u64, u64, u64, u64, u64, bool, u64, u64) {}
inline void StreamInfo(u64, u64, bool, u64) {}
inline void UbFast(u64, u32, u32, u32, u64, std::span<const u8>, bool) {}
inline void Submit(u64, u64, u64, u64, u64, u64, u64, u64) {}
inline void GpEntry(u64, u64, u64, bool, u64, u64) {}
inline void UbClassic(u32, u32, u32, u64, u64, u64, u64, bool, bool, bool) {}
inline void Ssbo(u32, u32, u64, u64, u64, u64, bool) {}
inline void IndexBuffer(u64, u64, u64, u64, bool, u64, u64, u64) {}
inline void VertexBuffer(u32, u64, u64, u64, u64, u64) {}
inline void IndirectBuffer(u32, u64, u64, u64, u64) {}
[[nodiscard]] inline u64 DrawOpen() { return 0; }
inline void DrawFinish() {}
inline void DrawNote(u32, u64, u32, bool, u64, u64) {}
inline void CondDecision(u64, u32, u32, bool, bool, const std::array<u32, 4>&) {}
inline void CondWait(u64, u64, u32, u32, u64, const std::array<u32, 8>&, const std::array<u32, 8>&) {}
[[nodiscard]] inline u64 ReportRequest(u64, u64, u32, u32, u32, u32) { return 0; }
[[nodiscard]] inline u64 ReportRequestOf(u32) { return 0; }
inline void ReportPublish(u64, u64, u32, u32, u32, u32, u64, const void*) {}
inline void SyncPath(u32, u64) {}
inline void FenceWait(u32, bool, bool, u64) {}
#endif

} // namespace Common::CGT
