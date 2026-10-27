// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_vulkan/vk_npu_frame_gen.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "common/logging.h"
#include "common/npu_fg_settings.h"
#include "common/thread.h"
#include "core/frontend/framebuffer_layout.h"
#include "video_core/frame_gen/npu/snapryu_npu.h"
#include "video_core/frame_gen/npu/quality_capture.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {
namespace {
constexpr VkExtent2D ProcessingExtent{1280, 720};
constexpr size_t ProcessingBytes = size_t{1280} * 720 * 4;
constexpr size_t HistoryCount = 4;
constexpr size_t TransferCount = 3;
using Clock = std::chrono::steady_clock;

VkImageSubresourceRange ColorRange() { return {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}; }

VkImageSubresourceLayers ColorLayers() { return {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; }

VkImageMemoryBarrier ImageBarrier(VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
                                  VkAccessFlags src, VkAccessFlags dst) {
    return {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = src,
            .dstAccessMask = dst,
            .oldLayout = old_layout,
            .newLayout = new_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = ColorRange()};
}

vk::Image TransferImage(MemoryAllocator& allocator, VkExtent2D extent, VkFormat format) {
    return allocator.CreateImage(
        {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .pNext = nullptr,
         .flags = 0,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = format,
         .extent = {extent.width, extent.height, 1},
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
         .queueFamilyIndexCount = 0,
         .pQueueFamilyIndices = nullptr,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED});
}

void RecordImageTransfer(Scheduler& scheduler, VkImage source, VkExtent2D source_extent,
                         VkImage destination, VkExtent2D destination_extent, bool initialized) {
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([=](vk::CommandBuffer cmd) {
        const std::array before{
            ImageBarrier(source, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
            ImageBarrier(destination,
                         initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         initialized ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : 0,
                         VK_ACCESS_TRANSFER_WRITE_BIT)};
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                            {}, {}, before);
        if (source_extent.width == destination_extent.width &&
            source_extent.height == destination_extent.height) {
            const VkImageCopy copy{ColorLayers(),
                                   {},
                                   ColorLayers(),
                                   {},
                                   {source_extent.width, source_extent.height, 1}};
            cmd.CopyImage(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copy);
        } else {
            const VkImageBlit blit{
                ColorLayers(),
                {{0, 0, 0}, {s32(source_extent.width), s32(source_extent.height), 1}},
                ColorLayers(),
                {{0, 0, 0}, {s32(destination_extent.width), s32(destination_extent.height), 1}}};
            cmd.BlitImage(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, blit, VK_FILTER_LINEAR);
        }
        const std::array after{
            ImageBarrier(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_ACCESS_TRANSFER_READ_BIT,
                         VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT),
            ImageBarrier(destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)};
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                            {}, {}, after);
    });
}

void RecordDownload(Scheduler& scheduler, VkImage image, VkBuffer buffer,
                    VkExtent2D extent = ProcessingExtent) {
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([=](vk::CommandBuffer cmd) {
        const auto before =
            ImageBarrier(image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                            before);
        const VkBufferImageCopy copy{0, 0, 0, ColorLayers(), {}, {extent.width, extent.height, 1}};
        cmd.CopyImageToBuffer(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, copy);
        const auto after =
            ImageBarrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_WRITE_BIT);
        const VkBufferMemoryBarrier host{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                         .pNext = nullptr,
                                         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                         .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
                                         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .buffer = buffer,
                                         .offset = 0,
                                         .size = VkDeviceSize{extent.width} * extent.height * 4};
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, {},
                            host, after);
    });
}

void RecordUpload(Scheduler& scheduler, VkBuffer buffer, VkImage image, bool initialized) {
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([=](vk::CommandBuffer cmd) {
        const VkBufferMemoryBarrier host{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                         .pNext = nullptr,
                                         .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
                                         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                                         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .buffer = buffer,
                                         .offset = 0,
                                         .size = ProcessingBytes};
        const auto before =
            ImageBarrier(image, initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         initialized ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : 0,
                         VK_ACCESS_TRANSFER_WRITE_BIT);
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, host, before);
        const VkBufferImageCopy copy{0, 0, 0, ColorLayers(), {}, {1280, 720, 1}};
        cmd.CopyBufferToImage(buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copy);
        const auto after =
            ImageBarrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                            after);
    });
}

