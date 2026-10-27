// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <thread>

#include <ranges>
#include <array>
#include <chrono>
#include <cstdlib>
#include <functional>
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#if SNAPRYU_DIAGNOSTICS
#include "video_core/renderer_vulkan/vk_submit_work_trace.h"
#endif
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

constexpr u64 FENCE_RESERVE_SIZE = 8;

#if SNAPRYU_DIAGNOSTICS
namespace {
bool StallTimingEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_GPU_STALL_TRACE");
        const bool on = value && value[0] == '1' && value[1] == '\0';
        LOG_INFO(Render_Vulkan, "GPU_STALL_TRACE enabled={} timeline_wait_threshold_us=50000 "
                               "clock=steady_epoch_us extra_gpu_queries=0 extra_gpu_waits=0", on);
        return on;
    }();
    return enabled;
}

s64 StallClockUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Log after the existing wait returns, before Refresh can observe an invalid counter.
// The cached value is only a prior observation, never an exact outstanding-work count.
void NoteTimelineStall(u64 target, u64 cached_before, u64 logical_before, s64 begin_us,
                       s64 end_us, u64 calls, const void* caller, const void* owner) {
    if (end_us - begin_us < 50'000) {
        return;
    }
    static thread_local u64 events{};
    ++events;
    if (events > 128 && events % 128 != 0) {
        return;
    }
    LOG_WARNING(Render_Vulkan,
                "GPU_TIMELINE_WAIT_STALL event={} begin_us={} end_us={} wait_us={} target={} "
                "cached_before={} logical_before={} calls={} returned_success=true thread={} "
                "cache_is_prior_observation=true log_first=128 then_every=128 caller={} anchor_pc={}",
                events, begin_us, end_us, end_us - begin_us, target, cached_before, logical_before,
                calls, std::hash<std::thread::id>{}(std::this_thread::get_id()), caller,
                reinterpret_cast<const void*>(&StallTimingEnabled));
    SubmitWorkTrace::Report(owner, target, cached_before, begin_us, end_us);
}

bool CounterTraceEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_TIMELINE_COUNTER_TRACE");
        const bool on = value && value[0] == '1' && value[1] == '\0';
        LOG_INFO(Render_Vulkan, "TIMELINE_COUNTER_TRACE enabled={} history=32 extra_gpu_queries=0 "
                               "extra_gpu_waits=0 preserves_counter_behavior=true", on);
        return on;
    }();
    return enabled;
}

// Keep the successful path thread-local. This observes the existing query, without polling,
// waiting, clamping, or accepting a different completion value from the original implementation.
class CounterTrace {
public:
    static constexpr u64 OUTPUT_SENTINEL = 0xA17E'B36D'C04F'91E2ULL;
    static constexpr u64 GUARD = 0x1357'9BDF'2468'ACE0ULL;
    struct Sample {
        u64 cached_before{}, raw{}, logical_after{};
        VkResult result{};
        bool guards_ok{};
        s64 begin_us{}, query_us{};
    };

