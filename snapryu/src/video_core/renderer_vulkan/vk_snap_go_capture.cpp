// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_vulkan/vk_snap_go_capture.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "common/logging.h"
#include "common/snapryu_go.h"
#include "common/thread.h"
#include "core/frontend/framebuffer_layout.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/snap_go_pacing.h"
#include "video_core/renderer_vulkan/snap_go_transfer.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {
namespace {
using Clock = std::chrono::steady_clock;
constexpr u32 Width = SNAP_GO_WIDTH, Height = SNAP_GO_HEIGHT;
constexpr u64 Bytes = u64{Width} * Height * 4;
u64 NowUs() { return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count(); }
VkFormat EncodedFormat(VkFormat f) {
    if (f == VK_FORMAT_B8G8R8A8_SRGB) return VK_FORMAT_B8G8R8A8_UNORM;
    if (f == VK_FORMAT_R8G8B8A8_SRGB) return VK_FORMAT_R8G8B8A8_UNORM;
    return f;
}
vk::Image Image(MemoryAllocator& allocator,u32 width,u32 height,VkFormat format) {
    VkImageCreateInfo c{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    c.imageType=VK_IMAGE_TYPE_2D;c.format=format;c.extent={width,height,1};
    c.mipLevels=c.arrayLayers=1;c.samples=VK_SAMPLE_COUNT_1_BIT;c.tiling=VK_IMAGE_TILING_OPTIMAL;
    c.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return allocator.CreateImage(c);
}
}

struct SnapGoCapture::Impl {
    struct Slot {
        vk::Buffer download;
        vk::Image encoded, scaled;
        u32 width{},height{};
        VkFormat format{};
        bool initialized{};
        std::atomic<bool> occupied{};
    };
    struct Task {
        std::array<size_t,2> slots{};
        u32 count{},format{},fps{};
        u64 tick{},generation{},epoch{},captured_us{};
        s64 half_ns{};
    };
    const Device& device;
    MemoryAllocator& allocator;
    Scheduler& scheduler;
    std::array<Slot,4> slots;
    std::array<Task,4> tasks{};
    size_t head{},count{};
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    std::atomic<bool> stop{},failed{};
    std::atomic<u64> epoch{1};
    u64 next_capture{},producer_generation{};

