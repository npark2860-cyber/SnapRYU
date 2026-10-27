// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// TEX_REMAP / TEX_STALE: diagnostic traces for terrain squares (TotK skydive, 2026-10-16).
//
// Question: does a sampled image hold content that no longer matches what the GPU would read?
//
// TEX_REMAP (GPU page table changes under cached images):
//   silent_pages      a Mapped page now maps another device address. MemoryManager notifies the
//                     rasterizer only when the entry type changes, so the texture cache is not told.
//   type_pages        a page's entry type changed (Free/Reserved/Mapped); the texture cache is told.
//   remap_images      images UnmapGPUMemory visited
//   remap_gpu_images  ... of which held GPU-written content (GpuModified). Marking them CpuModified
//                     makes the next use upload the whole image from guest memory over that content.
//   refresh_gpu       uploads from guest memory onto an image holding GPU-written content
//   refresh_remapped  ... of which the image was Remapped
//   remapped_uses     texture table hits that returned a Remapped image
//   stale_tracking    ... whose stored device address differs from what its GPU address maps to now
//
// TEX_STALE (missed writes): each upload stores a hash of the guest memory it read. A sampled image
// with neither CpuModified nor GpuModified is re-read through its GPU address at most every
// RECHECK_TICKS frames, within FRAME_BUDGET bytes per frame. A difference seen on two checks in a row,
// with the image still clean, is counted as confirmed: the cache shows content that memory no
// longer holds and nothing told it.
//
// Experiments (EDEN_REMAP_EXP, read once):
//   preserve  UnmapGPUMemory leaves images holding GPU-written content untouched.
//   notify    silent remaps notify the rasterizer like a type change.
//
// Diagnostic builds only; release builds compile none of this.
#pragma once

#if SNAPRYU_DIAGNOSTICS
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>
#include <ankerl/unordered_dense.h>
#include "common/cityhash.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/memory_manager.h"

