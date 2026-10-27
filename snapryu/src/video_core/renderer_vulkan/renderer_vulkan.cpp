#ifdef HAS_SNAPRYU_LSFG_ARM64
#include "video_core/renderer_vulkan/vk_lsfg_arm64.h"
#endif
// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <fmt/ranges.h>

#include "common/logging.h"
#include <ranges>
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/npu_fg_settings.h"
#include "common/snapryu_go.h"
#include "video_core/renderer_vulkan/vk_snap_go_capture.h"
#include "core/core_timing.h"
#include "core/frontend/graphics_context.h"
#include "video_core/capture.h"
#include "video_core/gpu.h"
#include "video_core/present.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/renderer_vulkan/vk_adreno_profiler.h"
#include "video_core/renderer_vulkan/vk_blit_screen.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/textures/decoders.h"
#include "video_core/vulkan_common/vulkan_debug_callback.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_surface.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#if SNAPRYU_DIAGNOSTICS
#include <string_view>
#include "common/fs/fs_util.h"
#ifdef HAS_LSFG
#include "video_core/frame_gen/lossless_dll.h"
#endif
#endif
#ifdef __ANDROID__
#include <jni.h>
#endif
namespace Vulkan {
namespace {

constexpr VkExtent2D CaptureImageSize{
    .width = VideoCore::Capture::LinearWidth,
    .height = VideoCore::Capture::LinearHeight,
};

#ifdef HAS_LSFG
[[nodiscard]] VkExtent2D GuestExtent(std::span<const Tegra::FramebufferConfig> framebuffers) {
    if (framebuffers.empty()) {
        return VkExtent2D{};
    }

    const auto& framebuffer = framebuffers.front();
    if (framebuffer.crop_rect.IsEmpty()) {
        return VkExtent2D{.width = framebuffer.width, .height = framebuffer.height};
    }
    return VkExtent2D{
        .width = static_cast<u32>(framebuffer.crop_rect.GetWidth()),
        .height = static_cast<u32>(framebuffer.crop_rect.GetHeight()),
    };
}
#endif
#if SNAPRYU_DIAGNOSTICS
#if defined(HAS_LSFG) && defined(HAS_SNAPRYU_LSFG_ARM64)
// b102 SNAPRYU_LSFG_BISECT=eden: Eden's own frame generation (present/frame_gen.cpp, the path Eden uses on
// Android) instead of Arm64Lsfg::Process; lsfg_arm64.dll is never loaded. Shaders come from Lossless.dll next to
// the exe. Splits the Turnip LSFG crash between Windows Turnip with extra presented frames and SnapRYU's own LSFG
// host code.
bool EdenFrameGenBisect() {
    static const bool on = [] {
        const char* const value = std::getenv("SNAPRYU_LSFG_BISECT");
        const bool eden = value != nullptr && std::string_view{value} == "eden";
        if (eden) {
            LOG_INFO(Render_Vulkan, "LSFG_BISECT mode=eden lossless_status={} path={}",
                     static_cast<u32>(VideoCore::FrameGen::GetInstalledLosslessStatus()),
                     Common::FS::PathToUTF8String(VideoCore::FrameGen::GetLosslessDllPath()));
        }
        return eden;
    }();
    return on;
}
#endif
#endif

constexpr VkExtent3D CaptureImageExtent{
    .width = VideoCore::Capture::LinearWidth,
    .height = VideoCore::Capture::LinearHeight,
    .depth = VideoCore::Capture::LinearDepth,
};

constexpr VkFormat CaptureFormat = VK_FORMAT_A8B8G8R8_UNORM_PACK32;

// b93 frame stall log (release and diagnostic): the gap between the starts of two Composite calls, i.e. two guest
// frames handed to presentation. Gaps of 50 ms or more are logged with their time (first 300, then every 50th);
// every 10 s a summary gives the frame count and the gaps over 33/50/100 ms. Logging only; nothing else changes.
void NoteFrameStart() {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point previous{};
    static Clock::time_point window{};
    static u64 frames = 0;
    static u64 over33 = 0;
    static u64 over50 = 0;
    static u64 over100 = 0;
    static double max_gap_ms = 0.0;
    static u64 stalls = 0;
    const Clock::time_point now = Clock::now();
    if (previous == Clock::time_point{}) {
        previous = now;
        window = now;
        LOG_INFO(Render_Vulkan, "FRAME_STALL ready b93 threshold_ms=50 summary_s=10 "
                                "measurement=composite_start_gap");
        return;
    }
    const double gap_ms = std::chrono::duration<double, std::milli>(now - previous).count();
    previous = now;
    ++frames;
    max_gap_ms = (std::max)(max_gap_ms, gap_ms);
    if (gap_ms >= 33.0) {
        ++over33;
    }
    if (gap_ms >= 100.0) {
        ++over100;
    }
    if (gap_ms >= 50.0) {
        ++over50;
        ++stalls;
        if (stalls <= 300 || stalls % 50 == 0) {
            LOG_INFO(Render_Vulkan, "FRAME_STALL n={} gap_ms={:.1f}", stalls, gap_ms);
        }
    }
    const double seconds = std::chrono::duration<double>(now - window).count();
    if (seconds >= 10.0) {
        LOG_INFO(Render_Vulkan, "FRAME_STALL_SUMMARY window_s={:.1f} fps={:.1f} max_gap_ms={:.1f} "
                                "over33={} over50={} over100={} total_stalls={}",
                 seconds, static_cast<double>(frames) / seconds, max_gap_ms, over33, over50, over100,
                 stalls);
        window = now;
        frames = over33 = over50 = over100 = 0;
        max_gap_ms = 0.0;
    }
}

std::string GetReadableVersion(u32 version) {
    return fmt::format("{}.{}.{}", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version),
                       VK_VERSION_PATCH(version));
}

std::string GetDriverVersion(const Device& device) {
    // Extracted from
    // https://github.com/SaschaWillems/vulkan.gpuinfo.org/blob/5dddea46ea1120b0df14eef8f15ff8e318e35462/functions.php#L308-L314
    const u32 version = device.GetDriverVersion();

    if (device.GetDriverID() == VK_DRIVER_ID_NVIDIA_PROPRIETARY) {
        const u32 major = (version >> 22) & 0x3ff;
        const u32 minor = (version >> 14) & 0x0ff;
        const u32 secondary = (version >> 6) & 0x0ff;
        const u32 tertiary = version & 0x003f;
        return fmt::format("{}.{}.{}.{}", major, minor, secondary, tertiary);
    }
    if (device.GetDriverID() == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS) {
        const u32 major = version >> 14;
        const u32 minor = version & 0x3fff;
        return fmt::format("{}.{}", major, minor);
    }
    return GetReadableVersion(version);
}

std::string BuildCommaSeparatedExtensions(
    const std::set<std::string, std::less<>>& available_extensions) {
    return fmt::format("{}", fmt::join(available_extensions, ","));
}

} // Anonymous namespace

