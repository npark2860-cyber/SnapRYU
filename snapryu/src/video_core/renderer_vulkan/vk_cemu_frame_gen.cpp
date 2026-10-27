// SPDX-License-Identifier: GPL-3.0-or-later
// SnapRYU Scheduler/lifetime adapter for the original Cemu GPU + Anvil contract.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>
#include "video_core/renderer_vulkan/vk_npu_frame_gen.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "common/logging.h"
#include "common/npu_fg_settings.h"
#include "common/thread.h"
#include "core/frontend/framebuffer_layout.h"
#include "video_core/frame_gen/npu/cemu_gpu_pipeline.h"
#include "video_core/frame_gen/npu/packed_anvil.h"
#include "video_core/frame_gen/npu/quality_capture.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {
namespace {
using Clock = std::chrono::steady_clock;
s64 Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
        .count();
}
double Ms(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}
VkImageSubresourceRange Range() {
    return {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
}
VkImageSubresourceLayers Layers() {
    return {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
}
VkFormat EncodedFormat(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB:
        return VK_FORMAT_B8G8R8A8_UNORM;
    default:
        return f;
    }
}
VkImageMemoryBarrier ImageBarrier(VkImage image, VkImageLayout old, VkImageLayout next,
                                  VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.image = image;
    b.oldLayout = old;
    b.newLayout = next;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange = Range();
    return b;
}
vk::Image TransferImage(MemoryAllocator& allocator, VkExtent2D extent, VkFormat format) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {extent.width, extent.height, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    return allocator.CreateImage(ci);
}
// Copy preserves encoded bytes across the compatible UNORM/SRGB formats. Blit is
// only between UNORM resources and therefore performs no hidden gamma conversion.
void Transfer(Scheduler& scheduler, VkImage src, VkExtent2D se, VkImage dst, VkExtent2D de,
              bool initialized, bool copy) {
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([=](vk::CommandBuffer cmd) {
        const std::array before{
            ImageBarrier(src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
            ImageBarrier(dst, initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         initialized ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : 0,
                         VK_ACCESS_TRANSFER_WRITE_BIT)};
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                            {}, {}, before);
        if (copy) {
            VkImageCopy region{Layers(), {}, Layers(), {}, {se.width, se.height, 1}};
            cmd.CopyImage(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region);
        } else {
            VkImageBlit region{Layers(),
                               {{0, 0, 0}, {s32(se.width), s32(se.height), 1}},
                               Layers(),
                               {{0, 0, 0}, {s32(de.width), s32(de.height), 1}}};
            cmd.BlitImage(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region, VK_FILTER_LINEAR);
        }
        const std::array after{
            ImageBarrier(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_ACCESS_TRANSFER_READ_BIT,
                         VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT),
            ImageBarrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)};
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                            {}, {}, after);
    });
}
#if SNAPRYU_DIAGNOSTICS
void Download(Scheduler& scheduler, VkImage image, VkBuffer buffer, VkExtent2D extent) {
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([=](vk::CommandBuffer cmd) {
        auto before =
            ImageBarrier(image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                            before);
        VkBufferImageCopy copy{0, 0, 0, Layers(), {}, {extent.width, extent.height, 1}};
        cmd.CopyImageToBuffer(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, copy);
        auto after = ImageBarrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT,
                                  VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        host.buffer = buffer;
        host.size = VkDeviceSize(extent.width) * extent.height * 4;
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, {},
                            host, after);
    });
}
#endif
std::filesystem::path ExecutableDirectory() {
    std::array<wchar_t, 32768> name{};
    auto n = GetModuleFileNameW(nullptr, name.data(), DWORD(name.size()));
    if (!n || n >= name.size())
        throw std::runtime_error("Cannot locate executable");
    return std::filesystem::path(std::wstring(name.data(), n)).parent_path();
}
} // namespace
struct NpuFrameGenerator::Impl {
    enum class State { Free, Reserved, GpuPending, NpuRunning, ResidualReady, ConsumerPending };
    struct Original {
        vk::Image image;
        u64 id{}, tick{};
        bool initialized{};
    };
    struct Slot {
        std::unique_ptr<SnapRyuCemu::Pipeline> gpu;
        vk::Image expanded;
        vk::QueryPool queries;
        std::atomic<State> state{State::Free};
        u64 epoch{}, previous{}, current{}, prepare_tick{}, consumer_tick{};
        s64 deadline{};
        Clock::time_point start{};
        double prepare_npu_ms{}, gpu_prepare_ms{}, gpu_compose_ms{};
        SnapRyuCemu::AnvilTiming anvil_timing{};
        bool expanded_initialized{}, warmup{};
#if SNAPRYU_DIAGNOSTICS
        bool selected{}, captured{};
        std::array<vk::Buffer, 5> dumps;
#endif
    };
    Impl(const Device& d, MemoryAllocator& a, Scheduler& s, PresentManager& p, Swapchain& sc)
        : device{d}, allocator{a}, scheduler{s}, present{p}, swapchain{sc},
          profile{SnapRyuCemu::Profile::FromHeight(Settings::NpuSession::Height())} {
        // OFF does not load the NPU provider, allocate FG resources, or query GPU completion.
        if (Settings::npu_fg_mode.GetValue() != Settings::NpuFgMode::Off) {
            worker = std::thread([this] { Work(); });
        }
    }
    ~Impl() {
        Shutdown();
    }
    void Shutdown() {
        if (shutdown)
            return;
        shutdown = true;
        stop.store(true, std::memory_order_release);
        wake.notify_all();
        if (worker.joinable())
            worker.join();
        if (allocated) {
            try {
                scheduler.Finish();
#if SNAPRYU_DIAGNOSTICS
                for (auto& slot : slots)
                    if (slot.state.load(std::memory_order_acquire) == State::ConsumerPending &&
                        slot.selected && !slot.captured) {
                        slot.dumps[4].Invalidate();
                        SnapRyuQuality::Capture::Get().Add(
                            slot.epoch, slot.previous, slot.current, "cemu_composed_rgba",
                            slot.dumps[4].Mapped().data(), profile.pixels * 4, profile.width,
                            profile.height, profile.width * 4,
                            "{\"format\":\"RGBA8_UNORM\",\"scanout_verified\":false}");
                        slot.captured = true;
                    }
#endif
            } catch (const std::exception& e) {
                LOG_WARNING(Render_Vulkan, "Cemu NPU teardown: {}", e.what());
            }
        }
    }
    void Reset() {
        epoch.fetch_add(1, std::memory_order_acq_rel);
        source_id = last_shown = 0;
        last_time = 0;
        interval_ns = 0;
        control.Reset();
        for (auto& o : originals)
            o.id = 0;
        // Pending GPU/NPU/consumer work keeps its slot. Worker/root discard only after completion.
        for (auto& slot : slots)
            if (slot.state.load(std::memory_order_acquire) == State::ResidualReady)
                slot.state.store(State::Free, std::memory_order_release);
    }
    bool Ensure(const Layout::FramebufferLayout& layout) {
        const auto next_format = swapchain.GetImageFormat();
        const auto encoded = EncodedFormat(next_format);
        if (encoded != VK_FORMAT_R8G8B8A8_UNORM && encoded != VK_FORMAT_B8G8R8A8_UNORM)
            return false;
        if (allocated && width == layout.width && height == layout.height && format == next_format)
            return true;
        if (allocated) {
            stop.store(true, std::memory_order_release);
            wake.notify_all();
            if (worker.joinable())
                worker.join();
            scheduler.Finish();
            Reset();
            for (auto& slot : slots) {
                slot.gpu.reset();
                slot.state.store(State::Free, std::memory_order_release);
            }
            stop.store(false, std::memory_order_release);
            ready.store(false, std::memory_order_release);
            worker = std::thread([this] { Work(); });
        }
        constexpr VkFormatFeatureFlags rgba_needed =
            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
            VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
            VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        auto rgba = device.GetPhysical().GetFormatProperties(VK_FORMAT_R8G8B8A8_UNORM);
        if ((rgba.optimalTilingFeatures & rgba_needed) != rgba_needed)
            return false;
        auto native = device.GetPhysical().GetFormatProperties(encoded);
        if ((native.optimalTilingFeatures &
             (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) !=
            (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
             VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
            return false;
        width = layout.width;
        height = layout.height;
        format = next_format;
        native_format = encoded;
        auto root = ExecutableDirectory() / L"npu/cemu" / profile.label;
        const auto memory = device.GetPhysical().GetMemoryProperties().memoryProperties;
        const auto props = device.GetPhysical().GetProperties();
        const auto queues = device.GetPhysical().GetQueueFamilyProperties();
        const auto queue = queues.at(device.GetGraphicsFamily());
        if (!(queue.queueFlags & VK_QUEUE_COMPUTE_BIT) || !queue.timestampValidBits ||
            props.limits.maxComputeSharedMemorySize < 31488 ||
            props.limits.maxComputeWorkGroupInvocations < 256 ||
            props.limits.maxStorageBufferRange < profile.input_bytes)
            return false;
        timestamp_mask = queue.timestampValidBits == 64
                             ? UINT64_MAX
                             : ((uint64_t(1) << queue.timestampValidBits) - 1);
        for (auto& o : originals) {
            o.image = TransferImage(allocator, {width, height}, native_format);
            o.id = o.tick = 0;
            o.initialized = false;
        }
        for (auto& slot : slots) {
            slot.gpu = std::make_unique<SnapRyuCemu::Pipeline>(
                SnapRyuCemu::Api{device.GetDispatchLoader()}, *device.GetLogical(), memory, profile,
                root / L"shaders");
            slot.expanded = TransferImage(allocator, {width, height}, native_format);
            slot.expanded_initialized = false;
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 4;
            slot.queries = device.GetLogical().CreateQueryPool(qi);
            slot.state.store(State::Free, std::memory_order_release);
        }
        timestamp_period = device.GetPhysical().GetProperties().limits.timestampPeriod;
        warmups_done = 0;
        warmup_full_ms = 0;
        last_job_ms = 0;
        last_full_job_time = 0;
        allocated = true;
        LOG_INFO(Render_Vulkan,
                 "SNAPRYU_CEMU_GPU profile={}x{} grid={}x{} original_spirv=true "
                 "vulkan_host_import=false slots=2 timeline_delay=1",
                 profile.width, profile.height, profile.grid_width, profile.grid_height);
        return true;
    }
    bool Completed(u64 tick) {
        auto& master = scheduler.GetMasterSemaphore();
        if (!master.IsFree(tick))
            master.Refresh();
        return master.IsFree(tick);
    }
    bool Observe(u64 tick) {
        while (!Completed(tick)) {
            if (stop.load(std::memory_order_acquire))
                return false;
            std::unique_lock lock{mutex};
            wake.wait_for(lock, std::chrono::milliseconds(1), [this] { return stop.load(); });
        }
        return true;
    }
    double GpuDuration(const Slot& slot, unsigned first) const {
        uint64_t ticks[2]{};
        auto result = device.GetDispatchLoader().vkGetQueryPoolResults(
            *device.GetLogical(), *slot.queries, first, 2, sizeof(ticks), ticks, sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT);
        SnapRyuCemu::Check(result, "Completed Cemu timestamp queries");
        return double((ticks[1] - ticks[0]) & timestamp_mask) * timestamp_period / 1e6;
    }
    bool Warmup() {
        if (warmups_done >= 2)
            return true;
        auto& slot = slots[0];
        const auto state = slot.state.load(std::memory_order_acquire);
        if (state == State::ConsumerPending && Completed(slot.consumer_tick)) {
            warmup_full_ms = slot.prepare_npu_ms + GpuDuration(slot, 2);
            ++warmups_done;
            slot.state.store(State::Free, std::memory_order_release);
            if (warmups_done == 2) {
                LOG_INFO(Render_Vulkan,
                         "SNAPRYU_CEMU_WARMUP profile={}x{} "
                         "full_gpu_queue_copy_htp_compose_ms={:.3f} repeats=2",
                         profile.width, profile.height, warmup_full_ms);
                return true;
            }
        }
        if (slot.state.load(std::memory_order_acquire) == State::Free) {
            slot.warmup = true;
            slot.epoch = epoch.load(std::memory_order_acquire);
            slot.previous = slot.current = 0;
            slot.deadline = INT64_MAX;
            slot.start = Clock::now();
            slot.state.store(State::Reserved, std::memory_order_release);
#if SNAPRYU_DIAGNOSTICS
            slot.selected = false;
#endif
            auto* gpu = slot.gpu.get();
            scheduler.RequestOutsideRenderPassOperationContext();
            scheduler.InvalidateState();
            scheduler.Record([this, gpu, &slot](vk::CommandBuffer cmd) {
                gpu->Initialize(*cmd);
                const auto& api = device.GetDispatchLoader();
                api.vkCmdResetQueryPool(*cmd, *slot.queries, 0, 4);
                api.vkCmdWriteTimestamp(*cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, *slot.queries, 0);
                gpu->RecordBarrier(*cmd, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT);
                const VkClearColorValue clear{{0.f, 0.f, 0.f, 1.f}};
                const auto range = Range();
                for (unsigned i = 0; i < 2; ++i)
                    api.vkCmdClearColorImage(*cmd, gpu->images[i].image, VK_IMAGE_LAYOUT_GENERAL,
                                             &clear, 1, &range);
                gpu->Prepare(*cmd);
                api.vkCmdWriteTimestamp(*cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *slot.queries,
                                        1);
            });
            scheduler.InvalidateState();
            slot.prepare_tick = scheduler.Flush();
            slot.state.store(State::GpuPending, std::memory_order_release);
            wake.notify_one();
            scheduler.DispatchWork();
        } else if (state == State::ResidualReady) {
            auto* gpu = slot.gpu.get();
            scheduler.RequestOutsideRenderPassOperationContext();
            scheduler.InvalidateState();
            scheduler.Record([this, gpu, &slot](vk::CommandBuffer cmd) {
                const auto& api = device.GetDispatchLoader();
                api.vkCmdWriteTimestamp(*cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, *slot.queries, 2);
                gpu->Compose(*cmd);
            });
            scheduler.InvalidateState();
            const VkExtent2D extent{width, height}, processing{profile.width, profile.height};
            const bool exact_rgba = native_format == VK_FORMAT_R8G8B8A8_UNORM &&
                                    width == profile.width && height == profile.height;
            Transfer(scheduler, gpu->images[2].image, processing, *slot.expanded, extent,
                     slot.expanded_initialized, exact_rgba);
            slot.expanded_initialized = true;
            Transfer(scheduler, *slot.expanded, extent, *originals[0].image, extent,
                     originals[0].initialized, true);
            originals[0].initialized = true;
            scheduler.Record([this, &slot](vk::CommandBuffer cmd) {
                device.GetDispatchLoader().vkCmdWriteTimestamp(
                    *cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *slot.queries, 3);
            });
            slot.consumer_tick = scheduler.Flush();
            slot.state.store(State::ConsumerPending, std::memory_order_release);
            scheduler.DispatchWork();
        }
        return false;
    }
    void Work() noexcept {
        Common::SetCurrentThreadName("SnapRYUCemuHTP");
        try {
            const auto root = ExecutableDirectory() / L"npu";
            SnapRyuCemu::PackedAnvil anvil{root / L"runtime",
                                           root / L"cemu" / profile.label / L"anvil-u8nhwc.onnx",
                                           profile.height};
            ready.store(true, std::memory_order_release);
            while (!stop.load(std::memory_order_acquire)) {
                bool any = false;
                for (auto& slot : slots) {
                    if (slot.state.load(std::memory_order_acquire) != State::GpuPending)
                        continue;
                    any = true;
                    if (!Observe(slot.prepare_tick))
                        return;
                    if (slot.epoch != epoch.load(std::memory_order_acquire) ||
                        Now() > slot.deadline) {
                        slot.state.store(State::Free, std::memory_order_release);
                        continue;
                    }
                    slot.state.store(State::NpuRunning, std::memory_order_release);
                    auto& gpu = *slot.gpu;
                    gpu.Invalidate(SnapRyuCemu::Pipeline::Packed);
                    slot.gpu_prepare_ms = GpuDuration(slot, 0);
#if SNAPRYU_DIAGNOSTICS
                    if (slot.selected) {
                        auto& q = SnapRyuQuality::Capture::Get();
                        for (unsigned i = 0; i < 4; ++i) {
                            slot.dumps[i].Invalidate();
                            const char* names[]{"cemu_input_a_rgba", "cemu_input_b_rgba",
                                                "cemu_warped_a_rgba", "cemu_warped_b_rgba"};
                            q.Add(slot.epoch, slot.previous, slot.current, names[i],
                                  slot.dumps[i].Mapped().data(), profile.pixels * 4, profile.width,
                                  profile.height, profile.width * 4,
                                  "{\"format\":\"RGBA8_UNORM\"}");
                        }
                        gpu.Invalidate(SnapRyuCemu::Pipeline::Forward);
                        gpu.Invalidate(SnapRyuCemu::Pipeline::Backward);
                        q.Add(slot.epoch, slot.previous, slot.current, "cemu_forward_i32",
                              gpu.buffers[2].mapped, profile.flow_bytes);
                        q.Add(slot.epoch, slot.previous, slot.current, "cemu_backward_i32",
                              gpu.buffers[3].mapped, profile.flow_bytes);
                        q.Add(slot.epoch, slot.previous, slot.current, "cemu_packed_u8",
                              gpu.buffers[0].mapped, profile.input_bytes, profile.width,
                              profile.height, profile.width * 6);
                    }
#endif
                    slot.anvil_timing = anvil.Run({gpu.buffers[0].mapped, profile.input_bytes},
                                                  {gpu.buffers[1].mapped, profile.residual_bytes});
                    gpu.Flush(SnapRyuCemu::Pipeline::Residual);
                    slot.prepare_npu_ms = Ms(slot.start);
#if SNAPRYU_DIAGNOSTICS
                    if (slot.selected)
                        SnapRyuQuality::Capture::Get().Add(
                            slot.epoch, slot.previous, slot.current, "cemu_residual_u8",
                            gpu.buffers[1].mapped, profile.residual_bytes, profile.width,
                            profile.height, profile.width * 3,
                            "{\"provider\":\"QNN_HTP_CPU_FALLBACK_DISABLED\"}");
                    if (slot.selected) {
                        auto& q = SnapRyuQuality::Capture::Get();
                        q.Add(slot.epoch, slot.previous, slot.current, "cemu_qnn_input_u8",
                              anvil.InputTensor().data(), profile.input_bytes);
                        q.Add(slot.epoch, slot.previous, slot.current, "cemu_qnn_residual_u8",
                              anvil.ResidualTensor().data(), profile.residual_bytes);
                    }
#endif
                    if (slot.epoch != epoch.load(std::memory_order_acquire) ||
                        Now() > slot.deadline)
                        slot.state.store(State::Free, std::memory_order_release);
                    else
                        slot.state.store(State::ResidualReady, std::memory_order_release);
                }
                if (!any) {
                    std::unique_lock lock{mutex};
                    wake.wait_for(lock, std::chrono::milliseconds(1),
                                  [this] { return stop.load(); });
                }
            }
        } catch (const std::exception& e) {
            std::scoped_lock lock{mutex};
            error = e.what();
            Settings::NpuSession::Failure(error);
            failed.store(true, std::memory_order_release);
            LOG_WARNING(Render_Vulkan, "Cemu NPU unavailable; original frames: {}", error);
        } catch (...) {
            Settings::NpuSession::Failure("Unknown Cemu NPU failure");
            failed.store(true, std::memory_order_release);
            LOG_WARNING(Render_Vulkan, "Cemu NPU unavailable; unknown failure");
        }
    }
    void Recycle(s64 now, u64 display_id) {
        for (auto& slot : slots) {
            auto state = slot.state.load(std::memory_order_acquire);
            if (state == State::ConsumerPending && Completed(slot.consumer_tick)) {
                slot.gpu_compose_ms = GpuDuration(slot, 2);
                last_job_ms = slot.prepare_npu_ms + slot.gpu_compose_ms;
                last_full_job_time = now;
                last_stage = {slot.gpu_prepare_ms,
                              slot.anvil_timing.input_copy_ms,
                              slot.anvil_timing.htp_ms,
                              slot.anvil_timing.residual_copy_ms,
                              slot.gpu_compose_ms,
                              std::max(0., slot.prepare_npu_ms - slot.gpu_prepare_ms -
                                               slot.anvil_timing.service_ms)};
#if SNAPRYU_DIAGNOSTICS
                if (slot.selected && !slot.captured) {
                    slot.dumps[4].Invalidate();
                    SnapRyuQuality::Capture::Get().Add(
                        slot.epoch, slot.previous, slot.current, "cemu_composed_rgba",
                        slot.dumps[4].Mapped().data(), profile.pixels * 4, profile.width,
                        profile.height, profile.width * 4,
                        "{\"format\":\"RGBA8_UNORM\",\"scanout_verified\":false}");
                    slot.captured = true;
                }
#endif
                ++completed;
                slot.state.store(State::Free, std::memory_order_release);
            } else if (state == State::ResidualReady) {
                last_prepare_ms = slot.prepare_npu_ms;
                if (slot.epoch != epoch.load(std::memory_order_acquire) ||
                    slot.current < display_id || slot.deadline < now) {
                    ++dropped;
                    slot.state.store(State::Free, std::memory_order_release);
                }
            }
        }
    }
    bool Process(Frame* frame, const Layout::FramebufferLayout& layout) {
        if (shutdown || disabled)
            return false;
        if (Settings::npu_fg_mode.GetValue() == Settings::NpuFgMode::Off ||
            Settings::npu_fg_paused.load(std::memory_order_relaxed)) {
            if (source_id)
                Reset();
            return false;
        }
        if (failed.load(std::memory_order_acquire)) {
            disabled = true;
            Reset();
            return false;
        }
        if (!ready.load(std::memory_order_acquire))
            return false;
        try {
            if (!Ensure(layout)) {
                disabled = true;
                Settings::NpuSession::Failure(
                    "Selected processing profile unsupported by GPU/swapchain");
                LOG_WARNING(Render_Vulkan, "Cemu NPU processing profile unavailable for this "
                                           "GPU/swapchain; original frames");
                return false;
            }
        } catch (const std::exception& e) {
            disabled = true;
            Settings::NpuSession::Failure(e.what());
            LOG_WARNING(Render_Vulkan, "Cemu GPU resources unavailable; original frames: {}",
                        e.what());
            return false;
        }
        if (!Warmup())
            return false;
        const s64 now = Now();
        if (last_time && (now <= last_time || now - last_time > 200'000'000))
            Reset();
        if (last_time) {
            double delta = double(now - last_time);
            interval_ns = interval_ns == 0 ? delta : interval_ns + (delta - interval_ns) * .2;
        }
        last_time = now;
        const s64 interval =
            s64(std::clamp(interval_ns > 0 ? interval_ns : 33'333'333., 8'333'333., 100'000'000.));
        auto& original = originals[(source_id + 1) % 4];
        if (original.tick && !Completed(original.tick)) {
            Reset();
            return false;
        }
        const u64 id = ++source_id, display_id = id > 1 ? id - 1 : id,
                  current_epoch = epoch.load(std::memory_order_acquire);
        auto& displayed = originals[display_id % 4];
        Recycle(now, display_id);
        const VkExtent2D extent{width, height}, processing{profile.width, profile.height};
        Transfer(scheduler, *frame->image, extent, *original.image, extent, original.initialized,
                 true);
        original.initialized = true;
        original.id = id;
        Slot* admitted = nullptr;
        for (auto& slot : slots)
            if (slot.state.load(std::memory_order_acquire) == State::Free) {
                admitted = &slot;
                break;
            }
        // Same measured full-job recovery as the preserved legacy path: a transient
        // expensive job expires after1s, falling back to the actual warmed full path.
        double cost = last_full_job_time && now - last_full_job_time < 1'000'000'000
                          ? last_job_ms
                          : warmup_full_ms;
        const auto plan =
            control.Plan(Settings::npu_fg_mode.GetValue(), Settings::npu_fg_target_fps.GetValue(),
                         now, admitted != nullptr, cost);
        // Re-measure a real pair at most once per second if loading-time queue
        // contention made the warmed cost too high. Preserve deadlines/timeline.
        const bool wants_generation =
            Settings::npu_fg_mode.GetValue() == Settings::NpuFgMode::Fixed2x ||
            (plan.base_fps > 0 && Settings::npu_fg_target_fps.GetValue() > plan.base_fps);
        const bool recovery_probe = admitted && !plan.generate && plan.base_fps > 0 &&
                                    wants_generation && cost * 1.1 > interval / 1e6 &&
                                    now - last_probe_time >= 1'000'000'000;
        if (admitted && (plan.generate || recovery_probe) && id > 1 &&
            originals[(id - 1) % 4].id == id - 1) {
            if (recovery_probe) {
                last_probe_time = now;
                ++recovery_probes;
            }
            auto& slot = *admitted;
            slot.state.store(State::Reserved, std::memory_order_release);
            slot.warmup = false;
            slot.epoch = current_epoch;
            slot.previous = id - 1;
            slot.current = id;
            slot.deadline = now + interval * 2;
            slot.start = Clock::now();
            auto* gpu = slot.gpu.get();
            scheduler.RequestOutsideRenderPassOperationContext();
            scheduler.InvalidateState();
            scheduler.Record([gpu](vk::CommandBuffer cmd) { gpu->Initialize(*cmd); });
            const bool exact_rgba = native_format == VK_FORMAT_R8G8B8A8_UNORM &&
                                    width == profile.width && height == profile.height;
            scheduler.Record([this, &slot](vk::CommandBuffer cmd) {
                const auto& api = device.GetDispatchLoader();
                api.vkCmdResetQueryPool(*cmd, *slot.queries, 0, 4);
                api.vkCmdWriteTimestamp(*cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, *slot.queries, 0);
            });
            Transfer(scheduler, *originals[(id - 1) % 4].image, extent, gpu->images[0].image,
                     processing, true, exact_rgba);
            Transfer(scheduler, *original.image, extent, gpu->images[1].image, processing, true,
                     exact_rgba);
            scheduler.Record([this, gpu, &slot](vk::CommandBuffer cmd) {
                gpu->Prepare(*cmd);
                device.GetDispatchLoader().vkCmdWriteTimestamp(
                    *cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *slot.queries, 1);
            });
            scheduler.InvalidateState();
#if SNAPRYU_DIAGNOSTICS
            slot.selected = false;
            slot.captured = false;
            auto& q = SnapRyuQuality::Capture::Get();
            const auto details =
                "{\"pipeline\":\"Cemu_original_GPU_Anvil_GPU\",\"processing_width\":" +
                std::to_string(profile.width) +
                ",\"processing_height\":" + std::to_string(profile.height) +
                ",\"layout_width\":" + std::to_string(width) +
                ",\"layout_height\":" + std::to_string(height) +
                ",\"vk_format\":" + std::to_string(format) + ",\"scanout_verified\":false}";
            const size_t capture_raw = profile.pixels * 38 + profile.flow_bytes * 2;
            const size_t capture_gpu = profile.pixels * 20;
            if (q.Select(current_epoch, id - 1, id, width, height, details, capture_raw,
                         capture_gpu)) {
                try {
                    slot.selected = true;
                    for (auto& dump : slot.dumps)
                        dump = CreateWrappedBuffer(allocator, profile.pixels * 4,
                                                   MemoryUsage::Download);
                    const unsigned indexes[]{0, 1, 5, 6};
                    for (unsigned i = 0; i < 4; ++i)
                        Download(scheduler, gpu->images[indexes[i]].image, *slot.dumps[i],
                                 processing);
                } catch (const std::exception& e) {
                    slot.selected = false;
                    q.Event(current_epoch, id - 1, id, "capture_allocation_failure", e.what());
                }
            }
#endif
            ++accepted;
        } else
            admitted = nullptr;
        Frame* generated = nullptr;
        Slot* used = nullptr;
        if (id > 1 && display_id > last_shown) {
            for (auto& slot : slots)
                if (slot.state.load(std::memory_order_acquire) == State::ResidualReady &&
                    slot.epoch == current_epoch && slot.current == display_id &&
                    slot.deadline >= now) {
                    used = &slot;
                    break;
                }
        }
        if (used) {
            generated = present.TryGetRenderFrame(width, height);
            if (generated) {
                auto* gpu = used->gpu.get();
                scheduler.RequestOutsideRenderPassOperationContext();
                scheduler.InvalidateState();
                scheduler.Record([this, gpu, used](vk::CommandBuffer cmd) {
                    const auto& api = device.GetDispatchLoader();
                    api.vkCmdWriteTimestamp(*cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, *used->queries,
                                            2);
                    gpu->Compose(*cmd);
                });
                scheduler.InvalidateState();
#if SNAPRYU_DIAGNOSTICS
                if (used->selected)
                    Download(scheduler, gpu->images[2].image, *used->dumps[4], processing);
#endif
                const bool exact_rgba = native_format == VK_FORMAT_R8G8B8A8_UNORM &&
                                        width == profile.width && height == profile.height;
                Transfer(scheduler, gpu->images[2].image, processing, *used->expanded, extent,
                         used->expanded_initialized, exact_rgba);
                used->expanded_initialized = true;
                Transfer(scheduler, *used->expanded, extent, *generated->image, extent, false,
                         true);
                scheduler.Record([this, used](vk::CommandBuffer cmd) {
                    device.GetDispatchLoader().vkCmdWriteTimestamp(
                        *cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *used->queries, 3);
                });
                used->consumer_tick = scheduler.Flush(*generated->render_ready);
                used->state.store(State::ConsumerPending, std::memory_order_release);
#if SNAPRYU_DIAGNOSTICS
                if (used->selected) {
                    SnapRyuQuality::Capture::Get().Tag(generated, used->epoch, used->previous,
                                                       used->current);
                    SnapRyuQuality::Capture::Get().Add(used->epoch, used->previous, used->current,
                                                       "cemu_present_enqueued", nullptr, 0, 0, 0, 0,
                                                       "{\"scanout_verified\":false}");
                }
#endif
                // The private slot is retained through compose and copying into the pooled frame.
                // The pooled Frame owns its separate swapchain/present_done lifetime afterwards.
                present.Present(generated, now, true);
                ++presented;
            }
        }
        const bool already = display_id <= last_shown;
        if (!already && id > 1 && displayed.id == display_id)
            Transfer(scheduler, *displayed.image, extent, *frame->image, extent, true, true);
        const u64 tick = scheduler.Flush(*frame->render_ready);
        original.tick = tick;
        displayed.tick = tick;
        if (admitted) {
            admitted->prepare_tick = tick;
            originals[(id - 1) % 4].tick = tick;
            admitted->state.store(State::GpuPending, std::memory_order_release);
            wake.notify_one();
        }
        if (already)
            present.DiscardRenderedFrame(frame);
        else
            present.Present(frame, generated ? now + interval / 2 : now, false);
        last_shown = std::max(last_shown, display_id);
        scheduler.DispatchWork();
#if SNAPRYU_DIAGNOSTICS
        if (!report_time)
            report_time = now;
        if (now - report_time >= 5'000'000'000) {
            unsigned occupied{};
            for (auto& slot : slots)
                occupied += slot.state.load(std::memory_order_acquire) != State::Free;
            LOG_INFO(
                Render_Vulkan,
                "SNAPRYU_CEMU_NPU window_s={:.3f} profile={}x{} epoch={} accepted={} "
                "generated_requests={} completed={} dropped={} occupied={}/2 "
                "full_service_ms={:.3f} using_warmup_cost={} timeline_delay=1 "
                "scanout_verified=false "
                "gpu_prepare_ms={:.3f} input_copy_ms={:.3f} htp_ms={:.3f} residual_copy_ms={:.3f} "
                "gpu_compose_copy_ms={:.3f} gpu_queue_transport_ms={:.3f}",
                double(now - report_time) / 1e9, profile.width, profile.height, current_epoch,
                accepted - reported_accepted, presented - reported_presented,
                completed - reported_completed, dropped - reported_dropped, occupied, cost,
                last_job_ms == 0.0, last_stage[0], last_stage[1], last_stage[2], last_stage[3],
                last_stage[4], last_stage[5]);
            report_time = now;
            reported_accepted = accepted;
            reported_presented = presented;
            reported_completed = completed;
            reported_dropped = dropped;
        }
#endif
        return true;
    }
    const Device& device;
    MemoryAllocator& allocator;
    Scheduler& scheduler;
    PresentManager& present;
    Swapchain& swapchain;
    const SnapRyuCemu::Profile profile;
    std::array<Original, 4> originals;
    std::array<Slot, 2> slots;
    std::atomic<u64> epoch{1};
    std::atomic<bool> stop{}, ready{}, failed{};
    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    std::string error;
    VideoCore::NpuFgControl control;
    VkFormat format{}, native_format{};
    u32 width{}, height{}, warmups_done{};
    u64 source_id{}, last_shown{}, accepted{}, presented{}, completed{}, dropped{},
        timestamp_mask{UINT64_MAX};
    s64 last_time{}, last_full_job_time{}, last_probe_time{};
    u64 recovery_probes{};
    double interval_ns{}, timestamp_period{}, last_job_ms{}, last_prepare_ms{}, warmup_full_ms{};
    std::array<double, 6> last_stage{};
    bool allocated{}, disabled{}, shutdown{};
#if SNAPRYU_DIAGNOSTICS
    s64 report_time{};
    u64 reported_accepted{}, reported_presented{}, reported_completed{}, reported_dropped{};
#endif
};
NpuFrameGenerator::NpuFrameGenerator(const Device& d, MemoryAllocator& a, Scheduler& s,
                                     PresentManager& p, Swapchain& sc)
    : impl{std::make_unique<Impl>(d, a, s, p, sc)} {}
NpuFrameGenerator::~NpuFrameGenerator() = default;
bool NpuFrameGenerator::Process(Frame* frame, const Layout::FramebufferLayout& layout) {
    try {
        return impl->Process(frame, layout);
    } catch (const std::exception& e) {
        impl->disabled = true;
        Settings::NpuSession::Failure(e.what());
        impl->Reset();
        LOG_WARNING(Render_Vulkan, "Cemu NPU adapter failed; original frames: {}", e.what());
        return false;
    }
}
void NpuFrameGenerator::Reset() {
    impl->Reset();
}
void NpuFrameGenerator::Shutdown() {
    impl->Shutdown();
}
} // namespace Vulkan
