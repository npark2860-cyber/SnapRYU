// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_vulkan/vk_adreno_profiler.h"

#if SNAPRYU_DIAGNOSTICS

#include <algorithm>
#include <cstdlib>
#include <string_view>

#include "common/logging.h"

namespace Vulkan {
namespace {

u64 Take(std::atomic<u64>& value) {
    return value.exchange(0, std::memory_order_relaxed);
}

double PerFrame(u64 value, u64 frames) {
    return frames == 0 ? 0.0 : static_cast<double>(value) / static_cast<double>(frames);
}

double ToMs(u64 nanoseconds) {
    return static_cast<double>(nanoseconds) / 1'000'000.0;
}

double ToMiB(u64 bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

} // Anonymous namespace

AdrenoProfiler& AdrenoProfiler::Get() {
    static AdrenoProfiler profiler;
    return profiler;
}

AdrenoProfiler::AdrenoProfiler()
    : requested{ParseEnabled()}, report_every_frames{ParseReportFrames()} {}

bool AdrenoProfiler::ParseEnabled() {
#if SNAPRYU_DIAGNOSTICS
    const char* const raw = std::getenv("EDEN_ADRENO_PROFILE");
    if (!raw) {
        return false;
    }
    const std::string_view value{raw};
    return value == "1" || value == "true" || value == "TRUE" || value == "on" ||
 value == "ON";
#else
    return false;
#endif
}

u32 AdrenoProfiler::ParseReportFrames() {
    const char* const raw = std::getenv("EDEN_ADRENO_PROFILE_FRAMES");
    if (!raw) {
        return 120;
    }
    char* end{};
    const unsigned long parsed = std::strtoul(raw, &end, 10);
    if (end == raw || *end != '\0') {
        return 120;
    }
    return static_cast<u32>(std::clamp(parsed, 1UL, 3600UL));
}

void AdrenoProfiler::Initialize(bool is_qualcomm_proprietary) {
    const bool active = requested && is_qualcomm_proprietary;
    enabled.store(active, std::memory_order_relaxed);
    if (active) {
        LOG_INFO(Render_Vulkan,
       "[ADRENO-P0] profiler enabled; report interval={} frames",
       report_every_frames);
    } else if (requested) {
        LOG_WARNING(Render_Vulkan,
          "[ADRENO-P0] EDEN_ADRENO_PROFILE requested on a non-Qualcomm "
          "proprietary Vulkan driver; profiler disabled");
    }
}

void AdrenoProfiler::FrameEnd() {
    if (!Enabled()) {
        return;
    }
    ++frames_since_report;
    if (frames_since_report < report_every_frames) {
        return;
    }
    const u64 frames = frames_since_report;
    frames_since_report = 0;

    const u64 rp_begin = Take(counters.render_pass_begin);
    const u64 rp_reuse = Take(counters.render_pass_reuse);
    const u64 rp_end = Take(counters.render_pass_end);
    const u64 rp_images = Take(counters.render_pass_images);
    const u64 rp_end_unknown = Take(counters.render_pass_end_unknown);
    const u64 rp_end_deferred = Take(counters.render_pass_end_deferred_clear);
    const u64 rp_end_framebuffer = Take(counters.render_pass_end_framebuffer_change);
    const u64 rp_end_outside = Take(counters.render_pass_end_outside_operation);
    const u64 rp_end_submit = Take(counters.render_pass_end_submit);
    const u64 rp_end_flush_deferred = Take(counters.render_pass_end_flush_deferred_clear);
    const u64 rp_barriers = Take(counters.post_render_pass_image_barriers);
    const u64 deferred_clears = Take(counters.deferred_clears);
    const u64 submits = Take(counters.submits);
    const u64 finish_waits = Take(counters.finish_waits);
    const u64 finish_wait_ns = Take(counters.finish_wait_ns);
    const u64 worker_waits = Take(counters.worker_waits);
    const u64 worker_wait_ns = Take(counters.worker_wait_ns);
    const u64 gfx_builds = Take(counters.graphics_pipeline_builds);
    const u64 gfx_failures = Take(counters.graphics_pipeline_failures);
    const u64 gfx_build_ns = Take(counters.graphics_pipeline_build_ns);
    const u64 compute_builds = Take(counters.compute_pipeline_builds);
    const u64 compute_failures = Take(counters.compute_pipeline_failures);
    const u64 compute_build_ns = Take(counters.compute_pipeline_build_ns);
    const u64 descriptor_reservations = Take(counters.descriptor_reservations);
    const u64 descriptor_entries = Take(counters.descriptor_entries);
    const u64 descriptor_buffer_entries = Take(counters.descriptor_buffer_entries);
    const u64 descriptor_overflows = Take(counters.descriptor_overflows);
    const u64 descriptor_buffer_binds = Take(counters.descriptor_buffer_binds);
    const u64 staging_upload_requests = Take(counters.staging_upload_requests);
    const u64 staging_upload_bytes = Take(counters.staging_upload_bytes);
    const u64 staging_download_requests = Take(counters.staging_download_requests);
    const u64 staging_download_bytes = Take(counters.staging_download_bytes);
    const u64 deferred_download_requests = Take(counters.staging_deferred_download_requests);
    const u64 deferred_download_bytes = Take(counters.staging_deferred_download_bytes);
    const u64 buffer_copy_calls = Take(counters.buffer_copy_calls);
    const u64 buffer_copy_bytes = Take(counters.buffer_copy_bytes);
    const u64 reordered_upload_calls = Take(counters.reordered_upload_copy_calls);
    const u64 reordered_upload_bytes = Take(counters.reordered_upload_copy_bytes);

    LOG_INFO(
        Render_Vulkan,
        "[ADRENO-P0] frames={} | RP begin={} ({:.2f}/f) reuse={} end={} images={} "
        "postRPbarriers={} ({:.2f}/f) deferredClear={} | submit={} ({:.2f}/f) | "
        "finishWait={} {:.3f}ms workerWait={} {:.3f}ms | gfxPipe={} fail={} {:.3f}ms "
        "computePipe={} fail={} {:.3f}ms | descReserve={} entries={} ({:.1f}/f) "
        "dbufEntries={} dbufBinds={} overflow={}",
        frames, rp_begin, PerFrame(rp_begin, frames), rp_reuse, rp_end, rp_images,
        rp_barriers, PerFrame(rp_barriers, frames), deferred_clears, submits,
        PerFrame(submits, frames), finish_waits, ToMs(finish_wait_ns), worker_waits,
        ToMs(worker_wait_ns), gfx_builds, gfx_failures, ToMs(gfx_build_ns),
        compute_builds, compute_failures, ToMs(compute_build_ns),
        descriptor_reservations, descriptor_entries, PerFrame(descriptor_entries, frames),
        descriptor_buffer_entries, descriptor_buffer_binds, descriptor_overflows);

    LOG_INFO(
        Render_Vulkan,
        "[ADRENO-P0.2] frames={} | RPend unknown={} deferred={} framebuffer={} outside={} "
        "submit={} flushDeferred={} | stagingUpload={} {:.3f}MiB stagingDownload={} "
        "{:.3f}MiB deferredDownload={} {:.3f}MiB | bufferCopy={} {:.3f}MiB "
        "reorderedUpload={} {:.3f}MiB",
        frames, rp_end_unknown, rp_end_deferred, rp_end_framebuffer, rp_end_outside,
        rp_end_submit, rp_end_flush_deferred, staging_upload_requests,
        ToMiB(staging_upload_bytes), staging_download_requests,
        ToMiB(staging_download_bytes), deferred_download_requests,
        ToMiB(deferred_download_bytes), buffer_copy_calls, ToMiB(buffer_copy_bytes),
        reordered_upload_calls, ToMiB(reordered_upload_bytes));
}

void AdrenoProfiler::RecordRenderPassBegin(u32 image_count) {
    if (!Enabled()) return;
    counters.render_pass_begin.fetch_add(1, std::memory_order_relaxed);
    counters.render_pass_images.fetch_add(image_count, std::memory_order_relaxed);
}

void AdrenoProfiler::RecordRenderPassReuse() {
    if (Enabled()) counters.render_pass_reuse.fetch_add(1, std::memory_order_relaxed);
}

void AdrenoProfiler::RecordRenderPassEnd(u32, RenderPassEndReason reason) {
    if (!Enabled()) return;
    counters.render_pass_end.fetch_add(1, std::memory_order_relaxed);
    switch (reason) {
    case RenderPassEndReason::Unknown:
        counters.render_pass_end_unknown.fetch_add(1, std::memory_order_relaxed);
        break;
    case RenderPassEndReason::DeferredClear:
        counters.render_pass_end_deferred_clear.fetch_add(1, std::memory_order_relaxed);
        break;
    case RenderPassEndReason::FramebufferChange:
        counters.render_pass_end_framebuffer_change.fetch_add(1, std::memory_order_relaxed);
        break;
    case RenderPassEndReason::OutsideOperation:
        counters.render_pass_end_outside_operation.fetch_add(1, std::memory_order_relaxed);
        break;
    case RenderPassEndReason::Submit:
        counters.render_pass_end_submit.fetch_add(1, std::memory_order_relaxed);
        break;
    case RenderPassEndReason::FlushDeferredClear:
        counters.render_pass_end_flush_deferred_clear.fetch_add(1, std::memory_order_relaxed);
        break;
    }
}

void AdrenoProfiler::RecordPostRenderPassImageBarriers(u32 image_count) {
    if (Enabled()) {
        counters.post_render_pass_image_barriers.fetch_add(image_count,
                                                 std::memory_order_relaxed);
    }
}

void AdrenoProfiler::RecordDeferredClear() {
    if (Enabled()) counters.deferred_clears.fetch_add(1, std::memory_order_relaxed);
}

void AdrenoProfiler::RecordSubmit() {
    if (Enabled()) counters.submits.fetch_add(1, std::memory_order_relaxed);
}

void AdrenoProfiler::RecordFinishWait(u64 nanoseconds) {
    if (!Enabled()) return;
    counters.finish_waits.fetch_add(1, std::memory_order_relaxed);
    counters.finish_wait_ns.fetch_add(nanoseconds, std::memory_order_relaxed);
}

void AdrenoProfiler::RecordWorkerWait(u64 nanoseconds) {
    if (!Enabled()) return;
    counters.worker_waits.fetch_add(1, std::memory_order_relaxed);
    counters.worker_wait_ns.fetch_add(nanoseconds, std::memory_order_relaxed);
}

void AdrenoProfiler::RecordGraphicsPipelineBuild(u64 nanoseconds, bool success) {
    if (!Enabled()) return;
    counters.graphics_pipeline_builds.fetch_add(1, std::memory_order_relaxed);
    counters.graphics_pipeline_build_ns.fetch_add(nanoseconds, std::memory_order_relaxed);
    if (!success) {
        counters.graphics_pipeline_failures.fetch_add(1, std::memory_order_relaxed);
    }
}

void AdrenoProfiler::RecordComputePipelineBuild(u64 nanoseconds, bool success) {
    if (!Enabled()) return;
    counters.compute_pipeline_builds.fetch_add(1, std::memory_order_relaxed);
    counters.compute_pipeline_build_ns.fetch_add(nanoseconds, std::memory_order_relaxed);
    if (!success) {
        counters.compute_pipeline_failures.fetch_add(1, std::memory_order_relaxed);
    }
}

void AdrenoProfiler::RecordDescriptorReservation(size_t entries, bool descriptor_buffer) {
    if (!Enabled()) return;
    counters.descriptor_reservations.fetch_add(1, std::memory_order_relaxed);
    counters.descriptor_entries.fetch_add(static_cast<u64>(entries),
                                std::memory_order_relaxed);
    if (descriptor_buffer) {
        counters.descriptor_buffer_entries.fetch_add(static_cast<u64>(entries),
                                           std::memory_order_relaxed);
    }
}

void AdrenoProfiler::RecordDescriptorOverflow() {
    if (Enabled()) counters.descriptor_overflows.fetch_add(1, std::memory_order_relaxed);
}

void AdrenoProfiler::RecordDescriptorBufferBind() {
    if (Enabled()) {
        counters.descriptor_buffer_binds.fetch_add(1, std::memory_order_relaxed);
    }
}

void AdrenoProfiler::RecordStagingRequest(u64 bytes, bool download, bool deferred) {
    if (!Enabled()) return;
    if (download) {
        counters.staging_download_requests.fetch_add(1, std::memory_order_relaxed);
        counters.staging_download_bytes.fetch_add(bytes, std::memory_order_relaxed);
        if (deferred) {
  counters.staging_deferred_download_requests.fetch_add(1,
                                                       std::memory_order_relaxed);
  counters.staging_deferred_download_bytes.fetch_add(bytes,
                                                     std::memory_order_relaxed);
        }
    } else {
        counters.staging_upload_requests.fetch_add(1, std::memory_order_relaxed);
        counters.staging_upload_bytes.fetch_add(bytes, std::memory_order_relaxed);
    }
}

void AdrenoProfiler::RecordBufferCopy(u64 bytes, bool reordered_upload) {
    if (!Enabled()) return;
    counters.buffer_copy_calls.fetch_add(1, std::memory_order_relaxed);
    counters.buffer_copy_bytes.fetch_add(bytes, std::memory_order_relaxed);
    if (reordered_upload) {
        counters.reordered_upload_copy_calls.fetch_add(1, std::memory_order_relaxed);
        counters.reordered_upload_copy_bytes.fetch_add(bytes, std::memory_order_relaxed);
    }
}

} // namespace Vulkan

#endif // SNAPRYU_DIAGNOSTICS