Device CreateDevice(const vk::Instance& instance, const vk::InstanceDispatch& dld, VkSurfaceKHR surface) {
    const std::vector<VkPhysicalDevice> devices = instance.EnumeratePhysicalDevices();
    const u32 device_index = Settings::values.vulkan_device.GetValue();
    if (device_index >= u32(devices.size())) {
        LOG_ERROR(Render_Vulkan, "Invalid device index {}!", device_index);
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    const vk::PhysicalDevice physical_device(devices[device_index], dld);
    return Device(*instance, physical_device, surface, dld);
}

RendererVulkan::RendererVulkan(Core::Frontend::EmuWindow& emu_window,
                               Tegra::MaxwellDeviceMemoryManager& device_memory_,
                               Tegra::GPU& gpu_,
                               std::unique_ptr<Core::Frontend::GraphicsContext> context_)
try
    : RendererBase(emu_window, std::move(context_))
    , device_memory(device_memory_)
    , gpu(gpu_)
    , library(OpenLibrary(context.get()))
    , dld()
    // Create raw Vulkan instance first
    , instance(CreateInstance(*library,
                            dld,
                            VK_API_VERSION_1_1,
                            render_window.GetWindowInfo().type,
                            Settings::values.renderer_debug.GetValue()))
    // Create debug messenger if debug is enabled
    , debug_messenger(Settings::values.renderer_debug ? CreateDebugUtilsCallback(instance)
                                                    : vk::DebugUtilsMessenger{})
    // Create surface
    , surface(CreateSurface(instance, render_window.GetWindowInfo()))
    , device(CreateDevice(instance, dld, *surface))
    , memory_allocator(device)
    , state_tracker()
    , scheduler(device, state_tracker)
    , swapchain(*surface,
                device,
                scheduler,
               render_window.GetFramebufferLayout().width,
               render_window.GetFramebufferLayout().height)
    , present_manager(instance,
                      render_window,
                      device,
                      memory_allocator,
                      scheduler,
                      swapchain,
                      surface)
    , blit_swapchain(device_memory,
                   device,
                   memory_allocator,
                   present_manager,
                   scheduler,
                   PresentFiltersForDisplay)
    , blit_capture(device_memory,
                   device,
                   memory_allocator,
                   present_manager,
                   scheduler,
                   PresentFiltersForDisplay)
    , blit_applet(device_memory,
                  device,
                  memory_allocator,
                  present_manager,
                  scheduler,
                  PresentFiltersForAppletCapture)
    , rasterizer(render_window, gpu, device_memory, device, memory_allocator, state_tracker, scheduler)
#ifdef HAS_LSFG
    , frame_gen(memory_allocator, scheduler)
#endif
{

    AdrenoProfiler::Get().Initialize(
        device.GetDriverID() == VK_DRIVER_ID_QUALCOMM_PROPRIETARY);

    if (Settings::values.renderer_force_max_clock.GetValue() && device.ShouldBoostClocks()) {
        turbo_mode.emplace(instance, dld);
        scheduler.RegisterOnSubmit([this] { turbo_mode->QueueSubmitted(); });
    }

#ifdef HAS_SNAPRYU_LSFG_ARM64
    arm64_lsfg = std::make_unique<Arm64Lsfg>(*instance, device, memory_allocator, scheduler,
                                            present_manager, swapchain);
#endif
    Report();
} catch (const vk::Exception& exception) {
    LOG_ERROR(Render_Vulkan, "Vulkan initialization failed with error: {}", exception.what());
    throw std::runtime_error{fmt::format("Vulkan initialization error {}", exception.what())};
}

RendererVulkan::~RendererVulkan() {
    if (go_capture) go_capture->Stop();
#ifdef HAS_SNAPRYU_LSFG_ARM64
    arm64_lsfg.reset();
#endif
    scheduler.RegisterOnSubmit([] {});
    // Exits used to end in an uncaught vk::Exception (abort) from a failed teardown submission.
    scheduler.Shutdown();
    void(device.GetLogical().WaitIdle());
    go_capture.reset();
}

VideoCore::RendererPerformanceStats RendererVulkan::GetPerformanceStats() const {
    const auto [used, budget] = memory_allocator.GetDeviceLocalBudget();
    return {present_manager.GetPresentationCounts(), used, budget, budget != 0};
}

void RendererVulkan::Composite(std::span<const Tegra::FramebufferConfig> framebuffers) {
    if (present_manager.HasFailed())
        return;
    NoteFrameStart();
#if SNAPRYU_DIAGNOSTICS
    const auto perf_begin = std::chrono::steady_clock::now();
    double frame_wait_ms = 0.0;
    SCOPE_EXIT { ReportFrameTiming(perf_begin, frame_wait_ms); };
#endif
    SCOPE_EXIT {
        render_window.OnFrameDisplayed();
    };

    RenderAppletCaptureLayer(framebuffers);

    // Periodic GPU memory report (every ~1200 presented frames) to track memory growth.
    static u64 composite_count = 0;
    if (++composite_count % 1200 == 0) {
        memory_allocator.LogBudget();
    }

    const bool compact = SnapGo::IsCompactActive();
    if (compact != go_compact) {
        go_compact = compact;
        present_manager.ResetRemotePacing();
        if (go_capture) go_capture->Reset();
#ifdef HAS_SNAPRYU_LSFG_ARM64
        arm64_lsfg->Reset();
#endif
    }
    auto output_layout = render_window.GetFramebufferLayout();
    if (compact) {
        output_layout.width = SNAP_GO_WIDTH;
        output_layout.height = SNAP_GO_HEIGHT;
        output_layout.screen = {0, 0, SNAP_GO_WIDTH, SNAP_GO_HEIGHT};
    }
    if (!go_capture && SnapGo::VideoDemand().enabled) {
        try { go_capture = std::make_unique<SnapGoCapture>(device, memory_allocator, scheduler); }
        catch (...) { /* Remote capture allocation never terminates emulation. */ }
    }
    if (!render_window.IsShown() && !compact) {
#ifdef HAS_SNAPRYU_LSFG_ARM64
        arm64_lsfg->Reset();
#endif
        return;
    }

    RenderScreenshot(framebuffers);
#if SNAPRYU_DIAGNOSTICS
    const auto frame_wait_begin = std::chrono::steady_clock::now();
#endif
    Frame* frame = present_manager.GetRenderFrame();
    if (!frame)
        return;
#if SNAPRYU_DIAGNOSTICS
    frame_wait_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - frame_wait_begin).count();
#endif

    scheduler.RequestOutsideRenderPassOperationContext();
    blit_swapchain.DrawToFrame(device, rasterizer, frame, framebuffers,
                               output_layout, swapchain.GetImageCount(),
                               swapchain.GetImageViewFormat());


#ifdef HAS_SNAPRYU_LSFG_ARM64
#if SNAPRYU_DIAGNOSTICS
    if (!EdenFrameGenBisect())
#endif
    if (arm64_lsfg->Process(frame, output_layout, blit_swapchain,
                            GuestExtent(framebuffers), go_capture.get(), compact)) {
        gpu.RendererFrameEndNotify();
        rasterizer.TickFrame();
        AdrenoProfiler::Get().FrameEnd();
        return;
    }
#endif
#if SNAPRYU_DIAGNOSTICS
#if defined(HAS_LSFG) && defined(HAS_SNAPRYU_LSFG_ARM64)
    // Same as the Eden block below, which the ARM64 build compiles out.
    if (EdenFrameGenBisect()) {
        void(frame_gen.WantedGenerations(present_manager.MaxExtraFrames()));

        frame_gen.Process(device, frame, swapchain.GetImageFormat(), GuestExtent(framebuffers));

        const size_t generated_frames = frame_gen.GeneratedFrameCount();
        static u64 eden_composites = 0;
        static u64 eden_generated = 0;
        eden_generated += generated_frames;
        if (++eden_composites % 600 == 1) {
            LOG_INFO(Render_Vulkan, "LSFG_BISECT mode=eden composites={} generated={}",
                     eden_composites, eden_generated);
        }
        for (size_t generation = 0; generation < generated_frames; ++generation) {
            Frame* generated = present_manager.GetRenderFrame();
            if (!generated)
                return;
            blit_swapchain.PrepareFrame(device, generated, render_window.GetFramebufferLayout());
            frame_gen.GenerateInto(device, generated, generation);
            scheduler.Flush(*generated->render_ready);
            present_manager.Present(generated);
        }
    }
#endif
#endif
#if defined(HAS_LSFG) && !defined(HAS_SNAPRYU_LSFG_ARM64)
        void(frame_gen.WantedGenerations(present_manager.MaxExtraFrames()));

        frame_gen.Process(device, frame, swapchain.GetImageFormat(), GuestExtent(framebuffers));

        const size_t generated_frames = frame_gen.GeneratedFrameCount();
        for (size_t generation = 0; generation < generated_frames; ++generation) {
            Frame* generated = present_manager.GetRenderFrame();
            if (!generated)
                return;
            blit_swapchain.PrepareFrame(device, generated, render_window.GetFramebufferLayout());
            frame_gen.GenerateInto(device, generated, generation);
            scheduler.Flush(*generated->render_ready);
            present_manager.Present(generated);
        }
#endif

    if (go_capture) go_capture->Single(frame, swapchain.GetImageFormat(), output_layout);
    scheduler.Flush(*frame->render_ready);

    if (compact) present_manager.DiscardRenderedFrame(frame);
    else present_manager.Present(frame);
#ifdef HAS_LSFG
    scheduler.DispatchWork();
#endif

    gpu.RendererFrameEndNotify();
    rasterizer.TickFrame();
    AdrenoProfiler::Get().FrameEnd();
}