std::filesystem::path ExecutableDirectory() {
    std::array<wchar_t, 32768> path{};
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) {
        throw std::runtime_error("Cannot locate SnapRYU executable");
    }
    return std::filesystem::path{std::wstring{path.data(), length}}.parent_path();
}
} // namespace

struct NpuFrameGenerator::Impl {
#if SNAPRYU_DIAGNOSTICS
    struct QualityGpu {
        vk::Buffer a,b,uploaded,generated;
        u64 epoch{},previous{},current{};
        u32 width{},height{},format{};
    };
    struct QualityTask { QualityGpu* data{}; u64 tick{}; };
#endif
    struct Original {
        vk::Image image;
        u64 id{}, last_tick{};
        bool initialized{};
    };
    struct Capture {
        vk::Image image;
        vk::Buffer download;
        std::atomic<bool> occupied{};
        bool initialized{};
    };
    struct Upload {
        vk::Image image;
        vk::Buffer buffer;
        u64 last_tick{};
        bool initialized{};
    };
    struct CaptureTask {
        size_t slot{};
        u64 epoch{}, id{}, tick{};
        s64 deadline{};
        bool admitted{};
        u32 pixel_format{};
#if SNAPRYU_DIAGNOSTICS
        QualityGpu* quality{};
#endif
    };
    struct Output {
        SnapRyuNpuOutput metadata{};
        std::vector<u8> rgba;
    };

    Impl(const Device& device_, MemoryAllocator& allocator_, Scheduler& scheduler_,
         PresentManager& present_, Swapchain& swapchain_)
        : device{device_}, allocator{allocator_}, scheduler{scheduler_}, present{present_},
          swapchain{swapchain_} {
        const auto root = ExecutableDirectory() / L"npu";
        const auto runtime = (root / L"runtime").wstring();
        const auto motion = (root / L"models/eden-motion-720p.onnx").wstring();
        const auto anvil = (root / L"models/anvil-u8nhwc.onnx").wstring();
        const SnapRyuNpuConfig config{
            sizeof(config), SNAPRYU_NPU_ABI, runtime.c_str(), motion.c_str(), anvil.c_str(), 2, 0};
        if (SnapRyuNpuCreate(&config, &engine) != SNAPRYU_NPU_OK) {
            disabled = true;
            LOG_WARNING(Render_Vulkan, "NPU frame generation unavailable: engine creation failed");
        }
    }

    ~Impl() { Shutdown(); }

    void StartTransport() {
        transport_stop.store(false, std::memory_order_release);
        transport = std::thread{[this] { Transport(); }};
    }

    void StopTransport() {
        transport_stop.store(true, std::memory_order_release);
        wake.notify_all();
        if (transport.joinable()) {
            transport.join();
        }
        std::scoped_lock lock{capture_mutex};
        pending.clear();
#if SNAPRYU_DIAGNOSTICS
        quality_pending.clear();
#endif
        for (auto& capture : captures) {
            capture.occupied.store(false, std::memory_order_release);
        }
    }

    void Shutdown() {
        if (shutdown) {
            return;
        }
        shutdown = true;
        StopTransport();
        if (engine) {
            SnapRyuNpuDestroy(engine);
            engine = nullptr;
        }
        // Only the renderer producer calls this; no capture worker mutates Scheduler.
        // Private transfer images/buffers remain alive through their last submitted use.
        if (allocated) {
            try {
                scheduler.Finish();
            } catch (const std::exception& error) {
                LOG_WARNING(Render_Vulkan, "NPU transfer teardown: {}", error.what());
            }
        }
    }

    void ResetInferenceHistory() {
        if (!engine) {
            return;
        }
        epoch.store(SnapRyuNpuReset(engine), std::memory_order_release);
        control.Reset();
        outputs.clear();
    }

    void Reset() {
        ResetInferenceHistory();
        source_id = 0;
        last_time = 0;
        interval_ns = 0;
        last_shown = 0;
        // GPU/capture users still own their slots. Reset never frees them early.
        for (auto& original : originals) {
            original.id = 0;
        }
    }

