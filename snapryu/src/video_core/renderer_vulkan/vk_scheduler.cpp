// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <fmt/format.h>

#include "video_core/renderer_vulkan/vk_query_cache.h"

#include "common/logging.h"
#include "common/settings.h"
#include "common/thread.h"
#include "video_core/gpu_logging/gpu_logging.h"
#include "video_core/renderer_vulkan/vk_adreno_profiler.h"
#include "video_core/renderer_vulkan/vk_command_pool.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#if SNAPRYU_DIAGNOSTICS
#include "video_core/renderer_vulkan/vk_submit_work_trace.h"
#include "video_core/renderer_vulkan/vk_rp_boundary_trace.h"
#include "video_core/renderer_vulkan/vk_pass_capture.h"
#endif
#if SNAPRYU_DIAGNOSTICS
#include "video_core/renderer_vulkan/vk_queue_lock_trace.h"
#endif
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

#if SNAPRYU_DIAGNOSTICS
namespace {
// Worker-thread-only, bounded history. No extra GPU call or wait on successful submissions.
// A cached GPU tick can lag actual completion; it must not be read as an exact queue depth.
class SubmitFailureTrace {
public:
    using Clock = std::chrono::steady_clock;
    struct Sample {
        u64 tick{}, previous_submitted{}, known_gpu{}, queued_tick{}, render_pass_calls{};
        s64 submit_us{}, begin_us{}, lock_wait_us{};
        VkResult result{};
        bool signal_binary{}, wait_binary{};
        u32 recorded_render_passes{};
        bool render_pass_budget_split{};
    };

    bool Enabled() {
        static const bool enabled = [] {
            const char* value = std::getenv("EDEN_SUBMIT_FAILURE_TRACE");
            const bool on = value && value[0] == '1' && value[1] == '\0';
            LOG_INFO(Render_Vulkan, "SUBMIT_FAILURE_TRACE enabled={} history=64 extra_gpu_waits=0", on);
            return on;
        }();
        return enabled;
    }

    void Note(const Sample& sample) {
        samples[total++ % samples.size()] = sample;
        if (sample.render_pass_budget_split) {
            ++budget_splits;
            if (budget_splits <= 32 || budget_splits % 128 == 0) {
                LOG_INFO(Render_Vulkan,
                         "GPU_RP_BUDGET_SUBMIT event={} tick={} recorded_passes={} "
                         "render_pass_calls={} submit_us={} result={} known_gpu={} "
                         "log_first=32 then_every=128",
                         budget_splits, sample.tick, sample.recorded_render_passes,
                         sample.render_pass_calls, sample.submit_us, static_cast<int>(sample.result),
                         sample.known_gpu);
            }
        }
        if (sample.submit_us >= 50'000 || sample.lock_wait_us >= 50'000) {
            ++slow_events;
            if (slow_events <= 128 || slow_events % 128 == 0) {
                LOG_WARNING(Render_Vulkan,
                            "GPU_SUBMIT_STALL event={} begin_us={} tick={} known_gpu={} "
                            "render_pass_calls={} lock_wait_us={} submit_us={} result={} "
                            "clock=steady_epoch_us log_first=128 then_every=128",
                            slow_events, sample.begin_us, sample.tick, sample.known_gpu,
                            sample.render_pass_calls, sample.lock_wait_us, sample.submit_us,
                            static_cast<int>(sample.result));
            }
        }
        if (sample.result == VK_SUCCESS) {
            return;
        }
        const u64 count = std::min<u64>(total, samples.size());
        LOG_ERROR(Render_Vulkan,
                  "SUBMIT_FAILURE_TRACE failure_tick={} records={} total={} cached_gpu_tick_may_lag=true",
                  sample.tick, count, total);
        for (u64 i = total - count; i < total; ++i) {
            const auto& entry = samples[i % samples.size()];
            LOG_ERROR(Render_Vulkan,
                      "SUBMIT_FAILURE_HISTORY tick={} previous_submitted={} known_gpu={} queued_tick={} "
                      "render_pass_calls={} submit_us={} result={} signal_binary={} wait_binary={} "
                      "begin_us={} lock_wait_us={} recorded_passes={} rp_budget_split={}",
                      entry.tick, entry.previous_submitted, entry.known_gpu, entry.queued_tick,
                      entry.render_pass_calls, entry.submit_us, static_cast<int>(entry.result),
                      entry.signal_binary, entry.wait_binary, entry.begin_us, entry.lock_wait_us,
                      entry.recorded_render_passes, entry.render_pass_budget_split);
        }
    }

private:
    std::array<Sample, 64> samples{};
    u64 total{};
    u64 slow_events{};
    u64 budget_splits{};
};
} // namespace
#endif

void Scheduler::CommandChunk::ExecuteAll(vk::CommandBuffer cmdbuf,
                                         vk::CommandBuffer upload_cmdbuf) {
    auto command = first;
    while (command != nullptr) {
        auto next = command->GetNext();
        command->Execute(cmdbuf, upload_cmdbuf);
        command->~Command();
        command = next;
    }
    submit = false;
    command_offset = 0;
    first = nullptr;
    last = nullptr;
}