#if SNAPRYU_DIAGNOSTICS
void RendererVulkan::ReportFrameTiming(std::chrono::steady_clock::time_point start,
                                     double wait_ms) {
    const auto now = std::chrono::steady_clock::now();
    static const bool stall_timing = [] {
        const char* value = std::getenv("EDEN_GPU_STALL_TRACE");
        const bool on = value && value[0] == '1' && value[1] == '\0';
        LOG_INFO(Render_Vulkan, "GPU_FRAME_GAP_TRACE enabled={} threshold_us=100000 "
                               "clock=steady_epoch_us measurement=composite_start_gap scanout=false", on);
        return on;
    }();
    if (stall_timing) {
        static thread_local const RendererVulkan* owner{};
        static thread_local std::chrono::steady_clock::time_point previous_start{};
        static thread_local u64 events{};
        if (owner != this) {
            owner = this;
            previous_start = {};
            events = 0;
        }
        const auto us = [](auto time) {
            return std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count();
        };
        const s64 gap_us = us(start) - us(previous_start);
        if (previous_start.time_since_epoch().count() != 0 && gap_us >= 100'000) {
            ++events;
            if (events <= 128 || events % 128 == 0) {
                LOG_WARNING(Render_Vulkan,
                            "GPU_FRAME_GAP event={} previous_begin_us={} begin_us={} end_us={} "
                            "gap_us={} composite_us={} frame_pool_wait_ms={:.3f} logical_tick={} "
                            "measurement=composite_start_gap scanout=false log_first=128 then_every=128",
                            events, us(previous_start), us(start), us(now), gap_us, us(now) - us(start),
                            wait_ms, scheduler.CurrentTick());
            }
        }
        previous_start = start;
    }
    const auto counts = present_manager.GetPresentationCounts();
    const auto mode = static_cast<u32>(Settings::npu_fg_mode.GetValue());
    if (perf_window.time_since_epoch().count() == 0 || mode != perf_npu_mode) {
        perf_window = now;
        perf_presents = counts;
        perf_npu_mode = mode;
        perf_composites = 0;
        perf_wait_ms = perf_wait_max_ms = perf_composite_ms = 0.0;
        return;
    }
    ++perf_composites;
    perf_wait_ms += wait_ms;
    perf_wait_max_ms = std::max(perf_wait_max_ms, wait_ms);
    perf_composite_ms += std::chrono::duration<double, std::milli>(now - start).count();
    const double seconds = std::chrono::duration<double>(now - perf_window).count();
    if (seconds < 5.0) return;
    const double originals = static_cast<double>(counts.original - perf_presents.original);
    const double generated = static_cast<double>(counts.generated - perf_presents.generated);
    const double samples = static_cast<double>(perf_composites);
    LOG_INFO(Render_Vulkan, "SNAPRYU_PRESENT mode={} window_s={:.3f} composite_fps={:.2f} "
        "original_present_fps={:.2f} generated_present_fps={:.2f} total_present_fps={:.2f} "
        "frame_pool_wait_avg_ms={:.3f} frame_pool_wait_max_ms={:.3f} composite_cpu_avg_ms={:.3f} "
        "accepted_present_requests=true scanout_verified=false",
        mode, seconds, samples / seconds, originals / seconds, generated / seconds,
        (originals + generated) / seconds, perf_wait_ms / samples,
        perf_wait_max_ms, perf_composite_ms / samples);
    perf_window = now;
    perf_presents = counts;
    perf_composites = 0;
    perf_wait_ms = perf_wait_max_ms = perf_composite_ms = 0.0;
}
#endif

