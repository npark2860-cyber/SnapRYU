#include "video_core/query_cache/predicate_read_trace.h"
#include "video_core/query_cache/predicate_trace.h"
// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <deque>
#include <utility>
#include <atomic>
#include <array>
#include <memory>
#include <mutex>
#if SNAPRYU_DIAGNOSTICS
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <string>
#include <vector>
#endif

#include <fmt/format.h>

#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/renderer_vulkan/shader_bisect.h"

#include "common/assert.h"
#include "common/cgt_trace.h"
#include "common/ub_keep.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/gpu_logging/gpu_logging.h"
#include "video_core/control/channel_state.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/renderer_vulkan/blit_image.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/maxwell_to_vk.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_query_cache.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#if SNAPRYU_DIAGNOSTICS
#include "video_core/renderer_vulkan/square_probe.h"
#include "video_core/renderer_vulkan/vk_pass_capture.h"
#include "video_core/renderer_vulkan/vk_submit_work_trace.h"
#include "video_core/texture_cache/gpu_loss_trace.h"
#include "video_core/engines/dma_trace.h"
#include "video_core/engines/draw_target_trace.h"
#include "video_core/texture_cache/tic_trace.h"
#endif
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/shader_cache.h"
#include "video_core/texture_cache/texture_cache_base.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

using Maxwell = Tegra::Engines::Maxwell3D::Regs;
using VideoCommon::ImageViewId;
using VideoCommon::ImageViewType;


namespace {
struct DrawParams {
    u32 base_instance;
    u32 num_instances;
    u32 base_vertex;
    u32 num_vertices;
    u32 first_index;
    bool is_indexed;
};

VkViewport GetViewportState(const Device& device, const Maxwell& regs, size_t index, float scale) {
    const auto& src = regs.viewport_transform[index];
    const auto conv = [scale](float value) {
        float new_value = value * scale;
        if (scale < 1.0f) {
            const bool sign = std::signbit(value);
            new_value = std::round(std::abs(new_value));
            new_value = sign ? -new_value : new_value;
        }
        return new_value;
    };
    const float x = conv(src.translate_x - src.scale_x);
    const float width = conv(src.scale_x * 2.0f);
    float y = conv(src.translate_y - src.scale_y);
    float height = conv(src.scale_y * 2.0f);

    const bool lower_left = regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft;
    const bool y_negate = !device.IsNvViewportSwizzleSupported() &&
                          src.swizzle.y == Maxwell::ViewportSwizzle::NegativeY;

    if (lower_left) {
        // Flip by surface clip height
        y += conv(static_cast<f32>(regs.surface_clip.height));
        height = -height;
    }

    if (y_negate) {
        // Flip by viewport height
        y += height;
        height = -height;
    }

    const float reduce_z = regs.depth_mode == Maxwell::DepthMode::MinusOneToOne ? 1.0f : 0.0f;
    VkViewport viewport{
        .x = x,
        .y = y,
        .width = width != 0.0f ? width : 1.0f,
        .height = height != 0.0f ? height : 1.0f,
        .minDepth = src.translate_z - src.scale_z * reduce_z,
        .maxDepth = src.translate_z + src.scale_z,
    };
    if (!device.IsExtDepthRangeUnrestrictedSupported()) {
        viewport.minDepth = std::clamp(viewport.minDepth, 0.0f, 1.0f);
        viewport.maxDepth = std::clamp(viewport.maxDepth, 0.0f, 1.0f);
    }
    return viewport;
}

VkRect2D GetScissorState(const Maxwell& regs, size_t index, u32 up_scale = 1, u32 down_shift = 0) {
    const auto& src = regs.scissor_test[index];
    VkRect2D scissor{};
    const auto scale_up = [&](s32 value) -> s32 {
        if (value == 0) {
            return 0U;
        }
        const s32 upset = value * up_scale;
        s32 acumm = 0;
        if ((up_scale >> down_shift) == 0) {
            acumm = upset % 2;
        }
        const s32 converted_value = (value * up_scale) >> down_shift;
        return value < 0 ? std::min<s32>(converted_value - acumm, -1)
                         : std::max<s32>(converted_value + acumm, 1);
    };

    const bool lower_left = regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft;
    const s32 clip_height = regs.surface_clip.height;

    // Flip coordinates if lower left
    s32 min_y = lower_left ? (clip_height - src.max_y) : src.min_y.Value();
    s32 max_y = lower_left ? (clip_height - src.min_y) : src.max_y.Value();

    // Bound to render area
    min_y = (std::max)(min_y, 0);
    max_y = (std::max)(max_y, 0);

    if (src.enable) {
        scissor.offset.x = scale_up(src.min_x);
        scissor.offset.y = scale_up(min_y);
        scissor.extent.width = scale_up(src.max_x - src.min_x);
        scissor.extent.height = scale_up(max_y - min_y);
    } else {
        scissor.offset.x = 0;
        scissor.offset.y = 0;
        scissor.extent.width = (std::numeric_limits<s32>::max)();
        scissor.extent.height = (std::numeric_limits<s32>::max)();
    }
    return scissor;
}

DrawParams MakeDrawParams(const Tegra::Engines::Maxwell3D::DrawManager::State& draw_state, u32 num_instances, bool is_indexed) {
    DrawParams params{
        .base_instance = draw_state.base_instance,
        .num_instances = num_instances,
        .base_vertex = is_indexed ? draw_state.base_index : draw_state.vertex_buffer.first,
        .num_vertices = is_indexed ? draw_state.index_buffer.count : draw_state.vertex_buffer.count,
        .first_index = is_indexed ? draw_state.index_buffer.first : 0,
        .is_indexed = is_indexed,
    };
    // 6 triangle vertices per quad, base vertex is part of the index
    // See BindQuadIndexBuffer for more details
    if (draw_state.topology == Maxwell::PrimitiveTopology::Quads) {
        params.num_vertices = (params.num_vertices / 4) * 6;
        params.base_vertex = 0;
        params.is_indexed = true;
    } else if (draw_state.topology == Maxwell::PrimitiveTopology::QuadStrip) {
        params.num_vertices = (params.num_vertices - 2) / 2 * 6;
        params.base_vertex = 0;
        params.is_indexed = true;
    }
    return params;
}

bool SupportsPrimitiveRestart(VkPrimitiveTopology topology) {
    switch (topology) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
    case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
        return false;
    default:
        return true;
    }
}

bool IsPrimitiveRestartSupported(const Device& device, VkPrimitiveTopology topology) {
    return ((topology != VK_PRIMITIVE_TOPOLOGY_PATCH_LIST &&
             device.IsTopologyListPrimitiveRestartSupported()) ||
            SupportsPrimitiveRestart(topology) ||
            (topology == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST &&
             device.IsPatchListPrimitiveRestartSupported()));
}

#if SNAPRYU_DIAGNOSTICS
// EDEN_CGT (common/cgt_trace.h): GPU_SNAPSHOT of the bindings of draws whose shader pair is listed
// in EDEN_CGT_SNAPSHOT. The copies run outside the render pass right before the draw; the bytes
// are read once the copy's tick has completed.
struct CgtSnapshotPending {
    u64 tick;
    u64 snap_id;
    u64 draw_id;
    size_t dst_offset;
    size_t total;
    std::vector<Common::CGT::SnapRegion> regions;
};

struct CgtSnapshotState {
    vk::Buffer buffer;
    size_t size = 0;
    size_t head = 0;
    std::deque<CgtSnapshotPending> pending;
};

// Deleted in ~RasterizerVulkan, before the device goes away; never destroyed at exit.
CgtSnapshotState*& CgtSnapshots() {
    static CgtSnapshotState* state = nullptr;
    return state;
}

VkBuffer CgtToBuffer(u64 bits) {
    static_assert(sizeof(VkBuffer) == sizeof(u64));
    VkBuffer buffer{};
    std::memcpy(&buffer, &bits, sizeof(buffer));
    return buffer;
}

void CgtTakeSnapshot(Scheduler& scheduler, MemoryAllocator& allocator,
                     const Framebuffer* framebuffer, u64 draw_id) {
    auto& cgt = Common::CGT::Get();
    CgtSnapshotState*& state = CgtSnapshots();
    if (state == nullptr) {
        state = new CgtSnapshotState{};
        const VkBufferCreateInfo ci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = cgt.SnapshotBufferBytes(),
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        };
        state->buffer = allocator.CreateBuffer(ci, MemoryUsage::Download);
        state->size = static_cast<size_t>(ci.size);
        LOG_WARNING(Render_Vulkan, "CGT_SNAPSHOT buffer bytes={} mapped={} coherent={}", ci.size,
                    state->buffer.IsHostVisible(), state->buffer.IsHostCoherent());
    }
    if (!state->buffer.IsHostVisible()) {
        return;
    }
    std::vector<Common::CGT::SnapRegion> regions;
    size_t total = 0;
    for (const Common::CGT::DrawBinding& binding : cgt.Bindings()) {
        if (binding.handle == 0 || binding.size == 0) {
            continue;
        }
        const u64 size = (std::min)(binding.size, cgt.SnapshotMaxRegion());
        regions.push_back(Common::CGT::SnapRegion{
            .kind_slot = (u64{static_cast<u8>(binding.kind)} << 16) | (u64{binding.stage} << 8) |
                         binding.index,
            .src = binding.handle,
            .src_offset = binding.offset,
            .size = size,
            .dst_offset = total,
            .daddr = binding.daddr,
        });
        total += static_cast<size_t>((size + 255) & ~u64{255});
    }
    if (regions.empty()) {
        return;
    }
    if (total > state->size) {
        cgt.Emit(Common::CGT::Type::SnapshotSkip, 0, {draw_id, 1, total});
        return;
    }
    size_t start = state->head;
    if (start + total > state->size) {
        start = 0;
    }
    for (const CgtSnapshotPending& pending : state->pending) {
        if (start < pending.dst_offset + pending.total && pending.dst_offset < start + total) {
            // Still waiting for an earlier copy to be read.
            cgt.Emit(Common::CGT::Type::SnapshotSkip, 0, {draw_id, 2, total});
            return;
        }
    }
    state->head = start + total;
    for (Common::CGT::SnapRegion& region : regions) {
        region.dst_offset += start;
    }
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([dst = *state->buffer, copies = regions](vk::CommandBuffer cmdbuf) {
        const VkMemoryBarrier before{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, before);
        for (const Common::CGT::SnapRegion& region : copies) {
            const VkBufferCopy copy{
                .srcOffset = region.src_offset,
                .dstOffset = region.dst_offset,
                .size = region.size,
            };
            cmdbuf.CopyBuffer(CgtToBuffer(region.src), dst, copy);
        }
        // Copied bytes -> host read. ALL_COMMANDS in the destination stages also makes every later
        // command (the draw that follows, which may write a copied SSBO) wait for the copies'
        // reads of the source buffers (write-after-read). This serialises the GPU at each
        // snapshot; snapshots run only for the shader pairs in EDEN_CGT_SNAPSHOT.
        const VkMemoryBarrier after{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0,
                               after);
    });
    const u64 tick = scheduler.CurrentTick();
    scheduler.RequestRenderpass(framebuffer);
    const u64 snap_id = cgt.NextSnapshotId();
    cgt.Emit(Common::CGT::Type::SnapshotReq, 0,
             {snap_id, draw_id, tick, regions.size(), start, total}, regions.data(),
             static_cast<u32>(regions.size() * sizeof(Common::CGT::SnapRegion)));
    state->pending.push_back(
        CgtSnapshotPending{tick, snap_id, draw_id, start, total, std::move(regions)});
}

void CgtSnapshotReadback(Scheduler& scheduler) {
    CgtSnapshotState* const state = CgtSnapshots();
    if (state == nullptr) {
        return;
    }
    auto& cgt = Common::CGT::Get();
    const bool coherent = state->buffer.IsHostCoherent();
    bool invalidated = false;
    while (!state->pending.empty() && scheduler.IsFree(state->pending.front().tick)) {
        const CgtSnapshotPending& pending = state->pending.front();
        if (!coherent && !invalidated) {
            state->buffer.Invalidate();
            invalidated = true;
        }
        const std::span<const u8> mapped = state->buffer.Mapped();
        for (size_t i = 0; i < pending.regions.size(); ++i) {
            const Common::CGT::SnapRegion& region = pending.regions[i];
            cgt.SnapshotData(pending.snap_id, i, pending.draw_id, region, pending.tick, coherent,
                             mapped.subspan(region.dst_offset, region.size));
        }
        state->pending.pop_front();
    }
}
#endif
} // Anonymous namespace

