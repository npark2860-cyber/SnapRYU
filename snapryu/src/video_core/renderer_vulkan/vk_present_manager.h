// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <boost/container/deque.hpp>

#include "common/common_types.h"
#include "common/polyfill_thread.h"
#include "video_core/presentation_stats.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Core::Frontend {
class EmuWindow;
} // namespace Core::Frontend

namespace Vulkan {

class Device;
class Scheduler;
class Swapchain;

struct Frame {
    u32 width;
    u32 height;
    u32 index;
    vk::Image image;
    vk::ImageView image_view;
    vk::ImageView storage_view;
    vk::Framebuffer framebuffer;
    vk::CommandBuffer cmdbuf;
    vk::Semaphore render_ready;
    vk::Fence present_done;
    s64 not_before_ns{};
    // Generated frame only: present the next original no earlier than this long after
    // this frame's copy completed on the GPU.
    s64 original_after_ns{};
    bool generated{};
    bool discard{};
};

class PresentManager {
public:
    PresentManager(const vk::Instance& instance,
                   Core::Frontend::EmuWindow& render_window,
                   const Device& device,
                   MemoryAllocator& memory_allocator,
                   Scheduler& scheduler,
                   Swapchain& swapchain,
                   vk::SurfaceKHR& surface);
    ~PresentManager();

    bool HasFailed() const {
        return failure.load(std::memory_order_acquire) != VK_SUCCESS;
    }

    /// Returns the last used presentation frame, or nullptr after failure.
    Frame* GetRenderFrame();

    /// Optional generated frame: never wait for a fence or initialize/resize a frame.
    Frame* TryGetRenderFrame(u32 width, u32 height);

    /// Pushes a frame for presentation
    void Present(Frame* frame, s64 not_before_ns = 0, bool generated = false,
                 s64 original_after_ns = 0);
    // Retire a rendered frame without repeating an original on screen. Consumes
    // its binary render_ready semaphore and signals present_done before reuse.
    void DiscardRenderedFrame(Frame* frame);
    [[nodiscard]] u64 GeneratedPresentCount() const {
        return generated_presents.load(std::memory_order_relaxed);
    }
    [[nodiscard]] VideoCore::PresentationCounts GetPresentationCounts() const {
        return {original_presents.load(std::memory_order_relaxed),
                generated_presents.load(std::memory_order_relaxed), true};
    }

    /// Recreates the present frame to match the provided parameters
    void RecreateFrame(Frame* frame, u32 width, u32 height, VkFormat image_view_format,
                       VkRenderPass rd);

    /// Waits for the present thread to finish presenting all queued frames.
    void WaitPresent();
    // The present owner clears its old local-output deadline at the next boundary.
    void ResetRemotePacing() { reset_remote_pacing.store(true, std::memory_order_release); }

    /// How many additional frames can be queued without stalling the render thread
    [[nodiscard]] size_t MaxExtraFrames() const;

private:
    void PresentThread(std::stop_token token);
    void SetFailure(VkResult result);
    bool WaitFrameFence(Frame* frame, std::stop_token token = {});

    void EnqueueFrame(Frame* frame);
    void RetireDiscardedFrame(Frame* frame);

    void CopyToSwapchain(Frame* frame);

    void CopyToSwapchainImpl(Frame* frame);

    void RecreateSwapchain(Frame* frame);

    void SetImageCount();
#if SNAPRYU_DIAGNOSTICS
    void ReportPresentationSpacing(const Frame* frame);
    std::chrono::steady_clock::time_point perf_present_window{}, perf_last_present{};
    s64 perf_last_requested_ns{};
    bool perf_last_generated{};
    u64 perf_pairs{}, perf_compressed_pairs{};
    double perf_pair_gap_ms{}, perf_requested_gap_ms{};
#endif

private:
    const vk::Instance& instance;
    Core::Frontend::EmuWindow& render_window;
    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;
    Swapchain& swapchain;
    vk::SurfaceKHR& surface;
    vk::CommandPool cmdpool;
    std::vector<Frame> frames;
    boost::container::deque<Frame*> present_queue;
    boost::container::deque<Frame*> free_queue;
    std::condition_variable_any frame_cv;
    std::condition_variable free_cv;
    std::mutex swapchain_mutex;
    std::mutex queue_mutex;
    std::mutex free_mutex;
    std::mutex pacing_mutex;
    std::condition_variable_any pacing_cv;
    std::atomic<u64> generated_presents{};
    std::atomic<u64> original_presents{};
    s64 original_not_before_ns{}; ///< Present thread only.
    std::atomic<bool> reset_remote_pacing{};
    std::atomic<VkResult> failure{VK_SUCCESS};
    std::jthread present_thread;
    bool blit_supported;
    bool storage_supported;
    bool use_present_thread;
    std::size_t image_count{};
};

} // namespace Vulkan