void RendererVulkan::Report() const {
    using namespace Common::Literals;
    const std::string vendor_name{device.GetVendorName()};
    const std::string model_name{device.GetModelName()};
    const std::string driver_version = GetDriverVersion(device);
    const std::string driver_name = fmt::format("{} {}", vendor_name, driver_version);

    const std::string api_version = GetReadableVersion(device.ApiVersion());

    const std::string extensions = BuildCommaSeparatedExtensions(device.GetAvailableExtensions());

    const auto available_vram = static_cast<f64>(device.GetDeviceLocalMemory()) / f64{1_GiB};

    LOG_INFO(Render_Vulkan, "Driver: {}", driver_name);
    LOG_INFO(Render_Vulkan, "Device: {}", model_name);
    LOG_INFO(Render_Vulkan, "Vulkan: {}", api_version);
    LOG_INFO(Render_Vulkan, "Available VRAM: {:.2f} GiB", available_vram);
}

vk::Buffer RendererVulkan::RenderToBuffer(std::span<const Tegra::FramebufferConfig> framebuffers,
                                          const Layout::FramebufferLayout& layout, VkFormat format,
                                          VkDeviceSize buffer_size) {
    auto frame = [&]() {
        Frame f{};
        f.image =
            CreateWrappedImage(memory_allocator, VkExtent2D{layout.width, layout.height}, format);
        f.image_view = CreateWrappedImageView(device, f.image, format);
        f.framebuffer = blit_capture.CreateFramebuffer(device, layout, *f.image_view, format);
        return f;
    }();

    auto dst_buffer = CreateWrappedBuffer(memory_allocator, buffer_size, MemoryUsage::Download);
    blit_capture.DrawToFrame(device, rasterizer, &frame, framebuffers, layout, 1, format);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([&](vk::CommandBuffer cmdbuf) {
        DownloadColorImage(cmdbuf, *frame.image, *dst_buffer,
                           VkExtent3D{layout.width, layout.height, 1});
    });

    // Ensure the copy is fully completed before saving the capture
    scheduler.Finish();

    // Copy backing image data to the capture buffer
    dst_buffer.Invalidate();
    return dst_buffer;
}

