// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>
#include <variant>

#include "common/dynamic_library.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/renderer_base.h"
#ifdef HAS_LSFG
#include "video_core/renderer_vulkan/present/frame_gen.h"
#endif
#include "video_core/renderer_vulkan/vk_blit_screen.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/renderer_vulkan/vk_turbo_mode.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Core::Memory {
class Memory;
}

namespace Tegra {
class GPU;
}

namespace Vulkan {

class Arm64Lsfg;
class SnapGoCapture;

Device CreateDevice(const vk::Instance& instance, const vk::InstanceDispatch& dld,
                    VkSurfaceKHR surface);


class RendererVulkan final : public VideoCore::RendererBase {
public:
    explicit RendererVulkan(Core::Frontend::EmuWindow& emu_window,
                            Tegra::MaxwellDeviceMemoryManager& device_memory_, Tegra::GPU& gpu_,
                            std::unique_ptr<Core::Frontend::GraphicsContext> context_);
    ~RendererVulkan() override;

    void Composite(std::span<const Tegra::FramebufferConfig> framebuffers) override;

    std::vector<u8> GetAppletCaptureBuffer() override;

    [[nodiscard]] VideoCore::RendererPerformanceStats GetPerformanceStats() const override;

    VideoCore::RasterizerInterface* ReadRasterizer() override {
        return &rasterizer;
    }

    [[nodiscard]] std::string GetDeviceVendor() const override {
        return device.GetDriverName();
    }

    [[nodiscard]] VideoCore::VulkanBackendStatus GetVulkanBackendStatus() const override {
        const auto& policy = device.GetOutputPolicy();
        const auto mode_name = [](Settings::VulkanBackendMode mode) {
            return mode == Settings::VulkanBackendMode::Adreno ? "Adreno" : "Standard";
        };
        return {
            .requested = mode_name(policy.GetRequestedMode()),
            .effective = mode_name(policy.GetEffectiveMode()),
            .driver = device.GetDriverID() == VK_DRIVER_ID_QUALCOMM_PROPRIETARY ? "Qualcomm"
                      : device.GetDriverID() == VK_DRIVER_ID_MESA_TURNIP     ? "Turnip"
                                                                           : device.GetDriverName(),
            .fallback_reason = std::string{policy.GetFallbackReason()},
        };
    }

    // Enhanced platform-specific initialization
    void InitializePlatformSpecific();

private:
    void InterpolateFrames(Frame* prev_frame, Frame* curr_frame);
    Frame* previous_frame = nullptr;  // Store the previous frame for interpolation
    VkCommandBuffer BeginSingleTimeCommands();
    void EndSingleTimeCommands(VkCommandBuffer command_buffer);
    void Report() const;
#if SNAPRYU_DIAGNOSTICS
    void ReportFrameTiming(std::chrono::steady_clock::time_point start, double wait_ms);
    std::chrono::steady_clock::time_point perf_window{};
    VideoCore::PresentationCounts perf_presents{};
    u64 perf_composites{};
    u32 perf_npu_mode{};
    double perf_wait_ms{}, perf_wait_max_ms{}, perf_composite_ms{};
#endif

    vk::Buffer RenderToBuffer(std::span<const Tegra::FramebufferConfig> framebuffers,
                              const Layout::FramebufferLayout& layout, VkFormat format,
                              VkDeviceSize buffer_size);
    void RenderScreenshot(std::span<const Tegra::FramebufferConfig> framebuffers);
    void RenderAppletCaptureLayer(std::span<const Tegra::FramebufferConfig> framebuffers);

    Tegra::MaxwellDeviceMemoryManager& device_memory;
    Tegra::GPU& gpu;

    std::shared_ptr<Common::DynamicLibrary> library;
    vk::InstanceDispatch dld;

    // Keep original handles for compatibility with existing code
    vk::Instance instance;

    vk::DebugUtilsMessenger debug_messenger;

    vk::SurfaceKHR surface;

    Device device;
    MemoryAllocator memory_allocator;
    StateTracker state_tracker;
    Scheduler scheduler;
    Swapchain swapchain;
    PresentManager present_manager;
    BlitScreen blit_swapchain;
    BlitScreen blit_capture;
    BlitScreen blit_applet;
    RasterizerVulkan rasterizer;
    std::unique_ptr<SnapGoCapture> go_capture;
    bool go_compact{};
#ifdef HAS_SNAPRYU_LSFG_ARM64
    std::unique_ptr<Arm64Lsfg> arm64_lsfg;
#endif
#ifdef HAS_LSFG
    FrameGen frame_gen;
#endif
    std::optional<TurboMode> turbo_mode;

    Frame applet_frame;
};

} // namespace Vulkan