Scheduler::Scheduler(const Device& device_, StateTracker& state_tracker_)
    : device{device_}, state_tracker{state_tracker_},
      master_semaphore{std::make_unique<MasterSemaphore>(device)},
      command_pool{std::make_unique<CommandPool>(*master_semaphore, device)} {

#if SNAPRYU_DIAGNOSTICS
    SubmitWorkTrace::Reset(master_semaphore.get());
    gpu_time.Initialize(device);
#endif
    AcquireNewChunk();
    AllocateWorkerCommandBuffer();
    worker_thread = std::jthread([this](std::stop_token token) { WorkerThread(token); });
}

Scheduler::~Scheduler() = default;

u64 Scheduler::Flush(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore) {
    // When flushing, we only send data to the worker thread; no waiting is necessary.
    const u64 signal_value = SubmitExecution(signal_semaphore, wait_semaphore);
    AllocateNewContext();
    return signal_value;
}

void Scheduler::Finish(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore) {
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    SubmitExecution(signal_semaphore, wait_semaphore);
    auto& profiler = AdrenoProfiler::Get();
    const auto wait_start = profiler.Enabled() ? AdrenoProfiler::Now() : AdrenoProfiler::TimePoint{};
    Wait(presubmit_tick);
    if (profiler.Enabled()) {
        profiler.RecordFinishWait(AdrenoProfiler::ElapsedNs(wait_start));
    }
    AllocateNewContext();
}

void Scheduler::Shutdown() {
    shutting_down.store(true, std::memory_order_relaxed);
    try {
        Flush();
        WaitWorker();
    } catch (const vk::Exception& exception) {
        LOG_WARNING(Render_Vulkan, "Scheduler shutdown: {}", exception.what());
    }
}

void Scheduler::WaitWorker() {
    auto& profiler = AdrenoProfiler::Get();
    const auto wait_start = profiler.Enabled() ? AdrenoProfiler::Now() : AdrenoProfiler::TimePoint{};
    DispatchWork();

    // Ensure the queue is drained.
    {
        std::unique_lock ql{queue_mutex};
        event_cv.wait(ql, [this] { return work_queue.empty(); });
    }

    // Now wait for execution to finish.
    std::scoped_lock el{execution_mutex};
    if (profiler.Enabled()) {
        profiler.RecordWorkerWait(AdrenoProfiler::ElapsedNs(wait_start));
    }
}

void Scheduler::DispatchWork() {
    if (chunk && !chunk->Empty()) {
        {
            std::scoped_lock ql{queue_mutex};
            work_queue.push(std::move(chunk));
        }
        event_cv.notify_all();
        AcquireNewChunk();
    }
}

namespace {
// EDEN_GPU_FULL_BARRIERS=1: surround render passes and compute dispatches with full
// ALL_COMMANDS memory barriers. Diagnostic only, for missing-synchronisation artifacts on tiled
// GPUs (stale per-bin content).
[[nodiscard]] bool GpuFullBarriers() {
#if SNAPRYU_DIAGNOSTICS
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_GPU_FULL_BARRIERS");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
#else
    return false;
#endif
}

void RecordFullBarrier(vk::CommandBuffer cmdbuf) {
    static constexpr VkMemoryBarrier FULL_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           0, FULL_BARRIER);
}
} // namespace

void Scheduler::RecordFullBarrierNow() {
    Record([](vk::CommandBuffer cmdbuf) { RecordFullBarrier(cmdbuf); });
}

bool Scheduler::DebugFullBarriers() {
    return GpuFullBarriers();
}

void Scheduler::RecordDebugFullBarrier() {
    if (GpuFullBarriers()) {
        Record([](vk::CommandBuffer cmdbuf) { RecordFullBarrier(cmdbuf); });
    }
}

