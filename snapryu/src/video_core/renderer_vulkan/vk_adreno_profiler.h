// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>

#include "common/common_types.h"

namespace Vulkan {

enum class RenderPassEndReason : u8 {
    Unknown,
    DeferredClear,
    FramebufferChange,
    OutsideOperation,
    Submit,
    FlushDeferredClear,
};

#if SNAPRYU_DIAGNOSTICS
class AdrenoProfiler final {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    static AdrenoProfiler& Get();

    void Initialize(bool is_qualcomm_proprietary);

    [[nodiscard]] bool Enabled() const noexcept {
#if SNAPRYU_DIAGNOSTICS
        return enabled.load(std::memory_order_relaxed);
#else
        return false;
#endif
    }

    static TimePoint Now() noexcept {
#if SNAPRYU_DIAGNOSTICS
        return Clock::now();
#else
        return {};
#endif
    }

    static u64 ElapsedNs(TimePoint start) noexcept {
#if SNAPRYU_DIAGNOSTICS
        return static_cast<u64>(
  std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start)
      .count());
#else
        static_cast<void>(start);
        return 0;
#endif
    }

    void FrameEnd();
    void RecordRenderPassBegin(u32 image_count);
    void RecordRenderPassReuse();
    void RecordRenderPassEnd(u32 image_count, RenderPassEndReason reason);
    void RecordPostRenderPassImageBarriers(u32 image_count);
    void RecordDeferredClear();
    void RecordSubmit();
    void RecordFinishWait(u64 nanoseconds);
    void RecordWorkerWait(u64 nanoseconds);
    void RecordGraphicsPipelineBuild(u64 nanoseconds, bool success);
    void RecordComputePipelineBuild(u64 nanoseconds, bool success);
    void RecordDescriptorReservation(size_t entries, bool descriptor_buffer);
    void RecordDescriptorOverflow();
    void RecordDescriptorBufferBind();
    void RecordStagingRequest(u64 bytes, bool download, bool deferred);
    void RecordBufferCopy(u64 bytes, bool reordered_upload);

private:
    AdrenoProfiler();

    static bool ParseEnabled();
    static u32 ParseReportFrames();

    struct Counters {
        std::atomic<u64> render_pass_begin{0};
        std::atomic<u64> render_pass_reuse{0};
        std::atomic<u64> render_pass_end{0};
        std::atomic<u64> render_pass_images{0};
        std::atomic<u64> render_pass_end_unknown{0};
        std::atomic<u64> render_pass_end_deferred_clear{0};
        std::atomic<u64> render_pass_end_framebuffer_change{0};
        std::atomic<u64> render_pass_end_outside_operation{0};
        std::atomic<u64> render_pass_end_submit{0};
        std::atomic<u64> render_pass_end_flush_deferred_clear{0};
        std::atomic<u64> post_render_pass_image_barriers{0};
        std::atomic<u64> deferred_clears{0};
        std::atomic<u64> submits{0};
        std::atomic<u64> finish_waits{0};
        std::atomic<u64> finish_wait_ns{0};
        std::atomic<u64> worker_waits{0};
        std::atomic<u64> worker_wait_ns{0};
        std::atomic<u64> graphics_pipeline_builds{0};
        std::atomic<u64> graphics_pipeline_failures{0};
        std::atomic<u64> graphics_pipeline_build_ns{0};
        std::atomic<u64> compute_pipeline_builds{0};
        std::atomic<u64> compute_pipeline_failures{0};
        std::atomic<u64> compute_pipeline_build_ns{0};
        std::atomic<u64> descriptor_reservations{0};
        std::atomic<u64> descriptor_entries{0};
        std::atomic<u64> descriptor_buffer_entries{0};
        std::atomic<u64> descriptor_overflows{0};
        std::atomic<u64> descriptor_buffer_binds{0};
        std::atomic<u64> staging_upload_requests{0};
        std::atomic<u64> staging_upload_bytes{0};
        std::atomic<u64> staging_download_requests{0};
        std::atomic<u64> staging_download_bytes{0};
        std::atomic<u64> staging_deferred_download_requests{0};
        std::atomic<u64> staging_deferred_download_bytes{0};
        std::atomic<u64> buffer_copy_calls{0};
        std::atomic<u64> buffer_copy_bytes{0};
        std::atomic<u64> reordered_upload_copy_calls{0};
        std::atomic<u64> reordered_upload_copy_bytes{0};
    } counters;

    const bool requested;
    const u32 report_every_frames;
    std::atomic<bool> enabled{false};
    u64 frames_since_report{};
};

#else
// Compile diagnostic calls away at each call site, including startup environment reads.
class AdrenoProfiler final {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    static AdrenoProfiler& Get() {
        static AdrenoProfiler profiler;
        return profiler;
    }
    void Initialize(bool) {}
    [[nodiscard]] constexpr bool Enabled() const noexcept { return false; }
    static constexpr TimePoint Now() noexcept { return {}; }
    static constexpr u64 ElapsedNs(TimePoint) noexcept { return 0; }
    void FrameEnd() {}
    void RecordRenderPassBegin(u32) {}
    void RecordRenderPassReuse() {}
    void RecordRenderPassEnd(u32, RenderPassEndReason) {}
    void RecordPostRenderPassImageBarriers(u32) {}
    void RecordDeferredClear() {}
    void RecordSubmit() {}
    void RecordFinishWait(u64) {}
    void RecordWorkerWait(u64) {}
    void RecordGraphicsPipelineBuild(u64, bool) {}
    void RecordComputePipelineBuild(u64, bool) {}
    void RecordDescriptorReservation(size_t, bool) {}
    void RecordDescriptorOverflow() {}
    void RecordDescriptorBufferBind() {}
    void RecordStagingRequest(u64, bool, bool) {}
    void RecordBufferCopy(u64, bool) {}
};
#endif

} // namespace Vulkan
