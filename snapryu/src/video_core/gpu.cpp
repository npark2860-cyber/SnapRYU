// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <list>
#include <memory>
#include <utility>

#include "common/assert.h"
#include "common/cgt_trace.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/hle/service/nvdrv/nvdata.h"
#include "core/perf_stats.h"
#include "video_core/cdma_pusher.h"
#include "video_core/control/channel_state.h"
#include "video_core/control/scheduler.h"
#include "video_core/dma_pusher.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/kepler_memory.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/gpu.h"
#include "video_core/gpu_thread.h"
#include "video_core/host1x/host1x.h"
#include "video_core/host1x/syncpoint_manager.h"
#include "video_core/memory_manager.h"
#include "video_core/nce_image_readback.h"
#include "video_core/renderer_base.h"
#include "video_core/shader_notify.h"
#if SNAPRYU_DIAGNOSTICS
#include "common/nce_write_note.h"
#include "video_core/texture_cache/cpu_write_trace.h"
#include "video_core/texture_cache/gpu_loss_trace.h"
#endif

namespace Tegra {

namespace {
constexpr u64 GpuClockMultiplier(Settings::GpuClock clock) {
    switch (clock) {
    case Settings::GpuClock::Boost:
        return 256;
    case Settings::GpuClock::Overclock:
        return 512;
    default:
        return 1;
    }
}

// EDEN_NCE_IMAGE_READBACK (nce_image_readback.h). Counts preserving page invalidations whose page
// holds an image with GPU-written contents, and returns true when "1" wants them downloaded first.
#if SNAPRYU_DIAGNOSTICS
bool NceImageReadbackCheck(VideoCore::RasterizerInterface* rasterizer, DAddr addr, u64 size) {
    const NceImageReadback::Mode mode = NceImageReadback::GetMode();
    if (mode == NceImageReadback::Mode::Off) {
        return false;
    }
    auto& counters = NceImageReadback::Get();
    const u64 check = counters.checks.fetch_add(1, std::memory_order_relaxed) + 1;
    // TextureCache::IsRegionGpuModified. The rasterizer answers this only at High GPU accuracy.
    const bool hit =
        rasterizer->MustFlushRegion(addr, size, VideoCommon::CacheType::TextureCache);
    const bool download = hit && mode == NceImageReadback::Mode::Readback;
    if (hit) {
        const u64 hits = counters.hits.fetch_add(1, std::memory_order_relaxed) + 1;
        if (NceImageReadback::Sample(hits)) {
            LOG_INFO(HW_GPU, "NCE_IMAGE_READBACK hit={} checks={} addr=0x{:x} size={} download={}",
                     hits, check, addr, size, download);
        }
    }
    if (NceImageReadback::Sample(check)) {
        LOG_INFO(HW_GPU,
                 "NCE_IMAGE_READBACK checks={} hits={} completed={} wait_ms={} gpu_level_high={}",
                 check, counters.hits.load(std::memory_order_relaxed),
                 counters.completed.load(std::memory_order_relaxed),
                 counters.wait_us.load(std::memory_order_relaxed) / 1000,
                 Settings::IsGPULevelHigh());
    }
    return download;
}

void NceImageReadbackDone(DAddr addr, u64 size, u64 wait_us) {
    auto& counters = NceImageReadback::Get();
    const u64 total_us =
        counters.wait_us.fetch_add(wait_us, std::memory_order_relaxed) + wait_us;
    const u64 done = counters.completed.fetch_add(1, std::memory_order_relaxed) + 1;
    if (NceImageReadback::Sample(done)) {
        LOG_INFO(HW_GPU,
                 "NCE_IMAGE_READBACK completed={} addr=0x{:x} size={} wait_us={} total_wait_ms={}",
                 done, addr, size, wait_us, total_us / 1000);
    }
}
#endif
} // Anonymous namespace

struct GPU::Impl {
    explicit Impl(Core::System& system_, bool is_async_, bool use_nvdec_)
        : system{system_}
        , use_nvdec{use_nvdec_}
        , shader_notify()
        , is_async{is_async_}
        , gpu_thread{system_}
    {}

