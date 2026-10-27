// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/settings.h"
#include "common/thread.h"
#include "core/frontend/emu_window.h"
#ifdef HAS_LSFG
#include "video_core/renderer_vulkan/present/lsfg_common.h"
#endif
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#if SNAPRYU_DIAGNOSTICS
#include "video_core/renderer_vulkan/vk_queue_lock_trace.h"
#endif
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_surface.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {


namespace {

constexpr size_t MAX_FRAMES_IN_FLIGHT = 7;
#ifdef HAS_LSFG
static_assert(MAX_FRAMES_IN_FLIGHT <= LSFG_MAX_TARGETS);
#endif

bool FrameGenerationPresentationRequested() {
    return Settings::values.frame_gen.GetValue();
}

bool CanStoreToFrame(const vk::PhysicalDevice& physical_device, VkFormat format) {
#ifdef HAS_LSFG
    if (!Settings::values.frame_gen.GetValue()) {
        return false;
    }
    const VkFormatProperties props{physical_device.GetFormatProperties(format)};
    return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
#else
    return false;
#endif
}

bool CanBlitToSwapchain(const vk::PhysicalDevice& physical_device, VkFormat format) {
    const VkFormatProperties props{physical_device.GetFormatProperties(format)};
    return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
}

[[nodiscard]] VkImageSubresourceLayers MakeImageSubresourceLayers() {
    return VkImageSubresourceLayers{
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] VkImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                        s32 swapchain_height) {
    return VkImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            {
                {
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                {
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            {
                {
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                {
                    .x = swapchain_width,
                    .y = swapchain_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] VkImageCopy MakeImageCopy(u32 frame_width, u32 frame_height, u32 swapchain_width,
                                        u32 swapchain_height) {
    return VkImageCopy{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffset =
            {
                .x = 0,
                .y = 0,
                .z = 0,
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffset =
            {
                .x = 0,
                .y = 0,
                .z = 0,
            },
        .extent =
            {
                .width = (std::min)(frame_width, swapchain_width),
                .height = (std::min)(frame_height, swapchain_height),
                .depth = 1,
            },
    };
}

} // Anonymous namespace

PresentManager::PresentManager(const vk::Instance& instance_,
                               Core::Frontend::EmuWindow& render_window_, const Device& device_,
                               MemoryAllocator& memory_allocator_, Scheduler& scheduler_,
                               Swapchain& swapchain_, vk::SurfaceKHR& surface_)
    : instance{instance_}, render_window{render_window_}, device{device_},
      memory_allocator{memory_allocator_}, scheduler{scheduler_}, swapchain{swapchain_},
      surface{surface_},
      blit_supported{CanBlitToSwapchain(device.GetPhysical(), swapchain.GetImageViewFormat())},
      storage_supported{CanStoreToFrame(device.GetPhysical(), swapchain.GetImageFormat())},
      use_present_thread{Settings::values.async_presentation.GetValue() ||
                         FrameGenerationPresentationRequested()} {
    SetImageCount();

    auto& dld = device.GetLogical();
    cmdpool = dld.CreateCommandPool({
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags =
            VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = device.GetGraphicsFamily(),
    });
    auto cmdbuffers = cmdpool.Allocate(image_count);

    frames.resize(image_count);
    for (u32 i = 0; i < frames.size(); i++) {
        Frame& frame = frames[i];
        frame.index = i;
        frame.cmdbuf = vk::CommandBuffer{cmdbuffers[i], device.GetDispatchLoader()};
        frame.render_ready = dld.CreateSemaphore({
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
        });
        frame.present_done = dld.CreateFence({
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        });
        free_queue.push_back(&frame);
    }

    if (use_present_thread) {
        present_thread = std::jthread([this](std::stop_token token) { PresentThread(token); });
    }
}

PresentManager::~PresentManager() {
    if (present_thread.joinable()) {
        present_thread.request_stop();
        frame_cv.notify_all();
        pacing_cv.notify_all();
        present_thread.join();
    }
}

void PresentManager::SetFailure(VkResult result) {
    {
        std::scoped_lock lock{queue_mutex, free_mutex, pacing_mutex};
        if (HasFailed())
            return;
        failure.store(result, std::memory_order_release);
    }
    LOG_CRITICAL(Render_Vulkan, "Presentation stopped: {} (VkResult={})",
                 vk::Exception{result}.what(), static_cast<int>(result));
    frame_cv.notify_all();
    free_cv.notify_all();
    pacing_cv.notify_all();
}

bool PresentManager::WaitFrameFence(Frame* frame, std::stop_token token) {
    while (!HasFailed() && !token.stop_requested()) {
        const VkResult result = frame->present_done.Wait(10'000'000);
        if (result == VK_TIMEOUT)
            continue;
        vk::Check(result);
        return !HasFailed() && !token.stop_requested();
    }
    return false;
}

Frame* PresentManager::GetRenderFrame() try {

    // Wait for free presentation frames
    std::unique_lock lock{free_mutex};
    free_cv.wait(lock, [this] { return HasFailed() || !free_queue.empty(); });
    if (HasFailed())
        return nullptr;

    // Take the frame from the queue
    Frame* frame = free_queue.front();
    free_queue.pop_front();

    // Wait for the presentation to be finished so all frame resources are free
    lock.unlock();
    if (!WaitFrameFence(frame))
        return nullptr;
    frame->present_done.Reset();

    return frame;
} catch (const vk::Exception& error) {
    SetFailure(error.GetResult());
    return nullptr;
}

Frame* PresentManager::TryGetRenderFrame(u32 width, u32 height) try {
    std::unique_lock lock{free_mutex, std::try_to_lock};
    if (!lock || HasFailed() || free_queue.empty()) {
        return nullptr;
    }
    Frame* frame = free_queue.front();
    // Only the normal original-frame path may initialize or resize a pooled image.
    // Preparing it here can drain the whole renderer, even when its fence is ready.
    if (!frame->image || frame->width != width || frame->height != height) {
        return nullptr;
    }
    const auto status = frame->present_done.GetStatus();
    if (status == VK_NOT_READY) {
        return nullptr;
    }
    vk::Check(status);
    frame->present_done.Reset();
    free_queue.pop_front();
    return frame;
} catch (const vk::Exception& error) {
    SetFailure(error.GetResult());
    return nullptr;
}

void PresentManager::Present(Frame* frame, s64 not_before_ns, bool generated,
                             s64 original_after_ns) {
    frame->not_before_ns = not_before_ns;
    frame->original_after_ns = original_after_ns;
    frame->generated = generated;
    frame->discard = false;
    EnqueueFrame(frame);
}

void PresentManager::DiscardRenderedFrame(Frame* frame) {
    frame->not_before_ns = 0;
    frame->original_after_ns = 0;
    frame->generated = false;
    frame->discard = true;
    EnqueueFrame(frame);
}

void PresentManager::RetireDiscardedFrame(Frame* frame) {
    const VkSemaphore ready = *frame->render_ready;
    constexpr VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                              .pNext = nullptr,
                              .waitSemaphoreCount = 1,
                              .pWaitSemaphores = &ready,
                              .pWaitDstStageMask = &stage,
                              .commandBufferCount = 0,
                              .pCommandBuffers = nullptr,
                              .signalSemaphoreCount = 0,
                              .pSignalSemaphores = nullptr};
#if SNAPRYU_DIAGNOSTICS
    QueueLockTrace::Scope queue_trace{QueueLockTrace::Owner::PresentDiscard};
#endif
    std::scoped_lock lock{scheduler.submit_mutex};
#if SNAPRYU_DIAGNOSTICS
    queue_trace.Acquired();
#endif
    vk::Check(device.GetGraphicsQueue().Submit(submit, *frame->present_done));
}

void PresentManager::EnqueueFrame(Frame* frame) try {
    if (HasFailed())
        return;
    if (use_present_thread) {
        scheduler.Record([this, frame](vk::CommandBuffer) {
            std::unique_lock lock{queue_mutex};
            if (HasFailed())
                return;
            present_queue.push_back(frame);
            frame_cv.notify_one();
        });
    } else {
        scheduler.WaitWorker();
        if (frame->discard) {
            RetireDiscardedFrame(frame);
        } else {
            CopyToSwapchain(frame);
        }
        free_queue.push_back(frame);
    }
} catch (const vk::Exception& error) {
    SetFailure(error.GetResult());
}

size_t PresentManager::MaxExtraFrames() const {
    return image_count - 1;
}

void PresentManager::RecreateFrame(Frame* frame, u32 width, u32 height, VkFormat image_view_format,
                                   VkRenderPass rd) {
    auto& dld = device.GetLogical();

    frame->width = width;
    frame->height = height;

    const VkImageUsageFlags storage_usage =
        storage_supported ? static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_STORAGE_BIT) : 0;

    frame->image = memory_allocator.CreateImage({
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = swapchain.GetImageFormat(),
        .extent =
            {
                .width = width,
                .height = height,
                .depth = 1,
            },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | storage_usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });

    frame->image_view = dld.CreateImageView({
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = *frame->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = image_view_format,
        .components =
            {
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY,
            },
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    });

    frame->storage_view = vk::ImageView{};
    if (storage_supported) {
        frame->storage_view = dld.CreateImageView({
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = *frame->image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = swapchain.GetImageFormat(),
            .components =
                {
                    .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                    .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                    .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                    .a = VK_COMPONENT_SWIZZLE_IDENTITY,
                },
            .subresourceRange =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
        });
    }

    const VkImageView image_view{*frame->image_view};
    frame->framebuffer = dld.CreateFramebuffer({
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .renderPass = rd,
        .attachmentCount = 1,
        .pAttachments = &image_view,
        .width = width,
        .height = height,
        .layers = 1,
    });
}

void PresentManager::WaitPresent() {
    if (!use_present_thread) {
        return;
    }

    // Wait for the present queue to be empty
    {
        std::unique_lock queue_lock{queue_mutex};
        frame_cv.wait(queue_lock, [this] { return HasFailed() || present_queue.empty(); });
    }

    // The above condition will be satisfied when the last frame is taken from the queue.
    // To ensure that frame has been presented as well take hold of the swapchain
    // mutex.
    std::scoped_lock swapchain_lock{swapchain_mutex};
}

void PresentManager::PresentThread(std::stop_token token) try {
    Common::SetCurrentThreadName("VulkanPresent");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
    Common::SetCurrentThreadToPerformanceCores();
    while (!token.stop_requested() && !HasFailed()) {
        std::unique_lock lock{queue_mutex};
        // Wait for presentation frames
        frame_cv.wait(lock, token, [this] { return HasFailed() || !present_queue.empty(); });
        if (!token.stop_requested() && !HasFailed()) {
            // Take the frame and notify anyone waiting
            Frame* frame = present_queue.front();
            present_queue.pop_front();
            frame_cv.notify_one();

            // By exchanging the lock ownership we take the swapchain lock
            // before the queue lock goes out of scope. This way the swapchain
            // lock in WaitPresent is guaranteed to occur after here.
            std::exchange(lock, std::unique_lock{swapchain_mutex});
            s64 due_ns = frame->not_before_ns;
            if (reset_remote_pacing.exchange(false, std::memory_order_acq_rel) || frame->discard)
                original_not_before_ns = 0;
            if (!frame->discard && !frame->generated && original_not_before_ns > 0) {
                due_ns = (std::max)(due_ns, original_not_before_ns);
                original_not_before_ns = 0;
            }
            if (due_ns > 0) {
                const auto due = std::chrono::steady_clock::time_point{
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::nanoseconds{due_ns})};
                std::unique_lock pace_lock{pacing_mutex};
                pacing_cv.wait_until(pace_lock, token, due, [this] { return HasFailed(); });
                if (token.stop_requested() || HasFailed()) {
                    break;
                }
            }
            if (frame->discard) {
                RetireDiscardedFrame(frame);
            } else {
                CopyToSwapchain(frame);
                if (frame->generated && frame->original_after_ns > 0) {
                    // Submitted before its queued generation finished on the GPU: pace the
                    // next original from the moment this copy really completed.
                    if (!WaitFrameFence(frame, token))
                        return;
                    original_not_before_ns =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count() +
                        frame->original_after_ns;
                }
            }

            // Free the frame for reuse
            std::scoped_lock fl{free_mutex};
            free_queue.push_back(frame);
            free_cv.notify_one();
        }
    }
} catch (const vk::Exception& error) {
    SetFailure(error.GetResult());
}

void PresentManager::RecreateSwapchain(Frame* frame) {
    swapchain.Create(*surface, frame->width, frame->height); // Pass raw pointer
    SetImageCount();
}

void PresentManager::SetImageCount() {
    // We cannot have more than 7 images in flight at any given time.
    // FRAMES_IN_FLIGHT is 8, and the cache TICKS_TO_DESTROY is 8.
    // Mali drivers will give us 6.
#ifdef HAS_LSFG
    const size_t generations = Settings::FrameGenMaxGenerations();
    const size_t queued_composites = Settings::values.frame_gen_queue_target.GetValue() + 1;
    image_count =
        std::clamp<size_t>((generations + 1) * queued_composites, swapchain.GetImageCount(),
                           MAX_FRAMES_IN_FLIGHT);
#else
    image_count = std::min<size_t>(swapchain.GetImageCount(), MAX_FRAMES_IN_FLIGHT);
#endif
    if (FrameGenerationPresentationRequested()) {
        image_count = std::clamp<size_t>(image_count, 4, MAX_FRAMES_IN_FLIGHT);
    }
}

void PresentManager::CopyToSwapchain(Frame* frame) {
    bool requires_recreation = false;

    while (true) {
        try {
            // Recreate surface and swapchain if needed.
            if (requires_recreation) {
#ifdef __ANDROID__
                surface = CreateSurface(instance, render_window.GetWindowInfo());
#endif
                RecreateSwapchain(frame);
            }

            // Draw to swapchain.
            return CopyToSwapchainImpl(frame);
        } catch (const vk::Exception& except) {
            if (except.GetResult() != VK_ERROR_SURFACE_LOST_KHR) {
                throw;
            }

            requires_recreation = true;
        }
    }
}

void PresentManager::CopyToSwapchainImpl(Frame* frame) {

    // If the size of the incoming frames has changed, recreate the swapchain
    // to account for that.
    const bool is_suboptimal = swapchain.NeedsRecreation();
    const bool size_changed =
        swapchain.GetWidth() != frame->width || swapchain.GetHeight() != frame->height;
    if (is_suboptimal || size_changed) {
        RecreateSwapchain(frame);
    }

    while (swapchain.AcquireNextImage()) {
        RecreateSwapchain(frame);
    }

    const vk::CommandBuffer cmdbuf{frame->cmdbuf};
    cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });

    const VkImage image{swapchain.CurrentImage()};
    const VkExtent2D extent = swapchain.GetExtent();
    const std::array pre_barriers{
        VkImageMemoryBarrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        },
        VkImageMemoryBarrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = *frame->image,
            .subresourceRange{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        },
    };
    const std::array post_barriers{
        VkImageMemoryBarrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        },
        VkImageMemoryBarrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = *frame->image,
            .subresourceRange{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        },
    };

    cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER, VK_PIPELINE_STAGE_TRANSFER_BIT, {},
                           {}, {}, pre_barriers);

    if (blit_supported) {
        cmdbuf.BlitImage(*frame->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         MakeImageBlit(frame->width, frame->height, extent.width, extent.height),
                         VK_FILTER_LINEAR);
    } else {
        cmdbuf.CopyImage(*frame->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         MakeImageCopy(frame->width, frame->height, extent.width, extent.height));
    }

    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, {},
                           {}, {}, post_barriers);

    cmdbuf.End();

    const VkSemaphore present_semaphore = swapchain.CurrentPresentSemaphore();
    const VkSemaphore render_semaphore = swapchain.CurrentRenderSemaphore();
    const std::array wait_semaphores = {present_semaphore, *frame->render_ready};

    static constexpr std::array<VkPipelineStageFlags, 2> wait_stage_masks{
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
    };

    const VkSubmitInfo submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 2U,
        .pWaitSemaphores = wait_semaphores.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = 1,
        .pCommandBuffers = cmdbuf.address(),
        .signalSemaphoreCount = 1U,
        .pSignalSemaphores = &render_semaphore,
    };

    // Submit the image copy/blit to the swapchain
    {
#if SNAPRYU_DIAGNOSTICS
        QueueLockTrace::Scope queue_trace{QueueLockTrace::Owner::PresentCopy};
#endif
        std::scoped_lock submit_lock{scheduler.submit_mutex};
#if SNAPRYU_DIAGNOSTICS
        queue_trace.Acquired();
#endif
        switch (const VkResult result =
                    device.GetGraphicsQueue().Submit(submit_info, *frame->present_done)) {
        case VK_SUCCESS:
            break;
        case VK_ERROR_DEVICE_LOST:
            device.ReportLoss();
            [[fallthrough]];
        default:
            vk::Check(result);
            break;
        }
    }

    // Present
    // Count only accepted presentation requests, in both threaded and direct paths.
    // Retired/late frames and failed or out-of-date presents do not inflate FPS.
    const bool accepted = swapchain.Present(render_semaphore);
    if (accepted) {
        (frame->generated ? generated_presents : original_presents)
            .fetch_add(1, std::memory_order_relaxed);
#if SNAPRYU_DIAGNOSTICS
        ReportPresentationSpacing(frame);
#endif
    }
}