    bool Note(const void* owner, Sample sample, const void* handle, const void* caller) {
        if (last_owner != owner) {
            last_owner = owner;
            total = 0;
        }
        samples[total++ % samples.size()] = sample;
        // NextTick reserves the signal value before any submission. Read this upper bound
        // AFTER querying the driver, so concurrent CPU advancement cannot cause a false alarm.
        const bool invalid = sample.result != VK_SUCCESS || !sample.guards_ok ||
                             sample.raw >= sample.logical_after ||
                             sample.cached_before >= sample.logical_after;
        if (!invalid) {
            return false;
        }
        static std::atomic_flag reported = ATOMIC_FLAG_INIT;
        if (reported.test_and_set(std::memory_order_relaxed)) {
            return false;
        }
        LOG_ERROR(Render_Vulkan,
                  "TIMELINE_COUNTER_ANOMALY result={} raw={} raw_hex=0x{:016X} cached_before={} "
                  "logical_after={} output_unchanged={} guards_ok={} thread={} semaphore={} caller={} "
                  "anchor_pc={} begin_us={} query_us={} raw_query_precedes_cache_store=true",
                  static_cast<int>(sample.result), sample.raw, sample.raw, sample.cached_before,
                  sample.logical_after, sample.raw == OUTPUT_SENTINEL, sample.guards_ok,
                  std::hash<std::thread::id>{}(std::this_thread::get_id()), handle, caller,
                  reinterpret_cast<const void*>(&CounterTraceEnabled), sample.begin_us,
                  sample.query_us);
        const u64 count = std::min<u64>(total, samples.size());
        for (u64 i = total - count; i < total; ++i) {
            const auto& entry = samples[i % samples.size()];
            LOG_ERROR(Render_Vulkan,
                      "TIMELINE_COUNTER_HISTORY query={} cached_before={} raw={} logical_after={} "
                      "result={} guards_ok={} begin_us={} query_us={} scope=current_thread_only",
                      i + 1, entry.cached_before, entry.raw, entry.logical_after,
                      static_cast<int>(entry.result), entry.guards_ok, entry.begin_us, entry.query_us);
        }
        return true;
    }

private:
    const void* last_owner{};
    std::array<Sample, 32> samples{};
    u64 total{};
};
} // namespace
#endif