RasterizerVulkan::RasterizerVulkan(Core::Frontend::EmuWindow& emu_window_, Tegra::GPU& gpu_,
                                   Tegra::MaxwellDeviceMemoryManager& device_memory_,
                                   const Device& device_, MemoryAllocator& memory_allocator_,
                                   StateTracker& state_tracker_, Scheduler& scheduler_)
    : gpu{gpu_}, device_memory{device_memory_}, device{device_},
      memory_allocator{memory_allocator_}, state_tracker{state_tracker_}, scheduler{scheduler_},
      staging_pool(device, memory_allocator, scheduler), descriptor_pool(device, scheduler),
      guest_descriptor_queue(device, UpdateDescriptorQueue::GUEST_FRAME_PAYLOAD_SIZE,
                             device.IsExtDescriptorBufferSupported()),
      compute_pass_descriptor_queue(device, UpdateDescriptorQueue::COMPUTE_FRAME_PAYLOAD_SIZE),
      descriptor_buffer_ring(device, memory_allocator),
      blit_image(device, scheduler, state_tracker, descriptor_pool), render_pass_cache(device),
      texture_cache_runtime{
          device,     scheduler,         memory_allocator, staging_pool,
          blit_image, render_pass_cache, descriptor_pool,  compute_pass_descriptor_queue},
      texture_cache(texture_cache_runtime, device_memory),
      buffer_cache_runtime(device, memory_allocator, scheduler, staging_pool,
                           guest_descriptor_queue, compute_pass_descriptor_queue, descriptor_pool),
      buffer_cache(device_memory, buffer_cache_runtime),
      query_cache_runtime(this, device_memory, buffer_cache, device, memory_allocator, scheduler,
                          staging_pool, compute_pass_descriptor_queue, descriptor_pool, texture_cache),
      query_cache(gpu, *this, device_memory, query_cache_runtime),
      pipeline_cache(device_memory, device, scheduler, descriptor_pool, guest_descriptor_queue,
                     descriptor_buffer_ring, render_pass_cache, buffer_cache, texture_cache,
                     gpu.ShaderNotify()),
      accelerate_dma(buffer_cache, texture_cache, scheduler),
      fence_manager(*this, gpu, texture_cache, buffer_cache, query_cache, device, scheduler),
      wfi_event(device.GetLogical().CreateEvent()) {
    scheduler.SetQueryCache(query_cache);
#if SNAPRYU_DIAGNOSTICS
    if (Common::CGT::Enabled()) {
        std::string text = fmt::format(
            "gpu_accuracy={} gpu_fence_behavior={} async_gpu={} cpu_backend={}",
            static_cast<u32>(Settings::values.gpu_accuracy.GetValue()),
            static_cast<u32>(Settings::values.gpu_fence_behavior.GetValue()),
            Settings::values.use_asynchronous_gpu_emulation.GetValue(),
            static_cast<u32>(Settings::values.cpu_backend.GetValue()));
        for (const char* const name :
             {"EDEN_FORCE_WAIT_SITES", "EDEN_COND_WAIT", "EDEN_QUERY_COMPLETION_REUSE",
              "EDEN_PAYLOAD_IN_ORDER", "EDEN_GPU_OWNER_RETAIN", "EDEN_NCE_WRITE_STEP",
              "EDEN_CGT_SNAPSHOT", "EDEN_CGT_FRAMES", "EDEN_CGT_RING_MB", "EDEN_CGT_SNAP_MAX",
              "EDEN_CGT_SNAP_MB"}) {
            const char* const value = std::getenv(name);
            text += fmt::format(" {}={}", name, value != nullptr ? value : "<unset>");
        }
        Common::CGT::Config(text);
    }
#endif
}

RasterizerVulkan::~RasterizerVulkan() {
    scheduler.WaitWorker();
    scheduler.Finish();
#if SNAPRYU_DIAGNOSTICS
    PassCapture::Shutdown();
    SquareProbe::Shutdown();
    delete std::exchange(CgtSnapshots(), nullptr);
#endif
}

template <typename Func>
void RasterizerVulkan::PrepareDraw(bool is_indexed, Func&& draw_func) {

    SCOPE_EXIT {
        gpu.TickWork();
    };
    FlushWork();
    gpu_memory->FlushCaching();

    GraphicsPipeline* const pipeline{pipeline_cache.CurrentGraphicsPipeline()};
    if (!pipeline) {
#if SNAPRYU_DIAGNOSTICS
        VideoCommon::BufTrace::null_graphics_pipeline.fetch_add(1, std::memory_order_relaxed);
        VideoCommon::DrawTargetTrace::Report(VideoCommon::DrawTargetTrace::Outcome::NullPipeline);
#endif
        return;
    }
    if (ShaderBisect::Enabled() && ShaderBisect::ShouldSkipDraw(pipeline->ShaderHashes())) {
#if SNAPRYU_DIAGNOSTICS
        VideoCommon::DrawTargetTrace::Report(VideoCommon::DrawTargetTrace::Outcome::Bisect);
#endif
        return;
    }
    // EDEN_VS_STORAGE_BARRIER=1: on tile-based GPUs (Adreno) the binning pass runs vertex shaders
    // of a render pass before earlier draws' fragment shaders have executed, so a vertex shader
    // reading a storage buffer written by previous fragment shaders sees stale data. Break the
    // render pass and insert a full barrier before such draws.
    static const bool vs_storage_barrier = [] {
        const char* const value = std::getenv("EDEN_VS_STORAGE_BARRIER");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    if (vs_storage_barrier && pipeline->VertexReadsStorage()) {
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.RecordFullBarrierNow();
    }
#if SNAPRYU_DIAGNOSTICS
    if (pipeline->VertexReadsStorage()) {
        static std::atomic<u32> logged_vs_storage{};
        if (logged_vs_storage.fetch_add(1, std::memory_order_relaxed) < 60) {
            const auto& draw_state = maxwell3d->draw_manager.draw_state;
            LOG_INFO(Render_Vulkan,
                     "VS_STORAGE_DRAW vs={:016x} fs={:016x} indexed={} barrier={} topology={} "
                     "base_instance={} base_index={} vertex_first={} vertex_count={} "
                     "index_count={} instance_count={}",
                     pipeline->ShaderHashes()[1], pipeline->ShaderHashes()[5], is_indexed,
                     vs_storage_barrier, static_cast<u32>(draw_state.topology),
                     draw_state.base_instance, draw_state.base_index,
                     draw_state.vertex_buffer.first, draw_state.vertex_buffer.count,
                     draw_state.index_buffer.count, draw_state.instance_count);
        }
    }
    // b61 PASS_CAPTURE: write the held copies to disk before this draw is configured
    // (vk_pass_capture.h Spill).
    if (PassCapture::Capturing() && PassCapture::SpillDue()) [[unlikely]] {
        PassCapture::Spill(scheduler);
    }
    // b62 SQUARE_PROBE: WAIT (F3) and SLEEP (F12) act before the first terrain draw of a run
    // (square_probe.h).
    if (SquareProbe::Enabled()) [[unlikely]] {
        SquareProbe::BeforeLock(scheduler, pipeline->ShaderHashes()[5]);
    }
#endif
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
#if SNAPRYU_DIAGNOSTICS
    // DISPATCH_TRACE: storage and feedback writes of this draw belong to its VS/FS pair.
    {
        const auto& trace_hashes = pipeline->ShaderHashes();
        VideoCommon::BufTrace::BeginDraw(trace_hashes[1] != 0 ? trace_hashes[1] : trace_hashes[0],
                                         trace_hashes[5]);
    }
#endif
    // update engine as channel may be different.
    pipeline->SetEngine(maxwell3d, gpu_memory);
    // EDEN_CGT: the buffer bindings Configure records belong to this draw id.
#if SNAPRYU_DIAGNOSTICS
    const u64 cgt_draw = Common::CGT::Enabled() ? Common::CGT::DrawOpen() : 0;
#endif
    // EDEN_UB_KEEP: fast uniform buffers of selected fragment shaders arm their pages.
    const Common::UbKeep::DrawScope ub_keep_draw{pipeline->ShaderHashes()[5]};
#if SNAPRYU_DIAGNOSTICS
    // DRAW_TARGET (b69): Configure binds the render targets (ConfigureImpl calls
    // UpdateRenderTargets before its only early return).
    VideoCommon::DrawTargetTrace::Report(VideoCommon::DrawTargetTrace::Outcome::Ran);
#endif
    if (!pipeline->Configure(is_indexed))
        return;
#if SNAPRYU_DIAGNOSTICS
    if (cgt_draw != 0) [[unlikely]] {
        const auto& cgt_hashes = pipeline->ShaderHashes();
        const u64 cgt_vs = cgt_hashes[1] != 0 ? cgt_hashes[1] : cgt_hashes[0];
        const u64 cgt_fs = cgt_hashes[5];
        const Framebuffer* const cgt_framebuffer = texture_cache.GetFramebuffer();
        const VkExtent2D cgt_area = cgt_framebuffer->RenderArea();
        const auto& cgt_state = maxwell3d->draw_manager.draw_state;
        auto& cgt = Common::CGT::Get();
        cgt.DrawKey(cgt_draw, cgt_vs, cgt_fs, Common::CGT::HandleBits(cgt_framebuffer->Handle()),
                    (u64{cgt_area.width} << 32) | cgt_area.height, is_indexed ? 1U : 0U,
                    static_cast<u64>(cgt_state.topology),
                    is_indexed ? cgt_state.index_buffer.count : cgt_state.vertex_buffer.count,
                    is_indexed ? cgt_state.index_buffer.first : cgt_state.vertex_buffer.first,
                    cgt_state.base_index, cgt_state.base_instance, cgt_state.instance_count,
                    scheduler.CurrentTick());
        if (cgt.SnapshotSelected(cgt_vs, cgt_fs)) {
            CgtTakeSnapshot(scheduler, memory_allocator, cgt_framebuffer, cgt_draw);
        }
        if (cgt.WatchSelected(cgt_fs)) {
            // EDEN_CGT_WATCH_FS: make CPU writes to this draw's fast uniform buffer pages trap.
            for (const Common::CGT::DrawBinding& binding : cgt.Bindings()) {
                if (binding.kind != Common::CGT::BindKind::UbFast || binding.daddr == 0 ||
                    binding.size == 0) {
                    continue;
                }
                const u64 first_page = binding.daddr & ~u64{Core::DEVICE_PAGEMASK};
                for (u64 page = first_page; page < binding.daddr + binding.size;
                     page += Core::DEVICE_PAGESIZE) {
                    if (cgt.WatchPage(page, cgt_draw, cgt_fs)) {
                        device_memory.UpdatePagesCachedCount(page, Core::DEVICE_PAGESIZE, 1);
                    }
                }
            }
        }
        Common::CGT::DrawFinish();
    }
#endif
#if SNAPRYU_DIAGNOSTICS
    // b59 PASS_CAPTURE: a focus draw gets a render pass of its own (vk_pass_capture.h).
    const bool pass_capture_focus =
        PassCapture::Capturing() && PassCapture::IsFocus(pipeline->ShaderHashes()[5]) &&
        PassCapture::FocusBefore(scheduler, texture_cache, texture_cache.GetFramebuffer(),
                                 pipeline->ShaderHashes()[5], *maxwell3d, *gpu_memory);
    // b62 SQUARE_PROBE: BARRIER (F1) gives a terrain draw a render pass of its own between full
    // barriers (square_probe.h).
    const bool square_probe_split =
        SquareProbe::Enabled() &&
        SquareProbe::BarrierBefore(scheduler, texture_cache.GetFramebuffer(),
                                   pipeline->ShaderHashes()[5]);
#endif

    const auto& trace_hashes = pipeline->ShaderHashes();
    VideoCommon::PredicateTrace::Pipeline(maxwell3d,
        trace_hashes[1] != 0 ? trace_hashes[1] : trace_hashes[0], trace_hashes[5]);
    UpdateDynamicStates();

    if (MaxwellToVK::RelaxDepthEqualForStorageWriters() && pipeline->FragmentWritesStorage() &&
        maxwell3d->regs.depth_test_enable != 0 &&
        MaxwellToVK::ComparisonOp(maxwell3d->regs.depth_test_func) == VK_COMPARE_OP_EQUAL &&
        device.IsExtExtendedDynamicStateSupported()) {
#if SNAPRYU_DIAGNOSTICS
        static std::atomic<u32> logged_relax{};
        if (logged_relax.fetch_add(1, std::memory_order_relaxed) < 4) {
            LOG_INFO(Render_Vulkan, "DEPTH_EQUAL relaxed to GEQUAL for vs={:016x} fs={:016x}",
                     pipeline->ShaderHashes()[1], pipeline->ShaderHashes()[5]);
        }
#endif
        scheduler.Record([](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetDepthCompareOpEXT(VK_COMPARE_OP_GREATER_OR_EQUAL);
        });
        // Make the next draw re-emit its own compare op.
        state_tracker.InvalidateDepthCompareOp();
    }

    query_cache.NotifySegment(true);
    HandleTransformFeedback();
    query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64, maxwell3d->regs.zpass_pixel_count_enable);
    draw_func(pipeline);
#if SNAPRYU_DIAGNOSTICS
    if (PassCapture::Capturing()) [[unlikely]] {
        PassCapture::NoteDraw(texture_cache.GetFramebuffer(), pipeline->ShaderHashes(),
                              pass_capture_focus);
        if (pass_capture_focus) {
            PassCapture::FocusAfter(scheduler);
        }
    }
    if (square_probe_split) [[unlikely]] {
        SquareProbe::BarrierAfter(scheduler);
    }
#endif
}

void RasterizerVulkan::Draw(bool is_indexed, u32 instance_count) {
    PrepareDraw(is_indexed, [this, is_indexed, instance_count](GraphicsPipeline* pipeline) {
        const auto& draw_state = maxwell3d->draw_manager.draw_state;
        const u32 num_instances{instance_count};
        const DrawParams draw_params{MakeDrawParams(draw_state, num_instances, is_indexed)};

        scheduler.Record([pipeline, draw_params](vk::CommandBuffer cmdbuf) {
            if (!pipeline->IsBound()) {
                return;
            }
#if SNAPRYU_DIAGNOSTICS
            SubmitWorkTrace::Graphics(pipeline->ShaderHashes(), draw_params.num_vertices,
                                      draw_params.num_instances);
#endif
            if (draw_params.is_indexed) {
                cmdbuf.DrawIndexed(draw_params.num_vertices, draw_params.num_instances,
                                   draw_params.first_index, draw_params.base_vertex,
                                   draw_params.base_instance);
            } else {
                cmdbuf.Draw(draw_params.num_vertices, draw_params.num_instances,
                            draw_params.base_vertex, draw_params.base_instance);
            }
        });

        // Log draw call
        if (GPU::Logging::IsActive() &&
            Settings::values.gpu_log_vulkan_calls.GetValue()) {
            const std::string params = is_indexed ?
                fmt::format("vertices={}, instances={}, firstIndex={}, baseVertex={}, baseInstance={}",
                    draw_params.num_vertices, draw_params.num_instances,
                    draw_params.first_index, draw_params.base_vertex, draw_params.base_instance) :
                fmt::format("vertices={}, instances={}, firstVertex={}, firstInstance={}",
                    draw_params.num_vertices, draw_params.num_instances,
                    draw_params.base_vertex, draw_params.base_instance);
            GPU::Logging::GPULogger::GetInstance().LogVulkanCall(
                is_indexed ? "vkCmdDrawIndexed" : "vkCmdDraw", params, VK_SUCCESS);
        }
    });
}