void Scheduler::BeginRenderPassImpl(const Framebuffer* framebuffer, VkRenderPass renderpass,
                                    const VkClearValue* clear_values, u32 clear_value_count) {
    ++recorded_render_passes;
    // Close any query opened by a deferred clear before entering its render pass.
    query_cache->CounterClose(VideoCommon::QueryType::ZPassPixelCount64);
    const VkFramebuffer framebuffer_handle = framebuffer->Handle();
    const VkExtent2D render_area = framebuffer->RenderArea();
    state.renderpass = renderpass;
    state.framebuffer = framebuffer_handle;
    state.render_area = render_area;

    if (GPU::Logging::IsActive() && Settings::values.gpu_log_vulkan_calls.GetValue()) {
        const std::string render_pass_info =
            fmt::format("renderArea={}x{}, numImages={}", render_area.width, render_area.height,
                        framebuffer->NumImages());
        GPU::Logging::GPULogger::GetInstance().LogRenderPassBegin(render_pass_info);
    }

    RecordDebugFullBarrier();

    std::array<VkClearValue, 9> values{};
    for (u32 i = 0; i < clear_value_count && i < values.size(); ++i) {
        values[i] = clear_values[i];
    }
    Record([renderpass, framebuffer_handle, render_area, values, clear_value_count](
               vk::CommandBuffer cmdbuf) {
        const VkRenderPassBeginInfo renderpass_bi{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .pNext = nullptr,
            .renderPass = renderpass,
            .framebuffer = framebuffer_handle,
            .renderArea =
                {
                    .offset = {.x = 0, .y = 0},
                    .extent = render_area,
                },
            .clearValueCount = clear_value_count,
            .pClearValues = clear_value_count != 0 ? values.data() : nullptr,
        };
        cmdbuf.BeginRenderPass(renderpass_bi, VK_SUBPASS_CONTENTS_INLINE);
    });
    num_renderpass_images = framebuffer->NumImages();
    renderpass_images = framebuffer->Images();
    renderpass_image_ranges = framebuffer->ImageRanges();
#if SNAPRYU_DIAGNOSTICS
    // b47/b49: a pass starts with nothing written or sampled; a load-op clear writes every image
    // (conservative: the clear mask is not inspected).
    pass_written.Clear();
    pass_sampled.Clear();
    pass_unchecked = false;
    pass_depth_image = framebuffer->DepthImage();
    if (clear_value_count != 0) {
        pass_written.AddAll({renderpass_images.data(), num_renderpass_images});
    }
#endif
    framebuffer->MarkResolveShadowsUpToDate();
    AdrenoProfiler::Get().RecordRenderPassBegin(framebuffer->NumImages());
}

void Scheduler::RealizeDeferredClear() {
    if (deferred_clear.framebuffer == nullptr) {
        return;
    }
    const DeferredClear dc = deferred_clear;
    deferred_clear = {};
    AdrenoProfiler::Get().RecordDeferredClear();

    std::array<VkClearValue, 9> clear_values{};
    u32 count = 0;
    const RenderPassKey& base = dc.framebuffer->RenderPassKeyBase();
    for (u32 slot = 0; slot < 8; ++slot) {
        if (base.color_formats[slot] == VideoCore::Surface::PixelFormat::Invalid) {
            continue;
        }
        clear_values[count++] = dc.color_values[slot];
    }
    if (base.depth_format != VideoCore::Surface::PixelFormat::Invalid) {
        clear_values[count++] = dc.depth_stencil_value;
    }
    const u32 color_discard_mask =
        dc.framebuffer->DiscardsMsaaColor() ? dc.color_clear_mask : 0u;
    const bool depth_stencil_discard =
        dc.depth_stencil && dc.framebuffer->DiscardsMsaaDepthStencil();
    const VkRenderPass renderpass = dc.framebuffer->RenderPassVariant(
        dc.color_clear_mask, dc.depth_stencil, color_discard_mask, depth_stencil_discard);
    EndRenderPass(RenderPassEndReason::DeferredClear);
    BeginRenderPassImpl(dc.framebuffer, renderpass, clear_values.data(), count);
}

bool Scheduler::DeferColorClear(const Framebuffer* framebuffer, u32 rt_slot,
                                const VkClearValue& value) {
    if (IsRenderPassActive()) {
        return false;
    }
    if (deferred_clear.framebuffer != nullptr && deferred_clear.framebuffer != framebuffer) {
        RealizeDeferredClear();
        EndRenderPass(RenderPassEndReason::DeferredClear);
    }
    deferred_clear.framebuffer = framebuffer;
    deferred_clear.color_clear_mask |= 1u << rt_slot;
    deferred_clear.color_values[rt_slot] = value;
    return true;
}

bool Scheduler::DeferDepthStencilClear(const Framebuffer* framebuffer, const VkClearValue& value) {
    if (IsRenderPassActive()) {
        return false;
    }
    if (deferred_clear.framebuffer != nullptr && deferred_clear.framebuffer != framebuffer) {
        RealizeDeferredClear();
        EndRenderPass(RenderPassEndReason::DeferredClear);
    }
    deferred_clear.framebuffer = framebuffer;
    deferred_clear.depth_stencil = true;
    deferred_clear.depth_stencil_value = value;
    return true;
}

void Scheduler::FlushDeferredClear() {
    if (deferred_clear.framebuffer == nullptr) {
        return;
    }
    RealizeDeferredClear();
    EndRenderPass(RenderPassEndReason::FlushDeferredClear);
}

