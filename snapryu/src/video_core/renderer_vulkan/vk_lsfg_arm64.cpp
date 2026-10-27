// SPDX-License-Identifier: GPL-3.0-or-later
// Host adapter for the existing, unmodified LSFG ARM64 C ABI. No Anvil/ORT/QNN.
#include "video_core/renderer_vulkan/vk_lsfg_arm64.h"
#include "video_core/renderer_vulkan/vk_snap_go_capture.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include "common/fs/fs_util.h"
#include "common/logging.h"
#include "common/lsfg_runtime.h"
#include "common/settings.h"
#include "video_core/frame_gen/lsfg_cemu.h"
#include "video_core/frame_gen/lossless_dll.h"
#include "video_core/renderer_vulkan/vk_blit_screen.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#if SNAPRYU_DIAGNOSTICS
#include <cstdlib>
#include <string_view>
#include "video_core/renderer_vulkan/vk_queue_lock_trace.h"
#endif
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {
namespace {
s64 Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::filesystem::path ExecutableDirectory() {
    std::array<wchar_t, 32768> path{};
    const auto size = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (!size || size >= path.size()) throw std::runtime_error("Cannot locate executable");
    return std::filesystem::path(std::wstring(path.data(), size)).parent_path();
}
// Dependencies come only from the folder of SnapRYU.exe (user rule): no environment variable,
// no Eden data folder and no Steam installation is searched.
std::filesystem::path LosslessPath() {
    const auto local = ExecutableDirectory() / L"Lossless.dll";
    // Existing external-shader deployments keep their explicit file. If absent,
    // the self-contained DLL selects its embedded original shaders.
    return std::filesystem::is_regular_file(local) ? local : std::filesystem::path{};
}
VkImageMemoryBarrier Barrier(VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
                             VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.image = image; b.oldLayout = old_layout; b.newLayout = new_layout;
    b.srcAccessMask = src; b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}
#if SNAPRYU_DIAGNOSTICS
// SNAPRYU_LSFG_BISECT splits a crash that only happens with LSFG generating:
//   nodll  lsfg_arm64.dll never generates (no Initialize, no pipelines, no submits); the worker copies
//          the current frame into the output instead, the same path a failed generation takes.
//          Everything else in SnapRYU's LSFG path (copies, two presents per frame) stays.
//   drain  each generation is followed by drain() under submit_mutex, so its GPU work is complete
//          before anything else is submitted.
//   single nodll, and no generated frame is acquired or presented: the same copies are recorded,
//          then Composite presents the original once, as without LSFG (b101).
//   noshow nodll, and the generated frame is acquired, prepared, filled and flushed as usual but retired
//          with DiscardRenderedFrame instead of being presented (b103).
enum class LsfgBisect { Off, NoDll, Drain, Single, NoShow };
LsfgBisect BisectMode() {
    static const LsfgBisect mode = [] {
        const char* const value = std::getenv("SNAPRYU_LSFG_BISECT");
        const std::string_view v = value != nullptr ? value : "";
        return v == "nodll"    ? LsfgBisect::NoDll
               : v == "drain"  ? LsfgBisect::Drain
               : v == "single" ? LsfgBisect::Single
               : v == "noshow" ? LsfgBisect::NoShow
                               : LsfgBisect::Off;
    }();
    return mode;
}
const char* BisectName(LsfgBisect mode) {
    return mode == LsfgBisect::NoDll    ? "nodll"
           : mode == LsfgBisect::Drain  ? "drain"
           : mode == LsfgBisect::Single ? "single"
           : mode == LsfgBisect::NoShow ? "noshow"
                                        : "off";
}
bool BisectSkipsDll() {
    return BisectMode() == LsfgBisect::NoDll || BisectMode() == LsfgBisect::Single ||
           BisectMode() == LsfgBisect::NoShow;
}
#endif
vk::Image Image(MemoryAllocator& allocator, VkExtent2D extent, VkFormat format) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = format;
    ci.extent = {extent.width, extent.height, 1}; ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return allocator.CreateImage(ci);
}
void Transfer(Scheduler& scheduler, VkImage src, VkImage dst, VkExtent2D extent,
              bool initialized, bool copy) {
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([=](vk::CommandBuffer cmd) {
        const std::array before{
            Barrier(src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
            Barrier(dst, initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    initialized ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT)};
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, {}, {}, before);
        constexpr VkImageSubresourceLayers layer{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        if (copy) {
            const VkImageCopy region{layer, {}, layer, {}, {extent.width, extent.height, 1}};
            cmd.CopyImage(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region);
        } else {
            const VkImageBlit region{layer, {{0,0,0},{s32(extent.width),s32(extent.height),1}},
                                     layer, {{0,0,0},{s32(extent.width),s32(extent.height),1}}};
            cmd.BlitImage(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region, VK_FILTER_NEAREST);
        }
        const std::array after{
            Barrier(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT),
            Barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)};
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                            0, {}, {}, after);
    });
}
// Same rule as Eden's own frame generation (present/frame_gen.cpp ConfiguredFlowScale):
// auto follows the rendered/presented width in 5% steps, manual is the percent setting.
float FlowScale(VkExtent2D guest, u32 presented_width) {
    if (!Settings::values.frame_gen_flow_scale_auto.GetValue()) {
        return std::clamp(float(Settings::values.frame_gen_flow_scale.GetValue()) / 100.0f,
                          0.25f, 1.0f);
    }
    if (guest.width == 0 || presented_width == 0) return 1.0f;
    const float ratio = float(guest.width) * Settings::values.resolution_info.up_factor /
                        float(presented_width);
    return std::clamp(std::ceil(ratio * 20.0f) / 20.0f, 0.25f, 1.0f);
}
VkFormat EncodedFormat(VkFormat format) {
    if (format == VK_FORMAT_B8G8R8A8_SRGB) return VK_FORMAT_B8G8R8A8_UNORM;
    if (format == VK_FORMAT_R8G8B8A8_SRGB) return VK_FORMAT_R8G8B8A8_UNORM;
    return format;
}
} // namespace