namespace VideoCommon::RemapTrace {

enum Counter : size_t {
    SilentPages,
    TypePages,
    RemapImages,
    RemapGpuImages,
    RefreshGpu,
    RefreshRemapped,
    RemappedUses,
    StaleTracking,
    StaleChecks,
    StaleCheckedBytes,
    StalePending,
    StaleConfirmed,
    NumCounters,
};

enum class Experiment { None, Preserve, Notify };

inline std::array<std::atomic<u64>, NumCounters> counters{};
inline std::array<std::atomic<u32>, NumCounters> details{};
inline constexpr u32 MAX_DETAILS = 32;

inline const char* ExperimentName(Experiment experiment) {
    switch (experiment) {
    case Experiment::Preserve:
        return "preserve";
    case Experiment::Notify:
        return "notify";
    default:
        return "none";
    }
}

inline Experiment GetExperiment() {
    static const Experiment experiment = [] {
        const char* const value = std::getenv("EDEN_REMAP_EXP");
        const std::string_view mode = value ? value : "";
        const Experiment result = mode == "preserve" ? Experiment::Preserve
                                  : mode == "notify" ? Experiment::Notify
                                                     : Experiment::None;
        LOG_INFO(Render_Vulkan, "TEX_REMAP experiment requested={} active={}",
                 mode.empty() ? "none" : mode, ExperimentName(result));
        return result;
    }();
    return experiment;
}

inline void Add(Counter counter, u64 amount = 1) {
    counters[counter].fetch_add(amount, std::memory_order_relaxed);
}

// True for the first MAX_DETAILS events of a counter; the caller then logs the event.
inline bool Detail(Counter counter) {
    return details[counter].fetch_add(1, std::memory_order_relaxed) < MAX_DETAILS;
}

// Upload snapshots and the check budget are used only by the GPU thread.
struct Snapshot {
    u64 hash{};
    u64 upload_hash{}; // what the last upload read (SQUARES_MARK); hash follows TEX_STALE
    GPUVAddr gpu_addr{};
    u32 size{};
    u64 upload_tick{};
    u64 checked_tick{};
    bool pending{};
};

inline constexpr u64 RECHECK_TICKS = 60;
inline constexpr u64 PENDING_RECHECK_TICKS = 15;
inline constexpr u64 FRAME_BUDGET = 4ULL << 20;

inline ankerl::unordered_dense::map<u32, Snapshot> snapshots;
inline std::vector<u8> scratch;
inline u64 frame_bytes = 0;

// Reads page by page and never touches the rasterizer. A page without a device page reads as zeros,
// as an upload of a sparse page does, instead of dereferencing a null pointer.
inline u64 HashGuest(const Tegra::MemoryManager& gpu_memory, GPUVAddr gpu_addr, u32 size) {
    constexpr u64 PAGE_SIZE = 4096;
    scratch.resize(size);
    u64 done = 0;
    while (done < size) {
        const GPUVAddr addr = gpu_addr + done;
        const u64 chunk = (std::min)(PAGE_SIZE - (addr & (PAGE_SIZE - 1)), u64{size} - done);
        const u8* const source = gpu_memory.GetPointer(addr);
        if (source) {
            std::memcpy(scratch.data() + done, source, chunk);
        } else {
            std::memset(scratch.data() + done, 0, chunk);
        }
        done += chunk;
    }
    return Common::CityHash64(reinterpret_cast<const char*>(scratch.data()), size);
}

inline void RecordUpload(const Tegra::MemoryManager& gpu_memory, u32 image_index,
                         GPUVAddr gpu_addr, u32 size, u64 tick) {
    const u64 hash = HashGuest(gpu_memory, gpu_addr, size);
    snapshots.insert_or_assign(image_index, Snapshot{
                                                .hash = hash,
                                                .upload_hash = hash,
                                                .gpu_addr = gpu_addr,
                                                .size = size,
                                                .upload_tick = tick,
                                                .checked_tick = tick,
                                                .pending = false,
                                            });
}

inline void Forget(u32 image_index) {
    snapshots.erase(image_index);
}

// Returns the snapshot when this image is due for a check and fits this frame's budget.
inline Snapshot* DueSnapshot(u32 image_index, GPUVAddr gpu_addr, u32 size, u64 tick) {
    const auto it = snapshots.find(image_index);
    if (it == snapshots.end()) {
        return nullptr;
    }
    Snapshot& snapshot = it->second;
    if (snapshot.gpu_addr != gpu_addr || snapshot.size != size) {
        return nullptr;
    }
    const u64 interval = snapshot.pending ? PENDING_RECHECK_TICKS : RECHECK_TICKS;
    if (tick - snapshot.checked_tick < interval) {
        return nullptr;
    }
    if (frame_bytes != 0 && frame_bytes + size > FRAME_BUDGET) {
        return nullptr;
    }
    frame_bytes += size;
    snapshot.checked_tick = tick;
    Add(StaleChecks);
    Add(StaleCheckedBytes, size);
    return &snapshot;
}

// Called once per frame by the texture cache: resets the check budget, logs every 5 s on change.
inline void EndFrame() {
    frame_bytes = 0;
    static std::chrono::steady_clock::time_point last_log{};
    static std::array<u64, NumCounters> last_values{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last_log < std::chrono::seconds(5)) {
        return;
    }
    last_log = now;
    std::array<u64, NumCounters> values{};
    for (size_t i = 0; i < NumCounters; ++i) {
        values[i] = counters[i].load(std::memory_order_relaxed);
    }
    if (values == last_values) {
        return;
    }
    last_values = values;
    LOG_INFO(Render_Vulkan,
             "TEX_REMAP silent_pages={} type_pages={} remap_images={} remap_gpu_images={} "
             "refresh_gpu={} refresh_remapped={} remapped_uses={} stale_tracking={} experiment={}",
             values[SilentPages], values[TypePages], values[RemapImages], values[RemapGpuImages],
             values[RefreshGpu], values[RefreshRemapped], values[RemappedUses],
             values[StaleTracking], ExperimentName(GetExperiment()));
    LOG_INFO(Render_Vulkan,
             "TEX_STALE checks={} checked_mib={} pending={} confirmed={} snapshots={}",
             values[StaleChecks], values[StaleCheckedBytes] >> 20, values[StalePending],
             values[StaleConfirmed], snapshots.size());
}

} // namespace VideoCommon::RemapTrace
#endif