#if SNAPRYU_DIAGNOSTICS
void PresentManager::ReportPresentationSpacing(const Frame* frame) {
    const auto now = std::chrono::steady_clock::now();
    if (perf_present_window.time_since_epoch().count() == 0) perf_present_window = now;
    const double requested_ms = double(frame->not_before_ns - perf_last_requested_ns) / 1e6;
    if (perf_last_generated && !frame->generated && requested_ms > 0.0) {
        const double gap_ms = std::chrono::duration<double, std::milli>(now - perf_last_present).count();
        ++perf_pairs;
        perf_pair_gap_ms += gap_ms;
        perf_requested_gap_ms += requested_ms;
        perf_compressed_pairs += gap_ms < requested_ms * 0.5;
    }
    perf_last_present = now;
    perf_last_requested_ns = frame->not_before_ns;
    perf_last_generated = frame->generated;
    if (std::chrono::duration<double>(now - perf_present_window).count() < 5.0) return;
    const double pairs = static_cast<double>(perf_pairs);
    LOG_INFO(Render_Vulkan, "SNAPRYU_PRESENT_SPACING generated_to_original_pairs={} "
        "requested_gap_avg_ms={:.3f} accepted_gap_avg_ms={:.3f} "
        "compressed_below_half_requested={} scanout_verified=false",
        perf_pairs, perf_pairs ? perf_requested_gap_ms / pairs : 0.0,
        perf_pairs ? perf_pair_gap_ms / pairs : 0.0, perf_compressed_pairs);
    perf_present_window = now;
    perf_pairs = perf_compressed_pairs = 0;
    perf_pair_gap_ms = perf_requested_gap_ms = 0.0;
}
#endif

} // namespace Vulkan