void RasterizerVulkan::DrawIndirect() {
    const auto& params = maxwell3d->draw_manager.indirect_state;
    buffer_cache.SetDrawIndirect(&params);
    PrepareDraw(params.is_indexed, [this, &params](GraphicsPipeline* pipeline) {
        const auto indirect_buffer = buffer_cache.GetDrawIndirectBuffer();
        const auto& buffer = indirect_buffer.first;
        const auto& offset = indirect_buffer.second;
        if (params.is_byte_count) {
            scheduler.Record([pipeline, buffer_obj = buffer->Handle(), offset,
                              stride = params.stride](vk::CommandBuffer cmdbuf) {
                if (!pipeline->IsBound()) {
                    return;
                }
#if SNAPRYU_DIAGNOSTICS
                SubmitWorkTrace::Graphics(pipeline->ShaderHashes(), 0, 0, true, 1);
#endif
                cmdbuf.DrawIndirectByteCountEXT(1, 0, buffer_obj, offset, 0,
                                                static_cast<u32>(stride));
            });
            return;
        }
        if (params.include_count) {
            const auto count = buffer_cache.GetDrawIndirectCount();
            const auto& draw_buffer = count.first;
            const auto& offset_base = count.second;
            scheduler.Record([pipeline, draw_buffer_obj = draw_buffer->Handle(),
                              buffer_obj = buffer->Handle(), offset_base, offset,
                              params](vk::CommandBuffer cmdbuf) {
                if (!pipeline->IsBound()) {
                    return;
                }
#if SNAPRYU_DIAGNOSTICS
                SubmitWorkTrace::Graphics(pipeline->ShaderHashes(), 0, 0, true,
                                          static_cast<u32>(params.max_draw_counts));
#endif
                if (params.is_indexed) {
                    cmdbuf.DrawIndexedIndirectCount(
                        buffer_obj, offset, draw_buffer_obj, offset_base,
                        static_cast<u32>(params.max_draw_counts), static_cast<u32>(params.stride));
                } else {
                    cmdbuf.DrawIndirectCount(buffer_obj, offset, draw_buffer_obj, offset_base,
                                             static_cast<u32>(params.max_draw_counts),
                                             static_cast<u32>(params.stride));
                }
            });
            return;
        }
        scheduler.Record([pipeline, buffer_obj = buffer->Handle(), offset, params](vk::CommandBuffer cmdbuf) {
            if (!pipeline->IsBound()) {
                return;
            }
#if SNAPRYU_DIAGNOSTICS
            SubmitWorkTrace::Graphics(pipeline->ShaderHashes(), 0, 0, true,
                                      static_cast<u32>(params.max_draw_counts));
#endif
            if (params.is_indexed) {
                cmdbuf.DrawIndexedIndirect(buffer_obj, offset,
                                           static_cast<u32>(params.max_draw_counts),
                                           static_cast<u32>(params.stride));
            } else {
                cmdbuf.DrawIndirect(buffer_obj, offset, static_cast<u32>(params.max_draw_counts),
                                    static_cast<u32>(params.stride));
            }
        });

        // Log indirect draw call
        if (GPU::Logging::IsActive() &&
            Settings::values.gpu_log_vulkan_calls.GetValue()) {
            const std::string log_params = fmt::format("drawCount={}, stride={}",
                params.max_draw_counts, params.stride);
            GPU::Logging::GPULogger::GetInstance().LogVulkanCall(
                params.is_indexed ? "vkCmdDrawIndexedIndirect" : "vkCmdDrawIndirect",
                log_params, VK_SUCCESS);
        }
    });
    buffer_cache.SetDrawIndirect(nullptr);
}

void RasterizerVulkan::DrawTexture() {

    SCOPE_EXIT {
        gpu.TickWork();
    };
    FlushWork();

    std::scoped_lock l{texture_cache.mutex};
    texture_cache.SynchronizeDescriptors(false);
    texture_cache.UpdateRenderTargets(false);

    UpdateDynamicStates();

    query_cache.NotifySegment(true);
    query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64, maxwell3d->regs.zpass_pixel_count_enable);
    const auto& draw_texture_state = maxwell3d->draw_manager.draw_texture_state;
    const auto& sampler = texture_cache.GetSampler(draw_texture_state.src_sampler, false);
    const auto& texture = texture_cache.GetImageView(draw_texture_state.src_texture);
    const auto* framebuffer = texture_cache.GetFramebuffer();

    const bool src_rescaling = texture_cache.IsRescaling() && texture.IsRescaled();
    const bool dst_rescaling = texture_cache.IsRescaling() && framebuffer->IsRescaled();

    const auto ScaleSrc = [&](auto dim_f) -> s32 {
        auto dim = static_cast<s32>(dim_f);
        return src_rescaling ? Settings::values.resolution_info.ScaleUp(dim) : dim;
    };

    const auto ScaleDst = [&](auto dim_f) -> s32 {
        auto dim = static_cast<s32>(dim_f);
        return dst_rescaling ? Settings::values.resolution_info.ScaleUp(dim) : dim;
    };

    Region2D dst_region = {Offset2D{.x = ScaleDst(draw_texture_state.dst_x0),
                                    .y = ScaleDst(draw_texture_state.dst_y0)},
                           Offset2D{.x = ScaleDst(draw_texture_state.dst_x1),
                                    .y = ScaleDst(draw_texture_state.dst_y1)}};
    Region2D src_region = {Offset2D{.x = ScaleSrc(draw_texture_state.src_x0),
                                    .y = ScaleSrc(draw_texture_state.src_y0)},
                           Offset2D{.x = ScaleSrc(draw_texture_state.src_x1),
                                    .y = ScaleSrc(draw_texture_state.src_y1)}};
    Extent3D src_size = {static_cast<u32>(ScaleSrc(texture.size.width)),
                         static_cast<u32>(ScaleSrc(texture.size.height)), texture.size.depth};
    blit_image.BlitColor(framebuffer, texture.RenderTarget(), texture.ImageHandle(),
                         sampler->Handle(), dst_region, src_region, src_size);
}

namespace {
bool DrawPartialDepthClears() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_DRAW_DEPTH_CLEARS");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}
} // Anonymous namespace