    bool EnsureResources(const Layout::FramebufferLayout& layout) {
        const auto next_format = swapchain.GetImageFormat();
        if (next_format != VK_FORMAT_R8G8B8A8_UNORM && next_format != VK_FORMAT_R8G8B8A8_SRGB &&
            next_format != VK_FORMAT_B8G8R8A8_UNORM && next_format != VK_FORMAT_B8G8R8A8_SRGB) {
            return false;
        }
        if (allocated && width == layout.width && height == layout.height &&
            format == next_format) {
            return true;
        }
        const auto props = device.GetPhysical().GetFormatProperties(next_format);
        constexpr VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                                VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                                VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((props.optimalTilingFeatures & needed) != needed) {
            return false;
        }
        if (allocated) {
            StopTransport();
            scheduler.Finish();
        }
        Reset();
        width = layout.width;
        height = layout.height;
        format = next_format;
        for (auto& original : originals) {
            original.image = TransferImage(allocator, {width, height}, format);
            original.id = original.last_tick = 0;
            original.initialized = false;
        }
        for (auto& capture : captures) {
            capture.image = TransferImage(allocator, ProcessingExtent, format);
            capture.download =
                CreateWrappedBuffer(allocator, ProcessingBytes, MemoryUsage::Download);
            capture.occupied.store(false, std::memory_order_relaxed);
            capture.initialized = false;
        }
        for (auto& upload : uploads) {
            upload.image = TransferImage(allocator, ProcessingExtent, format);
            upload.buffer = CreateWrappedBuffer(allocator, ProcessingBytes, MemoryUsage::Upload);
            upload.last_tick = 0;
            upload.initialized = false;
        }
        allocated = true;
        StartTransport();
        return true;
    }