    ~Impl() = default;

    std::shared_ptr<Control::ChannelState> CreateChannel(s32 channel_id) {
        auto channel_state = std::make_shared<Tegra::Control::ChannelState>(channel_id);
        channels.emplace(channel_id, channel_state);
        scheduler.DeclareChannel(channel_state);
        return channel_state;
    }

    void BindChannel(s32 channel_id) {
        if (bound_channel != channel_id) {
            auto it = channels.find(channel_id);
            ASSERT(it != channels.end());
            bound_channel = channel_id;
            current_channel = it->second.get();
            renderer->ReadRasterizer()->BindChannel(*current_channel);
        }
    }

    std::shared_ptr<Control::ChannelState> AllocateChannel() {
        return CreateChannel(new_channel_id++);
    }

    void InitChannel(Control::ChannelState& to_init, u64 program_id) {
        to_init.Init(system, program_id);
        to_init.BindRasterizer(renderer->ReadRasterizer());
        renderer->ReadRasterizer()->InitializeChannel(to_init);
    }

    void InitAddressSpace(Tegra::MemoryManager& memory_manager) {
        memory_manager.BindRasterizer(renderer->ReadRasterizer());
    }

    void ReleaseChannel(Control::ChannelState& to_release) {
        UNIMPLEMENTED();
    }

    /// Binds a renderer to the GPU.
    void BindRenderer(std::unique_ptr<VideoCore::RendererBase> renderer_) {
        renderer = std::move(renderer_);
        system.Host1x().memory_manager.BindInterface(renderer->ReadRasterizer());
        system.Host1x().gmmu_manager.BindRasterizer(renderer->ReadRasterizer());
    }

    /// Flush all current written commands into the host GPU for execution.
    void FlushCommands() {
        renderer->ReadRasterizer()->FlushCommands();
    }

    /// Synchronizes CPU writes with Host GPU memory.
    void InvalidateGPUCache() {
        if (Common::CGT::Enabled()) [[unlikely]] {
            // EDEN_CGT: each applied range with the guest bytes it holds now (GUEST_READ).
            auto& device_memory = system.Host1x().MemoryManager();
            const u64 gather_id = Common::CGT::GatherBegin();
            std::function<void(PAddr, size_t)> cgt_writes(
                [this, &device_memory, gather_id](PAddr address, size_t size) {
                    Common::CGT::GatherRange(gather_id, address, size,
                                             [&](void* dst, size_t bytes) {
                                                 device_memory.ReadBlockUnsafe(address, dst, bytes);
                                             });
                    renderer->ReadRasterizer()->OnCacheInvalidation(address, size);
                });
            {
                const Common::CGT::SiteScope cgt_site{Common::CGT::Site::Gather};
                system.GatherGPUDirtyMemory(cgt_writes);
            }
            Common::CGT::GatherEnd(gather_id);
            return;
        }
        std::function<void(PAddr, size_t)> callback_writes([this](PAddr address, size_t size) {
            renderer->ReadRasterizer()->OnCacheInvalidation(address, size);
        });
        system.GatherGPUDirtyMemory(callback_writes);
    }

    /// Signal the ending of command list.
    void OnCommandListEnd() {
        renderer->ReadRasterizer()->ReleaseFences(false);
        Settings::UpdateGPUAccuracy();
        renderer->ReadRasterizer()->RefreshGPUSettings();
    }

    /// Request a host GPU memory flush from the CPU.
    template <typename Func>
    [[nodiscard]] u64 RequestSyncOperation(Func&& action) {
        std::unique_lock lck{sync_request_mutex};
        const u64 fence = ++last_sync_fence;
        sync_requests.emplace_back(std::forward<Func>(action));
        return fence;
    }

    /// Obtains current flush request fence id.
    [[nodiscard]] u64 CurrentSyncRequestFence() const {
        return current_sync_fence.load(std::memory_order_relaxed);
    }

    void WaitForSyncOperation(const u64 fence) {
        std::unique_lock lck{sync_request_mutex};
        sync_request_cv.wait(lck, [this, fence] { return CurrentSyncRequestFence() >= fence; });
    }

