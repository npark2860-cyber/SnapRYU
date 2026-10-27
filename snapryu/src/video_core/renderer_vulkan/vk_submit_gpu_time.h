// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#if SNAPRYU_DIAGNOSTICS
#include <algorithm>
#include <array>
#include <cstdlib>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_fragment_barrier.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

// b46, EDEN_SUBMIT_GPU_TIME=1: GPU time of every submission from two timestamp queries
// (TOP_OF_PIPE right after the primary command buffer begins, BOTTOM_OF_PIPE right before it
// ends). Results are read on the worker thread with a non-waiting vkGetQueryPoolResults after
// later submissions; no GPU wait is added. A ring slot is reused only after its result was read,
// so the in-command-buffer vkCmdResetQueryPool never touches a query that may still execute.
// Every member is used by the worker thread only; Initialize runs before that thread starts.
class SubmitGpuTime {
public:
    static constexpr u32 RING = 1024;

    static bool Requested() {
        static const bool requested = [] {
            const char* value = std::getenv("EDEN_SUBMIT_GPU_TIME");
            return value && value[0] == '1' && value[1] == '\0';
        }();
        return requested;
    }

    [[nodiscard]] bool Enabled() const noexcept {
        return enabled;
    }

    void Initialize(const Device& device_) {
        device = &device_;
        if (!Requested()) {
            LOG_INFO(Render_Vulkan, "SUBMIT_GPU_TIME enabled=false");
            return;
        }
        const VkPhysicalDeviceProperties properties = device->GetPhysical().GetProperties();
        const auto families = device->GetPhysical().GetQueueFamilyProperties();
        const u32 family = device->GetGraphicsFamily();
        const u32 valid_bits =
            family < families.size() ? families[family].timestampValidBits : 0;
        period_ns = properties.limits.timestampPeriod;
        if (valid_bits == 0 || !(period_ns > 0.0f)) {
            LOG_WARNING(Render_Vulkan,
                        "SUBMIT_GPU_TIME enabled=false reason=unsupported valid_bits={} "
                        "period_ns={}",
                        valid_bits, period_ns);
            return;
        }
        try {
            pool = device->GetLogical().CreateQueryPool({
                .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .queryType = VK_QUERY_TYPE_TIMESTAMP,
                .queryCount = RING * 2,
                .pipelineStatistics = 0,
            });
        } catch (const vk::Exception& exception) {
            LOG_WARNING(Render_Vulkan, "SUBMIT_GPU_TIME enabled=false reason=create_pool {}",
                        exception.what());
            return;
        }
        mask = valid_bits >= 64 ? ~u64{0} : ((u64{1} << valid_bits) - 1);
        enabled = true;
        LOG_INFO(Render_Vulkan,
                 "SUBMIT_GPU_TIME enabled=true ring={} period_ns={} valid_bits={} "
                 "sample_log_ms=20 report_every_s=10 extra_gpu_waits=0 "
                 "extra_api=vkGetQueryPoolResults_nonwaiting",
                 RING, period_ns, valid_bits);
    }

    ~SubmitGpuTime() {
        if (enabled && device) {
            // The pool must outlive every submission that writes it.
            static_cast<void>(device->GetLogical().WaitIdle());
        }
    }

    // Worker thread: right after the primary command buffer began, outside any render pass.
    void RecordBegin(vk::CommandBuffer& cmdbuf) {
        if (!enabled) {
            return;
        }
        Collect();
        if (pending_count == RING) {
            ++skipped;
            current_valid = false;
            return;
        }
        const u32 first = next_slot * 2;
        cmdbuf.ResetQueryPool(*pool, first, 2);
        device->GetDispatchLoader().vkCmdWriteTimestamp(*cmdbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                                        *pool, first);
        current_valid = true;
    }

    // Worker thread: right before the primary command buffer ends.
    void RecordEnd(vk::CommandBuffer& cmdbuf) {
        if (!enabled || !current_valid) {
            return;
        }
        device->GetDispatchLoader().vkCmdWriteTimestamp(
            *cmdbuf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *pool, next_slot * 2 + 1);
    }

    // Worker thread: after vkQueueSubmit returned for that command buffer.
    void NoteSubmitted(u64 tick, u32 recorded_passes, u64 render_pass_calls, VkResult result,
                       s64 submit_begin_us, s64 submit_api_us) {
        if (!enabled || !current_valid) {
            return;
        }
        current_valid = false;
        if (result != VK_SUCCESS) {
            // Never executed (or retried later); the slot is simply reused.
            return;
        }
        Pending& entry = pending[(pending_head + pending_count) % RING];
        entry = Pending{tick, next_slot, recorded_passes, render_pass_calls, submit_begin_us,
                        submit_api_us};
        ++pending_count;
        next_slot = (next_slot + 1) % RING;
        Collect();
    }

private:
    struct Pending {
        u64 tick{};
        u32 slot{};
        u32 recorded_passes{};
        u64 render_pass_calls{};
        s64 submit_begin_us{};
        s64 submit_api_us{};
    };