void RasterizerVulkan::Clear(u32 layer_count) {
    FlushWork();
    gpu_memory->FlushCaching();

    auto& regs = maxwell3d->regs;
    const bool use_color = regs.clear_surface.R || regs.clear_surface.G || regs.clear_surface.B ||
                           regs.clear_surface.A;
    const bool use_depth = regs.clear_surface.Z;
    const bool use_stencil = regs.clear_surface.S;
    if (!use_color && !use_depth && !use_stencil) {
#if SNAPRYU_DIAGNOSTICS
        VideoCommon::DrawTargetTrace::Report(
            VideoCommon::DrawTargetTrace::Outcome::ClearNoChannels);
#endif
        return;
    }

    std::scoped_lock lock{texture_cache.mutex};
    texture_cache.UpdateRenderTargets(true);
#if SNAPRYU_DIAGNOSTICS
    VideoCommon::DrawTargetTrace::Report(VideoCommon::DrawTargetTrace::Outcome::Ran);
#endif
    const Framebuffer* const framebuffer = texture_cache.GetFramebuffer();
    const VkExtent2D render_area = framebuffer->RenderArea();

    // EDEN_GPU_NO_DEFERRED_CLEAR=1 records clears as vkCmdClearAttachments instead of folding
    // them into the next render pass load op (artifact bisection on tiled GPUs such as Adreno).
    static const bool ENABLE_DEFERRED_CLEAR = [] {
        const char* const value = std::getenv("EDEN_GPU_NO_DEFERRED_CLEAR");
        return !(value != nullptr && value[0] != '\0' && value[0] != '0');
    }();
    const bool color_full_channels = regs.clear_surface.R && regs.clear_surface.G &&
                                     regs.clear_surface.B && regs.clear_surface.A;
    const bool stencil_partial = use_stencil && framebuffer->HasAspectStencilBit() &&
                                 regs.stencil_front_mask != 0xFF && regs.stencil_front_mask != 0;
    const bool ds_used = use_depth || use_stencil;
    const bool ds_deferrable =
        !ds_used || ((!framebuffer->HasAspectDepthBit() || use_depth) &&
                     (!framebuffer->HasAspectStencilBit() || use_stencil) && !stencil_partial);
    u32 up_scale = 1;
    u32 down_shift = 0;
    if (texture_cache.IsRescaling()) {
        up_scale = Settings::values.resolution_info.up_scale;
        down_shift = Settings::values.resolution_info.down_shift;
    }
    VkRect2D default_scissor{};
    default_scissor.offset.x = 0;
    default_scissor.offset.y = 0;
    default_scissor.extent.width = (std::numeric_limits<s32>::max)();
    default_scissor.extent.height = (std::numeric_limits<s32>::max)();

    VkClearRect clear_rect{
        .rect = regs.clear_control.use_scissor ? GetScissorState(regs, 0, up_scale, down_shift)
                                               : default_scissor,
        .baseArrayLayer = regs.clear_surface.layer,
        .layerCount = layer_count,
    };
    const auto clamp_rect_to_render_area = [render_area](VkRect2D& rect) -> bool {
        const auto clamp_axis = [](s32& offset, u32& extent, u32 limit) {
            auto clamp_offset = [&offset, limit]() {
                if (limit == 0) {
                    offset = 0;
                    return;
                }
                offset = std::clamp(offset, 0, static_cast<s32>(limit));
            };

            if (extent == 0) {
                clamp_offset();
                return;
            }
            if (offset < 0) {
                const u32 shrink = (std::min)(extent, static_cast<u32>(-offset));
                extent -= shrink;
                offset = 0;
            }
            if (limit == 0) {
                extent = 0;
                offset = 0;
                return;
            }
            if (offset >= s32(limit)) {
                offset = s32(limit);
                extent = 0;
                return;
            }
            const u64 end_coord = u64(offset) + extent;
            if (end_coord > limit) {
                extent = limit - u32(offset);
            }
        };

        clamp_axis(rect.offset.x, rect.extent.width, render_area.width);
        clamp_axis(rect.offset.y, rect.extent.height, render_area.height);
        return rect.extent.width != 0 && rect.extent.height != 0;
    };
#if SNAPRYU_DIAGNOSTICS
    if (use_depth || use_stencil) {
        static std::atomic<u32> logged_ds_clears{};
        const bool partial = regs.clear_control.use_scissor;
        const bool layered = clear_rect.baseArrayLayer != 0 || clear_rect.layerCount != 1 ||
                             framebuffer->NumLayers() != 1;
        const bool out_of_range =
            clear_rect.baseArrayLayer + clear_rect.layerCount > framebuffer->NumLayers();
        const bool covers_area = clear_rect.rect.offset.x <= 0 && clear_rect.rect.offset.y <= 0 &&
                                 clear_rect.rect.offset.x + s32(clear_rect.rect.extent.width) >=
                                     s32(render_area.width) &&
                                 clear_rect.rect.offset.y + s32(clear_rect.rect.extent.height) >=
                                     s32(render_area.height);
        if (((partial && !covers_area) || layered || out_of_range) &&
            logged_ds_clears.fetch_add(1, std::memory_order_relaxed) < 200) {
            LOG_INFO(Render_Vulkan,
                     "DS_CLEAR rect=({},{} {}x{}) layer={}+{} fb_layers={} render_area={}x{} "
                     "scissor={} out_of_range={}",
                     clear_rect.rect.offset.x, clear_rect.rect.offset.y,
                     clear_rect.rect.extent.width, clear_rect.rect.extent.height,
                     clear_rect.baseArrayLayer, clear_rect.layerCount, framebuffer->NumLayers(),
                     render_area.width, render_area.height, partial, out_of_range);
        }
    }
#endif
    if (!clamp_rect_to_render_area(clear_rect.rect)) {
        return;
    }

    const bool clear_covers_render_area =
        clear_rect.rect.offset.x == 0 && clear_rect.rect.offset.y == 0 &&
        clear_rect.rect.extent.width >= render_area.width &&
        clear_rect.rect.extent.height >= render_area.height;
    const bool can_defer_clear = ENABLE_DEFERRED_CLEAR && (!regs.clear_control.use_scissor || clear_covers_render_area) &&
                                 regs.clear_surface.layer == 0 &&
                                 !scheduler.IsRenderPassActive() &&
                                 (!use_color || color_full_channels) && ds_deferrable;
    if (!can_defer_clear) {
        scheduler.RequestRenderpass(framebuffer);
    }

    query_cache.NotifySegment(true);
    query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64, maxwell3d->regs.zpass_pixel_count_enable);
    UpdateViewportsState(regs);

    const u32 color_attachment = regs.clear_surface.RT;
    if (use_color && framebuffer->HasAspectColorBit(color_attachment)) {
        const auto format = VideoCore::Surface::PixelFormatFromRenderTargetFormat(regs.rt[color_attachment].format);
        bool is_integer = IsPixelFormatInteger(format);
        bool is_signed = IsPixelFormatSignedInteger(format);
        size_t int_size = PixelComponentSizeBitsInteger(format);
        VkClearValue clear_value{};
        if (!is_integer) {
            std::memcpy(clear_value.color.float32, regs.clear_color.data(), regs.clear_color.size() * sizeof(f32));
        } else if (!is_signed) {
            for (size_t i = 0; i < 4; i++)
                clear_value.color.uint32[i] = u32(f32(u64(int_size) << 1U) * regs.clear_color[i]);
        } else {
            for (size_t i = 0; i < 4; i++)
                clear_value.color.int32[i] = s32(f32(s64(int_size - 1) << 1) * (regs.clear_color[i] - 0.5f));
        }

        if (color_full_channels) {
            if (can_defer_clear) {
                scheduler.DeferColorClear(framebuffer, color_attachment, clear_value);
            } else {
                scheduler.Record([color_attachment, clear_value, clear_rect](vk::CommandBuffer cmdbuf) {
                    const VkClearAttachment attachment{
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .colorAttachment = color_attachment,
                        .clearValue = clear_value,
                    };
                    cmdbuf.ClearAttachments(attachment, clear_rect);
                });
            }
        } else {
            u8 color_mask = u8(regs.clear_surface.R | regs.clear_surface.G << 1 | regs.clear_surface.B << 2 | regs.clear_surface.A << 3);
            Region2D dst_region = {
                Offset2D{.x = clear_rect.rect.offset.x, .y = clear_rect.rect.offset.y},
                Offset2D{.x = clear_rect.rect.offset.x + s32(clear_rect.rect.extent.width),
                         .y = clear_rect.rect.offset.y + s32(clear_rect.rect.extent.height)}};
            blit_image.ClearColor(framebuffer, color_mask, regs.clear_color, dst_region);
        }
    }

    if (!use_depth && !use_stencil) {
        return;
    }
    VkImageAspectFlags aspect_flags = 0;
    if (use_depth && framebuffer->HasAspectDepthBit()) {
        aspect_flags |= VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    if (use_stencil && framebuffer->HasAspectStencilBit()) {
        aspect_flags |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    if (aspect_flags == 0) {
        return;
    }

    if (use_stencil && framebuffer->HasAspectStencilBit() && regs.stencil_front_mask != 0xFF &&
        regs.stencil_front_mask != 0) {
        Region2D dst_region = {
            Offset2D{.x = clear_rect.rect.offset.x, .y = clear_rect.rect.offset.y},
            Offset2D{.x = clear_rect.rect.offset.x + s32(clear_rect.rect.extent.width),
                     .y = clear_rect.rect.offset.y + s32(clear_rect.rect.extent.height)}};
        blit_image.ClearDepthStencil(framebuffer, use_depth, regs.clear_depth,
                                     u8(regs.stencil_front_mask), regs.clear_stencil,
                                     regs.stencil_front_func_mask, dst_region);
    } else if (DrawPartialDepthClears() && use_depth && !use_stencil &&
               framebuffer->HasAspectDepthBit() && !clear_covers_render_area) {
        // EDEN_DRAW_DEPTH_CLEARS=1 (diagnostic): clear partial depth regions with a quad draw
        // instead of vkCmdClearAttachments.
        Region2D dst_region = {
            Offset2D{.x = clear_rect.rect.offset.x, .y = clear_rect.rect.offset.y},
            Offset2D{.x = clear_rect.rect.offset.x + s32(clear_rect.rect.extent.width),
                     .y = clear_rect.rect.offset.y + s32(clear_rect.rect.extent.height)}};
        blit_image.ClearDepthStencil(framebuffer, true, regs.clear_depth, 0, 0, 0, dst_region);
    } else if (can_defer_clear) {
        VkClearValue ds_value{};
        ds_value.depthStencil.depth = regs.clear_depth;
        ds_value.depthStencil.stencil = regs.clear_stencil;
        scheduler.DeferDepthStencilClear(framebuffer, ds_value);
    } else {
        scheduler.Record([clear_depth = regs.clear_depth, clear_stencil = regs.clear_stencil,
                          clear_rect, aspect_flags](vk::CommandBuffer cmdbuf) {
            VkClearAttachment attachment;
            attachment.aspectMask = aspect_flags;
            attachment.colorAttachment = 0;
            attachment.clearValue.depthStencil.depth = clear_depth;
            attachment.clearValue.depthStencil.stencil = clear_stencil;
            cmdbuf.ClearAttachments(attachment, clear_rect);
        });
    }
}

void RasterizerVulkan::DispatchCompute() {
    VideoCommon::PredicateTrace::Dispatch();
    FlushWork();
    gpu_memory->FlushCaching();

    ComputePipeline* const pipeline{pipeline_cache.CurrentComputePipeline()};
    if (!pipeline) {
#if SNAPRYU_DIAGNOSTICS
        VideoCommon::BufTrace::null_compute_pipeline.fetch_add(1, std::memory_order_relaxed);
#endif
        return;
    }
    if (ShaderBisect::Enabled() && ShaderBisect::ShouldSkipDispatch(pipeline->ShaderHash())) {
        return;
    }
    std::scoped_lock lock{texture_cache.mutex, buffer_cache.mutex};
#if SNAPRYU_DIAGNOSTICS
    // DISPATCH_TRACE: storage writes Configure marks belong to this dispatch.
    {
        const auto& trace_qmd{kepler_compute->launch_description};
        const std::array<u32, 3> trace_grid{trace_qmd.grid_dim_x, trace_qmd.grid_dim_y,
                                            trace_qmd.grid_dim_z};
        const std::array<u32, 3> trace_block{trace_qmd.block_dim_x, trace_qmd.block_dim_y,
                                             trace_qmd.block_dim_z};
        VideoCommon::BufTrace::BeginDispatch(pipeline->ShaderHash(), trace_grid, trace_block);
    }
#endif
    if (!pipeline->Configure(*kepler_compute, *gpu_memory, scheduler, buffer_cache,
                             texture_cache)) {
#if SNAPRYU_DIAGNOSTICS
        VideoCommon::BufTrace::EndDispatch(VideoCommon::BufTrace::DispatchStatus::ConfigureFailed,
                                           false);
#endif
        return;
    }

    const auto& qmd{kepler_compute->launch_description};
    auto indirect_address = kepler_compute->GetIndirectComputeAddress();
    if (indirect_address) {
#if SNAPRYU_DIAGNOSTICS
        VideoCommon::BufTrace::EndDispatch(VideoCommon::BufTrace::DispatchStatus::Recorded, true);
#endif
        // DispatchIndirect
        // EDEN_INDIRECT_DISPATCH_MODE (diagnostic): 0 = upstream (full sync + discard write),
        // 1 = use the GPU copy as-is (no sync, keep GPU-modified tracking),
        // 2 = full sync but keep GPU-modified tracking (no discard).
        static const int indirect_mode = [] {
            const char* const value = std::getenv("EDEN_INDIRECT_DISPATCH_MODE");
            return value != nullptr ? std::atoi(value) : 0;
        }();
#if SNAPRYU_DIAGNOSTICS
        static std::atomic<u32> logged_indirect{};
        if (logged_indirect.fetch_add(1, std::memory_order_relaxed) < 8) {
            LOG_INFO(Render_Vulkan, "INDIRECT_DISPATCH shader={:016x} gpu_addr={:#x} mode={}",
                     pipeline->ShaderHash(), *indirect_address, indirect_mode);
        }
#endif
        const auto sync_info = indirect_mode == 1
                                   ? VideoCommon::ObtainBufferSynchronize::NoSynchronize
                                   : VideoCommon::ObtainBufferSynchronize::FullSynchronize;
        const auto post_op = indirect_mode == 0 ? VideoCommon::ObtainBufferOperation::DiscardWrite
                                                : VideoCommon::ObtainBufferOperation::DoNothing;
        const auto [buffer, offset] =
            buffer_cache.ObtainBuffer(*indirect_address, 12, sync_info, post_op);
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([pipeline, indirect_buffer = buffer->Handle(),
                          indirect_offset = offset](vk::CommandBuffer cmdbuf) {
            if (!pipeline->IsBound()) {
#if SNAPRYU_DIAGNOSTICS
                VideoCommon::BufTrace::unbound_dispatch.fetch_add(1, std::memory_order_relaxed);
#endif
                return;
            }
#if SNAPRYU_DIAGNOSTICS
            SubmitWorkTrace::Compute(pipeline->ShaderHash(), 0, 0, 0, true);
#endif
            cmdbuf.DispatchIndirect(indirect_buffer, indirect_offset);
        });
        scheduler.RecordDebugFullBarrier();
        return;
    }
    const std::array<u32, 3> dim{qmd.grid_dim_x, qmd.grid_dim_y, qmd.grid_dim_z};
    const std::array<u32, 3> max_dim{device.GetMaxComputeWorkGroupCount()};
    if (dim[0] > max_dim[0] || dim[1] > max_dim[1] || dim[2] > max_dim[2]) {
#if SNAPRYU_DIAGNOSTICS
        VideoCommon::BufTrace::EndDispatch(VideoCommon::BufTrace::DispatchStatus::OverMaxDim,
                                           false);
#endif
        return;
    }
#if SNAPRYU_DIAGNOSTICS
    VideoCommon::BufTrace::EndDispatch(VideoCommon::BufTrace::DispatchStatus::Recorded, false);
#endif
    scheduler.RequestOutsideRenderPassOperationContext();
    static constexpr VkMemoryBarrier READ_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
    };
    scheduler.Record([](vk::CommandBuffer cmdbuf) { cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               0, READ_BARRIER); });
    scheduler.Record([pipeline, dim](vk::CommandBuffer cmdbuf) {
        if (!pipeline->IsBound()) {
#if SNAPRYU_DIAGNOSTICS
            VideoCommon::BufTrace::unbound_dispatch.fetch_add(1, std::memory_order_relaxed);
#endif
            return;
        }
#if SNAPRYU_DIAGNOSTICS
        SubmitWorkTrace::Compute(pipeline->ShaderHash(), dim[0], dim[1], dim[2]);
#endif
        cmdbuf.Dispatch(dim[0], dim[1], dim[2]);
    });
    scheduler.RecordDebugFullBarrier();

    // Log compute dispatch
    if (GPU::Logging::IsActive() &&
        Settings::values.gpu_log_vulkan_calls.GetValue()) {
        const std::string params = fmt::format("groupCountX={}, groupCountY={}, groupCountZ={}",
            dim[0], dim[1], dim[2]);
        GPU::Logging::GPULogger::GetInstance().LogVulkanCall(
            "vkCmdDispatch", params, VK_SUCCESS);
    }
}

void RasterizerVulkan::ResetCounter(VideoCommon::QueryType type) {
    if (type == VideoCommon::QueryType::ZPassPixelCount64)
        VideoCommon::PredicateTrace::ResetCounter(maxwell3d);
    switch (type) {
    case VideoCommon::QueryType::ZPassPixelCount64:
    case VideoCommon::QueryType::StreamingByteCount:
    case VideoCommon::QueryType::StreamingPrimitivesSucceeded:
    case VideoCommon::QueryType::VtgPrimitivesOut:
        query_cache.CounterReset(type);
        return;
    default:
        LOG_DEBUG(Render_Vulkan, "Unimplemented counter reset={}", type);
        return;
    }
}

void RasterizerVulkan::Query(GPUVAddr gpu_addr, VideoCommon::QueryType type,
                             VideoCommon::QueryPropertiesFlags flags, u32 payload, u32 subreport) {
    if (type == VideoCommon::QueryType::Payload) {
        // EDEN_UB_KEEP: a semaphore released by a GPU command (acquires of it keep copies).
        Common::UbKeep::OnGpuRelease(gpu_addr, payload);
    }
    query_cache.CounterReport(gpu_addr, type, flags, payload, subreport);
}

void RasterizerVulkan::BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr,
                                                 u32 size) {
    buffer_cache.BindGraphicsUniformBuffer(stage, index, gpu_addr, size);
}

void Vulkan::RasterizerVulkan::DisableGraphicsUniformBuffer(size_t stage, u32 index) {
    buffer_cache.DisableGraphicsUniformBuffer(stage, index);
}

void RasterizerVulkan::FlushAll() {}

void RasterizerVulkan::FlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    if (addr == 0 || size == 0) {
        return;
    }
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.DownloadMemory(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::BufferCache))) {
        std::scoped_lock lock{buffer_cache.mutex};
        VideoCommon::PredicateReadTrace::Observe(VideoCommon::PredicateReadTrace::Stage::BeforeBuffer);
        buffer_cache.DownloadMemory(addr, size);
        VideoCommon::PredicateReadTrace::Observe(VideoCommon::PredicateReadTrace::Stage::AfterBuffer);
    }
    if ((True(which & VideoCommon::CacheType::QueryCache))) {
        VideoCommon::PredicateReadTrace::Observe(VideoCommon::PredicateReadTrace::Stage::BeforeQuery);
        query_cache.FlushRegion(addr, size);
        VideoCommon::PredicateReadTrace::Observe(VideoCommon::PredicateReadTrace::Stage::AfterQuery);
    }
}

bool RasterizerVulkan::MustFlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    if ((True(which & VideoCommon::CacheType::BufferCache))) {
        std::scoped_lock lock{buffer_cache.mutex};
        if (buffer_cache.IsRegionGpuModified(addr, size)) {
            return true;
        }
    }
    if (!Settings::IsGPULevelHigh()) {
        return false;
    }
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        return texture_cache.IsRegionGpuModified(addr, size);
    }
    return false;
}

VideoCore::RasterizerDownloadArea RasterizerVulkan::GetFlushArea(DAddr addr, u64 size) {
    {
        std::scoped_lock lock{texture_cache.mutex};
        auto area = texture_cache.GetFlushArea(addr, size);
        if (area) {
            return *area;
        }
    }
    VideoCore::RasterizerDownloadArea new_area{
        .start_address = Common::AlignDown(addr, Core::DEVICE_PAGESIZE),
        .end_address = Common::AlignUp(addr + size, Core::DEVICE_PAGESIZE),
        .preemtive = true,
    };
    return new_area;
}