    void Transport() noexcept {
        Common::SetCurrentThreadName("SnapRYUCapture");
        std::vector<u8> previous, current(ProcessingBytes);
        u64 previous_id = 0, previous_epoch = 0;
        try {
            for (;;) {
                CaptureTask task;
#if SNAPRYU_DIAGNOSTICS
                QualityTask quality_task;
#endif
                {
                    std::unique_lock lock{capture_mutex};
                    wake.wait(lock, [this] { return transport_stop.load() || !pending.empty()
#if SNAPRYU_DIAGNOSTICS
                        || !quality_pending.empty()
#endif
                        ; });
                    if (transport_stop.load()) {
                        return;
                    }
#if SNAPRYU_DIAGNOSTICS
                    if (!quality_pending.empty()) {
                        quality_task=quality_pending.front();quality_pending.pop_front();
                    } else
#endif
                    { task = pending.front();pending.pop_front(); }
                }
#if SNAPRYU_DIAGNOSTICS
                if (quality_task.data) {
                    auto& completion=scheduler.GetMasterSemaphore();
                    while (!completion.IsFree(quality_task.tick)) {
                        if (transport_stop.load()) return;
                        completion.Refresh();
                        std::unique_lock lock{capture_mutex};
                        wake.wait_for(lock,std::chrono::milliseconds{1},[this]{return transport_stop.load();});
                    }
                    auto& q=*quality_task.data;
                    q.uploaded.Invalidate();q.generated.Invalidate();
                    auto& quality=SnapRyuQuality::Capture::Get();
                    const auto format_json="{\"vk_format\":"+std::to_string(q.format)+
                        ",\"completed_gpu_tick\":"+std::to_string(quality_task.tick)+"}";
                    quality.Add(q.epoch,q.previous,q.current,"gpu_upload_720",q.uploaded.Mapped().data(),
                        ProcessingBytes,1280,720,1280*4,format_json);
                    quality.Add(q.epoch,q.previous,q.current,"gpu_generated_display",q.generated.Mapped().data(),
                        size_t{q.width}*q.height*4,q.width,q.height,q.width*4,format_json);
                    continue;
                }
#endif
                auto& capture = captures[task.slot];
                // Completion queries are read-only. In particular this must not call
                // Scheduler::Wait, which may Flush and update producer pacing state.
                auto& completion = scheduler.GetMasterSemaphore();
                while (!completion.IsFree(task.tick)) {
                    if (transport_stop.load(std::memory_order_acquire)) {
                        return;
                    }
                    completion.Refresh();
                    if (completion.IsFree(task.tick)) {
                        break;
                    }
                    std::unique_lock lock{capture_mutex};
                    wake.wait_for(lock, std::chrono::milliseconds{1},
                                  [this] { return transport_stop.load(); });
                }
#if SNAPRYU_DIAGNOSTICS
                if (task.quality) {
                    auto& q=*task.quality;
                    q.a.Invalidate();q.b.Invalidate();
                    auto& quality=SnapRyuQuality::Capture::Get();
                    const auto extra="{\"vk_format\":"+std::to_string(q.format)+
                        ",\"completed_gpu_tick\":"+std::to_string(task.tick)+"}";
                    quality.Add(q.epoch,q.previous,q.current,"original_a_display",q.a.Mapped().data(),
                        size_t{q.width}*q.height*4,q.width,q.height,q.width*4,extra);
                    quality.Add(q.epoch,q.previous,q.current,"original_b_display",q.b.Mapped().data(),
                        size_t{q.width}*q.height*4,q.width,q.height,q.width*4,extra);
                }
#endif
                if (task.epoch != epoch.load(std::memory_order_acquire)) {
#if SNAPRYU_DIAGNOSTICS
                    if (task.quality) {
                        auto& q=*task.quality;
                        SnapRyuQuality::Capture::Get().Add(q.epoch,q.previous,q.current,"capture_reset",
                            nullptr,0,0,0,0,"{\"reason\":\"epoch changed after GPU completion\"}");
                    }
#endif
                    capture.occupied.store(false, std::memory_order_release);
                    previous_id = 0;
                    continue;
                }
                capture.download.Invalidate();
                std::memcpy(current.data(), capture.download.Mapped().data(), ProcessingBytes);
                capture.occupied.store(false, std::memory_order_release);
                if (task.admitted && previous_id + 1 == task.id && previous_id &&
                    previous_epoch == task.epoch) {
                    const SnapRyuNpuImage a{previous.data(), previous.size(),  1280, 720,
                                            1280 * 4,        task.pixel_format};
                    const SnapRyuNpuImage b{current.data(), current.size(),   1280, 720,
                                            1280 * 4,       task.pixel_format};
                    const SnapRyuNpuPair pair{sizeof(pair),  0, task.epoch, previous_id, task.id,
                                              task.deadline, a, b};
                    const auto status=SnapRyuNpuSubmit(engine, &pair);
#if SNAPRYU_DIAGNOSTICS
                    SnapRyuQuality::Capture::Get().Add(task.epoch,previous_id,task.id,"submit",
                        nullptr,0,0,0,0,"{\"status\":"+std::to_string(status)+
                        ",\"gpu_tick\":"+std::to_string(task.tick)+
                        ",\"deadline_ns\":"+std::to_string(task.deadline)+"}");
#else
                    (void)status;
#endif
                }
                previous.swap(current);
                if (current.size() != ProcessingBytes) {
                    current.resize(ProcessingBytes);
                }
                previous_id = task.id;
                previous_epoch = task.epoch;
            }
        } catch (const std::exception& error) {
            LOG_WARNING(Render_Vulkan, "NPU capture stopped: {}", error.what());
            transport_failed.store(true, std::memory_order_release);
        } catch (...) {
            LOG_WARNING(Render_Vulkan, "NPU capture stopped after an unknown failure");
            transport_failed.store(true, std::memory_order_release);
        }
    }