void Scheduler::RequestRenderpass(const Framebuffer* framebuffer
                                  , std::span<const VkImage> shader_images, bool checked_draw
#if SNAPRYU_DIAGNOSTICS
                                  , u64 vertex_hash, u64 fragment_hash
                                  , std::span<const VkImage> written_images
#endif
                                  ) {
    const bool defer_enabled = FragmentBarrier::Enabled();
    // Resolve-shadow attachments are not enumerated by Framebuffer::Images().
#if SNAPRYU_DIAGNOSTICS
    const bool written_rule = defer_enabled && WfiDefer::WrittenRule();
    const std::span<const VkImage> fb_images{framebuffer->Images().data(),
                                             framebuffer->NumImages()};
    // b48 rule: a safe draw samples no attachment of its framebuffer. b47/b49 written rule
    // (EDEN_WFI_DEFER=2): a safe draw is merely checked (no storage/XFB/MSAA, no null inputs);
    // overlap with this pass is decided below from what the pass already wrote and sampled.
    const bool safe_draw = defer_enabled && framebuffer->Samples() == VK_SAMPLE_COUNT_1_BIT &&
        (written_rule
             ? FragmentBarrier::CheckedInputs<VkImage>(fb_images, shader_images, checked_draw)
             : FragmentBarrier::SafeImages<VkImage>(fb_images, shader_images, checked_draw));
    if (defer_enabled && (fragment_barrier.pending || wfi_defer.pending)) {
        bool join = safe_draw;
        if (written_rule) {
            // RAW: samples something this pass wrote. WAR: writes something this pass sampled.
            join = join && !pass_written.Any(shader_images) && !pass_sampled.Any(written_images);
        } else {
            join = join && FragmentBarrier::SafeImages<VkImage>(
                               {renderpass_images.data(), num_renderpass_images}, shader_images,
                               checked_draw);
        }
        if (!join) {
            fragment_barrier.overlap_or_unknown += fragment_barrier.pending;
            wfi_defer.overlap_or_unknown += wfi_defer.pending;
            wfi_defer.written_rejects += written_rule;
            RequestOutsideRenderPassOperationContext();
        } else if (written_rule &&
                   !FragmentBarrier::SafeImages<VkImage>(fb_images, shader_images, checked_draw)) {
            ++wfi_defer.written_merges;
        }
    }
    // b47/b49: account the draw to the (possibly new) pass, in every mode (the inspect counters
    // use it too). Unchecked draws may touch any attachment, so they taint every image.
    const auto note_sets = [&] {
        if (checked_draw) {
            pass_sampled.AddAll(shader_images);
            pass_written.AddAll(written_images);
        } else {
            pass_unchecked = true;
            pass_sampled.AddAll(fb_images);
            pass_written.AddAll(fb_images);
        }
    };
#else
    const bool safe_draw = defer_enabled && framebuffer->Samples() == VK_SAMPLE_COUNT_1_BIT &&
        FragmentBarrier::SafeImages<VkImage>(
        {framebuffer->Images().data(), framebuffer->NumImages()}, shader_images, checked_draw);
    if (defer_enabled && (fragment_barrier.pending || wfi_defer.pending) &&
        (!safe_draw || !FragmentBarrier::SafeImages<VkImage>(
            {renderpass_images.data(), num_renderpass_images}, shader_images, checked_draw))) {
        fragment_barrier.overlap_or_unknown += fragment_barrier.pending;
        wfi_defer.overlap_or_unknown += wfi_defer.pending;
        RequestOutsideRenderPassOperationContext();
    }
#endif
    if (deferred_clear.framebuffer == framebuffer) {
        RealizeDeferredClear();
        if (defer_enabled) fragment_barrier.pass_safe &= safe_draw;
#if SNAPRYU_DIAGNOSTICS
        note_sets();
        pass_break.Draw(false, vertex_hash, fragment_hash, checked_draw, framebuffer->Handle(),
                        fb_images, framebuffer->RenderArea(), shader_images, written_images);
#endif
        return;
    }
    const VkRenderPass renderpass = framebuffer->RenderPass();
    const VkFramebuffer framebuffer_handle = framebuffer->Handle();
    const VkExtent2D render_area = framebuffer->RenderArea();
    if (renderpass == state.renderpass && framebuffer_handle == state.framebuffer &&
        render_area.width == state.render_area.width &&
        render_area.height == state.render_area.height) {
        AdrenoProfiler::Get().RecordRenderPassReuse();
        if (defer_enabled) {
            fragment_barrier.reused_draws += fragment_barrier.pending;
            wfi_defer.reused_draws += wfi_defer.pending;
            fragment_barrier.pass_safe &= safe_draw;
        }
#if SNAPRYU_DIAGNOSTICS
        note_sets();
        pass_break.Draw(true, vertex_hash, fragment_hash, checked_draw, framebuffer->Handle(),
                        fb_images, framebuffer->RenderArea(), shader_images, written_images);
#endif
        return;
    }
    // Ends any active pass and realizes a deferred clear
    EndRenderPass(RenderPassEndReason::FramebufferChange);
    BeginRenderPassImpl(framebuffer, renderpass, nullptr, 0);
    if (defer_enabled) fragment_barrier.pass_safe &= safe_draw;
#if SNAPRYU_DIAGNOSTICS
    note_sets();
    pass_break.Draw(false, vertex_hash, fragment_hash, checked_draw, framebuffer->Handle(),
                    fb_images, framebuffer->RenderArea(), shader_images, written_images);
#endif
}

void Scheduler::RequestFragmentBarrier() {
    // Preserve full barriers for deferred clears, feedback, storage writes, XFB,
    // uninspected helper work and the explicit full-barrier diagnostic.
    if (fragment_barrier.Request(state.renderpass != VK_NULL_HANDLE,
                                 deferred_clear.framebuffer != nullptr, GpuFullBarriers())) {
        return;
    }
    RequestOutsideRenderPassOperationContext();
}