void RasterizerVulkan::InvalidateRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    if (addr == 0 || size == 0) {
        return;
    }
#if SNAPRYU_DIAGNOSTICS
    const VideoCommon::GpuLossTrace::Scope loss_source{
        VideoCommon::GpuLossTrace::Source::InvalidateRegion};
#endif
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::BufferCache))) {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.WriteMemory(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::QueryCache))) {
        query_cache.InvalidateRegion(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::ShaderCache))) {
        pipeline_cache.InvalidateRegion(addr, size);
    }
}

void RasterizerVulkan::InnerInvalidation(std::span<const std::pair<DAddr, std::size_t>> sequences) {
#if SNAPRYU_DIAGNOSTICS
    const VideoCommon::GpuLossTrace::Scope loss_source{
        VideoCommon::GpuLossTrace::Source::InnerInvalidation};
#endif
    const Common::CGT::SiteScope cgt_site{Common::CGT::Site::FlushCaching};
    {
        std::scoped_lock lock{texture_cache.mutex};
        for (const auto& [addr, size] : sequences) {
            texture_cache.WriteMemory(addr, size);
        }
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        for (const auto& [addr, size] : sequences) {
            buffer_cache.WriteMemory(addr, size);
        }
    }
    {
        for (const auto& [addr, size] : sequences) {
            query_cache.InvalidateRegion(addr, size);
            pipeline_cache.InvalidateRegion(addr, size);
        }
    }
}

bool RasterizerVulkan::OnCPUWrite(DAddr addr, u64 size) {
    DEBUG_ASSERT(addr != 0 || size != 0);
#if SNAPRYU_DIAGNOSTICS
    const VideoCommon::GpuLossTrace::Scope loss_source{
        VideoCommon::GpuLossTrace::Source::CpuWrite};
#endif
    {
        std::scoped_lock lock{buffer_cache.mutex};
        if (buffer_cache.OnCPUWrite(addr, size)) {
            return true;
        }
    }
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(addr, size);
    }
    pipeline_cache.InvalidateRegion(addr, size);
    return false;
}

void RasterizerVulkan::OnCacheInvalidation(DAddr addr, u64 size) {
    if (addr == 0 || size == 0) {
        return;
    }
#if SNAPRYU_DIAGNOSTICS
    const VideoCommon::GpuLossTrace::Scope loss_source{
        VideoCommon::GpuLossTrace::Source::CacheInvalidation};
#endif

    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(addr, size);
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.WriteMemory(addr, size);
    }
    pipeline_cache.InvalidateRegion(addr, size);
}

void RasterizerVulkan::InvalidateGPUCache() {
    gpu.InvalidateGPUCache();
}

void RasterizerVulkan::UnmapMemory(DAddr addr, u64 size) {
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.UnmapMemory(addr, size);
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.WriteMemory(addr, size);
    }
    pipeline_cache.OnCacheInvalidation(addr, size);
}

void RasterizerVulkan::ModifyGPUMemory(size_t as_id, GPUVAddr addr, u64 size) {
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.UnmapGPUMemory(as_id, addr, size);
    }
}

void RasterizerVulkan::SignalFence(std::function<void()>&& func) {
    fence_manager.SignalFence(std::move(func));
}

void RasterizerVulkan::SyncOperation(std::function<void()>&& func) {
    fence_manager.SyncOperation(std::move(func));
}

void RasterizerVulkan::SignalSyncPoint(u32 value) {
    fence_manager.SignalSyncPoint(value);
}

void RasterizerVulkan::SignalReference() {
    fence_manager.SignalReference();
}

void RasterizerVulkan::ReleaseFences(bool force) {
    fence_manager.WaitPendingFences(force);
}

void RasterizerVulkan::RefreshGPUSettings() {
    query_cache_runtime.RefreshGPUSettings();
}

void RasterizerVulkan::FlushAndInvalidateRegion(DAddr addr, u64 size,
                                                VideoCommon::CacheType which) {
    if (Settings::IsGPULevelHigh()) {
        FlushRegion(addr, size, which);
    }
    InvalidateRegion(addr, size, which);
}

void RasterizerVulkan::WaitForIdle() {
    // Everything but wait pixel operations. This intentionally includes FRAGMENT_SHADER_BIT because
    // fragment shaders can still write storage buffers.
    VkPipelineStageFlags flags =
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
        VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
        VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (device.IsExtTransformFeedbackSupported()) {
        flags |= VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT;
    }

    query_cache.NotifyWFI();

    if (WfiDefer::Enabled()) {
        // b46 (release default since b48): keep the guest WFI inside a safe render pass; the
        // scheduler records the same event barrier when that pass ends, or immediately when it
        // cannot be deferred.
        scheduler.RequestWaitForIdle(*wfi_event, flags);
        fence_manager.SignalOrdering();
        return;
    }
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([event = *wfi_event, flags](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetEvent(event, flags);
        cmdbuf.WaitEvents(event, flags, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, {}, {}, {});
    });
    fence_manager.SignalOrdering();
}

void RasterizerVulkan::FragmentBarrier() {
    if (FragmentBarrier::Enabled()) {
        scheduler.RequestFragmentBarrier();
        return;
    }
    // We already put barriers when a render pass finishes
    scheduler.RequestOutsideRenderPassOperationContext();
}

void RasterizerVulkan::TiledCacheBarrier() {
    // TODO: Implementing tiled barriers requires rewriting a good chunk of the Vulkan backend
}

void RasterizerVulkan::FlushCommands() {
    if (draw_counter == 0) {
        return;
    }
    draw_counter = 0;
    scheduler.Flush();
}

void RasterizerVulkan::TickFrame() {
#if SNAPRYU_DIAGNOSTICS
    PassCapture::Frame(scheduler, memory_allocator, device);
    SquareProbe::Frame();
    if (Common::CGT::Enabled()) [[unlikely]] {
        CgtSnapshotReadback(scheduler);
        Common::CGT::Frame(scheduler.CurrentTick(),
                           scheduler.GetMasterSemaphore().KnownGpuTick());
    }
#endif
    VideoCommon::PredicateTrace::Frame();
    VideoCommon::PredicateReadTrace::Frame();
    draw_counter = 0;
    guest_descriptor_queue.TickFrame();
    compute_pass_descriptor_queue.TickFrame();
    descriptor_buffer_ring.TickFrame();
    fence_manager.TickFrame();
    staging_pool.TickFrame();
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.TickFrame();
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.TickFrame();
    }
}

bool RasterizerVulkan::AccelerateConditionalRendering() {
    gpu_memory->FlushCaching();
    return query_cache.AccelerateHostConditionalRendering();
}

bool RasterizerVulkan::HasDrawTransformFeedback() {
    return device.IsTransformFeedbackDrawSupported();
}

bool RasterizerVulkan::AccelerateSurfaceCopy(const Tegra::Engines::Fermi2D::Surface& src,
                                             const Tegra::Engines::Fermi2D::Surface& dst,
                                             const Tegra::Engines::Fermi2D::Config& copy_config) {
    std::scoped_lock lock{texture_cache.mutex};
    return texture_cache.BlitImage(dst, src, copy_config);
}

Tegra::Engines::AccelerateDMAInterface& RasterizerVulkan::AccessAccelerateDMA() {
    return accelerate_dma;
}

void RasterizerVulkan::AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                                std::span<const u8> memory) {
#if SNAPRYU_DIAGNOSTICS
    const VideoCommon::GpuLossTrace::Scope loss_source{
        VideoCommon::GpuLossTrace::Source::InlineMemory};
#endif
    auto cpu_addr = gpu_memory->GpuToCpuAddress(address);
    if (!cpu_addr) [[unlikely]] {
        gpu_memory->WriteBlock(address, memory.data(), copy_size);
        return;
    }
    gpu_memory->WriteBlockUnsafe(address, memory.data(), copy_size);
    {
        std::unique_lock<std::recursive_mutex> lock{buffer_cache.mutex};
        if (!buffer_cache.InlineMemory(*cpu_addr, copy_size, memory)) {
            buffer_cache.WriteMemory(*cpu_addr, copy_size);
        }
    }
    {
        std::scoped_lock lock_texture{texture_cache.mutex};
        texture_cache.WriteMemory(*cpu_addr, copy_size);
    }
    pipeline_cache.InvalidateRegion(*cpu_addr, copy_size);
    query_cache.InvalidateRegion(*cpu_addr, copy_size);
#if SNAPRYU_DIAGNOSTICS
    // TIC_TRACE (b70): an inline write into a TIC table.
    VideoCommon::TicTrace::NoteInline(*gpu_memory, address, copy_size);
#endif
}

std::optional<FramebufferTextureInfo> RasterizerVulkan::AccelerateDisplay(
    const Tegra::FramebufferConfig& config, DAddr framebuffer_addr, u32 pixel_stride) {
    if (!framebuffer_addr) {
        return {};
    }
    std::scoped_lock lock{texture_cache.mutex};
    const auto [image_view, scaled] =
        texture_cache.TryFindFramebufferImageView(config, framebuffer_addr);
    if (!image_view) {
        return {};
    }

    query_cache.NotifySegment(false);

    const auto& resolution = Settings::values.resolution_info;

    FramebufferTextureInfo info{};
    info.image = image_view->ImageHandle();
    info.image_view = image_view->Handle(Shader::TextureType::Color2D);
    info.width = image_view->size.width;
    info.height = image_view->size.height;
    info.scaled_width = scaled ? resolution.ScaleUp(info.width) : info.width;
    info.scaled_height = scaled ? resolution.ScaleUp(info.height) : info.height;
    return info;
}

void RasterizerVulkan::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                         const VideoCore::DiskResourceLoadCallback& callback) {
    pipeline_cache.LoadDiskResources(title_id, stop_loading, callback);
}

void RasterizerVulkan::FlushWork() {
#ifdef __ANDROID__
    static constexpr u32 DRAWS_TO_DISPATCH = 512;
    static constexpr u32 CHECK_MASK = 3;
#else
    static constexpr u32 DRAWS_TO_DISPATCH = 4096;
    static constexpr u32 CHECK_MASK = 7;
#endif // __ANDROID__

    static_assert(DRAWS_TO_DISPATCH % (CHECK_MASK + 1) == 0);
    // Submission render-pass budget (b40/b42 experiments): bound the guest render passes recorded
    // into one submission so that no single command buffer can run past the Windows TDR limit on
    // a tiled GPU. Each pass break costs a tile store/load round trip; a 4096-draw submission with
    // ~1700 single-draw passes was observed to exceed the 2 s watchdog (LiveKernelEvent 141).
    static constexpr u32 RENDER_PASS_BUDGET = 256;
#if SNAPRYU_DIAGNOSTICS
    // Isolate submission boundaries from the much broader always-condition wait policy.
    // FlushWork runs before preparing a draw/clear/dispatch, at an existing safe flush site.
    static const u32 draws_to_dispatch = [] {
        const char* value = std::getenv("EDEN_SUBMIT_BATCH_512");
        const bool enabled = value && value[0] == '1' && value[1] == '\0';
        const u32 threshold = enabled ? 512U : DRAWS_TO_DISPATCH;
        LOG_INFO(Render_Vulkan, "SUBMIT_BATCH_512 enabled={} threshold={} check_mask={} "
                               "first_periodic_flush={} explicit_gpu_waits_added=0 "
                               "counts=draw_clear_compute_entry_calls", enabled, threshold,
                 CHECK_MASK, threshold + CHECK_MASK);
        return threshold;
    }();
    static const bool render_pass_budget = [] {
        // Default on, as in the release build; EDEN_SUBMIT_RP_BUDGET=0 disables it for A/B only.
        const char* value = std::getenv("EDEN_SUBMIT_RP_BUDGET");
        const bool enabled = !(value && value[0] == '0' && value[1] == '\0');
        LOG_INFO(Render_Vulkan,
                 "SUBMIT_RP_BUDGET enabled={} pass_limit={} check_mask={} default=on "
                 "scope=scheduled_guest_pass_starts explicit_gpu_waits_added=0",
                 enabled, RENDER_PASS_BUDGET, CHECK_MASK);
        return enabled;
    }();
#else
    static constexpr u32 draws_to_dispatch = DRAWS_TO_DISPATCH;
    static constexpr bool render_pass_budget = true;
#endif
    if ((++draw_counter & CHECK_MASK) != CHECK_MASK) {
        return;
    }
    // Split only pass-heavy recordings, before preparing the next draw/clear/dispatch.
    // Flushing inside Configure would invalidate already-recorded bindings for that draw.
    if (render_pass_budget && scheduler.RecordedRenderPasses() >= RENDER_PASS_BUDGET) {
#if !SNAPRYU_DIAGNOSTICS
        // Release: one line so the basic log shows that the budget is active (diagnostic builds
        // log every split as GPU_RP_BUDGET_SUBMIT instead).
        static bool logged_first_split = false;
        if (!logged_first_split) {
            logged_first_split = true;
            LOG_INFO(Render_Vulkan, "GPU_RP_BUDGET pass_limit={} default=on first_split_at_draw={}",
                     RENDER_PASS_BUDGET, draw_counter);
        }
#endif
        scheduler.MarkRenderPassBudgetSplit();
        scheduler.Flush();
        draw_counter = 0;
        return;
    }
    if (draw_counter < draws_to_dispatch) {
        scheduler.DispatchWork();
        return;
    }
    scheduler.Flush();
    draw_counter = 0;
}

AccelerateDMA::AccelerateDMA(BufferCache& buffer_cache_, TextureCache& texture_cache_,
                             Scheduler& scheduler_)
    : buffer_cache{buffer_cache_}, texture_cache{texture_cache_}, scheduler{scheduler_} {}

