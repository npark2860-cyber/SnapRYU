// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/hang_diag.h"

// Guest memory write record, diagnostic only (EDEN_HANG_CPU_DIAG=1; off by default). Writes that
// the emulator makes to guest DRAM on behalf of emulated hardware or HLE (Core::Memory writes,
// device memory writes, GPU memory manager writes, query report publications, direct guest memory
// spans) are filed by DRAM offset: host backing pointer minus the DeviceMemory backing base. Both
// process virtual addresses and device addresses resolve to backing pointers, so a CPU fault
// address and a GPU-side write meet on the same key. Native guest stores are not recorded.
//
// The ring keeps the newest RING_SIZE records. Writers never wait: each slot is a seqlock (odd while
// written, even when complete) plus a checksum, so a reader can tell complete, torn and overwritten
// entries apart. A per-page table keeps the newest record number and time of each DRAM page for
// longer retention; a later write to the same page replaces it. A missing record never proves that
// no emulator write happened.
namespace Common::GuestWriteWatch {

enum class Site : u8 {
    None,
    CpuWrite,      ///< Core::Memory::Write<T>.
    CpuExclusive,  ///< Core::Memory::WriteExclusive that stored.
    CpuBlock,      ///< Core::Memory::WriteBlock / WriteBlockUnsafe (also CopyBlock).
    CpuZero,       ///< Core::Memory::ZeroBlock.
    DevWrite,      ///< DeviceMemoryManager::Write<T>.
    DevBlock,      ///< DeviceMemoryManager::WriteBlock.
    DevBlockUnsafe, ///< DeviceMemoryManager::WriteBlockUnsafe (buffer downloads and others).
    GpuWrite,      ///< Tegra::MemoryManager::Write<T>.
    GpuBlock,      ///< Tegra::MemoryManager::WriteBlockImpl direct copies.
    ScopedSpan,    ///< GuestMemoryScoped direct span: written somewhere in [begin, end].
    QueryReport,   ///< Query report publication (fence release).
    QueryEarly,    ///< Early fence payload publication.
    QueryFlush,    ///< Query value written by a flush of pending queries.
    NumSites,
};

inline constexpr std::array<const char*, static_cast<size_t>(Site::NumSites)> SITE_NAMES{
    "none",       "cpu_write", "cpu_exclusive", "cpu_block",    "cpu_zero",
    "dev_write",  "dev_block", "dev_block_unsafe", "gpu_write", "gpu_block",
    "scoped_span", "query_report", "query_early", "query_flush",
};

/// Who is writing, set where the write actually happens (RAII TagScope).
enum class Tag : u8 {
    None,
    NceFaultEmulate, ///< One faulting guest instruction executed by the fallback runner.
    NceFallback,     ///< x18 fallback runner dispatch.
    FenceRelease,    ///< Fence release operations (query publications, syncpoint increments).
    GpuThread,       ///< GPU command processing thread.
    NumTags,
};

inline constexpr std::array<const char*, static_cast<size_t>(Tag::NumTags)> TAG_NAMES{
    "none", "nce_fault_emulate", "nce_fallback", "fence_release", "gpu_thread",
};

inline constexpr u64 RING_SIZE = u64{1} << 16;
inline constexpr u64 RING_MASK = RING_SIZE - 1;
inline constexpr u64 PAGE_BITS = 12;
inline constexpr u64 MAX_PAGES_PER_RECORD = 64;

struct Entry {
    std::atomic<u64> seq{};   ///< 2n+1 while record n is written, 2n+2 once complete.
    std::atomic<u64> dram{};  ///< DRAM offset of the first byte.
    std::atomic<u64> packed{}; ///< size (32) | site (8) | tag (8) | thread (16).
    std::atomic<s64> begin_ns{};
    std::atomic<s64> end_ns{};
    std::atomic<u64> value{}; ///< First 8 bytes written (fewer for smaller writes), little-endian.
    std::atomic<u64> extra0{};
    std::atomic<u64> extra1{};
    std::atomic<u64> check{};
};

struct PageLast {
    std::atomic<u64> record{}; ///< Record number + 1 of the newest write touching the page.
    std::atomic<s64> end_ns{};
};

/// A complete, verified copy of one record.
struct Record {
    u64 number;
    u64 dram;
    u32 size;
    Site site;
    Tag tag;
    u16 thread;
    s64 begin_ns;
    s64 end_ns;
    u64 value;
    u64 extra0;
    u64 extra1;
};

[[nodiscard]] inline s64 NowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] inline bool EnabledByEnvironment() noexcept {
    if constexpr (!Common::HangDiag::COMPILED) {
        return false;
    }
    static const bool value = [] {
        const char* const text = std::getenv("EDEN_HANG_CPU_DIAG");
        return text != nullptr && text[0] == '1' && text[1] == '\0';
    }();
    return value;
}