void Scheduler::RecordWaitForIdleBarrier(VkEvent event, VkPipelineStageFlags flags) {
    if (WfiDefer::UseBarrier()) {
        // Execution-only dependency with the same scopes as the event pair below.
        Record([flags](vk::CommandBuffer cmdbuf) {
            cmdbuf.PipelineBarrier(flags, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, nullptr, nullptr,
                                   nullptr);
        });
        return;
    }
    // Identical to the original RasterizerVulkan::WaitForIdle recording.
    Record([event, flags](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetEvent(event, flags);
        cmdbuf.WaitEvents(event, flags, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, {}, {}, {});
    });
}

void Scheduler::RequestWaitForIdle(VkEvent event, VkPipelineStageFlags flags) {
    // Same eligibility as RequestFragmentBarrier. A deferred WFI is materialised by
    // EndRenderPass right after the attachment barriers, before any command that could consume
    // the pass's results (copies, dispatches, query syncs, framebuffer changes, submissions).
    if (wfi_defer.Request(state.renderpass != VK_NULL_HANDLE,
                          deferred_clear.framebuffer != nullptr, fragment_barrier.pass_safe,
                          GpuFullBarriers(), event, flags)) {
        return;
    }
    RequestOutsideRenderPassOperationContext();
    RecordWaitForIdleBarrier(event, flags);
}

#if SNAPRYU_DIAGNOSTICS
void Scheduler::RequestFeedbackLoopBarrier() {
    // The texture cache found the depth/stencil attachment bound as a texture for the next draw.
    // Original behaviour (and every mode but the written rule): end the pass. Written rule: the
    // pass only has to end when it may have written that attachment; the draw's own RAW/WAR test
    // in RequestRenderpass runs afterwards as well.
    if (!WfiDefer::WrittenRule() || deferred_clear.framebuffer != nullptr || !state.renderpass) {
        RequestOutsideRenderPassOperationContext();
        return;
    }
    if (pass_unchecked || !pass_depth_image || pass_written.Contains(pass_depth_image)) {
        ++wfi_defer.feedback_breaks;
        RequestOutsideRenderPassOperationContext();
        return;
    }
    ++wfi_defer.feedback_skips;
}
#endif

void Scheduler::RequestOutsideRenderPassOperationContext() {
#if SNAPRYU_DIAGNOSTICS
    const bool boundary_trace = RpBoundaryTrace::Enabled();
    const bool closing_pass = boundary_trace && (state.renderpass || deferred_clear.framebuffer);
    const u32 closing_images = closing_pass
        ? (deferred_clear.framebuffer ? deferred_clear.framebuffer->NumImages() : num_renderpass_images)
        : 0;
    const auto* last_pipeline = state.graphics_pipeline;
    const bool shader_match = closing_pass && last_pipeline &&
        last_pipeline->ShaderHashes()[1] == 0x46cd8b9ce104b053ULL &&
        last_pipeline->ShaderHashes()[5] == 0x53fe89d883ee502cULL;
#if defined(__GNUC__) || defined(__clang__)
    const void* const boundary_caller = __builtin_extract_return_addr(__builtin_return_address(0));
#else
    const void* const boundary_caller = nullptr;
#endif
#endif
    EndRenderPass(RenderPassEndReason::OutsideOperation);
#if SNAPRYU_DIAGNOSTICS
    if (closing_pass) RpBoundaryTrace::Note(boundary_caller, closing_images, shader_match);
#endif
}

bool Scheduler::UpdateGraphicsPipeline(GraphicsPipeline* pipeline) {
    if (state.graphics_pipeline == pipeline) {
        if (pipeline && pipeline->UsesExtendedDynamicState() &&
            state.needs_state_enable_refresh) {
            state_tracker.InvalidateStateEnableFlag();
            state.needs_state_enable_refresh = false;
        }
        return false;
    }

    state.graphics_pipeline = pipeline;

    if (!pipeline) {
        return true;
    }

    if (!pipeline->UsesExtendedDynamicState()) {
        state.needs_state_enable_refresh = true;
    } else if (state.needs_state_enable_refresh) {
        state_tracker.InvalidateStateEnableFlag();
        state.needs_state_enable_refresh = false;
    }

    return true;
}

bool Scheduler::UpdateRescaling(bool is_rescaling) {
    if (state.rescaling_defined && is_rescaling == state.is_rescaling) {
        return false;
    }
    state.rescaling_defined = true;
    state.is_rescaling = is_rescaling;
    return true;
}

bool Scheduler::UpdateDescriptorBufferChunk(u32 descriptor_chunk) {
    if (state.descriptor_buffer_bound && descriptor_chunk == state.descriptor_buffer_chunk) {
        return false;
    }
    state.descriptor_buffer_bound = true;
    state.descriptor_buffer_chunk = descriptor_chunk;
    AdrenoProfiler::Get().RecordDescriptorBufferBind();
    return true;
}