void RendererVulkan::RenderScreenshot(std::span<const Tegra::FramebufferConfig> framebuffers) {
    if (!renderer_settings.screenshot_requested) {
        return;
    }

    const auto& layout{renderer_settings.screenshot_framebuffer_layout};
    const auto dst_buffer = RenderToBuffer(framebuffers, layout, VK_FORMAT_B8G8R8A8_UNORM,
                                           layout.width * layout.height * 4);

    std::memcpy(renderer_settings.screenshot_bits, dst_buffer.Mapped().data(),
                dst_buffer.Mapped().size());
    renderer_settings.screenshot_complete_callback(false);
    renderer_settings.screenshot_requested = false;
}

std::vector<u8> RendererVulkan::GetAppletCaptureBuffer() {
    using namespace VideoCore::Capture;

    std::vector<u8> out(VideoCore::Capture::TiledSize);

    if (!applet_frame.image) {
        return out;
    }

    const auto dst_buffer =
        CreateWrappedBuffer(memory_allocator, VideoCore::Capture::TiledSize, MemoryUsage::Download);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([&](vk::CommandBuffer cmdbuf) {
        DownloadColorImage(cmdbuf, *applet_frame.image, *dst_buffer, CaptureImageExtent);
    });

    // Ensure the copy is fully completed before writing the capture
    scheduler.Finish();

    // Swizzle image data to the capture buffer
    dst_buffer.Invalidate();
    Tegra::Texture::SwizzleTexture(out, dst_buffer.Mapped(), BytesPerPixel, LinearWidth,
                                   LinearHeight, LinearDepth, BlockHeight, BlockDepth);

    return out;
}

void RendererVulkan::RenderAppletCaptureLayer(
    std::span<const Tegra::FramebufferConfig> framebuffers) {
    if (!applet_frame.image) {
        applet_frame.image = CreateWrappedImage(memory_allocator, CaptureImageSize, CaptureFormat);
        applet_frame.image_view = CreateWrappedImageView(device, applet_frame.image, CaptureFormat);
        applet_frame.framebuffer = blit_applet.CreateFramebuffer(device,
            VideoCore::Capture::Layout, *applet_frame.image_view, CaptureFormat);
    }

    scheduler.RequestOutsideRenderPassOperationContext();
    blit_applet.DrawToFrame(device, rasterizer, &applet_frame, framebuffers, VideoCore::Capture::Layout, 1,
                            CaptureFormat);
}

} // namespace Vulkan