bool AccelerateDMA::BufferClear(GPUVAddr src_address, u64 amount, u32 value) {
    std::scoped_lock lock{buffer_cache.mutex};
    return buffer_cache.DMAClear(src_address, amount, value);
}

bool AccelerateDMA::BufferCopy(GPUVAddr src_address, GPUVAddr dest_address, u64 amount) {
#if SNAPRYU_DIAGNOSTICS
    // DMA_TRACE (b68): does the texture cache hold GPU content the buffer copy will not see?
    VideoCommon::DmaTrace::BeforeBufferCopy(texture_cache, buffer_cache, amount);
#endif
    std::scoped_lock lock{buffer_cache.mutex};
    return buffer_cache.DMACopy(src_address, dest_address, amount);
}

template <bool IS_IMAGE_UPLOAD>
bool AccelerateDMA::DmaBufferImageCopy(const Tegra::DMA::ImageCopy& copy_info,
                                       const Tegra::DMA::BufferOperand& buffer_operand,
                                       const Tegra::DMA::ImageOperand& image_operand) {
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    const auto image_id = texture_cache.DmaImageId(image_operand, IS_IMAGE_UPLOAD);
    if (image_id == VideoCommon::NULL_IMAGE_ID) {
        return false;
    }
    const u32 buffer_size = static_cast<u32>(buffer_operand.pitch * buffer_operand.height);
    static constexpr auto sync_info = VideoCommon::ObtainBufferSynchronize::FullSynchronize;
    const auto post_op = IS_IMAGE_UPLOAD ? VideoCommon::ObtainBufferOperation::DoNothing
                                         : VideoCommon::ObtainBufferOperation::MarkAsWritten;
    const auto [buffer, offset] =
        buffer_cache.ObtainBuffer(buffer_operand.address, buffer_size, sync_info, post_op);

    const auto [image, copy] = texture_cache.DmaBufferImageCopy(
        copy_info, buffer_operand, image_operand, image_id, IS_IMAGE_UPLOAD);
    const std::span copy_span{&copy, 1};

    if constexpr (IS_IMAGE_UPLOAD) {
        texture_cache.PrepareImage(image_id, true, false);
        image->UploadMemory(buffer->Handle(), offset, copy_span);
    } else {
        if (offset % BytesPerBlock(image->info.format)) {
            return false;
        }
        texture_cache.DownloadImageIntoBuffer(image, buffer->Handle(), offset, copy_span,
                                              buffer_operand.address, buffer_size);
    }
    return true;
}

bool AccelerateDMA::ImageToBuffer(const Tegra::DMA::ImageCopy& copy_info,
                                  const Tegra::DMA::ImageOperand& image_operand,
                                  const Tegra::DMA::BufferOperand& buffer_operand) {
    return DmaBufferImageCopy<false>(copy_info, buffer_operand, image_operand);
}

bool AccelerateDMA::BufferToImage(const Tegra::DMA::ImageCopy& copy_info,
                                  const Tegra::DMA::BufferOperand& buffer_operand,
                                  const Tegra::DMA::ImageOperand& image_operand) {
    return DmaBufferImageCopy<true>(copy_info, buffer_operand, image_operand);
}

void RasterizerVulkan::UpdateDynamicStates() {
    auto& regs = maxwell3d->regs;
    auto& flags = maxwell3d->dirty.flags;
    const auto topology = maxwell3d->draw_manager.draw_state.topology;
    const bool topology_changed = state_tracker.ChangePrimitiveTopology(topology);
    if (topology_changed) {
        flags[Dirty::DepthBiasEnable] = true;
        flags[Dirty::PrimitiveRestartEnable] = true;
    }

    UpdateViewportsState(regs);
    UpdateScissorsState(regs);
    UpdateDepthBias(regs);
    UpdateBlendConstants(regs);
    UpdateDepthBounds(regs);
    UpdateStencilFaces(regs);
    UpdateLineWidth(regs);

    if (device.IsExtExtendedDynamicStateSupported()) {
        UpdateCullMode(regs);
        UpdateDepthCompareOp(regs);
        UpdateFrontFace(regs);
        UpdateStencilOp(regs);
        if (state_tracker.TouchStateEnable()) {
            UpdateDepthBoundsTestEnable(regs);
            UpdateDepthTestEnable(regs);
            UpdateDepthWriteEnable(regs);
            UpdateStencilTestEnable(regs);
        }
        if (topology_changed) {
            scheduler.Record([topology_vk = MaxwellToVK::PrimitiveTopology(device, topology)](
                                 vk::CommandBuffer cmdbuf) {
                cmdbuf.SetPrimitiveTopologyEXT(topology_vk);
            });
        }
    }

    if (device.IsExtExtendedDynamicState2Supported()) {
        UpdatePrimitiveRestartEnable(regs);
        UpdateRasterizerDiscardEnable(regs);
        UpdateDepthBiasEnable(regs);
    }

    if (device.IsExtExtendedDynamicState2ExtrasSupported()) {
        UpdateLogicOp(regs);
    }

    if (device.IsExtExtendedDynamicState3EnablesSupported()) {
        using namespace Tegra::Engines;
        // AMD Workaround: LogicOp incompatible with float render targets
        if (device.GetDriverID() == VkDriverIdKHR::VK_DRIVER_ID_AMD_OPEN_SOURCE ||
            device.GetDriverID() == VkDriverIdKHR::VK_DRIVER_ID_AMD_PROPRIETARY) {
            const auto has_float = std::any_of(
                regs.vertex_attrib_format.begin(), regs.vertex_attrib_format.end(),
                [](const auto& attrib) {
                    return attrib.type == Maxwell3D::Regs::VertexAttribute::Type::Float;
                }
            );
            if (regs.logic_op.enable) {
                regs.logic_op.enable = static_cast<u32>(!has_float);
            }
        }
        UpdateLogicOpEnable(regs);
        UpdateDepthClampEnable(regs);
        UpdateLineRasterizationMode(regs);
        UpdateLineStippleEnable(regs);
        UpdateConservativeRasterizationMode(regs);
        UpdateAlphaToCoverageEnable(regs);
        UpdateAlphaToOneEnable(regs);
    }

    if (device.IsExtExtendedDynamicState3BlendingSupported()) {
        UpdateBlending(regs);
    } else if (device.IsExtColorWriteEnableSupported()) {
        UpdateColorWriteEnable(regs);
    }

    if (device.IsExtVertexInputDynamicStateSupported()) {
        if (auto* gp = pipeline_cache.CurrentGraphicsPipeline(); gp && gp->HasDynamicVertexInput()) {
            UpdateVertexInput(regs);
        }
    }
}

void RasterizerVulkan::HandleTransformFeedback() {
    static std::once_flag warn_unsupported;

    const auto& regs = maxwell3d->regs;
    if (!device.IsExtTransformFeedbackSupported()) {
        if (regs.transform_feedback_enabled != 0) {
            std::call_once(warn_unsupported, [&] {
                LOG_WARNING(Render_Vulkan, "Transform feedback requested by guest but VK_EXT_transform_feedback is unavailable; queries disabled");
            });
        } else {
            std::call_once(warn_unsupported, [&] {
                LOG_INFO(Render_Vulkan, "VK_EXT_transform_feedback not available on device");
            });
        }
        return;
    }
    query_cache.CounterEnable(VideoCommon::QueryType::StreamingByteCount,
                              regs.transform_feedback_enabled);
    if (regs.transform_feedback_enabled != 0) {
        // Log extension usage for transform feedback
        if (GPU::Logging::IsActive()) {
            GPU::Logging::GPULogger::GetInstance().LogExtensionUsage(
                "VK_EXT_transform_feedback", "HandleTransformFeedback");
        }
        UNIMPLEMENTED_IF(regs.IsShaderConfigEnabled(Maxwell::ShaderType::TessellationInit) ||
                         regs.IsShaderConfigEnabled(Maxwell::ShaderType::Tessellation));
    }
}

#if SNAPRYU_DIAGNOSTICS
namespace {
// SNAPRYU_VIEWPORT_LOG=1: every 5 s, log the guest viewport 0 sizes set in that window, by number of
// viewport updates. Guest units, before Eden's resolution scaling. Shows the game's own render size
// (TotK fixed 1080p r3 vs native dynamic resolution) without reading guest memory.
bool ViewportLogEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SNAPRYU_VIEWPORT_LOG");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

void RecordViewportSize(float width, float height) {
    static std::mutex mutex;
    static std::map<std::pair<u32, u32>, u64> counts;
    static auto window_start = std::chrono::steady_clock::now();
    const std::pair<u32, u32> size{static_cast<u32>(std::lround(std::fabs(width))),
                                   static_cast<u32>(std::lround(std::fabs(height)))};
    std::scoped_lock lock{mutex};
    ++counts[size];
    const auto now = std::chrono::steady_clock::now();
    if (now - window_start < std::chrono::seconds{5}) {
        return;
    }
    window_start = now;
    std::vector<std::pair<u64, std::pair<u32, u32>>> sorted;
    for (const auto& [key, count] : counts) {
        sorted.emplace_back(count, key);
    }
    std::ranges::sort(sorted, std::greater{});
    std::string line;
    for (size_t i = 0; i < std::min<size_t>(sorted.size(), 12); ++i) {
        line += fmt::format(" {}x{}:{}", sorted[i].second.first, sorted[i].second.second,
                            sorted[i].first);
    }
    LOG_WARNING(Render_Vulkan, "VIEWPORT_LOG 5s guest viewport0 size:updates{}", line);
    counts.clear();
}
} // Anonymous namespace
#endif

void RasterizerVulkan::UpdateViewportsState(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchViewports()) {
        return;
    }

    maxwell3d->dirty.flags[Dirty::Scissors] = true;
#if SNAPRYU_DIAGNOSTICS
    if (ViewportLogEnabled()) {
        if (regs.viewport_scale_offset_enabled) {
            RecordViewportSize(regs.viewport_transform[0].scale_x * 2.0f,
                               regs.viewport_transform[0].scale_y * 2.0f);
        } else {
            RecordViewportSize(static_cast<float>(regs.surface_clip.width),
                               static_cast<float>(regs.surface_clip.height));
        }
    }
#endif

    if (!regs.viewport_scale_offset_enabled) {
        float x = static_cast<float>(regs.surface_clip.x);
        float y = static_cast<float>(regs.surface_clip.y);
        float width = (std::max)(1.0f, static_cast<float>(regs.surface_clip.width));
        float height = (std::max)(1.0f, static_cast<float>(regs.surface_clip.height));
        if (regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft) {
            y += height;
            height = -height;
        }
        VkViewport viewport{
            .x = x,
            .y = y,
            .width = width,
            .height = height,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        scheduler.Record([this, viewport](vk::CommandBuffer cmdbuf) {
            const u32 num_viewports = std::min<u32>(device.GetMaxViewports(), Maxwell::NumViewports);
            std::array<VkViewport, Maxwell::NumViewports> viewport_list{};
            viewport_list.fill(viewport);
            const vk::Span<VkViewport> viewports(viewport_list.data(), num_viewports);
            cmdbuf.SetViewport(0, viewports);
        });
        return;
    }
    const bool is_rescaling{texture_cache.IsRescaling()};
    const float scale = is_rescaling ? Settings::values.resolution_info.up_factor : 1.0f;
    const std::array viewport_list{
        GetViewportState(device, regs, 0, scale),  GetViewportState(device, regs, 1, scale),
        GetViewportState(device, regs, 2, scale),  GetViewportState(device, regs, 3, scale),
        GetViewportState(device, regs, 4, scale),  GetViewportState(device, regs, 5, scale),
        GetViewportState(device, regs, 6, scale),  GetViewportState(device, regs, 7, scale),
        GetViewportState(device, regs, 8, scale),  GetViewportState(device, regs, 9, scale),
        GetViewportState(device, regs, 10, scale), GetViewportState(device, regs, 11, scale),
        GetViewportState(device, regs, 12, scale), GetViewportState(device, regs, 13, scale),
        GetViewportState(device, regs, 14, scale), GetViewportState(device, regs, 15, scale),
    };
    scheduler.Record([this, viewport_list](vk::CommandBuffer cmdbuf) {
        const u32 num_viewports = std::min<u32>(device.GetMaxViewports(), Maxwell::NumViewports);
        const vk::Span<VkViewport> viewports(viewport_list.data(), num_viewports);
        cmdbuf.SetViewport(0, viewports);
    });
}

void RasterizerVulkan::UpdateScissorsState(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchScissors()) {
        return;
    }
    if (!regs.viewport_scale_offset_enabled) {
        u32 x = regs.surface_clip.x;
        u32 y = regs.surface_clip.y;
        u32 width = (std::max)(1u, static_cast<u32>(regs.surface_clip.width));
        u32 height = (std::max)(1u, static_cast<u32>(regs.surface_clip.height));
        if (regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft) {
            y = regs.surface_clip.height - (y + height);
        }
        VkRect2D scissor{};
        scissor.offset.x = static_cast<int32_t>(x);
        scissor.offset.y = static_cast<int32_t>(y);
        scissor.extent.width  = width;
        scissor.extent.height = height;
        scheduler.Record([this, scissor](vk::CommandBuffer cmdbuf) {
            const u32 num_scissors = std::min<u32>(device.GetMaxViewports(), Maxwell::NumViewports);
            std::array<VkRect2D, Maxwell::NumViewports> scissor_list{};
            scissor_list.fill(scissor);
            const vk::Span<VkRect2D> scissors(scissor_list.data(), num_scissors);
            cmdbuf.SetScissor(0, scissors);
        });
        return;
    }
    u32 up_scale = 1;
    u32 down_shift = 0;
    if (texture_cache.IsRescaling()) {
        up_scale = Settings::values.resolution_info.up_scale;
        down_shift = Settings::values.resolution_info.down_shift;
    }
    const std::array scissor_list{
        GetScissorState(regs, 0, up_scale, down_shift),
        GetScissorState(regs, 1, up_scale, down_shift),
        GetScissorState(regs, 2, up_scale, down_shift),
        GetScissorState(regs, 3, up_scale, down_shift),
        GetScissorState(regs, 4, up_scale, down_shift),
        GetScissorState(regs, 5, up_scale, down_shift),
        GetScissorState(regs, 6, up_scale, down_shift),
        GetScissorState(regs, 7, up_scale, down_shift),
        GetScissorState(regs, 8, up_scale, down_shift),
        GetScissorState(regs, 9, up_scale, down_shift),
        GetScissorState(regs, 10, up_scale, down_shift),
        GetScissorState(regs, 11, up_scale, down_shift),
        GetScissorState(regs, 12, up_scale, down_shift),
        GetScissorState(regs, 13, up_scale, down_shift),
        GetScissorState(regs, 14, up_scale, down_shift),
        GetScissorState(regs, 15, up_scale, down_shift),
    };
    scheduler.Record([this, scissor_list](vk::CommandBuffer cmdbuf) {
        const u32 num_scissors = std::min<u32>(device.GetMaxViewports(), Maxwell::NumViewports);
        const vk::Span<VkRect2D> scissors(scissor_list.data(), num_scissors);
        cmdbuf.SetScissor(0, scissors);
    });
}