void Scheduler::WorkerThread(std::stop_token stop_token) {
    Common::SetCurrentThreadName("VulkanWorker");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    Common::SetCurrentThreadToPerformanceCores();

    const auto TryPopQueue{[this](auto& work) -> bool {
        if (work_queue.empty()) {
            return false;
        }

        work = std::move(work_queue.front());
        work_queue.pop();
        event_cv.notify_all();
        return true;
    }};

    while (!stop_token.stop_requested()) {
        std::unique_ptr<CommandChunk> work;

        {
            std::unique_lock lk{queue_mutex};

            // Wait for work.
            event_cv.wait(lk, stop_token, [&] { return TryPopQueue(work); });

            // If we've been asked to stop, we're done.
            if (stop_token.stop_requested()) {
                return;
            }

            // Exchange lock ownership so that we take the execution lock before
            // the queue lock goes out of scope. This allows us to force execution
            // to complete in the next step.
            std::exchange(lk, std::unique_lock{execution_mutex});

            // Perform the work, tracking whether the chunk was a submission
            // before executing.
            const bool has_submit = work->HasSubmit();
            work->ExecuteAll(current_cmdbuf, current_upload_cmdbuf);

            // If the chunk was a submission, reallocate the command buffer.
            if (has_submit) {
                AllocateWorkerCommandBuffer();
            }
        }

        {
            std::scoped_lock rl{reserve_mutex};

            // Recycle the chunk back to the reserve.
            chunk_reserve.emplace_back(std::move(work));
        }
    }
}

void Scheduler::AllocateWorkerCommandBuffer() {
    // Account the driver objects recorded into the pair just submitted and keep the Adreno
    // driver's object pool from filling up (see CommandPool::Maintain).
    command_pool->Maintain(std::exchange(vk::render_pass_calls, u64{0}), last_submitted_tick);
    current_cmdbuf = vk::CommandBuffer(command_pool->Commit(), device.GetDispatchLoader());
    current_cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });
#if SNAPRYU_DIAGNOSTICS
    gpu_time.RecordBegin(current_cmdbuf);
#endif
    current_upload_cmdbuf = vk::CommandBuffer(command_pool->Commit(), device.GetDispatchLoader());
    current_upload_cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });
}

u64 Scheduler::SubmitExecution(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore) {
    EndPendingOperations();
    InvalidateState();
#if SNAPRYU_DIAGNOSTICS
    RpBoundaryTrace::MaybeReport();
    fragment_barrier.MaybeReport();
    wfi_defer.MaybeReport();
    pass_break.MaybeReport();
#endif

    // EndPendingOperations may realize one last deferred clear; count it before resetting.
    const u32 submitted_render_passes = std::exchange(recorded_render_passes, 0);
    const bool budget_split = std::exchange(render_pass_budget_split, false);
#if !SNAPRYU_DIAGNOSTICS
    static_cast<void>(submitted_render_passes);
    static_cast<void>(budget_split);
#endif

    const u64 signal_value = master_semaphore->NextTick();
    RecordWithUploadBuffer([signal_semaphore, wait_semaphore, signal_value,
                            this
#if SNAPRYU_DIAGNOSTICS
                            , submitted_render_passes, budget_split
#endif
                            ](vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf) {
        static constexpr VkMemoryBarrier WRITE_BARRIER{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        };
        upload_cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, WRITE_BARRIER);
        upload_cmdbuf.End();
#if SNAPRYU_DIAGNOSTICS
        gpu_time.RecordEnd(cmdbuf);
#endif
        cmdbuf.End();

        if (on_submit) {
            on_submit();
        }

#if SNAPRYU_DIAGNOSTICS
        const bool work_trace = SubmitWorkTrace::Enabled();
        const auto submit_work = SubmitWorkTrace::Take();
#endif
#if SNAPRYU_DIAGNOSTICS
        static thread_local SubmitFailureTrace trace;
        const bool trace_enabled = trace.Enabled();
        const auto lock_begin = trace_enabled ? SubmitFailureTrace::Clock::now()
                                              : SubmitFailureTrace::Clock::time_point{};
#endif
#if SNAPRYU_DIAGNOSTICS
        QueueLockTrace::Scope queue_trace{QueueLockTrace::Owner::Scheduler};
#endif
        std::scoped_lock lock{submit_mutex};
#if SNAPRYU_DIAGNOSTICS
        queue_trace.Acquired();
#endif
#if SNAPRYU_DIAGNOSTICS
        const auto lock_end = trace_enabled ? SubmitFailureTrace::Clock::now()
                                            : SubmitFailureTrace::Clock::time_point{};
        SubmitFailureTrace::Sample trace_sample{};
        if (trace_enabled) {
            trace_sample.tick = signal_value;
            trace_sample.previous_submitted = last_submitted_tick;
            trace_sample.known_gpu = master_semaphore->KnownGpuTick();
            trace_sample.queued_tick = master_semaphore->CurrentTick();
            trace_sample.render_pass_calls = vk::render_pass_calls;
            trace_sample.recorded_render_passes = submitted_render_passes;
            trace_sample.render_pass_budget_split = budget_split;
            trace_sample.signal_binary = signal_semaphore != VK_NULL_HANDLE;
            trace_sample.wait_binary = wait_semaphore != VK_NULL_HANDLE;
            trace_sample.lock_wait_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                           lock_end - lock_begin).count();
        }
        const auto submit_start = trace_enabled ? SubmitFailureTrace::Clock::now()
                                                : SubmitFailureTrace::Clock::time_point{};
#endif
#if SNAPRYU_DIAGNOSTICS
        const u64 work_known_gpu = work_trace ? master_semaphore->KnownGpuTick() : 0;
        const s64 work_begin = work_trace ? SubmitWorkTrace::NowUs() : 0;
#endif
#if SNAPRYU_DIAGNOSTICS
        const s64 gpu_time_begin_us = gpu_time.Enabled() ? FragmentBarrier::NowUs() : 0;
#endif
        VkResult submit_result = master_semaphore->SubmitQueue(
            cmdbuf, upload_cmdbuf, signal_semaphore, wait_semaphore, signal_value);
#if SNAPRYU_DIAGNOSTICS
        const s64 work_end = work_trace ? SubmitWorkTrace::NowUs() : 0;
        if (gpu_time.Enabled()) {
            gpu_time.NoteSubmitted(signal_value, submitted_render_passes, vk::render_pass_calls,
                                   submit_result, gpu_time_begin_us,
                                   FragmentBarrier::NowUs() - gpu_time_begin_us);
        }
#endif
#if SNAPRYU_DIAGNOSTICS
        if (trace_enabled) {
            trace_sample.begin_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                        submit_start.time_since_epoch()).count();
            trace_sample.submit_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                         SubmitFailureTrace::Clock::now() - submit_start).count();
            trace_sample.result = submit_result;
            trace.Note(trace_sample);
        }