    /// Tick pending requests within the GPU.
    void TickWork() {
        std::unique_lock lck{sync_request_mutex};
        while (!sync_requests.empty()) {
            auto request = std::move(sync_requests.front());
            sync_requests.pop_front();
            sync_request_mutex.unlock();
            request();
            current_sync_fence.fetch_add(1, std::memory_order_release);
            sync_request_mutex.lock();
            sync_request_cv.notify_all();
        }
    }

    [[nodiscard]] u64 GetTicks() const {
        const u64 gpu_tick = system.CoreTiming().GetGPUTicks();
        return gpu_tick / GpuClockMultiplier(Settings::values.gpu_clock.GetValue());
    }

    void RendererFrameEndNotify() {
        system.GetPerfStats().EndGameFrame();
    }

    /// Performs any additional setup necessary in order to begin GPU emulation.
    /// This can be used to launch any necessary threads and register any necessary
    /// core timing events.
    void Start() {
        Settings::UpdateGPUAccuracy();
        renderer->ReadRasterizer()->RefreshGPUSettings();
        gpu_thread.StartThread(*renderer, renderer->Context(), scheduler);
    }

    void NotifyShutdown() {
        std::unique_lock lk{sync_mutex};
        shutting_down.store(true, std::memory_order::relaxed);
        sync_cv.notify_all();
    }

    /// Obtain the CPU Context
    void ObtainContext() {
        if (!cpu_context) {
            cpu_context = renderer->GetRenderWindow().CreateSharedContext();
        }
        cpu_context->MakeCurrent();
    }

    /// Release the CPU Context
    void ReleaseContext() {
        cpu_context->DoneCurrent();
    }

    /// Push GPU command entries to be processed
    void PushGPUEntries(s32 channel, Tegra::CommandList&& entries) {
        gpu_thread.SubmitList(channel, std::move(entries), is_async);
    }

    /// Notify rasterizer that any caches of the specified region should be flushed to Switch memory
    void FlushRegion(DAddr addr, u64 size) {
        gpu_thread.FlushRegion(addr, size, is_async);
    }

    VideoCore::RasterizerDownloadArea OnCPURead(DAddr addr, u64 size) {
        auto raster_area = renderer->ReadRasterizer()->GetFlushArea(addr, size);
        if (raster_area.preemtive) {
            return raster_area;
        }
        raster_area.preemtive = true;
        const u64 fence = RequestSyncOperation([this, &raster_area]() {
            renderer->ReadRasterizer()->FlushRegion(raster_area.start_address, raster_area.end_address - raster_area.start_address);
        });
        gpu_thread.TickGPU(is_async);
        WaitForSyncOperation(fence);
        return raster_area;
    }