inline std::atomic<uintptr_t> g_dram_base{0};
inline std::atomic<u64> g_dram_size{0};
// Allocated once and never freed: threads may still file writes while statics are destroyed.
inline Entry* g_ring = nullptr;
inline PageLast* g_pages = nullptr;
inline u64 g_page_count = 0;
inline std::atomic<bool> g_enabled{false};
inline std::atomic<u64> g_next{0};
inline std::atomic<u64> g_outside{0};      ///< Writes outside DRAM (not recorded).
inline std::atomic<u64> g_page_skipped{0}; ///< Pages of long writes left out of the page table.
inline std::atomic<u16> g_thread_count{0};
inline thread_local u16 t_thread = 0;
inline thread_local Tag t_tag = Tag::None;

[[nodiscard]] inline u64 Mix(u64 h, u64 v) noexcept {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

[[nodiscard]] inline u64 Checksum(u64 number, u64 dram, u64 packed, s64 begin_ns, s64 end_ns,
                                  u64 value, u64 extra0, u64 extra1) noexcept {
    u64 h = Mix(0x6a09e667f3bcc909ULL, number);
    h = Mix(h, dram);
    h = Mix(h, packed);
    h = Mix(h, static_cast<u64>(begin_ns));
    h = Mix(h, static_cast<u64>(end_ns));
    h = Mix(h, value);
    h = Mix(h, extra0);
    h = Mix(h, extra1);
    return h;
}

/// Core::DeviceMemory, once: enables the record when EDEN_HANG_CPU_DIAG=1. Returns the bytes
/// allocated (0 when disabled).
inline u64 Initialize(const void* backing_base, u64 backing_size) {
    if (!EnabledByEnvironment() || g_enabled.load(std::memory_order_acquire)) {
        return 0;
    }
    g_page_count = (backing_size + (u64{1} << PAGE_BITS) - 1) >> PAGE_BITS;
    g_ring = new Entry[RING_SIZE]{};
    g_pages = new PageLast[g_page_count]{};
    g_dram_base.store(reinterpret_cast<uintptr_t>(backing_base), std::memory_order_relaxed);
    g_dram_size.store(backing_size, std::memory_order_relaxed);
    g_enabled.store(true, std::memory_order_release);
    return RING_SIZE * sizeof(Entry) + g_page_count * sizeof(PageLast);
}

[[nodiscard]] inline bool Enabled() noexcept {
    return Common::HangDiag::COMPILED && g_enabled.load(std::memory_order_relaxed);
}

/// DRAM offset of a host backing pointer, if it is one.
[[nodiscard]] inline bool DramOffset(const void* host, u64& offset) noexcept {
    const uintptr_t pointer = reinterpret_cast<uintptr_t>(host);
    const uintptr_t base = g_dram_base.load(std::memory_order_relaxed);
    if (host == nullptr || pointer < base ||
        pointer - base >= g_dram_size.load(std::memory_order_relaxed)) {
        return false;
    }
    offset = pointer - base;
    return true;
}

[[nodiscard]] inline u16 ThreadIndex() noexcept {
    if (t_thread == 0) {
        t_thread = static_cast<u16>(g_thread_count.fetch_add(1, std::memory_order_relaxed) + 1);
    }
    return t_thread;
}

class TagScope {
public:
    explicit TagScope(Tag tag) noexcept : previous{t_tag} {
        if constexpr (Common::HangDiag::COMPILED) {
            t_tag = tag;
        }
    }
    ~TagScope() {
        if constexpr (Common::HangDiag::COMPILED) {
            t_tag = previous;
        }
    }
    TagScope(const TagScope&) = delete;
    TagScope& operator=(const TagScope&) = delete;

private:
    Tag previous;
};

/// Files a write of size bytes that landed at host (a backing pointer). data is the source of the
/// first bytes (nullptr: read them from host after the write). begin_ns 0 means "now".
inline void RecordWrite(const void* host, u64 size, Site site, const void* data = nullptr,
                        u64 extra0 = 0, u64 extra1 = 0, s64 begin_ns = 0) noexcept {
    if (!Enabled() || size == 0) {
        return;
    }
    u64 offset{};
    if (!DramOffset(host, offset)) {
        g_outside.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    u64 value = 0;
    std::memcpy(&value, data != nullptr ? data : host, static_cast<size_t>((std::min)(size, u64{8})));
    const s64 end_ns = NowNs();
    if (begin_ns == 0) {
        begin_ns = end_ns;
    }
    const u64 packed = (std::min)(size, u64{0xffffffff}) |
                       (u64{static_cast<u8>(site)} << 32) | (u64{static_cast<u8>(t_tag)} << 40) |
                       (u64{ThreadIndex()} << 48);
    const u64 number = g_next.fetch_add(1, std::memory_order_relaxed);
    Entry& entry = g_ring[number & RING_MASK];
    entry.seq.store(2 * number + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    entry.dram.store(offset, std::memory_order_relaxed);
    entry.packed.store(packed, std::memory_order_relaxed);
    entry.begin_ns.store(begin_ns, std::memory_order_relaxed);
    entry.end_ns.store(end_ns, std::memory_order_relaxed);
    entry.value.store(value, std::memory_order_relaxed);
    entry.extra0.store(extra0, std::memory_order_relaxed);
    entry.extra1.store(extra1, std::memory_order_relaxed);
    entry.check.store(Checksum(number, offset, packed, begin_ns, end_ns, value, extra0, extra1),
                      std::memory_order_relaxed);
    entry.seq.store(2 * number + 2, std::memory_order_release);

    const u64 first_page = offset >> PAGE_BITS;
    const u64 last_page = (std::min)((offset + size - 1) >> PAGE_BITS, g_page_count - 1);
    const u64 pages = last_page - first_page + 1;
    const u64 filed = (std::min)(pages, MAX_PAGES_PER_RECORD);
    for (u64 page = first_page; page < first_page + filed; ++page) {
        g_pages[page].record.store(number + 1, std::memory_order_relaxed);
        g_pages[page].end_ns.store(end_ns, std::memory_order_relaxed);
    }
    if (filed < pages) {
        g_page_skipped.fetch_add(pages - filed, std::memory_order_relaxed);
    }
}

/// Reads ring slot index; true when it holds a complete record that passes its checksum.
[[nodiscard]] inline bool ReadSlot(u64 index, Record& out, bool& torn) noexcept {
    torn = false;
    const Entry& entry = g_ring[index & RING_MASK];
    const u64 seq_before = entry.seq.load(std::memory_order_acquire);
    if (seq_before == 0 || (seq_before & 1) != 0) {
        torn = seq_before != 0;
        return false;
    }
    const u64 dram = entry.dram.load(std::memory_order_relaxed);
    const u64 packed = entry.packed.load(std::memory_order_relaxed);
    const s64 begin_ns = entry.begin_ns.load(std::memory_order_relaxed);
    const s64 end_ns = entry.end_ns.load(std::memory_order_relaxed);
    const u64 value = entry.value.load(std::memory_order_relaxed);
    const u64 extra0 = entry.extra0.load(std::memory_order_relaxed);
    const u64 extra1 = entry.extra1.load(std::memory_order_relaxed);
    const u64 check = entry.check.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    const u64 seq_after = entry.seq.load(std::memory_order_relaxed);
    const u64 number = seq_before / 2 - 1;
    if (seq_after != seq_before ||
        check != Checksum(number, dram, packed, begin_ns, end_ns, value, extra0, extra1)) {
        torn = true;
        return false;
    }
    out = Record{
        .number = number,
        .dram = dram,
        .size = static_cast<u32>(packed & 0xffffffff),
        .site = static_cast<Site>((packed >> 32) & 0xff),
        .tag = static_cast<Tag>((packed >> 40) & 0xff),
        .thread = static_cast<u16>(packed >> 48),
        .begin_ns = begin_ns,
        .end_ns = end_ns,
        .value = value,
        .extra0 = extra0,
        .extra1 = extra1,
    };
    return true;
}

struct RingStats {
    u64 total;    ///< Records ever filed.
    u64 oldest;   ///< Oldest record number the ring can still hold.
    u64 complete; ///< Slots read back complete.
    u64 torn;     ///< Slots being written or torn by a lapping writer.
    u64 outside;
    u64 page_skipped;
};

/// One pass over the ring for several byte ranges (offset, size): result[i] holds the complete
/// records overlapping ranges[i], oldest first (at most limit, the newest kept). Also fills stats
/// for the whole ring.
inline std::vector<std::vector<Record>> FindOverlappingMany(
    std::span<const std::pair<u64, u64>> ranges, size_t limit, RingStats& stats) {
    std::vector<std::vector<Record>> found(ranges.size());
    stats = RingStats{};
    if (!Enabled()) {
        return found;
    }
    const u64 total = g_next.load(std::memory_order_acquire);
    stats.total = total;
    stats.oldest = total > RING_SIZE ? total - RING_SIZE : 0;
    stats.outside = g_outside.load(std::memory_order_relaxed);
    stats.page_skipped = g_page_skipped.load(std::memory_order_relaxed);
    for (u64 index = 0; index < (std::min)(total, RING_SIZE); ++index) {
        Record record{};
        bool torn = false;
        if (!ReadSlot(index, record, torn)) {
            stats.torn += torn ? 1 : 0;
            continue;
        }
        ++stats.complete;
        for (size_t i = 0; i < ranges.size(); ++i) {
            const auto [offset, size] = ranges[i];
            if (record.dram < offset + size && offset < record.dram + record.size) {
                found[i].push_back(record);
            }
        }
    }
    for (std::vector<Record>& records : found) {
        std::sort(records.begin(), records.end(),
                  [](const Record& a, const Record& b) { return a.number < b.number; });
        if (records.size() > limit) {
            records.erase(records.begin(), records.end() - static_cast<std::ptrdiff_t>(limit));
        }
    }
    return found;
}

/// FindOverlappingMany for one range.
inline std::vector<Record> FindOverlapping(u64 offset, u64 size, size_t limit, RingStats& stats) {
    const std::array<std::pair<u64, u64>, 1> range{{{offset, size}}};
    return std::move(FindOverlappingMany(range, limit, stats)[0]);
}

/// Newest record number + 1 and its time for the DRAM page holding offset (0: none filed).
[[nodiscard]] inline std::pair<u64, s64> PageLastWrite(u64 offset) noexcept {
    if (!Enabled() || (offset >> PAGE_BITS) >= g_page_count) {
        return {0, 0};
    }
    const PageLast& page = g_pages[offset >> PAGE_BITS];
    return {page.record.load(std::memory_order_relaxed), page.end_ns.load(std::memory_order_relaxed)};
}

} // namespace Common::GuestWriteWatch