#endif
#if SNAPRYU_DIAGNOSTICS
        if (work_trace) {
            SubmitWorkTrace::Publish({master_semaphore.get(), signal_value, work_known_gpu,
                                      vk::render_pass_calls, work_begin, work_end,
                                      static_cast<int>(submit_result), submit_work});
        }
#endif
        // The Adreno Windows driver occasionally rejects a submit with a transient error
        // (observed: VK_ERROR_INITIALIZATION_FAILED from vkQueueSubmit2), which used to escape as
        // an uncaught vk::Exception on the worker thread. Drain the device and retry a few times
        // before treating it as fatal. Device loss is never retried.
        for (int attempt = 0; attempt < 3 && submit_result != VK_SUCCESS &&
                              submit_result != VK_ERROR_DEVICE_LOST;
             ++attempt) {
            LOG_WARNING(Render_Vulkan, "vkQueueSubmit failed with {} (tick {}), retry {}",
                        static_cast<int>(submit_result), signal_value, attempt + 1);
            const VkResult idle_result = device.GetLogical().WaitIdle();
#if SNAPRYU_DIAGNOSTICS
            if (trace_enabled) {
                LOG_ERROR(Render_Vulkan, "SUBMIT_FAILURE_RETRY tick={} attempt={} device_wait_idle_result={} known_gpu={} previous_submitted={}",
                          signal_value, attempt + 1, static_cast<int>(idle_result),
                          master_semaphore->KnownGpuTick(), last_submitted_tick);
            }
#else
            static_cast<void>(idle_result);
#endif
            submit_result = master_semaphore->SubmitQueue(cmdbuf, upload_cmdbuf, signal_semaphore,
                                                          wait_semaphore, signal_value);
        }
        switch (const VkResult result = submit_result) {
        case VK_SUCCESS:
            last_submitted_tick = signal_value;
            // Log successful queue submission
            if (GPU::Logging::IsActive() &&
                Settings::values.gpu_log_vulkan_calls.GetValue()) {
                GPU::Logging::GPULogger::GetInstance().LogVulkanCall(
                    "vkQueueSubmit", "", VK_SUCCESS);
            }
            break;
        case VK_ERROR_DEVICE_LOST:
            device.ReportLoss();
            [[fallthrough]];
        default:
            if (shutting_down.load(std::memory_order_relaxed)) {
                // Emulation is stopping; dropping this submission is harmless.
                LOG_WARNING(Render_Vulkan, "vkQueueSubmit failed with {} during shutdown",
                            static_cast<int>(result));
                break;
            }
            // The process terminates right after this; flush the log so the VkResult survives.
            LOG_CRITICAL(Render_Vulkan, "vkQueueSubmit failed permanently with {} (tick {})",
                         static_cast<int>(result), signal_value);
            Common::Log::Stop();
            vk::Check(result);
            break;
        }
    });
    chunk->MarkSubmit();
    AdrenoProfiler::Get().RecordSubmit();
    DispatchWork();
    return signal_value;
}

void Scheduler::AllocateNewContext() {
    // Enable counters once again. These are disabled when a command buffer is finished.
}