    /// Notify rasterizer that any caches of the specified region should be invalidated
    void InvalidateRegion(DAddr addr, u64 size, bool preserve_gpu_writes) {
#if SNAPRYU_DIAGNOSTICS
        if (Common::CGT::Enabled()) [[unlikely]] {
            Common::CGT::RegionInvalidate(addr, size, preserve_gpu_writes);
        }
        // Existing local implementation of upstream #4490; the isolated launcher enables it.
        static const bool readback_enabled = [] {
            const char* const value = std::getenv("EDEN_NCE_GPU_READBACK");
            const bool enabled = value != nullptr && value[0] != '\0' && value[0] != '0';
            LOG_INFO(HW_GPU, "NCE_READBACK_4490 enabled={} boundary=before_cache_invalidation", enabled);
            return enabled;
        }();
        static std::atomic<u64> calls{0};
        static std::atomic<u64> preserve_calls{0};
        static std::atomic<u64> requested{0};
        static std::atomic<u64> completed{0};
        const u64 call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (preserve_gpu_writes) {
            preserve_calls.fetch_add(1, std::memory_order_relaxed);
        }
        const auto sample = [](u64 count) { return count <= 4 || (count & (count - 1)) == 0; };
        const auto record_call = [&] {
            if (sample(call)) {
                LOG_INFO(HW_GPU,
                         "NCE_READBACK_4490 calls={} preserve_calls={} requested={} completed={} "
                         "addr=0x{:x} size={} preserve={}",
                         call, preserve_calls.load(std::memory_order_relaxed),
                         requested.load(std::memory_order_relaxed),
                         completed.load(std::memory_order_relaxed), addr, size, preserve_gpu_writes);
            }
        };
        VideoCore::RasterizerInterface* rasterizer = renderer->ReadRasterizer();
        // EDEN_NCE_IMAGE_READBACK: OnCacheInvalidation below marks every image on the page
        // CpuModified, and the next use uploads guest RAM over GPU-drawn contents. "1" downloads
        // those images into guest RAM first, in the same sync operation as the #4490 buffers.
        const bool image_readback =
            preserve_gpu_writes && NceImageReadbackCheck(rasterizer, addr, size);
        const bool buffer_readback =
            readback_enabled && preserve_gpu_writes &&
            rasterizer->MustFlushRegion(addr, size, VideoCommon::CacheType::BufferCache);
        if (buffer_readback || image_readback) {
            VideoCommon::CacheType which = VideoCommon::CacheType::None;
            if (buffer_readback) {
                which |= VideoCommon::CacheType::BufferCache;
            }
            if (image_readback) {
                which |= VideoCommon::CacheType::TextureCache;
            }
            const u64 request =
                buffer_readback ? requested.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
            const bool sampled = buffer_readback && sample(request);
            const bool timed = sampled || image_readback;
            const auto started = timed ? std::chrono::steady_clock::now()
                                       : std::chrono::steady_clock::time_point{};
            if (sampled) {
                LOG_INFO(HW_GPU, "NCE_READBACK_4490 request={} addr=0x{:x} size={}", request, addr, size);
            }
            const u64 fence = RequestSyncOperation([rasterizer, addr, size, which] {
                rasterizer->FlushRegion(addr, size, which);
                rasterizer->OnCacheInvalidation(addr, size);
            });
            gpu_thread.TickGPU(is_async);
            WaitForSyncOperation(fence);
            const u64 elapsed =
                timed ? static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                             std::chrono::steady_clock::now() - started)
                                             .count())
                      : 0;
            if (buffer_readback) {
                const u64 done = completed.fetch_add(1, std::memory_order_relaxed) + 1;
                if (sampled) {
                    LOG_INFO(HW_GPU,
                             "NCE_READBACK_4490 request={} completed={} addr=0x{:x} size={} wait_us={}",
                             request, done, addr, size, elapsed);
                }
            }
            if (image_readback) {
                NceImageReadbackDone(addr, size, elapsed);
            }
            record_call();
            return;
        }
        if (preserve_gpu_writes) {
            // CPUW_TRACE (b71): every noted NCE store's page, also pages no tile image covers
            // (an empty note, as in the exception handler, is not recorded).
            VideoCommon::CpuWriteTrace::NotePage(addr, size, Common::NceWriteNote::current);
        }
        gpu_thread.InvalidateRegion(addr, size);
        record_call();
#else
        static_cast<void>(preserve_gpu_writes);
        gpu_thread.InvalidateRegion(addr, size);
#endif
    }

    bool OnCPUWrite(DAddr addr, u64 size) {
        return renderer->ReadRasterizer()->OnCPUWrite(addr, size);
    }

    /// Notify rasterizer that any caches of the specified region should be flushed and invalidated
    void FlushAndInvalidateRegion(DAddr addr, u64 size) {
        gpu_thread.FlushAndInvalidateRegion(addr, size, is_async);
    }

    void RequestComposite(std::vector<Tegra::FramebufferConfig>&& layers, std::vector<Service::Nvidia::NvFence>&& fences) {
        const size_t num_fences{fences.size()};
        size_t current_request_counter{};
        if (num_fences != 0) {
            std::unique_lock<std::mutex> lk(request_swap_mutex);
            if (free_swap_counters.empty()) {
                current_request_counter = request_swap_counters.size();
                request_swap_counters.emplace_back(num_fences);
            } else {
                current_request_counter = free_swap_counters.front();
                request_swap_counters[current_request_counter] = num_fences;
                free_swap_counters.pop_front();
            }
        }
        pending_composite_fence = RequestSyncOperation(
            [this, current_request_counter, num_fences, composite_layers = std::move(layers),
             composite_fences = std::move(fences)] {
                if (num_fences == 0) {
                    renderer->Composite(composite_layers);
                    return;
                }
                auto& syncpoint_manager = system.Host1x().GetSyncpointManager();
                const auto executer = [this, current_request_counter, composite_layers]() {
                    {
                        std::unique_lock<std::mutex> lk(request_swap_mutex);
                        if (--request_swap_counters[current_request_counter] != 0) {
                            return;
                        }
                        free_swap_counters.push_back(current_request_counter);
                    }
                    renderer->Composite(composite_layers);
                };
                for (size_t i = 0; i < num_fences; i++) {
                    syncpoint_manager.RegisterGuestAction(composite_fences[i].id,
                                                          composite_fences[i].value, executer);
                }
            });
        gpu_thread.TickGPU(is_async);
    }

    void WaitForComposite() {
        const u64 fence = pending_composite_fence;
        if (fence == 0) {
            return;
        }
        pending_composite_fence = 0;
        if (shutting_down.load(std::memory_order_relaxed)) {
            return;
        }
        WaitForSyncOperation(fence);
    }

    std::vector<u8> GetAppletCaptureBuffer() {
        std::vector<u8> out;

        const auto wait_fence =
            RequestSyncOperation([&] { out = renderer->GetAppletCaptureBuffer(); });
        gpu_thread.TickGPU(is_async);
        WaitForSyncOperation(wait_fence);

        return out;
    }

    Core::System& system;

    Tegra::Control::Scheduler scheduler;
    ankerl::unordered_dense::map<s32, std::shared_ptr<Tegra::Control::ChannelState>> channels;

    std::unique_ptr<VideoCore::RendererBase> renderer;
    const bool use_nvdec;

    s32 new_channel_id{1};
    /// Shader build notifier
    VideoCore::ShaderNotify shader_notify;
    /// When true, we are about to shut down emulation session, so terminate outstanding tasks
    std::atomic_bool shutting_down{};

    std::array<std::atomic<u32>, Service::Nvidia::MaxSyncPoints> syncpoints{};

    std::array<std::list<u32>, Service::Nvidia::MaxSyncPoints> syncpt_interrupts;

    std::mutex sync_mutex;
    std::mutex device_mutex;

    std::condition_variable sync_cv;

    std::list<std::function<void()>> sync_requests;
    std::atomic<u64> current_sync_fence{};
    u64 last_sync_fence{};
    std::mutex sync_request_mutex;
    std::condition_variable sync_request_cv;

    const bool is_async;

    VideoCommon::GPUThread::ThreadManager gpu_thread;
    std::unique_ptr<Core::Frontend::GraphicsContext> cpu_context;

    Tegra::Control::ChannelState* current_channel;
    s32 bound_channel{-1};

    std::deque<size_t> free_swap_counters;
    std::deque<size_t> request_swap_counters;
    std::mutex request_swap_mutex;
    u64 pending_composite_fence{};
};