void RasterizerVulkan::UpdateDepthBias(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthBias()) {
        return;
    }
    float units = regs.depth_bias / 2.0f;
    const bool is_d24 = regs.zeta.format == Tegra::DepthFormat::Z24_UNORM_S8_UINT ||
                        regs.zeta.format == Tegra::DepthFormat::X8Z24_UNORM ||
                        regs.zeta.format == Tegra::DepthFormat::S8Z24_UNORM ||
                        regs.zeta.format == Tegra::DepthFormat::V8Z24_UNORM;

    if (is_d24 && !device.SupportsD24DepthBuffer()) {
        static constexpr const size_t length = sizeof(NEEDS_D24) / sizeof(NEEDS_D24[0]);

        static constexpr const u64* start = NEEDS_D24;
        static constexpr const u64* end = NEEDS_D24 + length;

        const u64* it = std::find(start, end, program_id);

        if (it != end) {
            // the base formulas can be obtained from here:
            //   https://docs.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-output-merger-stage-depth-bias
            const double rescale_factor =
                static_cast<double>(1ULL << (32 - 24)) / (static_cast<double>(0x1.ep+127));
            units = static_cast<float>(static_cast<double>(units) * rescale_factor);
        }
    }

    scheduler.Record([constant = units, clamp = regs.depth_bias_clamp,
                      factor = regs.slope_scale_depth_bias, this](vk::CommandBuffer cmdbuf) {
        if (device.IsExtDepthBiasControlSupported()) {
            static VkDepthBiasRepresentationInfoEXT bias_info{
                .sType = VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT,
                .pNext = nullptr,
                .depthBiasRepresentation =
                    VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT,
                .depthBiasExact = VK_FALSE,
            };

            cmdbuf.SetDepthBias(constant, clamp, factor, &bias_info);
        } else {
            cmdbuf.SetDepthBias(constant, clamp, factor);
        }
    });
}

void RasterizerVulkan::UpdateBlendConstants(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchBlendConstants()) {
        return;
    }
    const std::array blend_color = {regs.blend_color.r, regs.blend_color.g, regs.blend_color.b,
                                    regs.blend_color.a};
    scheduler.Record(
        [blend_color](vk::CommandBuffer cmdbuf) { cmdbuf.SetBlendConstants(blend_color.data()); });
}

void RasterizerVulkan::UpdateDepthBounds(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthBounds()) {
        return;
    }
    scheduler.Record([min = regs.depth_bounds[0], max = regs.depth_bounds[1]](
                         vk::CommandBuffer cmdbuf) { cmdbuf.SetDepthBounds(min, max); });
}

void RasterizerVulkan::UpdateStencilFaces(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchStencilProperties()) {
        return;
    }
    bool update_references = state_tracker.TouchStencilReference();
    bool update_write_mask = state_tracker.TouchStencilWriteMask();
    bool update_compare_masks = state_tracker.TouchStencilCompare();
    if (state_tracker.TouchStencilSide(regs.stencil_two_side_enable != 0)) {
        update_references = true;
        update_write_mask = true;
        update_compare_masks = true;
    }
    if (update_references) {
        [&]() {
            bool changed;
            if (regs.stencil_two_side_enable) {
                const bool front_changed =
                    state_tracker.CheckStencilReferenceFront(regs.stencil_front_ref);
                const bool back_changed =
                    state_tracker.CheckStencilReferenceBack(regs.stencil_back_ref);
                changed = front_changed || back_changed;
            } else {
                const bool front_changed =
                    state_tracker.CheckStencilReferenceFront(regs.stencil_front_ref);
                const bool back_changed =
                    state_tracker.CheckStencilReferenceBack(regs.stencil_front_ref);
                changed = front_changed || back_changed;
            }
            if (!changed) {
                return;
            }
            scheduler.Record([front_ref = regs.stencil_front_ref, back_ref = regs.stencil_back_ref,
                              two_sided = regs.stencil_two_side_enable](vk::CommandBuffer cmdbuf) {
                const bool set_back = two_sided && front_ref != back_ref;
                // Front face
                cmdbuf.SetStencilReference(set_back ? VK_STENCIL_FACE_FRONT_BIT
                                                    : VK_STENCIL_FACE_FRONT_AND_BACK,
                                           front_ref);
                if (set_back) {
                    cmdbuf.SetStencilReference(VK_STENCIL_FACE_BACK_BIT, back_ref);
                }
            });
        }();
    }
    if (update_write_mask) {
        [&]() {
            bool changed;
            if (regs.stencil_two_side_enable) {
                const bool front_changed =
                    state_tracker.CheckStencilWriteMaskFront(regs.stencil_front_mask);
                const bool back_changed =
                    state_tracker.CheckStencilWriteMaskBack(regs.stencil_back_mask);
                changed = front_changed || back_changed;
            } else {
                const bool front_changed =
                    state_tracker.CheckStencilWriteMaskFront(regs.stencil_front_mask);
                const bool back_changed =
                    state_tracker.CheckStencilWriteMaskBack(regs.stencil_front_mask);
                changed = front_changed || back_changed;
            }
            if (!changed) {
                return;
            }
            scheduler.Record([front_write_mask = regs.stencil_front_mask,
                              back_write_mask = regs.stencil_back_mask,
                              two_sided = regs.stencil_two_side_enable](vk::CommandBuffer cmdbuf) {
                const bool set_back = two_sided && front_write_mask != back_write_mask;
                // Front face
                cmdbuf.SetStencilWriteMask(set_back ? VK_STENCIL_FACE_FRONT_BIT
                                                    : VK_STENCIL_FACE_FRONT_AND_BACK,
                                           front_write_mask);
                if (set_back) {
                    cmdbuf.SetStencilWriteMask(VK_STENCIL_FACE_BACK_BIT, back_write_mask);
                }
            });
        }();
    }
    if (update_compare_masks) {
        [&]() {
            bool changed;
            if (regs.stencil_two_side_enable) {
                const bool front_changed =
                    state_tracker.CheckStencilCompareMaskFront(regs.stencil_front_func_mask);
                const bool back_changed =
                    state_tracker.CheckStencilCompareMaskBack(regs.stencil_back_func_mask);
                changed = front_changed || back_changed;
            } else {
                const bool front_changed =
                    state_tracker.CheckStencilCompareMaskFront(regs.stencil_front_func_mask);
                const bool back_changed =
                    state_tracker.CheckStencilCompareMaskBack(regs.stencil_front_func_mask);
                changed = front_changed || back_changed;
            }
            if (!changed) {
                return;
            }
            scheduler.Record([front_test_mask = regs.stencil_front_func_mask,
                              back_test_mask = regs.stencil_back_func_mask,
                              two_sided = regs.stencil_two_side_enable](vk::CommandBuffer cmdbuf) {
                const bool set_back = two_sided && front_test_mask != back_test_mask;
                // Front face
                cmdbuf.SetStencilCompareMask(set_back ? VK_STENCIL_FACE_FRONT_BIT
                                                      : VK_STENCIL_FACE_FRONT_AND_BACK,
                                             front_test_mask);
                if (set_back) {
                    cmdbuf.SetStencilCompareMask(VK_STENCIL_FACE_BACK_BIT, back_test_mask);
                }
            });
        }();
    }
    state_tracker.ClearStencilReset();
}

void RasterizerVulkan::UpdateLineWidth(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchLineWidth()) {
        return;
    }
    const float width =
        regs.line_anti_alias_enable ? regs.line_width_smooth : regs.line_width_aliased;
    scheduler.Record([width](vk::CommandBuffer cmdbuf) { cmdbuf.SetLineWidth(width); });
}

void RasterizerVulkan::UpdateCullMode(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchCullMode()) {
        return;
    }
    scheduler.Record([enabled = regs.gl_cull_test_enabled,
                      cull_face = regs.gl_cull_face](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetCullModeEXT(enabled ? MaxwellToVK::CullFace(cull_face) : VK_CULL_MODE_NONE);
    });
}

void RasterizerVulkan::UpdateDepthBoundsTestEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthBoundsTestEnable()) {
        return;
    }
    bool enabled = regs.depth_bounds_enable;
    if (enabled && !device.IsDepthBoundsSupported()) {
        LOG_WARNING(Render_Vulkan, "Depth bounds is enabled but not supported");
        enabled = false;
    }
    scheduler.Record([enable = enabled](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetDepthBoundsTestEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdateDepthTestEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthTestEnable()) {
        return;
    }
    scheduler.Record([enable = regs.depth_test_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetDepthTestEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdateDepthWriteEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthWriteEnable()) {
        return;
    }
    scheduler.Record([enable = regs.depth_write_enabled](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetDepthWriteEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdatePrimitiveRestartEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchPrimitiveRestartEnable()) {
        return;
    }

    bool enable = regs.primitive_restart.enabled != 0;
    if (device.IsMoltenVK()) {
        enable = true;
    } else if (enable) {
        const auto topology = MaxwellToVK::PrimitiveTopology(device, maxwell3d->draw_manager.draw_state.topology);
        enable = IsPrimitiveRestartSupported(device, topology);
    }

    scheduler.Record([enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetPrimitiveRestartEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdateRasterizerDiscardEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchRasterizerDiscardEnable()) {
        return;
    }
    scheduler.Record([disable = regs.rasterize_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetRasterizerDiscardEnableEXT(disable == 0);
    });
}

void RasterizerVulkan::UpdateConservativeRasterizationMode(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchConservativeRasterizationMode()) {
        return;
    }

    if (!device.SupportsDynamicState3ConservativeRasterizationMode()) {
        return;
    }

    scheduler.Record([enable = regs.conservative_raster_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetConservativeRasterizationModeEXT(
            enable ? VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT
                   : VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT);
    });
}

void RasterizerVulkan::UpdateLineStippleEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchLineStippleEnable()) {
        return;
    }

    if (!device.SupportsDynamicState3LineStippleEnable()) {
        return;
    }

    scheduler.Record([enable = regs.line_stipple_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetLineStippleEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdateLineRasterizationMode(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!device.IsExtLineRasterizationSupported()) {
        return;
    }
    if (!state_tracker.TouchLineRasterizationMode()) {
        return;
    }

    if (!device.SupportsDynamicState3LineRasterizationMode()) {
        static std::once_flag warn_missing_rect;
        std::call_once(warn_missing_rect, [] {
            LOG_WARNING(Render_Vulkan,
                        "Driver lacks rectangular line rasterization support; skipping dynamic "
                        "line state updates");
        });
        return;
    }

    const bool wants_smooth = regs.line_anti_alias_enable != 0;
    VkLineRasterizationModeEXT mode = VK_LINE_RASTERIZATION_MODE_RECTANGULAR_EXT;
    if (wants_smooth) {
        if (device.SupportsSmoothLines()) {
            mode = VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_EXT;
        } else {
            static std::once_flag warn_missing_smooth;
            std::call_once(warn_missing_smooth, [] {
                LOG_WARNING(Render_Vulkan,
                            "Line anti-aliasing requested but smoothLines feature unavailable; "
                            "using rectangular rasterization");
            });
        }
    }
    scheduler.Record([mode](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetLineRasterizationModeEXT(mode);
    });
}

void RasterizerVulkan::UpdateDepthBiasEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthBiasEnable()) {
        return;
    }
    constexpr size_t POINT = 0;
    constexpr size_t LINE = 1;
    constexpr size_t POLYGON = 2;
    static constexpr std::array POLYGON_OFFSET_ENABLE_LUT = {
        POINT,   // Points
        LINE,    // Lines
        LINE,    // LineLoop
        LINE,    // LineStrip
        POLYGON, // Triangles
        POLYGON, // TriangleStrip
        POLYGON, // TriangleFan
        POLYGON, // Quads
        POLYGON, // QuadStrip
        POLYGON, // Polygon
        LINE,    // LinesAdjacency
        LINE,    // LineStripAdjacency
        POLYGON, // TrianglesAdjacency
        POLYGON, // TriangleStripAdjacency
        POLYGON, // Patches
    };
    const std::array enabled_lut{
        regs.polygon_offset_point_enable,
        regs.polygon_offset_line_enable,
        regs.polygon_offset_fill_enable,
    };
    const u32 topology_index = u32(maxwell3d->draw_manager.draw_state.topology);
    const u32 enable = enabled_lut[POLYGON_OFFSET_ENABLE_LUT[topology_index]];
    scheduler.Record([enable](vk::CommandBuffer cmdbuf) { cmdbuf.SetDepthBiasEnableEXT(enable != 0); });
}