    Impl(const Device& d,MemoryAllocator& a,Scheduler& s):device(d),allocator(a),scheduler(s) {}
    ~Impl(){Stop();}
    void Reset() noexcept {
        epoch.fetch_add(1,std::memory_order_acq_rel);next_capture=0;wake.notify_all();
    }
    void Stop() noexcept {
        stop.store(true,std::memory_order_release);wake.notify_all();
        if(worker.joinable())worker.join();
    }
    bool Current(const Task& task) const {
        const auto d=SnapGo::VideoDemand();
        return !stop.load(std::memory_order_acquire)&&task.epoch==epoch.load(std::memory_order_acquire)
            &&d.enabled&&d.generation==task.generation;
    }
    bool WaitUntil(u64 due,const Task& task) {
        for (;;) {
            const auto now = NowUs();
            if (now >= due) break;
            if(!Current(task))return false;
            std::unique_lock lock{mutex};
            wake.wait_for(lock,std::chrono::microseconds{(std::min)(due-now,u64{2000})},[this]{return stop.load();});
        }
        return Current(task);
    }
    bool Publish(const Task& task,size_t index,u64 due,u64 source_pts) {
        if(!WaitUntil(due,task))return false;
        auto& slot=slots[task.slots[index]];
        slot.download.Invalidate();
        const SnapGoVideoFrame frame{sizeof(SnapGoVideoFrame),SNAP_GO_ABI,Width,Height,Width*4,
                                     task.format,task.generation,source_pts,slot.download.Mapped().data(),Bytes};
        return SnapGo::PushVideoFrame(frame);
    }
    void Run() noexcept {
        Common::SetCurrentThreadName("SnapGoCapture");
        u64 last_due{},last_source_pts{},generation{},output_epoch{};
        try {
            for(;;) {
                Task task;
                {
                    std::unique_lock lock{mutex};
                    wake.wait(lock,[this]{return stop.load()||count;});
                    if(stop.load())return;
                    task=tasks[head];head=(head+1)%tasks.size();--count;
                }
                auto& completion=scheduler.GetMasterSemaphore();
                while(!completion.IsFree(task.tick)) {
                    if(stop.load())return;
                    completion.Refresh();
                    if(completion.IsFree(task.tick))break;
                    std::unique_lock lock{mutex};
                    wake.wait_for(lock,std::chrono::milliseconds{1},[this]{return stop.load();});
                }
                if(Current(task)) {
                    if(generation!=task.generation||output_epoch!=task.epoch){
                        generation=task.generation;output_epoch=task.epoch;last_due=last_source_pts=0;
                    }
                    const auto due=SnapGoPacing::NextDue(NowUs(),last_due,task.fps);
                    const auto first_pts=task.count==2
                        ? SnapGoPacing::GeneratedSourcePts(task.captured_us,task.half_ns,task.fps)
                        : task.captured_us;
                    bool generated_sent=false;
                    if(SnapGoPacing::IsNewSourcePts(first_pts,last_source_pts)&&Publish(task,0,due,first_pts)){
                        last_due=due;last_source_pts=first_pts;generated_sent=true;
                    }
                    if(task.count==2) {
                        const auto original_due=generated_sent
                            ? SnapGoPacing::OriginalDue(due,task.half_ns,task.fps)
                            : SnapGoPacing::NextDue(NowUs(),last_due,task.fps);
                        if(SnapGoPacing::IsNewSourcePts(task.captured_us,last_source_pts)&&
                           Publish(task,1,original_due,task.captured_us)){
                            last_due=original_due;last_source_pts=task.captured_us;
                        }
                    }
                }
                for(u32 i=0;i<task.count;++i)slots[task.slots[i]].occupied.store(false,std::memory_order_release);
            }
        }catch(const std::exception& e){LOG_WARNING(Render_Vulkan,"SnapRYU Go capture stopped: {}",e.what());}
        catch(...){LOG_WARNING(Render_Vulkan,"SnapRYU Go capture stopped");}
        // Preserve in-flight slot resources until the renderer's normal device-idle teardown.
        failed.store(true,std::memory_order_release);
    }
    void Ensure(Slot& slot,Frame* frame,VkFormat format,bool scale) {
        if(!slot.download)slot.download=CreateWrappedBuffer(allocator,Bytes,MemoryUsage::Download);
        if(!scale)return;
        if(slot.encoded&&slot.width==frame->width&&slot.height==frame->height&&slot.format==format)return;
        auto encoded=Image(allocator,frame->width,frame->height,format);
        auto scaled=Image(allocator,Width,Height,format);
        slot.encoded=std::move(encoded);slot.scaled=std::move(scaled);
        slot.width=frame->width;slot.height=frame->height;slot.format=format;slot.initialized=false;
    }
    void Record(Frame* first,Frame* second,VkFormat source_format,
                const Layout::FramebufferLayout& layout,s64 half_ns) noexcept {
        if(stop.load()||failed.load())return;
        const auto demand=SnapGo::VideoDemand();
        if(!demand.enabled||!demand.generation||demand.width!=Width||demand.height!=Height)return;
        const auto now=NowUs();
        if(producer_generation!=demand.generation){producer_generation=demand.generation;next_capture=0;}
        if(now+1000<next_capture)return;
        if(!first||first->width<2||first->height<2||first->width>4096||first->height>2160)return;
        if(second&&(second->width!=first->width||second->height!=first->height))return;
        const auto format=EncodedFormat(source_format);
        if(format!=VK_FORMAT_R8G8B8A8_UNORM&&format!=VK_FORMAT_B8G8R8A8_UNORM)return;
        const VkOffset3D crop0{static_cast<s32>(layout.screen.left),static_cast<s32>(layout.screen.top),0};
        const VkOffset3D crop1{static_cast<s32>(layout.screen.right),static_cast<s32>(layout.screen.bottom),1};
        if(crop0.x<0||crop0.y<0||crop1.x>static_cast<s32>(first->width)||crop1.y>static_cast<s32>(first->height)||crop1.x<=crop0.x||crop1.y<=crop0.y)return;
        const bool scale=first->width!=Width||first->height!=Height||crop0.x||crop0.y||crop1.x!=Width||crop1.y!=Height;
        if(scale){
            const auto features=device.GetPhysical().GetFormatProperties(format).optimalTilingFeatures;
            constexpr auto need=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            if((features&need)!=need)return;
        }
        Task task{};task.count=second?2:1;task.generation=demand.generation;task.fps=demand.fps==30?30:60;
        task.captured_us=now;
        task.epoch=epoch.load(std::memory_order_acquire);
        task.format=format==VK_FORMAT_B8G8R8A8_UNORM?1:0;task.half_ns=half_ns;
        u32 reserved=0;
        for(size_t i=0;i<slots.size()&&reserved<task.count;++i){bool available=false;if(slots[i].occupied.compare_exchange_strong(available,true,std::memory_order_acq_rel))task.slots[reserved++]=i;}
        if(reserved!=task.count){for(u32 i=0;i<reserved;++i)slots[task.slots[i]].occupied.store(false,std::memory_order_release);return;}
        bool recorded=false;
        try {
            if(!worker.joinable())worker=std::thread([this]{Run();});
            const std::array<Frame*,2> frames{first,second};
            for(u32 i=0;i<task.count;++i)Ensure(slots[task.slots[i]],frames[i],format,scale);
            scheduler.RequestOutsideRenderPassOperationContext();
            task.tick=scheduler.CurrentTick();
            // All resources are reserved before recording. Only the renderer mutates Scheduler.
            for(u32 i=0;i<task.count;++i){
                auto& slot=slots[task.slots[i]];
                const VkImage src=*frames[i]->image,encoded=scale?*slot.encoded:VkImage{},scaled=scale?*slot.scaled:VkImage{};
                const VkBuffer dst=*slot.download;
                const u32 source_width=frames[i]->width,source_height=frames[i]->height;
                const bool initialized=slot.initialized;
                const auto& dispatch=device.GetDispatchLoader();
                const SnapGoTransfer::Commands commands{dispatch.vkCmdPipelineBarrier,
                    dispatch.vkCmdCopyImage,dispatch.vkCmdBlitImage,dispatch.vkCmdCopyImageToBuffer};
                const SnapGoTransfer::Transfer transfer{src,encoded,scaled,dst,source_width,source_height,
                                                        crop0,crop1,scale,initialized};
                scheduler.Record([commands,transfer](vk::CommandBuffer cmd){
                    SnapGoTransfer::Record(commands,*cmd,transfer);
                });
                recorded=true;if(scale)slot.initialized=true;
            }
            {std::lock_guard lock{mutex};tasks[(head+count)%tasks.size()]=task;++count;}
            next_capture=now+SnapGoPacing::AdmissionPeriod(task.fps,second!=nullptr);wake.notify_one();
        }catch(...){
            // If any command was recorded, retain those slots through their tick.
            if(recorded){task.generation=0;std::lock_guard lock{mutex};tasks[(head+count)%tasks.size()]=task;++count;wake.notify_one();}
            else for(u32 i=0;i<task.count;++i)slots[task.slots[i]].occupied.store(false,std::memory_order_release);
        }
    }
};

SnapGoCapture::SnapGoCapture(const Device& d,MemoryAllocator& a,Scheduler& s):impl{std::make_unique<Impl>(d,a,s)}{}
SnapGoCapture::~SnapGoCapture()=default;
void SnapGoCapture::Stop() noexcept{impl->Stop();}
void SnapGoCapture::Reset() noexcept{impl->Reset();}
void SnapGoCapture::Single(Frame* f,VkFormat format,const Layout::FramebufferLayout& layout) noexcept{impl->Record(f,nullptr,format,layout,0);}
void SnapGoCapture::Pair(Frame* generated,Frame* original,VkFormat format,const Layout::FramebufferLayout& layout,s64 half_ns) noexcept{impl->Record(generated,original,format,layout,half_ns);}
}