GPU::GPU(Core::System& system, bool is_async, bool use_nvdec)
    : impl{std::make_unique<Impl>(system, is_async, use_nvdec)}
{}

GPU::~GPU() = default;

std::shared_ptr<Control::ChannelState> GPU::AllocateChannel() {
    return impl->AllocateChannel();
}

void GPU::InitChannel(Control::ChannelState& to_init, u64 program_id) {
    impl->InitChannel(to_init, program_id);
}

void GPU::BindChannel(s32 channel_id) {
    impl->BindChannel(channel_id);
}

void GPU::ReleaseChannel(Control::ChannelState& to_release) {
    impl->ReleaseChannel(to_release);
}

void GPU::InitAddressSpace(Tegra::MemoryManager& memory_manager) {
    impl->InitAddressSpace(memory_manager);
}

void GPU::BindRenderer(std::unique_ptr<VideoCore::RendererBase> renderer) {
    impl->BindRenderer(std::move(renderer));
}

void GPU::FlushCommands() {
    impl->FlushCommands();
}

void GPU::InvalidateGPUCache() {
    impl->InvalidateGPUCache();
}

void GPU::OnCommandListEnd() {
    impl->OnCommandListEnd();
}

u64 GPU::RequestFlush(DAddr addr, std::size_t size) {
    return impl->RequestSyncOperation([this, addr, size]() {
        impl->renderer->ReadRasterizer()->FlushRegion(addr, size);
    });
}