MasterSemaphore::MasterSemaphore(const Device& device_) : device(device_) {
#if SNAPRYU_DIAGNOSTICS
    if (CounterTraceEnabled()) {
        LOG_INFO(Render_Vulkan, "TIMELINE_COUNTER_MODE timeline={} synchronization2={}",
                 device.HasTimelineSemaphore(), device.HasSynchronization2());
    }
#endif
    if (!device.HasTimelineSemaphore()) {
        static constexpr VkFenceCreateInfo fence_ci{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0};
        free_queue.resize(FENCE_RESERVE_SIZE);
        std::ranges::generate(free_queue,
                              [&] { return device.GetLogical().CreateFence(fence_ci); });
        wait_thread = std::jthread([this](std::stop_token token) { WaitThread(token); });
        return;
    }

    static constexpr VkSemaphoreTypeCreateInfo semaphore_type_ci{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext = nullptr,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    static constexpr VkSemaphoreCreateInfo semaphore_ci{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &semaphore_type_ci,
        .flags = 0,
    };
    semaphore = device.GetLogical().CreateSemaphore(semaphore_ci);

    if (!Settings::values.renderer_debug) {
        return;
    }
    // Validation layers have a bug where they fail to track resource usage when using timeline
    // semaphores and synchronizing with GetSemaphoreCounterValue. To workaround this issue, have
    // a separate thread waiting for each timeline semaphore value.
    debug_thread = std::jthread([this](std::stop_token stop_token) {
        u64 counter = 0;
        while (!stop_token.stop_requested()) {
            if (semaphore.Wait(counter, 10'000'000)) {
                ++counter;
            }
        }
    });
}

MasterSemaphore::~MasterSemaphore() = default;

void MasterSemaphore::Refresh() {
    if (!semaphore) {
        // If we don't support timeline semaphores, there's nothing to refresh
        return;
    }

    u64 this_tick{};
    u64 counter{};
#if SNAPRYU_DIAGNOSTICS
    const bool trace_enabled = CounterTraceEnabled();
    bool reported = false;
#endif
    do {
        this_tick = gpu_tick.load(std::memory_order_acquire);
#if SNAPRYU_DIAGNOSTICS
        if (trace_enabled) {
            struct {
                u64 before{CounterTrace::GUARD};
                u64 value{CounterTrace::OUTPUT_SENTINEL};
                u64 after{CounterTrace::GUARD};
            } output;
            const bool timing = StallTimingEnabled();
            const s64 query_begin = timing ? StallClockUs() : 0;
            const VkResult result = device.GetDispatchLoader().vkGetSemaphoreCounterValue(
                *device.GetLogical(), *semaphore, &output.value);
            const s64 query_us = timing ? StallClockUs() - query_begin : 0;
            counter = output.value;
            static thread_local CounterTrace trace;
#if defined(__GNUC__) || defined(__clang__)
            const void* caller = __builtin_extract_return_addr(__builtin_return_address(0));
#else
            const void* caller = nullptr;
#endif
            reported |= trace.Note(
                this, {this_tick, counter, CurrentTick(), result,
                       output.before == CounterTrace::GUARD && output.after == CounterTrace::GUARD,
                       query_begin, query_us},
                reinterpret_cast<const void*>(*semaphore), caller);
            vk::Check(result); // Preserve the original wrapper's error handling.
        } else
#endif
        {
            counter = semaphore.GetCounter();
        }
        if (counter < this_tick) {
#if SNAPRYU_DIAGNOSTICS
            if (reported) {
                LOG_ERROR(Render_Vulkan, "TIMELINE_COUNTER_STORE action=skip_older cached_after={} raw={}",
                          KnownGpuTick(), counter);
            }
#endif
            return;
        }
    } while (!gpu_tick.compare_exchange_weak(this_tick, counter, std::memory_order_release,
                                             std::memory_order_relaxed));
#if SNAPRYU_DIAGNOSTICS
    if (reported) {
        LOG_ERROR(Render_Vulkan, "TIMELINE_COUNTER_STORE action=cas_success replaced={} raw={} "
                                "cached_after={} logical_after={}",
                  this_tick, counter, KnownGpuTick(), CurrentTick());
    }
#endif
}

void MasterSemaphore::Wait(u64 tick) {
    if (!semaphore) {
        // Fast check: already reached the requested tick?
        if (gpu_tick.load(std::memory_order_acquire) >= tick) {
            return;
        }

        u64 last_tick = gpu_tick.load(std::memory_order_relaxed);
        while (gpu_tick.load(std::memory_order_acquire) < tick) {
            gpu_tick.wait(last_tick, std::memory_order_acquire);
            last_tick = gpu_tick.load(std::memory_order_relaxed);
        }
        return;
    }

    // No need to wait if the GPU is ahead of the tick
    if (IsFree(tick)) {
        return;
    }

    // Update the GPU tick and try again
    Refresh();

    if (IsFree(tick)) {
        return;
    }

    // If none of the above is hit, fallback to a regular wait
#if SNAPRYU_DIAGNOSTICS
    const bool timing = StallTimingEnabled();
    const u64 cached_before = timing ? KnownGpuTick() : 0;
    const u64 logical_before = timing ? CurrentTick() : 0;
    const s64 wait_begin = timing ? StallClockUs() : 0;
    u64 wait_calls = 1;
#endif
    while (!semaphore.Wait(tick)) {
#if SNAPRYU_DIAGNOSTICS
        ++wait_calls;
#endif
    }
#if SNAPRYU_DIAGNOSTICS
    if (timing) {
#if defined(__GNUC__) || defined(__clang__)
        const void* caller = __builtin_extract_return_addr(__builtin_return_address(0));
#else
        const void* caller = nullptr;
#endif
        NoteTimelineStall(tick, cached_before, logical_before, wait_begin, StallClockUs(), wait_calls,
                          caller, this);
    }
#endif

    Refresh();
}

VkResult MasterSemaphore::SubmitQueue(vk::CommandBuffer& cmdbuf, vk::CommandBuffer& upload_cmdbuf,
                                      VkSemaphore signal_semaphore, VkSemaphore wait_semaphore,
                                      u64 host_tick) {
    if (semaphore) {
        return SubmitQueueTimeline(cmdbuf, upload_cmdbuf, signal_semaphore, wait_semaphore,
                                   host_tick);
    } else {
        return SubmitQueueFence(cmdbuf, upload_cmdbuf, signal_semaphore, wait_semaphore, host_tick);
    }
}

static constexpr VkPipelineStageFlags wait_stage_mask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

VkResult MasterSemaphore::SubmitQueueTimeline(vk::CommandBuffer& cmdbuf,
                                              vk::CommandBuffer& upload_cmdbuf,
                                              VkSemaphore signal_semaphore,
                                              VkSemaphore wait_semaphore, u64 host_tick) {
    const VkSemaphore timeline_semaphore = *semaphore;

    if (device.HasSynchronization2()) {
        const std::array<VkCommandBufferSubmitInfo, 2> cmdbuffer_infos{{
            {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .pNext = nullptr,
                .commandBuffer = *upload_cmdbuf,
                .deviceMask = 0,
            },
            {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .pNext = nullptr,
                .commandBuffer = *cmdbuf,
                .deviceMask = 0,
            },
        }};

        std::array<VkSemaphoreSubmitInfo, 2> signal_infos{{
            {
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .pNext = nullptr,
                .semaphore = timeline_semaphore,
                .value = host_tick,
                .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .deviceIndex = 0,
            },
            {},
        }};
        u32 num_signal_semaphores = 1;
        if (signal_semaphore) {
            signal_infos[1] = VkSemaphoreSubmitInfo{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .pNext = nullptr,
                .semaphore = signal_semaphore,
                .value = 0,
                .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .deviceIndex = 0,
            };
            num_signal_semaphores = 2;
        }

        const u32 num_wait_semaphores = wait_semaphore ? 1 : 0;
        const VkSemaphoreSubmitInfo wait_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = wait_semaphore,
            .value = 0,
            .stageMask = static_cast<VkPipelineStageFlags2>(wait_stage_mask),
            .deviceIndex = 0,
        };

        const VkSubmitInfo2 submit_info2{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .pNext = nullptr,
            .flags = 0,
            .waitSemaphoreInfoCount = num_wait_semaphores,
            .pWaitSemaphoreInfos = num_wait_semaphores ? &wait_info : nullptr,
            .commandBufferInfoCount = static_cast<u32>(cmdbuffer_infos.size()),
            .pCommandBufferInfos = cmdbuffer_infos.data(),
            .signalSemaphoreInfoCount = num_signal_semaphores,
            .pSignalSemaphoreInfos = signal_infos.data(),
        };
        return device.GetGraphicsQueue().Submit2(submit_info2);
    }

    const u32 num_signal_semaphores = signal_semaphore ? 2 : 1;
    const std::array signal_values{host_tick, u64(0)};
    const std::array signal_semaphores{timeline_semaphore, signal_semaphore};

    const std::array cmdbuffers{*upload_cmdbuf, *cmdbuf};

    const u32 num_wait_semaphores = wait_semaphore ? 1 : 0;
    // Pointers must be null when the count is zero (best-practices)
    const VkSemaphore* p_wait_sems =
        (num_wait_semaphores > 0) ? &wait_semaphore : nullptr;
    const VkPipelineStageFlags* p_wait_masks =
        (num_wait_semaphores > 0) ? &wait_stage_mask : nullptr;
    const VkSemaphore* p_signal_sems =
        (num_signal_semaphores > 0) ? signal_semaphores.data() : nullptr;
    const u64 wait_zero = 0; // dummy for binary wait
    const VkTimelineSemaphoreSubmitInfo timeline_si{
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreValueCount = num_wait_semaphores,
        .pWaitSemaphoreValues    = num_wait_semaphores ? &wait_zero : nullptr,
        .signalSemaphoreValueCount = num_signal_semaphores,
        .pSignalSemaphoreValues = signal_values.data(),
    };
    const VkSubmitInfo submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = &timeline_si,
        .waitSemaphoreCount = num_wait_semaphores,
        .pWaitSemaphores = p_wait_sems,
        .pWaitDstStageMask = p_wait_masks,
        .commandBufferCount = static_cast<u32>(cmdbuffers.size()),
        .pCommandBuffers = cmdbuffers.data(),
        .signalSemaphoreCount = num_signal_semaphores,
        .pSignalSemaphores = p_signal_sems,
    };

    return device.GetGraphicsQueue().Submit(submit_info);
}

