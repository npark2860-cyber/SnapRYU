// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#if SNAPRYU_DIAGNOSTICS
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <utility>

#include "common/common_types.h"
#include "common/logging.h"

namespace Vulkan::SubmitWorkTrace {

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_SUBMIT_WORK_TRACE");
        const bool on = value && value[0] == '1' && value[1] == '\0';
        LOG_INFO(Render_Vulkan, "SUBMIT_WORK_TRACE enabled={} history=512 shader_slots=8 "
                               "scope=recorded_guest_commands extra_gpu_queries=0 extra_gpu_waits=0", on);
        return on;
    }();
    return enabled;
}

inline s64 NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline u64 Add(u64 a, u64 b) {
    return b > (std::numeric_limits<u64>::max)() - a ? (std::numeric_limits<u64>::max)() : a + b;
}
inline u64 Multiply(u64 a, u64 b) {
    return a && b > (std::numeric_limits<u64>::max)() / a ? (std::numeric_limits<u64>::max)() : a * b;
}

struct ShaderWork {
    std::array<u64, 6> hashes{}; // Graphics: VA,VB,TC,TE,GS,FS. Compute: CS,0,0,0,0,0.
    bool compute{};
    u64 direct{}, indirect{}, declared_work{}, max_work{}, max_indirect_count{};
};
struct Work {
    u64 draws{}, indirect_draws{}, dispatches{}, indirect_dispatches{}, dropped_shader_commands{};
    std::array<ShaderWork, 8> shaders{};
    u32 shader_count{};

    void Note(const std::array<u64, 6>& hashes, bool compute, bool indirect,
              u64 declared_work, u64 max_indirect_count) {
        if (compute) ++(indirect ? indirect_dispatches : dispatches);
        else ++(indirect ? indirect_draws : draws);
        ShaderWork* found = nullptr;
        for (u32 i = 0; i < shader_count; ++i) {
            if (shaders[i].compute == compute && shaders[i].hashes == hashes) {
                found = &shaders[i];
                break;
            }
        }
        if (!found) {
            if (shader_count == shaders.size()) {
                ++dropped_shader_commands;
                return;
            }
            found = &shaders[shader_count++];
            found->hashes = hashes;
            found->compute = compute;
        }
        ++(indirect ? found->indirect : found->direct);
        found->declared_work = Add(found->declared_work, declared_work);
        found->max_work = std::max(found->max_work, declared_work);
        found->max_indirect_count = std::max(found->max_indirect_count, max_indirect_count);
    }
};
inline thread_local Work current{};

inline void Graphics(const std::array<u64, 6>& hashes, u32 vertices, u32 instances,
                     bool indirect = false, u32 max_count = 0) {
    if (Enabled()) current.Note(hashes, false, indirect,
                               indirect ? 0 : u64{vertices} * instances, max_count);
}
inline void Compute(u64 hash, u32 x, u32 y, u32 z, bool indirect = false) {
    if (Enabled()) current.Note({hash, 0, 0, 0, 0, 0}, true, indirect,
                               indirect ? 0 : Multiply(Multiply(x, y), z), 0);
}
inline Work Take() {
    return Enabled() ? std::exchange(current, Work{}) : Work{};
}

struct Sample {
    const void* owner{};
    u64 tick{}, known_gpu{}, rp_calls{};
    s64 begin_us{}, end_us{};
    int result{};
    Work work{};
};
template <typename Mutex = std::mutex>
class History {
public:
    // The submitter never waits for this diagnostic history lock.
    bool TryStore(const Sample& sample) {
        std::unique_lock lock{mutex, std::try_to_lock};
        if (!lock.owns_lock()) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        slots[sample.tick % slots.size()] = sample;
        return true;
    }
    bool Copy(const void* owner, u64 tick, Sample& out) {
        std::scoped_lock lock{mutex};
        const auto& entry = slots[tick % slots.size()];
        if (!tick || entry.owner != owner || entry.tick != tick) return false;
        out = entry;
        return true;
    }
    void Reset(const void* owner) {
        std::scoped_lock lock{mutex};
        for (auto& entry : slots) if (entry.owner == owner) entry = {};
    }
    u64 Dropped() const { return dropped.load(std::memory_order_relaxed); }
private:
    Mutex mutex;
    std::array<Sample, 512> slots{};
    std::atomic<u64> dropped{};
};
inline History<> history;
inline void Reset(const void* owner) {
    if (Enabled()) history.Reset(owner);
}
inline void Publish(const Sample& sample) {
    if (Enabled()) history.TryStore(sample);
}

// Called only for the already bounded slow-wait log, after the original wait returned.
// A cached tick may lag completion. These are candidate submissions, not culprit draws.
inline void Report(const void* owner, u64 target, u64 cached_before, s64 wait_begin, s64 wait_end) {
    if (!Enabled()) return;
    const std::array<u64, 4> candidates{
        cached_before < target ? cached_before + 1 : target,
        target > 2 ? target - 2 : target, target > 1 ? target - 1 : target, target};
    for (size_t i = 0; i < candidates.size(); ++i) {
        const u64 tick = candidates[i];
        if (std::find(candidates.begin(), candidates.begin() + i, tick) != candidates.begin() + i) continue;
        Sample sample;
        if (!history.Copy(owner, tick, sample)) {
            LOG_INFO(Render_Vulkan, "GPU_WAIT_WORK target={} tick={} found=false wait_begin_us={} "
                                   "wait_end_us={} history_dropped={} reason=absent_or_overwritten",
                     target, tick, wait_begin, wait_end, history.Dropped());
            continue;
        }
        LOG_INFO(Render_Vulkan,
                 "GPU_WAIT_WORK target={} tick={} found=true wait_begin_us={} wait_end_us={} "
                 "submit_begin_us={} submit_end_us={} result={} known_gpu={} rp_calls={} "
                 "draws={} indirect_draws={} dispatches={} indirect_dispatches={} shaders={} "
                 "dropped_shader_commands={} history_dropped={} measurement=submit_context_not_gpu_timer",
                 target, tick, wait_begin, wait_end, sample.begin_us, sample.end_us, sample.result,
                 sample.known_gpu, sample.rp_calls, sample.work.draws, sample.work.indirect_draws,
                 sample.work.dispatches, sample.work.indirect_dispatches, sample.work.shader_count,
                 sample.work.dropped_shader_commands, history.Dropped());
        for (u32 index = 0; index < sample.work.shader_count; ++index) {
            const auto& shader = sample.work.shaders[index];
            LOG_INFO(Render_Vulkan,
                     "GPU_WAIT_SHADER target={} tick={} slot={} kind={} "
                     "h0={:016x} h1={:016x} h2={:016x} h3={:016x} h4={:016x} h5={:016x} "
                     "direct={} indirect={} declared_work={} max_work={} max_indirect_count={} "
                     "indirect_arguments_not_read=true helper_and_lsfg_shaders_excluded=true",
                     target, tick, index, shader.compute ? "compute" : "graphics",
                     shader.hashes[0], shader.hashes[1], shader.hashes[2], shader.hashes[3],
                     shader.hashes[4], shader.hashes[5], shader.direct, shader.indirect,
                     shader.declared_work, shader.max_work, shader.max_indirect_count);
        }
    }
}
} // namespace Vulkan::SubmitWorkTrace
#endif