u64 GPU::CurrentSyncRequestFence() const {
    return impl->CurrentSyncRequestFence();
}

void GPU::WaitForSyncOperation(u64 fence) {
    return impl->WaitForSyncOperation(fence);
}

void GPU::TickWork() {
    impl->TickWork();
}

/// Gets a mutable reference to the Host1x interface
Host1x::Host1x& GPU::Host1x() {
    return impl->system.Host1x();
}

/// Gets an immutable reference to the Host1x interface.
const Host1x::Host1x& GPU::Host1x() const {
    return impl->system.Host1x();
}

Engines::Maxwell3D& GPU::Maxwell3D() {
    return impl->current_channel->payload->maxwell_3d;
}

const Engines::Maxwell3D& GPU::Maxwell3D() const {
    return impl->current_channel->payload->maxwell_3d;
}

Engines::KeplerCompute& GPU::KeplerCompute() {
    return impl->current_channel->payload->kepler_compute;
}

const Engines::KeplerCompute& GPU::KeplerCompute() const {
    return impl->current_channel->payload->kepler_compute;
}

Tegra::DmaPusher& GPU::DmaPusher() {
    return impl->current_channel->payload->dma_pusher;
}

const Tegra::DmaPusher& GPU::DmaPusher() const {
    return impl->current_channel->payload->dma_pusher;
}

VideoCore::RendererBase& GPU::Renderer() {
    return *impl->renderer;
}

const VideoCore::RendererBase& GPU::Renderer() const {
    return *impl->renderer;
}

VideoCore::ShaderNotify& GPU::ShaderNotify() {
    return impl->shader_notify;
}

const VideoCore::ShaderNotify& GPU::ShaderNotify() const {
    return impl->shader_notify;
}

void GPU::RequestComposite(std::vector<Tegra::FramebufferConfig>&& layers,
                           std::vector<Service::Nvidia::NvFence>&& fences) {
    impl->RequestComposite(std::move(layers), std::move(fences));
}

void GPU::WaitForComposite() {
    impl->WaitForComposite();
}

std::vector<u8> GPU::GetAppletCaptureBuffer() {
    return impl->GetAppletCaptureBuffer();
}

u64 GPU::GetTicks() const {
    return impl->GetTicks();
}

bool GPU::IsAsync() const {
    return impl->is_async;
}

bool GPU::UseNvdec() const {
    return impl->use_nvdec;
}

void GPU::RendererFrameEndNotify() {
    impl->RendererFrameEndNotify();
}

void GPU::Start() {
    impl->Start();
}

void GPU::NotifyShutdown() {
    impl->NotifyShutdown();
}

void GPU::ObtainContext() {
    impl->ObtainContext();
}

void GPU::ReleaseContext() {
    impl->ReleaseContext();
}

void GPU::PushGPUEntries(s32 channel, Tegra::CommandList&& entries) {
    impl->PushGPUEntries(channel, std::move(entries));
}

VideoCore::RasterizerDownloadArea GPU::OnCPURead(PAddr addr, u64 size) {
    return impl->OnCPURead(addr, size);
}

void GPU::FlushRegion(DAddr addr, u64 size) {
    impl->FlushRegion(addr, size);
}

void GPU::InvalidateRegion(DAddr addr, u64 size, bool preserve_gpu_writes) {
    impl->InvalidateRegion(addr, size, preserve_gpu_writes);
}

bool GPU::OnCPUWrite(DAddr addr, u64 size) {
    return impl->OnCPUWrite(addr, size);
}

void GPU::FlushAndInvalidateRegion(DAddr addr, u64 size) {
    impl->FlushAndInvalidateRegion(addr, size);
}

} // namespace Tegra