void RasterizerVulkan::UpdateLogicOpEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchLogicOpEnable()) {
        return;
    }
    if (!device.SupportsDynamicState3LogicOpEnable()) {
        return;
    }
    scheduler.Record([enable = regs.logic_op.enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetLogicOpEnableEXT(enable != 0);
    });
}

void RasterizerVulkan::UpdateDepthClampEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthClampEnable()) {
        return;
    }
    if (!device.SupportsDynamicState3DepthClampEnable()) {
        return;
    }
    bool is_enabled = !(regs.viewport_clip_control.geometry_clip ==
                            Maxwell::ViewportClipControl::GeometryClip::Passthrough ||
                        regs.viewport_clip_control.geometry_clip ==
                            Maxwell::ViewportClipControl::GeometryClip::FrustumXYZ ||
                        regs.viewport_clip_control.geometry_clip ==
                            Maxwell::ViewportClipControl::GeometryClip::FrustumZ);
    scheduler.Record(
        [is_enabled](vk::CommandBuffer cmdbuf) { cmdbuf.SetDepthClampEnableEXT(is_enabled); });
}

void RasterizerVulkan::UpdateAlphaToCoverageEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchAlphaToCoverageEnable()) {
        return;
    }
    if (!device.SupportsDynamicState3AlphaToCoverageEnable()) {
        return;
    }
    GraphicsPipeline* const pipeline = pipeline_cache.CurrentGraphicsPipeline();
    const bool enable = pipeline != nullptr && pipeline->SupportsAlphaToCoverage() &&
                        regs.anti_alias_alpha_control.alpha_to_coverage != 0;
    scheduler.Record([enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetAlphaToCoverageEnableEXT(enable ? VK_TRUE : VK_FALSE);
    });
}

void RasterizerVulkan::UpdateAlphaToOneEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchAlphaToOneEnable()) {
        return;
    }
    if (!device.SupportsDynamicState3AlphaToOneEnable()) {
        static std::once_flag warn_alpha_to_one;
        std::call_once(warn_alpha_to_one, [] {
            LOG_WARNING(Render_Vulkan,
                        "Alpha-to-one is not supported on this device; forcing it disabled");
        });
        return;
    }
    GraphicsPipeline* const pipeline = pipeline_cache.CurrentGraphicsPipeline();
    const bool enable = pipeline != nullptr && pipeline->SupportsAlphaToOne() &&
                        regs.anti_alias_alpha_control.alpha_to_one != 0;
    scheduler.Record([enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetAlphaToOneEnableEXT(enable ? VK_TRUE : VK_FALSE);
    });
}

void RasterizerVulkan::UpdateDepthCompareOp(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthCompareOp()) {
        return;
    }
    scheduler.Record([func = regs.depth_test_func](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetDepthCompareOpEXT(MaxwellToVK::DepthComparisonOp(func));
    });
}

void RasterizerVulkan::UpdateFrontFace(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchFrontFace()) {
        return;
    }

    VkFrontFace front_face = MaxwellToVK::FrontFace(regs.gl_front_face);
    if (regs.window_origin.flip_y != 0) {
        front_face = front_face == VK_FRONT_FACE_CLOCKWISE ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                           : VK_FRONT_FACE_CLOCKWISE;
    }
    scheduler.Record(
        [front_face](vk::CommandBuffer cmdbuf) { cmdbuf.SetFrontFaceEXT(front_face); });
}

void RasterizerVulkan::UpdateStencilOp(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchStencilOp()) {
        return;
    }
    const Maxwell::StencilOp::Op fail = regs.stencil_front_op.fail;
    const Maxwell::StencilOp::Op zfail = regs.stencil_front_op.zfail;
    const Maxwell::StencilOp::Op zpass = regs.stencil_front_op.zpass;
    const Maxwell::ComparisonOp compare = regs.stencil_front_op.func;
    if (regs.stencil_two_side_enable) {
        // Separate stencil op per face
        const Maxwell::StencilOp::Op back_fail = regs.stencil_back_op.fail;
        const Maxwell::StencilOp::Op back_zfail = regs.stencil_back_op.zfail;
        const Maxwell::StencilOp::Op back_zpass = regs.stencil_back_op.zpass;
        const Maxwell::ComparisonOp back_compare = regs.stencil_back_op.func;
        scheduler.Record([fail, zfail, zpass, compare, back_fail, back_zfail, back_zpass,
                          back_compare](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetStencilOpEXT(VK_STENCIL_FACE_FRONT_BIT, MaxwellToVK::StencilOp(fail),
                                   MaxwellToVK::StencilOp(zpass), MaxwellToVK::StencilOp(zfail),
                                   MaxwellToVK::ComparisonOp(compare));
            cmdbuf.SetStencilOpEXT(VK_STENCIL_FACE_BACK_BIT, MaxwellToVK::StencilOp(back_fail),
                                   MaxwellToVK::StencilOp(back_zpass),
                                   MaxwellToVK::StencilOp(back_zfail),
                                   MaxwellToVK::ComparisonOp(back_compare));
        });
    } else {
        // Front face defines the stencil op of both faces
        scheduler.Record([fail, zfail, zpass, compare](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetStencilOpEXT(VK_STENCIL_FACE_FRONT_AND_BACK, MaxwellToVK::StencilOp(fail),
                                   MaxwellToVK::StencilOp(zpass), MaxwellToVK::StencilOp(zfail),
                                   MaxwellToVK::ComparisonOp(compare));
        });
    }
}

void RasterizerVulkan::UpdateLogicOp(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchLogicOp()) {
        return;
    }
    const auto op_value = static_cast<u32>(regs.logic_op.op);
    auto op = op_value >= 0x1500 && op_value < 0x1510 ? static_cast<VkLogicOp>(op_value - 0x1500)
                                                      : VK_LOGIC_OP_NO_OP;
    scheduler.Record([op](vk::CommandBuffer cmdbuf) { cmdbuf.SetLogicOpEXT(op); });
}

void RasterizerVulkan::UpdateBlending(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchBlending()) {
        return;
    }

    if (state_tracker.TouchColorMask()) {
        std::array<VkColorComponentFlags, Maxwell::NumRenderTargets> setup_masks{};
        for (size_t index = 0; index < Maxwell::NumRenderTargets; index++) {
            const auto& mask = regs.color_mask[regs.color_mask_common ? 0 : index];
            auto& current = setup_masks[index];
            if (mask.R) {
                current |= VK_COLOR_COMPONENT_R_BIT;
            }
            if (mask.G) {
                current |= VK_COLOR_COMPONENT_G_BIT;
            }
            if (mask.B) {
                current |= VK_COLOR_COMPONENT_B_BIT;
            }
            if (mask.A) {
                current |= VK_COLOR_COMPONENT_A_BIT;
            }
        }
        scheduler.Record([setup_masks](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetColorWriteMaskEXT(0, setup_masks);
        });
    }

    if (state_tracker.TouchBlendEnable()) {
        std::array<VkBool32, Maxwell::NumRenderTargets> setup_enables{};
        for (size_t index = 0; index < Maxwell::NumRenderTargets; index++) {
            bool is_integer = false;
            if (regs.rt[index].format != Tegra::RenderTargetFormat::NONE) {
                const auto format =
                    VideoCore::Surface::PixelFormatFromRenderTargetFormat(regs.rt[index].format);
                is_integer = IsPixelFormatInteger(format);
            }
            setup_enables[index] =
                (!is_integer && regs.blend.enable[index] != 0) ? VK_TRUE : VK_FALSE;
        }
        scheduler.Record([setup_enables](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetColorBlendEnableEXT(0, setup_enables);
        });
    }

    if (state_tracker.TouchBlendEquations()) {
        std::array<VkColorBlendEquationEXT, Maxwell::NumRenderTargets> setup_blends{};

        const auto blend_setup = [&](auto& host_blend, const auto& guest_blend) {
            host_blend.srcColorBlendFactor = MaxwellToVK::BlendFactor(guest_blend.color_source);
            host_blend.dstColorBlendFactor = MaxwellToVK::BlendFactor(guest_blend.color_dest);
            host_blend.colorBlendOp = MaxwellToVK::BlendEquation(guest_blend.color_op);
            host_blend.srcAlphaBlendFactor = MaxwellToVK::BlendFactor(guest_blend.alpha_source);
            host_blend.dstAlphaBlendFactor = MaxwellToVK::BlendFactor(guest_blend.alpha_dest);
            host_blend.alphaBlendOp = MaxwellToVK::BlendEquation(guest_blend.alpha_op);
        };

        // Single blend equation for all targets
        if (!regs.blend_per_target_enabled) {
            // Temporary workaround for games that use iterated blending
            if (regs.iterated_blend.enable && Settings::values.use_squashed_iterated_blend) {
                setup_blends[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                setup_blends[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                setup_blends[0].colorBlendOp = VK_BLEND_OP_ADD;
                setup_blends[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
                setup_blends[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                setup_blends[0].alphaBlendOp = VK_BLEND_OP_ADD;
            } else {
                blend_setup(setup_blends[0], regs.blend);
            }

            // Copy first blend state to all other targets
            for (size_t index = 1; index < Maxwell::NumRenderTargets; index++) {
                setup_blends[index] = setup_blends[0];
            }
        } else {
            // Per-target blending
            for (size_t index = 0; index < Maxwell::NumRenderTargets; index++) {
                blend_setup(setup_blends[index], regs.blend_per_target[index]);
            }
        }

        scheduler.Record([setup_blends](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetColorBlendEquationEXT(0, setup_blends);
        });
    }
}

void RasterizerVulkan::UpdateColorWriteEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchColorMask()) {
        return;
    }
    std::array<VkBool32, Maxwell::NumRenderTargets> setup_enables{};
    for (size_t index = 0; index < Maxwell::NumRenderTargets; index++) {
        const auto& mask = regs.color_mask[regs.color_mask_common ? 0 : index];
        setup_enables[index] = (mask.R || mask.G || mask.B || mask.A) ? VK_TRUE : VK_FALSE;
    }
    scheduler.Record([setup_enables](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetColorWriteEnableEXT(setup_enables);
    });
}

void RasterizerVulkan::UpdateStencilTestEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchStencilTestEnable()) {
        return;
    }
    scheduler.Record([enable = regs.stencil_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetStencilTestEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdateVertexInput(Tegra::Engines::Maxwell3D::Regs& regs) {
    auto& dirty{maxwell3d->dirty.flags};
    const bool vertex_input_dirty = dirty[Dirty::VertexInput];
    const bool vertex_buffers_dirty = dirty[VideoCommon::Dirty::VertexBuffers];
    if (!vertex_input_dirty && !vertex_buffers_dirty) {
        return;
    }
    dirty[Dirty::VertexInput] = false;

    boost::container::static_vector<VkVertexInputBindingDescription2EXT, 32> bindings;
    boost::container::static_vector<VkVertexInputAttributeDescription2EXT, 32> attributes;

    const u32 max_attributes =
        static_cast<u32>(std::min<size_t>(Maxwell::NumVertexAttributes,
                                          device.GetMaxVertexInputAttributes()));
    const u32 max_bindings =
        static_cast<u32>(std::min<size_t>(Maxwell::NumVertexArrays,
                                          device.GetMaxVertexInputBindings()));


    for (u32 index = 0; index < max_attributes; ++index) {
        const Maxwell::VertexAttribute attribute{regs.vertex_attrib_format[index]};
        const u32 binding{attribute.buffer};
        if (attribute.constant || binding >= max_bindings) {
            continue;
        }
        attributes.push_back({
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
            .pNext = nullptr,
            .location = index,
            .binding = binding,
            .format = MaxwellToVK::VertexFormat(device, attribute.type, attribute.size),
            .offset = attribute.offset,
        });
    }

    for (u32 binding = 0; binding < max_bindings; ++binding) {
        const auto& input_binding{regs.vertex_streams[binding]};
        const bool is_instanced{regs.vertex_stream_instances.IsInstancingEnabled(binding)};
        bindings.push_back({
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT,
            .pNext = nullptr,
            .binding = binding,
            .stride = input_binding.stride,
            .inputRate = is_instanced ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX,
            .divisor = is_instanced ? input_binding.frequency : 1,
        });
    }

    for (size_t index = 0; index < Maxwell::NumVertexAttributes; ++index) {
        dirty[Dirty::VertexAttribute0 + index] = false;
    }
    for (size_t index = 0; index < Maxwell::NumVertexArrays; ++index) {
        dirty[Dirty::VertexBinding0 + index] = false;
    }

    scheduler.Record([bindings, attributes](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetVertexInputEXT(bindings, attributes);
    });
}

void RasterizerVulkan::InitializeChannel(Tegra::Control::ChannelState& channel) {
    CreateChannel(channel);
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        texture_cache.CreateChannel(channel);
        buffer_cache.CreateChannel(channel);
    }
    pipeline_cache.CreateChannel(channel);
    query_cache.CreateChannel(channel);
    state_tracker.SetupTables(channel);
}

void RasterizerVulkan::BindChannel(Tegra::Control::ChannelState& channel) {
    const s32 channel_id = channel.bind_id;
    BindToChannel(channel_id);
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        texture_cache.BindToChannel(channel_id);
        buffer_cache.BindToChannel(channel_id);
    }
    pipeline_cache.BindToChannel(channel_id);
    query_cache.BindToChannel(channel_id);
    state_tracker.ChangeChannel(channel);
    state_tracker.InvalidateState();
}

void RasterizerVulkan::ReleaseChannel(s32 channel_id) {
    EraseChannel(channel_id);
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        texture_cache.EraseChannel(channel_id);
        buffer_cache.EraseChannel(channel_id);
    }
    pipeline_cache.EraseChannel(channel_id);
    query_cache.EraseChannel(channel_id);
}

} // namespace Vulkan