    void Collect() {
        while (pending_count > 0) {
            const Pending entry = pending[pending_head];
            std::array<u64, 4> data{};
            const VkResult result = device->GetLogical().GetQueryResults(
                *pool, entry.slot * 2, 2, sizeof(data), data.data(), sizeof(u64) * 2,
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            if (result != VK_SUCCESS && result != VK_NOT_READY) {
                if (++errors <= 4) {
                    LOG_WARNING(Render_Vulkan, "SUBMIT_GPU_TIME query_result={} tick={}",
                                static_cast<int>(result), entry.tick);
                }
                return;
            }
            if (data[1] == 0 || data[3] == 0) {
                return; // Not available yet; later entries complete in order.
            }
            pending_head = (pending_head + 1) % RING;
            --pending_count;
            Note(entry, data[0], data[2]);
        }
    }

    void Note(const Pending& entry, u64 begin, u64 end) {
        const u64 span_ticks = (end - begin) & mask;
        const u64 since_prev_ticks = have_prev ? ((end - prev_end) & mask) : span_ticks;
        prev_end = end;
        have_prev = true;
        const u64 span_us = static_cast<u64>(static_cast<double>(span_ticks) * period_ns / 1000.0);
        const u64 busy_us =
            static_cast<u64>(static_cast<double>(since_prev_ticks) * period_ns / 1000.0);
        ++samples;
        sum_span_us += span_us;
        ++histogram[Bucket(span_us)];
        if (span_us > max_span_us) {
            max_span_us = span_us;
            max_tick = entry.tick;
            max_passes = entry.recorded_passes;
        }
        if (span_us >= 20'000) {
            ++long_samples;
            if (long_samples <= 256 || long_samples % 64 == 0) {
                LOG_WARNING(Render_Vulkan,
                            "GPU_SUBMIT_TIME event={} tick={} gpu_span_us={} "
                            "gpu_since_prev_end_us={} recorded_passes={} render_pass_calls={} "
                            "us_per_pass={:.1f} submit_begin_us={} submit_api_us={} "
                            "clock=gpu_timestamp log_first=256 then_every=64",
                            long_samples, entry.tick, span_us, busy_us, entry.recorded_passes,
                            entry.render_pass_calls,
                            static_cast<double>(span_us) /
                                static_cast<double>(std::max<u32>(entry.recorded_passes, 1)),
                            entry.submit_begin_us, entry.submit_api_us);
            }
        }
        const s64 now = FragmentBarrier::NowUs();
        if (!last_report_us) {
            last_report_us = now;
            return;
        }
        if (now - last_report_us < 10'000'000) {
            return;
        }
        last_report_us = now;
        LOG_INFO(Render_Vulkan,
                 "GPU_SUBMIT_TIME_REPORT report={} samples={} long20ms={} sum_ms={:.1f} "
                 "max_ms={:.1f} max_tick={} max_passes={} hist_lt5={} lt10={} lt20={} lt50={} "
                 "lt100={} lt200={} lt500={} ge500={} skipped={} pending={} "
                 "cumulative_worker=true",
                 ++reports, samples, long_samples, static_cast<double>(sum_span_us) / 1000.0,
                 static_cast<double>(max_span_us) / 1000.0, max_tick, max_passes, histogram[0],
                 histogram[1], histogram[2], histogram[3], histogram[4], histogram[5],
                 histogram[6], histogram[7], skipped, pending_count);
    }

    static size_t Bucket(u64 span_us) {
        static constexpr std::array<u64, 7> LIMITS_US{5'000,   10'000,  20'000, 50'000,
                                                       100'000, 200'000, 500'000};
        size_t bucket = 0;
        while (bucket < LIMITS_US.size() && span_us >= LIMITS_US[bucket]) {
            ++bucket;
        }
        return bucket;
    }

    const Device* device{};
    vk::QueryPool pool;
    bool enabled{};
    bool current_valid{};
    float period_ns{};
    u64 mask{};
    u32 next_slot{};
    std::array<Pending, RING> pending{};
    u32 pending_head{};
    u32 pending_count{};
    u64 prev_end{};
    bool have_prev{};
    u64 samples{}, long_samples{}, sum_span_us{}, max_span_us{}, max_tick{}, skipped{}, errors{},
        reports{};
    u32 max_passes{};
    std::array<u64, 8> histogram{};
    s64 last_report_us{};
};

} // namespace Vulkan
#endif