    void TakeOutputs() {
        for (size_t i = 0; i < 2; ++i) {
            // Two queued results plus one query slot. Only the renderer owns
            // these CPU buffers; uploads copy into separately fenced storage.
            size_t slot = 0;
            while (std::find(outputs.begin(), outputs.end(), slot) != outputs.end()) {
                ++slot;
            }
            auto& output = output_storage[slot];
            output.metadata = {};
            output.metadata.struct_size = sizeof(output.metadata);
            output.rgba.resize(ProcessingBytes);
            const auto result = SnapRyuNpuTake(engine, &output.metadata, output.rgba.data(),
                                               output.rgba.size(), 1280 * 4);
            if (result != SNAPRYU_NPU_OK) {
                break;
            }
#if SNAPRYU_DIAGNOSTICS
            SnapRyuQuality::Capture::Get().Add(output.metadata.epoch,output.metadata.previous_id,
                output.metadata.current_id,"take",nullptr,0);
#endif
            if (output.metadata.epoch == epoch.load(std::memory_order_acquire) &&
                output.metadata.current_id > last_shown) {
                if (output.metadata.anvil_ms > 0.0) {
                    last_full_job_ms = output.metadata.job_ms;
                    last_full_job_time = SnapRyuNpuNowNs();
                }
                if (output.metadata.scene_cut) {
#if SNAPRYU_DIAGNOSTICS
                    SnapRyuQuality::Capture::Get().Event(output.metadata.epoch,output.metadata.previous_id,
                        output.metadata.current_id,"renderer_drop","scene_cut");
#endif
                    // Do not lose or repeat the retained original B. Only the
                    // interpolation pair history is invalid across a scene cut.
                    ResetInferenceHistory();
                    return;
                }
                if (outputs.size() == 2) {
                    outputs.pop_front();
                }
                outputs.push_back(slot);
            }
#if SNAPRYU_DIAGNOSTICS
            else SnapRyuQuality::Capture::Get().Event(output.metadata.epoch,output.metadata.previous_id,
                output.metadata.current_id,"take_drop","old_epoch_or_already_shown");
#endif
        }
    }