void Scheduler::InvalidateState() {
    state.graphics_pipeline = nullptr;
    state.rescaling_defined = false;
    state.descriptor_buffer_bound = false;
    state_tracker.InvalidateCommandBufferState();
}

void Scheduler::EndPendingOperations() {
    query_cache->CounterReset(VideoCommon::QueryType::ZPassPixelCount64);
    EndRenderPass(RenderPassEndReason::Submit);
}

void Scheduler::EndRenderPass(RenderPassEndReason reason) {
        RealizeDeferredClear();
        // Every end path retains the original attachment/XFB barriers below.
        fragment_barrier.End();
        // b46: a deferred guest wait-for-idle is recorded right after this pass's attachment
        // barriers, i.e. before anything that could consume the pass's results.
        const bool materialize_wfi = wfi_defer.End();
        const VkEvent wfi_event = wfi_defer.event;
        const VkPipelineStageFlags wfi_flags = wfi_defer.flags;
        if (!state.renderpass) {
            if (materialize_wfi) {
                // Unreachable while "pending implies an open pass" holds; never drop a guest WFI.
                RecordWaitForIdleBarrier(wfi_event, wfi_flags);
            }
            return;
        }
#if SNAPRYU_DIAGNOSTICS
        pass_break.PassEnded(reason, state.framebuffer,
                             {renderpass_images.data(), num_renderpass_images},
                             pass_written.Span(), pass_sampled.Span(), pass_unchecked);
        pass_written.Clear();
        pass_sampled.Clear();
        pass_unchecked = false;
        pass_depth_image = VK_NULL_HANDLE;
#endif

        AdrenoProfiler::Get().RecordRenderPassEnd(num_renderpass_images, reason);
        AdrenoProfiler::Get().RecordPostRenderPassImageBarriers(num_renderpass_images);

        query_cache->CounterClose(VideoCommon::QueryType::StreamingByteCount);

        // Log render pass end
        if (GPU::Logging::IsActive() &&
            Settings::values.gpu_log_vulkan_calls.GetValue()) {
            GPU::Logging::GPULogger::GetInstance().LogRenderPassEnd();
        }

        query_cache->CounterEnable(VideoCommon::QueryType::ZPassPixelCount64, false);
        query_cache->NotifySegment(false);

        Record([num_images = num_renderpass_images,
                       images = renderpass_images,
                       ranges = renderpass_image_ranges,
                       has_transform_feedback = device.IsExtTransformFeedbackSupported(),
                       full_barrier = GpuFullBarriers()](
                          vk::CommandBuffer cmdbuf) {
            std::array<VkImageMemoryBarrier, 9> barriers;
            for (size_t i = 0; i < num_images; ++i) {
                const VkImageSubresourceRange& range = ranges[i];
                const bool is_color = (range.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) != 0;
                const bool is_depth_stencil = (range.aspectMask
                                              & (VK_IMAGE_ASPECT_DEPTH_BIT
                                                 | VK_IMAGE_ASPECT_STENCIL_BIT)) !=0;

                VkAccessFlags src_access = 0;

                if (is_color)
                    src_access |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                else if (is_depth_stencil)
                    src_access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                else
                    src_access |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                  | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

                barriers[i] = VkImageMemoryBarrier{
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .pNext = nullptr,
                        .srcAccessMask = src_access,
                        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
                                         | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                                         | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = images[i],
                        .subresourceRange = range,
                };
            }
            cmdbuf.EndRenderPass();
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, vk::PIPELINE_STAGE_GRAPHICS_COMPUTE,
                                   0, nullptr, nullptr, vk::Span(barriers.data(), num_images));
            if (has_transform_feedback) {
                static constexpr VkMemoryBarrier XFB_OUTPUT_BARRIER{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT,
                    .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                };
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT,
                                       VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       0, XFB_OUTPUT_BARRIER);
            }
            if (full_barrier) {
                RecordFullBarrier(cmdbuf);
            }
        });
        if (materialize_wfi) {
            RecordWaitForIdleBarrier(wfi_event, wfi_flags);
        }
#if SNAPRYU_DIAGNOSTICS
        // b59 PASS_CAPTURE: copy the attachments of a pass that ran guest draws.
        if (PassCapture::Capturing()) [[unlikely]] {
            PassCapture::PassEnded(*this, state.framebuffer,
                                   {renderpass_images.data(), num_renderpass_images},
                                   {renderpass_image_ranges.data(), num_renderpass_images},
                                   static_cast<u32>(reason));
        }
#endif

        state.renderpass = VkRenderPass{};
        num_renderpass_images = 0;
    }


void Scheduler::AcquireNewChunk() {
    std::scoped_lock rl{reserve_mutex};

    if (chunk_reserve.empty()) {
        // If we don't have anything reserved, we need to make a new chunk.
        chunk = std::make_unique<CommandChunk>();
    } else {
        // Otherwise, we can just take from the reserve.
        chunk = std::move(chunk_reserve.back());
        chunk_reserve.pop_back();
    }
}

} // namespace Vulkan