struct Arm64Lsfg::Impl {
    VkInstance instance;
    const Device& device;
    MemoryAllocator& allocator;
    Scheduler& scheduler;
    PresentManager& present;
    Swapchain& swapchain;
    HMODULE library{};
    LsfgCemu* module{};
    LsfgCemuContext* context{};
    std::array<vk::Image, 2> history;
    vk::Image encoded_input, encoded_output, output;
    VkExtent2D extent{}, peak_guest{};
    VkFormat format{};
    float flow{};
    u64 sequence{};
    s64 last_time{};
    double interval{};
    // Also set by the scheduler worker when a queued generation or reset fails.
    std::atomic<bool> failed{};
    bool allocated{};
    bool logged{};     ///< Scheduler worker only.
    s64 stats_time{};  ///< Scheduler worker only.
#if SNAPRYU_DIAGNOSTICS
    u64 bisect_frames{};  ///< Scheduler worker only.
#endif
#define LSFG_PROC(name) decltype(&lsfg_cemu_##name) name{}
    LSFG_PROC(create); LSFG_PROC(set_enabled); LSFG_PROC(create_context);
    LSFG_PROC(generate_async); LSFG_PROC(reset_history); LSFG_PROC(drain);
    LSFG_PROC(destroy_context); LSFG_PROC(destroy); LSFG_PROC(get_stats);
#undef LSFG_PROC
    Impl(VkInstance i, const Device& d, MemoryAllocator& a, Scheduler& s,
         PresentManager& p, Swapchain& sc)
        : instance{i}, device{d}, allocator{a}, scheduler{s}, present{p}, swapchain{sc} {}
    static void Check(LsfgCemuStatus status, const LsfgCemuError& error) {
        if (status != LSFG_CEMU_OK) throw std::runtime_error(
            "LSFG status " + std::to_string(status) + ": " + error.message);
    }
    void Load() {
        if (module) return;
        // ABI v2 (generate_async), deployed as lsfg_arm64.dll with this host.
        const auto path = ExecutableDirectory() / L"lsfg_arm64.dll";
        // Its own imports resolve from its folder or System32 only (no PATH, no user DLL dirs).
        library = LoadLibraryExW(path.c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library) throw std::runtime_error("Cannot load lsfg_arm64.dll (Win32 " +
                                               std::to_string(GetLastError()) + ")");
#define LOAD(name) name = reinterpret_cast<decltype(name)>(GetProcAddress(library, "lsfg_cemu_" #name)); \
    if (!name) throw std::runtime_error("Missing LSFG export: " #name)
        LOAD(create); LOAD(set_enabled); LOAD(create_context); LOAD(generate_async); LOAD(reset_history);
        LOAD(drain); LOAD(destroy_context); LOAD(destroy); LOAD(get_stats);
#undef LOAD
        const auto lossless = LosslessPath();
        const auto& dispatch = device.GetDispatchLoader();
        LsfgCemuCreateInfo ci{};
        ci.struct_size = sizeof(ci); ci.abi_version = LSFG_CEMU_ABI_VERSION;
        ci.instance = instance; ci.physical_device = device.GetPhysical();
        ci.device = *device.GetLogical();
        ci.queue_family_index = device.GetGraphicsFamily();
        dispatch.vkGetDeviceQueue(ci.device, ci.queue_family_index, 0, &ci.queue);
        ci.get_instance_proc_addr = dispatch.vkGetInstanceProcAddr;
        ci.get_device_proc_addr = dispatch.vkGetDeviceProcAddr;
        ci.lossless_dll_path = lossless.empty() ? nullptr : lossless.c_str();
        ci.gpu_wait_timeout_ns = 5'000'000'000ULL;
        ci.timeline_semaphore_enabled = device.IsTimelineSemaphoreEnabled();
        ci.shader_storage_image_extended_formats_enabled = device.HasStorageImageExtendedFormats();
        // Same choice as Eden's own frame generation (present/lsfg_shaders.cpp).
        const bool fp16 = device.IsFloat16Supported() && Settings::values.frame_gen_fp16.GetValue();
        ci.fp16_shaders_enabled = fp16 ? VK_TRUE : VK_FALSE;
        LsfgCemuError error{};
        Check(create(&ci, &module, &error), error);
        Check(set_enabled(module, VK_TRUE, &error), error);
        LOG_INFO(Render_Vulkan, "LSFG ARM64 module loaded: {}; original shader source: {}; Quality {} 2X; GPU only, queue-ordered",
                 Common::FS::PathToUTF8String(path),
                 lossless.empty() ? "embedded" : Common::FS::PathToUTF8String(lossless),
                 fp16 ? "FP16" : "FP32");
#if SNAPRYU_DIAGNOSTICS
        LOG_INFO(Render_Vulkan, "LSFG_BISECT mode={}", BisectName(BisectMode()));
#endif
    }
    void Fail(const char* what) noexcept {
        failed.store(true);
        Settings::LsfgRuntime::active.store(false);
        try {
            Settings::LsfgRuntime::Failure(what);
            LOG_ERROR(Render_Vulkan, "LSFG ARM64 unavailable; original frames only: {}", what);
        } catch (...) {
        }
    }
    void ResetHost() {
        sequence = 0; last_time = 0; interval = 0;
        Settings::LsfgRuntime::active.store(false);
    }
    void Reset() {
        // History is reset on the worker, in queue order after the last queued generation.
        if (sequence && context) scheduler.Record([this](vk::CommandBuffer) { ResetModule(); });
        ResetHost();
    }
    // Scheduler worker.
    void ResetModule() noexcept {
        if (failed.load()) return;
        try {
#if SNAPRYU_DIAGNOSTICS
            QueueLockTrace::Scope queue_trace{QueueLockTrace::Owner::LsfgReset};
#endif
            std::scoped_lock lock{scheduler.submit_mutex};
#if SNAPRYU_DIAGNOSTICS
            queue_trace.Acquired();
#endif
            LsfgCemuError error{};
            Check(drain(context, 5'000'000'000ULL, &error), error);
            Check(reset_history(context, &error), error);
        } catch (const std::exception& e) {
            Fail(e.what());
        }
    }
    void Ensure(u32 width, u32 height, float next_flow) {
        Load();
        const auto next_format = EncodedFormat(swapchain.GetImageFormat());
        if (allocated && width == extent.width && height == extent.height && next_format == format &&
            next_flow == flow) return;
        // Queued generations and resets use the context and images replaced below.
        scheduler.Finish();
        scheduler.WaitWorker();
        if (context) {
#if SNAPRYU_DIAGNOSTICS
            QueueLockTrace::Scope queue_trace{QueueLockTrace::Owner::LsfgDestroyContext};
#endif
            std::scoped_lock lock{scheduler.submit_mutex};
#if SNAPRYU_DIAGNOSTICS
            queue_trace.Acquired();
#endif
            LsfgCemuError error{};
            Check(destroy_context(&context, &error), error);
        }
        if (next_format != VK_FORMAT_R8G8B8A8_UNORM && next_format != VK_FORMAT_B8G8R8A8_UNORM)
            throw std::runtime_error("LSFG ARM64 requires SDR RGBA/BGRA8 output");
        for (const auto fmt : {next_format, VK_FORMAT_R8G8B8A8_UNORM}) {
            const auto flags = device.GetPhysical().GetFormatProperties(fmt).optimalTilingFeatures;
            if ((flags & (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) !=
                (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT))
                throw std::runtime_error("LSFG color-conversion blit is unsupported");
        }
        extent = {width, height}; format = next_format; flow = next_flow;
        for (auto& image : history) image = Image(allocator, extent, VK_FORMAT_R8G8B8A8_UNORM);
        output = Image(allocator, extent, VK_FORMAT_R8G8B8A8_UNORM);
        encoded_input = Image(allocator, extent, format);
        encoded_output = Image(allocator, extent, format);
        allocated = true;
        LsfgCemuContextInfo ci{sizeof(ci), width, height, VK_FORMAT_R8G8B8A8_UNORM, flow};
        LsfgCemuError error{};
        Check(create_context(module, &ci, &context, &error), error);
        ResetHost();
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([image = *output](vk::CommandBuffer cmd) {
            const auto barrier = Barrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                          0, VK_ACCESS_TRANSFER_WRITE_BIT);
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                0, barrier);
        });
        LOG_INFO(Render_Vulkan, "LSFG ARM64 processing native presentation extent {}x{} (no 720p downscale), flow scale {}%",
                 width, height, int(std::lround(flow * 100.0f)));
    }
    bool Process(Frame* frame, const Layout::FramebufferLayout& layout, BlitScreen& blit,
                 VkExtent2D guest, SnapGoCapture* capture, bool remote_only) {
        if (failed.load()) {
            // A worker failure can land after the previous frame set active.
            Settings::LsfgRuntime::active.store(false);
            return false;
        }
        if (!Settings::values.frame_gen.GetValue() || Settings::LsfgRuntime::paused.load()) {
            if (sequence) Reset();
            return false;
        }
        peak_guest.width = (std::max)(peak_guest.width, guest.width);
        peak_guest.height = (std::max)(peak_guest.height, guest.height);
        Ensure(frame->width, frame->height, FlowScale(peak_guest, frame->width));
        const s64 now = Now();
        if (last_time && now - last_time > 1'000'000'000) Reset();
        if (last_time) interval = interval ? interval * .8 + double(now - last_time) * .2 : double(now - last_time);
        last_time = now;
        const u64 id = ++sequence;
        // Copy into UNORM first to preserve encoded RGB bytes on an SRGB swapchain.
        // Only the subsequent UNORM-to-UNORM blit swaps BGRA/RGBA channels.
        Transfer(scheduler, *frame->image, *encoded_input, extent, id > 1, true);
        Transfer(scheduler, *encoded_input, *history[id % 2], extent, id > 2,
                 format == VK_FORMAT_R8G8B8A8_UNORM);
        if (id == 1) return false;
#if SNAPRYU_DIAGNOSTICS
        if (BisectMode() == LsfgBisect::Single) {
            scheduler.Flush();
            const LsfgCemuGenerateInfo single{sizeof(LsfgCemuGenerateInfo), *history[(id - 1) % 2],
                                              *history[id % 2], *output, id - 1, id, VK_NULL_HANDLE};
            scheduler.Record([this, single](vk::CommandBuffer cmd) { Generate(single, cmd); });
            return false;
        }
#endif
        // No CPU wait for the GPU here. The worker submits in FIFO order on the one
        // graphics queue: this frame's work, then the generation, then the generated
        // frame's copies recorded below.
        scheduler.Flush();
        const LsfgCemuGenerateInfo gi{sizeof(LsfgCemuGenerateInfo), *history[(id - 1) % 2],
                                      *history[id % 2], *output, id - 1, id, VK_NULL_HANDLE};
        scheduler.Record([this, gi](vk::CommandBuffer cmd) { Generate(gi, cmd); });
        Frame* generated = present.GetRenderFrame();
        if (!generated)
            return true;
        blit.PrepareFrame(device, generated, layout);
        Transfer(scheduler, *output, *encoded_output, extent, id > 2,
                 format == VK_FORMAT_R8G8B8A8_UNORM);
        Transfer(scheduler, *encoded_output, *generated->image, extent, false, true);
        if (capture) {
            const s64 capture_half = s64(std::clamp(interval > 0 ? interval * .5 : 16'666'667.,
                                                   4'166'667., 50'000'000.));
            capture->Pair(generated, frame, swapchain.GetImageFormat(), layout, capture_half);
        }
        scheduler.Flush(*generated->render_ready);
        const s64 display_time = Now();
        const s64 half_interval = s64(std::clamp(interval > 0 ? interval * .5 : 16'666'667.,
                                                4'166'667., 50'000'000.));
        // The original follows half an interval after the generated frame is really
        // ready (as when this thread waited for the generation), not after its submit.
#if SNAPRYU_DIAGNOSTICS
        if (BisectMode() == LsfgBisect::NoShow) {
            present.DiscardRenderedFrame(generated);
        } else
#endif
        if (remote_only) present.DiscardRenderedFrame(generated);
        else present.Present(generated, display_time, true, half_interval);
        scheduler.Flush(*frame->render_ready);
        if (remote_only) present.DiscardRenderedFrame(frame);
        else present.Present(frame, display_time + half_interval, false);
        scheduler.DispatchWork();
        Settings::LsfgRuntime::active.store(true);
        return true;
    }
    // Scheduler worker, between this frame's submit and the generated frame's copies,
    // which are recorded into cmd after this returns.
    void Generate(const LsfgCemuGenerateInfo& gi, vk::CommandBuffer cmd) noexcept {
#if SNAPRYU_DIAGNOSTICS
        if (BisectMode() != LsfgBisect::Off && ++bisect_frames % 600 == 1) {
            LOG_INFO(Render_Vulkan, "LSFG_BISECT mode={} frames={}", BisectName(BisectMode()),
                     bisect_frames);
        }
        if (!BisectSkipsDll() && !failed.load()) {
#else
        if (!failed.load()) {
#endif
            try {
#if SNAPRYU_DIAGNOSTICS
                QueueLockTrace::Scope queue_trace{QueueLockTrace::Owner::LsfgGenerate};
#endif
                std::scoped_lock lock{scheduler.submit_mutex};
#if SNAPRYU_DIAGNOSTICS
                queue_trace.Acquired();
#endif
                LsfgCemuError error{};
                Check(generate_async(context, &gi, &error), error);
#if SNAPRYU_DIAGNOSTICS
                if (BisectMode() == LsfgBisect::Drain) {
                    Check(drain(context, 5'000'000'000ULL, &error), error);
                }
#endif
                LogStats();
                return;
            } catch (const std::exception& e) {
                Fail(e.what());
            }
        }
        // The generated frame is already queued: show the current frame again instead of
        // a stale or undefined output.
        const std::array before{
            Barrier(gi.current_image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
            Barrier(gi.output_image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT)};
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, {}, {}, before);
        constexpr VkImageSubresourceLayers layer{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        const VkImageCopy region{layer, {}, layer, {}, {extent.width, extent.height, 1}};
        cmd.CopyImage(gi.current_image, VK_IMAGE_LAYOUT_GENERAL, gi.output_image,
                      VK_IMAGE_LAYOUT_GENERAL, region);
        const auto after = Barrier(gi.output_image, VK_IMAGE_LAYOUT_GENERAL,
                                   VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                   VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                            0, after);
    }
    // Scheduler worker, under submit_mutex.
    void LogStats() {
        const s64 now = Now();
        if (logged && now - stats_time < 10'000'000'000) return;
        LsfgCemuStats stats{}; stats.struct_size = sizeof(stats);
        LsfgCemuError error{};
        Check(get_stats(module, &stats, &error), error);
        if (!logged) {
            LOG_INFO(Render_Vulkan, "LSFG ARM64 real generation submitted: extracted={} shaders={} pipelines={} warmups={} outputs={}",
                     stats.shader_extraction_pass, stats.shader_modules, stats.compute_pipelines,
                     stats.warmup_generations, stats.generated_outputs);
            logged = true;
        } else {
            // previous_waits counts generations the GPU had not finished when the next
            // frame arrived; it stays near 0 when LSFG fits in the frame time.
            LOG_INFO(Render_Vulkan, "LSFG_ASYNC generations={} previous_waits={} history_resets={}",
                     stats.async_generations, stats.async_previous_waits, stats.history_resets);
        }
        stats_time = now;
    }
    bool Shutdown() {
        Settings::LsfgRuntime::active.store(false);
        if (allocated) {
            scheduler.Finish();
            scheduler.WaitWorker();
        }
#if SNAPRYU_DIAGNOSTICS
        QueueLockTrace::Scope queue_trace{QueueLockTrace::Owner::LsfgShutdown};
#endif
        std::scoped_lock lock{scheduler.submit_mutex};
#if SNAPRYU_DIAGNOSTICS
        queue_trace.Acquired();
#endif
        LsfgCemuError error{};
        if (context) {
            const auto status = drain(context, 5'000'000'000ULL, &error);
            if (status != LSFG_CEMU_OK && status != LSFG_CEMU_DEVICE_LOST) {
                LOG_ERROR(Render_Vulkan, "LSFG drain failed; retaining in-flight resources: {}", error.message);
                return false;
            }
            if (destroy_context(&context, &error) != LSFG_CEMU_OK) return false;
        }
        if (module && destroy(&module, &error) != LSFG_CEMU_OK) return false;
        if (library) { FreeLibrary(library); library = nullptr; }
        return true;
    }
};
Arm64Lsfg::Arm64Lsfg(VkInstance i, const Device& d, MemoryAllocator& a, Scheduler& s,
                     PresentManager& p, Swapchain& sc)
    : impl{std::make_unique<Impl>(i, d, a, s, p, sc)} {}
Arm64Lsfg::~Arm64Lsfg() {
    try { if (impl->Shutdown()) return; }
    catch (const std::exception& e) { LOG_ERROR(Render_Vulkan, "LSFG shutdown: {}", e.what()); }
    // A submitted GPU job must never outlive images or its DLL. On an unrecoverable
    // drain failure retain the bounded session until process exit/device teardown.
    (void)impl.release();
}
bool Arm64Lsfg::Process(Frame* frame, const Layout::FramebufferLayout& layout, BlitScreen& blit,
                        VkExtent2D guest, SnapGoCapture* capture, bool remote_only) {
    try { return impl->Process(frame, layout, blit, guest, capture, remote_only); }
    catch (const std::exception& e) {
        impl->Fail(e.what());
        return false;
    }
}
void Arm64Lsfg::Reset() { impl->Reset(); }
} // namespace Vulkan