VkResult MasterSemaphore::SubmitQueueFence(vk::CommandBuffer& cmdbuf,
                                           vk::CommandBuffer& upload_cmdbuf,
                                           VkSemaphore signal_semaphore, VkSemaphore wait_semaphore,
                                           u64 host_tick) {
    if (device.HasSynchronization2()) {
        const std::array<VkCommandBufferSubmitInfo, 2> cmdbuffer_infos{{
            {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .pNext = nullptr,
                .commandBuffer = *upload_cmdbuf,
                .deviceMask = 0,
            },
            {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .pNext = nullptr,
                .commandBuffer = *cmdbuf,
                .deviceMask = 0,
            },
        }};

        const u32 num_signal_semaphores = signal_semaphore ? 1 : 0;
        const VkSemaphoreSubmitInfo signal_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = signal_semaphore,
            .value = 0,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .deviceIndex = 0,
        };

        const u32 num_wait_semaphores = wait_semaphore ? 1 : 0;
        const VkSemaphoreSubmitInfo wait_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = wait_semaphore,
            .value = 0,
            .stageMask = static_cast<VkPipelineStageFlags2>(wait_stage_mask),
            .deviceIndex = 0,
        };

        const VkSubmitInfo2 submit_info2{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .pNext = nullptr,
            .flags = 0,
            .waitSemaphoreInfoCount = num_wait_semaphores,
            .pWaitSemaphoreInfos = num_wait_semaphores ? &wait_info : nullptr,
            .commandBufferInfoCount = static_cast<u32>(cmdbuffer_infos.size()),
            .pCommandBufferInfos = cmdbuffer_infos.data(),
            .signalSemaphoreInfoCount = num_signal_semaphores,
            .pSignalSemaphoreInfos = num_signal_semaphores ? &signal_info : nullptr,
        };

        auto fence = GetFreeFence();
        auto result = device.GetGraphicsQueue().Submit2(submit_info2, *fence);

        if (result == VK_SUCCESS) {
            std::scoped_lock lock{wait_mutex};
            wait_queue.emplace(host_tick, std::move(fence));
            wait_cv.notify_one();
        }

        return result;
    }

    const u32 num_signal_semaphores = signal_semaphore ? 1 : 0;
    const u32 num_wait_semaphores = wait_semaphore ? 1 : 0;

    const VkSemaphore* p_wait_sems =
            (num_wait_semaphores > 0) ? &wait_semaphore : nullptr;
    const VkPipelineStageFlags* p_wait_masks =
        (num_wait_semaphores > 0) ? &wait_stage_mask : nullptr;
    const VkSemaphore* p_signal_sems =
        (num_signal_semaphores > 0) ? &signal_semaphore : nullptr;
    const std::array cmdbuffers{*upload_cmdbuf, *cmdbuf};

    const VkSubmitInfo submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = num_wait_semaphores,
        .pWaitSemaphores = p_wait_sems,
        .pWaitDstStageMask = p_wait_masks,
        .commandBufferCount = static_cast<u32>(cmdbuffers.size()),
        .pCommandBuffers = cmdbuffers.data(),
        .signalSemaphoreCount = num_signal_semaphores,
        .pSignalSemaphores = p_signal_sems,
    };

    auto fence = GetFreeFence();
    auto result = device.GetGraphicsQueue().Submit(submit_info, *fence);

    if (result == VK_SUCCESS) {
        std::scoped_lock lock{wait_mutex};
        wait_queue.emplace(host_tick, std::move(fence));
        wait_cv.notify_one();
    }

    return result;
}

void MasterSemaphore::WaitThread(std::stop_token token) {
    while (!token.stop_requested()) {
        u64 host_tick;
        vk::Fence fence;
        {
            std::unique_lock lock{wait_mutex};
            wait_cv.wait(lock, token, [this] { return !wait_queue.empty(); });
            if (token.stop_requested()) {
                return;
            }
            std::tie(host_tick, fence) = std::move(wait_queue.front());
            wait_queue.pop();
        }

        fence.Wait();
        fence.Reset();

        {
            std::scoped_lock lock{free_mutex};
            free_queue.push_front(std::move(fence));
            gpu_tick.store(host_tick, std::memory_order_release);
        }
        gpu_tick.notify_one();
    }
}

vk::Fence MasterSemaphore::GetFreeFence() {
    std::scoped_lock lock{free_mutex};
    if (free_queue.empty()) {
        static constexpr VkFenceCreateInfo fence_ci{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0};
        return device.GetLogical().CreateFence(fence_ci);
    }

    auto fence = std::move(free_queue.back());
    free_queue.pop_back();
    return fence;
}

} // namespace Vulkan