    double AdmissionCost(s64 now) const {
        // After a transient expensive full job, recover to an actual warmed
        // full-job observation. Never substitute a cheap shortcut or zero.
        return last_full_job_time && now - last_full_job_time < 1'000'000'000
                   ? last_full_job_ms
                   : warmup_full_job_ms;
    }

#if SNAPRYU_DIAGNOSTICS
    void ReportProgress(const SnapRyuNpuStats& stats) {
        const s64 now = SnapRyuNpuNowNs();
        if (!perf_time) {
            perf_time = now;
            perf_stats = stats;
            return;
        }
        if (now - perf_time < 5'000'000'000) return;
        LOG_INFO(Render_Vulkan, "SNAPRYU_NPU window_s={:.3f} state={} epoch={} "
            "accepted={} completed={} taken={} busy={} expired_or_reset={} occupied={}/{} "
            "last_job_ms={:.3f} admission_full_job_ms={:.3f} timeline_drops={} upload_unavailable={} frame_unavailable={}",
            double(now - perf_time) / 1e9, stats.state, stats.epoch,
            stats.accepted - perf_stats.accepted, stats.completed - perf_stats.completed,
            stats.delivered - perf_stats.delivered, stats.busy - perf_stats.busy,
            stats.expired_or_reset - perf_stats.expired_or_reset, stats.occupied, stats.capacity,
            stats.last_job_ms, AdmissionCost(now), perf_timeline_drops, perf_upload_unavailable, perf_frame_unavailable);
        perf_time = now;
        perf_stats = stats;
        perf_timeline_drops = perf_upload_unavailable = perf_frame_unavailable = 0;
    }
#endif

    bool Process(Frame* frame, const Layout::FramebufferLayout& layout) {
        if (shutdown || disabled || !engine) {
            return false;
        }
        if (Settings::npu_fg_mode.GetValue() == Settings::NpuFgMode::Off) {
            if (source_id) {
                Reset();
            }
            return false;
        }
        SnapRyuNpuStats stats{};
        stats.struct_size = sizeof(stats);
        const auto stats_result = SnapRyuNpuGetStats(engine, &stats);
        if (stats_result == SNAPRYU_NPU_OK) {
            cached_stats = stats;
        } else if (cached_stats.state == SNAPRYU_NPU_READY) {
            stats = cached_stats;
        } else {
            return false;
        }
#if SNAPRYU_DIAGNOSTICS
        ReportProgress(stats);
#endif
        if (stats.state == SNAPRYU_NPU_FAILED || transport_failed.load(std::memory_order_acquire)) {
            disabled = true;
            LOG_WARNING(Render_Vulkan, "NPU unavailable; using original frames: {}",
                        stats.last_error);
            Reset();
            return false;
        }
        if (stats.state != SNAPRYU_NPU_READY) {
            return false;
        }
        if (warmup_full_job_ms == 0.0 && stats.completed == 0) {
            warmup_full_job_ms = stats.last_job_ms;
        }
        if (!EnsureResources(layout)) {
            disabled = true;
            LOG_WARNING(Render_Vulkan, "NPU unavailable for this swapchain image format");
            return false;
        }
        TakeOutputs();
        const s64 now = SnapRyuNpuNowNs();
        if (last_time && (now <= last_time || now - last_time > 200'000'000)) {
            Reset();
        }
        if (last_time) {
            const double delta = double(now - last_time);
            interval_ns = interval_ns == 0 ? delta : interval_ns + (delta - interval_ns) * 0.2;
        }
        last_time = now;
        const s64 interval = s64(
            std::clamp(interval_ns > 0 ? interval_ns : 33'333'333.0, 8'333'333.0, 100'000'000.0));
        scheduler.GetMasterSemaphore().Refresh();
        auto& original = originals[(source_id + 1) % HistoryCount];
        if (original.last_tick && !scheduler.IsFree(original.last_tick)) {
            Reset();
            return false;
        }
        const auto id = ++source_id;
        const auto display_id = id > 1 ? id - 1 : id;
        auto& displayed = originals[display_id % HistoryCount];
        const VkExtent2D extent{width, height};
        RecordImageTransfer(scheduler, *frame->image, extent, *original.image, extent,
                            original.initialized);
        original.initialized = true;
        original.id = id;

        Capture* capture = nullptr;
        size_t capture_index = 0;
        for (size_t i = 0; i < captures.size(); ++i) {
            bool expected = false;
            if (captures[i].occupied.compare_exchange_strong(expected, true,
                                                             std::memory_order_acq_rel)) {
                capture = &captures[i];
                capture_index = i;
                break;
            }
        }
        const auto plan =
            control.Plan(Settings::npu_fg_mode.GetValue(), Settings::npu_fg_target_fps.GetValue(),
                         now, capture && stats.occupied < stats.capacity,
                         AdmissionCost(now));
#if SNAPRYU_DIAGNOSTICS
        QualityGpu* selected_quality=nullptr;
        const auto current_epoch=epoch.load(std::memory_order_acquire);
        auto& quality=SnapRyuQuality::Capture::Get();
        if (capture && plan.generate && id>1 && originals[(id-1)%HistoryCount].id==id-1) {
            try {
            const auto size=swapchain.GetSize();
            const auto details="{\"effective_mode\":"+std::to_string(static_cast<u32>(Settings::npu_fg_mode.GetValue()))+
                ",\"cpu_backend\":"+std::to_string(static_cast<u32>(Settings::values.cpu_backend.GetValue()))+
                ",\"layout_width\":"+std::to_string(width)+",\"layout_height\":"+std::to_string(height)+
                ",\"screen_left\":"+std::to_string(layout.screen.left)+",\"screen_top\":"+std::to_string(layout.screen.top)+
                ",\"screen_right\":"+std::to_string(layout.screen.right)+",\"screen_bottom\":"+std::to_string(layout.screen.bottom)+
                ",\"vk_format\":"+std::to_string(static_cast<u32>(format))+
                ",\"swapchain_width\":"+std::to_string(size.width)+",\"swapchain_height\":"+std::to_string(size.height)+
                ",\"processing_width\":1280,\"processing_height\":720,\"resize_filter\":\"VK_FILTER_LINEAR\"}";
            if (quality.Select(current_epoch,id-1,id,width,height,details)) {
                try {
                    auto q=std::make_unique<QualityGpu>();
                    q->epoch=current_epoch;q->previous=id-1;q->current=id;
                    q->width=width;q->height=height;q->format=static_cast<u32>(format);
                    const size_t full=size_t{width}*height*4;
                    q->a=CreateWrappedBuffer(allocator,full,MemoryUsage::Download);
                    q->b=CreateWrappedBuffer(allocator,full,MemoryUsage::Download);
                    q->generated=CreateWrappedBuffer(allocator,full,MemoryUsage::Download);
                    q->uploaded=CreateWrappedBuffer(allocator,ProcessingBytes,MemoryUsage::Download);
                    selected_quality=q.get();quality_resources.push_back(std::move(q));
                    RecordDownload(scheduler,*originals[(id-1)%HistoryCount].image,*selected_quality->a,extent);
                    RecordDownload(scheduler,*original.image,*selected_quality->b,extent);
                } catch (...) {
                    quality.Add(current_epoch,id-1,id,"gpu_allocation_failure",nullptr,0);
                }
            }
            } catch (...) {
                // Keep presenting originals/generated frames if diagnostics cannot allocate.
            }
        }
#endif
        if (capture) {
            RecordImageTransfer(scheduler, *frame->image, extent, *capture->image, ProcessingExtent,
                                capture->initialized);
            capture->initialized = true;
            RecordDownload(scheduler, *capture->image, *capture->download);
        }

        while (!outputs.empty() && (output_storage[outputs.front()].metadata.current_id < display_id ||
                                    output_storage[outputs.front()].metadata.deadline_ns < now)) {
#if SNAPRYU_DIAGNOSTICS
            const auto& dropped=output_storage[outputs.front()].metadata;
            quality.Event(dropped.epoch,dropped.previous_id,dropped.current_id,"timeline_drop",
                dropped.deadline_ns<now?"deadline_expired":"source_id_too_old",now);
            ++perf_timeline_drops;
#endif
            outputs.pop_front();
        }
        Frame* generated = nullptr;
        Upload* used_upload = nullptr;
        if (id > 1 && display_id > last_shown && !outputs.empty() &&
            output_storage[outputs.front()].metadata.current_id == display_id) {
            for (auto& upload : uploads) {
                if (!upload.last_tick || scheduler.IsFree(upload.last_tick)) {
                    used_upload = &upload;
                    break;
                }
            }
            if (used_upload) {
                generated = present.TryGetRenderFrame(layout.width, layout.height);
            }
#if SNAPRYU_DIAGNOSTICS
            perf_upload_unavailable += !used_upload;
            perf_frame_unavailable += used_upload && !generated;
            if (!used_upload || !generated) {
                const auto& dropped=output_storage[outputs.front()].metadata;
                quality.Event(dropped.epoch,dropped.previous_id,dropped.current_id,"display_drop",
                    !used_upload?"upload_buffer_busy":"render_frame_unavailable",now);
            }
#endif
            if (generated) {
                auto& data = output_storage[outputs.front()].rgba;
                auto target = used_upload->buffer.Mapped();
                const bool bgra =
                    format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
                if (!bgra) {
                    std::memcpy(target.data(), data.data(), ProcessingBytes);
                } else {
                    for (size_t p = 0; p < ProcessingBytes; p += 4) {
                        target[p] = data[p + 2];
                        target[p + 1] = data[p + 1];
                        target[p + 2] = data[p];
                        target[p + 3] = 255;
                    }
                }
                used_upload->buffer.Flush();
                RecordUpload(scheduler, *used_upload->buffer, *used_upload->image,
                             used_upload->initialized);
                used_upload->initialized = true;
                RecordImageTransfer(scheduler, *used_upload->image, ProcessingExtent,
                                    *generated->image, extent, false);
#if SNAPRYU_DIAGNOSTICS
                QualityGpu* displayed_quality=nullptr;
                const auto& metadata=output_storage[outputs.front()].metadata;
                for (auto& q:quality_resources) if (q->epoch==metadata.epoch &&
                    q->previous==metadata.previous_id && q->current==metadata.current_id) {
                    displayed_quality=q.get();break;
                }
                if (displayed_quality) {
                    RecordDownload(scheduler,*used_upload->image,*displayed_quality->uploaded);
                    RecordDownload(scheduler,*generated->image,*displayed_quality->generated,extent);
                }
#endif
                used_upload->last_tick = scheduler.Flush(*generated->render_ready);
#if SNAPRYU_DIAGNOSTICS
                if (displayed_quality) {
                    quality.Tag(generated,metadata.epoch,metadata.previous_id,metadata.current_id);
                    quality.Add(metadata.epoch,metadata.previous_id,metadata.current_id,
                        "present_enqueued",nullptr,0,0,0,0,
                        "{\"gpu_tick\":"+std::to_string(used_upload->last_tick)+
                        ",\"requested_qpc_ns\":"+std::to_string(now)+",\"scanout_verified\":false}");
                    std::scoped_lock lock{capture_mutex};
                    quality_pending.push_back({displayed_quality,used_upload->last_tick});
                    wake.notify_one();
                }
#endif
                present.Present(generated, now, true);
                outputs.pop_front();
            }
        }
        const bool already_shown = display_id <= last_shown;
        if (!already_shown && id > 1 && displayed.id == display_id) {
            RecordImageTransfer(scheduler, *displayed.image, extent, *frame->image, extent, true);
        }
        const u64 tick = scheduler.Flush(*frame->render_ready);
        original.last_tick = tick;
        displayed.last_tick = tick;
        if (already_shown) {
            present.DiscardRenderedFrame(frame);
        } else {
            present.Present(frame, generated ? now + interval / 2 : now, false);
        }
        last_shown = std::max(last_shown, display_id);
        if (capture) {
            const bool bgra =
                format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
            const CaptureTask task{capture_index,
                                   epoch.load(std::memory_order_acquire),
                                   id,
                                   tick,
                                   now + interval * 2,
                                   plan.generate,
                                   bgra ? SNAPRYU_NPU_BGRA8 : SNAPRYU_NPU_RGBA8
#if SNAPRYU_DIAGNOSTICS
                                   ,selected_quality
#endif
                                   };
            {
                std::scoped_lock lock{capture_mutex};
                pending.push_back(task);
            }
            wake.notify_one();
        }
        scheduler.DispatchWork();
        return true;
    }

    const Device& device;
    MemoryAllocator& allocator;
    Scheduler& scheduler;
    PresentManager& present;
    Swapchain& swapchain;
    SnapRyuNpu* engine{};
    SnapRyuNpuStats cached_stats{};
#if SNAPRYU_DIAGNOSTICS
    SnapRyuNpuStats perf_stats{};
    s64 perf_time{};
    u64 perf_timeline_drops{}, perf_upload_unavailable{}, perf_frame_unavailable{};
#endif
    VideoCore::NpuFgControl control;
    std::array<Original, HistoryCount> originals;
    std::array<Capture, TransferCount> captures;
    std::array<Upload, TransferCount> uploads;
    // Slot capacity survives Take success, timeline drops and history resets.
    // ProcessingExtent is fixed; the enclosing Impl frees all three on teardown.
    std::array<Output, 3> output_storage;
    std::deque<size_t> outputs;
    std::atomic<u64> epoch{1};
    std::atomic<bool> transport_stop{}, transport_failed{};
    std::mutex capture_mutex;
    std::condition_variable wake;
    std::deque<CaptureTask> pending;
    std::thread transport;
#if SNAPRYU_DIAGNOSTICS
    // Never release download allocations before the shutdown/resize Finish.
    std::vector<std::unique_ptr<QualityGpu>> quality_resources;
    std::deque<QualityTask> quality_pending;
#endif
    VkFormat format{VK_FORMAT_UNDEFINED};
    u32 width{}, height{};
    u64 source_id{}, last_shown{};
    s64 last_time{};
    // Actual full-job observations, separate from shortcut job_ms in the ABI.
    double last_full_job_ms{}, warmup_full_job_ms{};
    s64 last_full_job_time{};
    double interval_ns{};
    bool allocated{}, disabled{}, shutdown{};
};

NpuFrameGenerator::NpuFrameGenerator(const Device& device, MemoryAllocator& allocator,
                                     Scheduler& scheduler, PresentManager& present,
                                     Swapchain& swapchain)
    : impl{std::make_unique<Impl>(device, allocator, scheduler, present, swapchain)} {}
NpuFrameGenerator::~NpuFrameGenerator() = default;
bool NpuFrameGenerator::Process(Frame* current, const Layout::FramebufferLayout& layout) {
    return impl->Process(current, layout);
}
void NpuFrameGenerator::Reset() { impl->Reset(); }
void NpuFrameGenerator::Shutdown() { impl->Shutdown(); }
} // namespace Vulkan
