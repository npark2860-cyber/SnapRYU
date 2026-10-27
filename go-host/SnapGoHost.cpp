#define SNAP_GO_BUILD_DLL
#include "SnapGoApi.h"
#include "AudioMixer.h"
#include "CemuPad/service/HostRuntime.h"
#include "CemuPad/service/NetworkBinding.h"
#include "CemuPad/service/PairingQr.h"
#include "CemuPad/media/VideoFrames.h"
#include "CemuPad/media/AudioFrames.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>

namespace {
using namespace CemuPad;
using namespace CemuPad::Service;
std::uint64_t Now(){return Transport::Now();}
struct Context {
    HostRuntime runtime;
    std::filesystem::path path;
    std::atomic<bool> stop{false},blocked{false},paired{false};
    std::array<std::atomic<float>,2> motors{};
    std::mutex inputMutex;
    std::uint64_t sequence=0;
    SnapGoHost::AudioMixer audio;
    std::thread worker;
    explicit Context(const wchar_t* p):path(p){
        if(!path.is_absolute())throw std::invalid_argument("Absolute profile required");
        runtime.Configure(path);
        worker=std::thread([this]{Loop();});
    }
    ~Context(){stop=true;if(worker.joinable())worker.join();runtime.StopRemoteRumble();runtime.Shutdown();}
    void RefreshTrust(){
        if(!std::filesystem::is_directory(path/"snapryugo"))return;
        Store::WindowsStore store(path/"snapryugo");auto loaded=store.Load();
        paired=loaded.snapshot&&loaded.snapshot->peer.has_value();
    }
    void Loop() noexcept {
        while(!stop){try{
            const auto now=Now();
            runtime.SetRemoteRumbleLevel(!blocked&&(motors[0].load()>0.001f||motors[1].load()>0.001f));
            auto& frames=Media::CemuAudioFrames();
            const auto generation=frames.Enabled()?frames.Generation():0;
            for(unsigned n=0;n<12;++n){auto block=audio.Pop(generation,now);if(!block)break;
                frames.PublishInterleaved(block->samples,block->time_us,generation);}
        }catch(...){/* Optional remote output must never unwind into the emulator. */}
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
};
template<class T>bool Valid(const T* out){return out&&out->size>=sizeof(T)&&out->version==SNAP_GO_ABI;}
void Text(char* out,std::size_t size,const std::string& text){if(!out||!size)return;const auto n=std::min(size-1,text.size());std::memcpy(out,text.data(),n);out[n]=0;}
void* Create(const wchar_t* path) noexcept {try{if(!path)return nullptr;return new Context(path);}catch(...){return nullptr;}}
void Destroy(void* context) noexcept {try{delete static_cast<Context*>(context);}catch(...){}}
int32_t Enabled(void* p,uint32_t value) noexcept {try{if(!p)return 0;auto& c=*static_cast<Context*>(p);
    std::lock_guard lock(c.inputMutex);
    if(!value){c.motors[0]=c.motors[1]=0;c.audio.Reset(0);}
    auto ok=c.runtime.SetEnabled(value!=0);if(ok&&value){
        if(c.blocked)c.runtime.ReadInput({},0,0,true);
        c.RefreshTrust();
    }return ok;
}catch(...){return 0;}}
int32_t Bind(void* p,const char* value) noexcept {try{return p&&value&&static_cast<Context*>(p)->runtime.SetBindAddress(value);}catch(...){return 0;}}
int32_t Initialize(void* p) noexcept {try{if(!p)return 0;auto& c=*static_cast<Context*>(p);auto result=c.runtime.InitializeIdentity();if(result.Committed())c.RefreshTrust();return result.Committed();}catch(...){return 0;}}
int32_t Reset(void* p) noexcept {try{if(!p)return 0;auto& c=*static_cast<Context*>(p);c.motors[0]=c.motors[1]=0;
    auto result=c.runtime.ResetPairing();if(result.Committed())c.RefreshTrust();return result.Committed();}catch(...){return 0;}}
int32_t Pair(void* p,uint8_t* modules,uint32_t capacity,uint32_t* side) noexcept {try{
    if(!p||!modules||!side)return 0;*side=0;auto& c=*static_cast<Context*>(p);
    auto code=c.runtime.OpenPairing();if(!code)return 0;auto qr=PairingQr::Encode(code->text);
    if(qr.size<1||qr.modules.size()>capacity){c.runtime.CancelPairing();return 0;}
    std::copy(qr.modules.begin(),qr.modules.end(),modules);*side=qr.size;return 1;
}catch(...){return 0;}}
void Cancel(void* p) noexcept {try{if(p)static_cast<Context*>(p)->runtime.CancelPairing();}catch(...){}}
int32_t Game(void* p,uint32_t value) noexcept {try{if(!p)return 0;auto& c=*static_cast<Context*>(p);
    c.motors[0]=c.motors[1]=0;c.audio.Reset(0);return c.runtime.SetGameRunning(value!=0);}catch(...){return 0;}}
void Suspend(void* p,uint32_t value) noexcept {try{if(!p)return;auto& c=*static_cast<Context*>(p);
    if(value){c.motors[0]=c.motors[1]=0;c.runtime.Suspend();}else c.runtime.Resume();}catch(...){}}
int32_t Status(void* p,SnapGoStatus* out) noexcept {try{if(!p||!Valid(out))return 0;auto& c=*static_cast<Context*>(p);auto s=c.runtime.Snapshot();
    SnapGoStatus result{};result.size=sizeof(result);result.version=SNAP_GO_ABI;
    result.enabled=s.enabled;result.game_running=s.gameRunning;result.connected=s.host.phase==Phase::Connected;
    result.video_active=s.host.videoStreaming;result.audio_active=Media::CemuAudioFrames().Enabled();
    result.audio_playback=Media::CemuAudioFrames().RemotePlayback();
    result.fault=s.fault;result.paired=c.paired||!s.host.peerPin.empty();result.suspended=s.suspended;result.port=26762;
    result.video_frames=s.host.videoFrames;result.video_generation=Media::CemuVideoFrames().Generation();
    Text(result.bind_address,sizeof(result.bind_address),s.bindAddress);
    auto message=c.runtime.StatusText();for(std::size_t at=0;(at=message.find("Cemu Pad",at))!=std::string::npos;at+=11)message.replace(at,8,"SnapRYU Go!");
    Text(result.message,sizeof(result.message),message);*out=result;return 1;
}catch(...){return 0;}}
int32_t Addresses(void* p,char* output,uint32_t capacity) noexcept {try{if(!p||!output||!capacity)return 0;std::string text;
    for(const auto& ip:LocalBindAddresses()){if(!text.empty())text+='\n';text+=ip;}
    if(text.size()+1>capacity)return 0;Text(output,capacity,text);return 1;}catch(...){return 0;}}
int32_t Input(void* p,SnapGoInput* out) noexcept {try{if(!p||!Valid(out))return 0;auto& c=*static_cast<Context*>(p);std::lock_guard lock(c.inputMutex);
    auto read=c.runtime.ReadInput({},0,0,c.blocked);SnapGoInput result{};result.size=sizeof(result);result.version=SNAP_GO_ABI;result.sample_sequence=++c.sequence;
    result.pad_owned=read.remotePad;result.release_required=read.takeover||(read.input&&read.input->handoverRelease);
    if(read.input){auto& input=*read.input;result.ownership_epoch=input.ownershipEpoch;
        result.remote_session_active=!input.handoverRelease;result.pad_valid=(input.state.validity&PadValid)&&!result.release_required;
        constexpr std::uint32_t flags[17]={0x8000,0x4000,0x2000,0x1000,0x200,0x100,0x800,0x400,0x20,0x10,0x80,0x40,8,4,0x40000,0x20000,2};
        if(result.pad_valid)for(unsigned n=0;n<17;++n)if(read.pad.hold&flags[n])result.buttons_wire17|=1u<<n;
        if(result.pad_valid)std::copy(read.pad.sticks.begin(),read.pad.sticks.end(),result.sticks);
        result.touch_valid=(input.state.validity&TouchValid)&&!input.handoverRelease;result.touch_down=result.touch_valid&&input.state.touchActive;
        result.touch_id=input.state.touchId;result.touch_x=input.state.touchX/65535.0f;result.touch_y=input.state.touchY/65535.0f;
    }
    const auto& motion=read.motion;result.motion_active=motion.active;result.motion_neutralize=motion.neutralize;
    result.motion_sample_sequence=motion.sensorTimeUs;result.motion_delta_us=motion.deltaUs;
    std::copy(motion.physicalGyro.begin(),motion.physicalGyro.end(),result.gyro_rad_s);
    std::copy(motion.physicalAccel.begin(),motion.physicalAccel.end(),result.accel_g);
    std::copy(motion.physicalQuaternion.begin(),motion.physicalQuaternion.end(),result.quaternion_wxyz);
    if(!result.remote_session_active){c.motors[0]=c.motors[1]=0;}
    *out=result;return 1;
}catch(...){return 0;}}
void Rumble(void* p,uint32_t side,float low,float high) noexcept {try{if(p&&side<2){auto& c=*static_cast<Context*>(p);
    c.motors[side]=std::isfinite(low)&&std::isfinite(high)?std::clamp(std::max(low,high),0.0f,1.0f):0.0f;}}catch(...){}}
int32_t Demand(void* p,SnapGoVideoDemand* out) noexcept {try{if(!p||!Valid(out))return 0;
    auto& frames=Media::CemuVideoFrames();SnapGoVideoDemand value{};value.size=sizeof(value);value.version=SNAP_GO_ABI;
    value.enabled=frames.Enabled();value.width=1280;value.height=720;value.fps=frames.Rate();value.generation=frames.Generation();*out=value;return 1;
}catch(...){return 0;}}
int32_t Video(void* p,const SnapGoVideoFrame* in) noexcept {try{if(!p||!Valid(in)||!in->pixels||in->format>1||in->width!=1280||in->height!=720
    ||in->stride<in->width*4||in->stride>32768||in->bytes<static_cast<std::uint64_t>(in->stride)*in->height)return 0;
    auto& frames=Media::CemuVideoFrames();if(!frames.Enabled()||frames.Generation()!=in->generation)return 0;
    auto frame=std::make_shared<Media::RgbaFrame>();frame->width=in->width;frame->height=in->height;frame->generation=in->generation;frame->timestampUs=in->timestamp_us;
    frame->rgba.resize(static_cast<std::size_t>(in->width)*in->height*4);
    frame->order=in->format?Media::PixelOrder::Bgra:Media::PixelOrder::Rgba;
    Media::CopyPixelRows(frame->rgba.data(),in->pixels,in->width,in->height,in->stride);
    return frames.Publish(std::move(frame));
}catch(...){return 0;}}
int32_t Audio(void* p,uint64_t id,uint64_t time,uint32_t channels,uint32_t count,const int16_t* samples) noexcept {try{
    if(!p)return 0;auto& frames=Media::CemuAudioFrames();if(!frames.Enabled())return 0;
    return static_cast<Context*>(p)->audio.Push(frames.Generation(),id,time,channels,count,samples);
}catch(...){return 0;}}
void CloseAudio(void* p,uint64_t id) noexcept {try{if(p)static_cast<Context*>(p)->audio.Close(id);}catch(...){}}
void BlockInput(void* p,uint32_t blocked) noexcept {try{if(p){
    auto& c=*static_cast<Context*>(p);std::lock_guard lock(c.inputMutex);
    c.blocked=blocked!=0;
    if(blocked){
        // Observe the consumer handover now, even if emulation is paused and
        // no HID tick occurs before the modal window or pause gate is cleared.
        c.runtime.ReadInput({},0,0,true);
        c.motors[0]=c.motors[1]=0;c.runtime.StopRemoteRumble();
    }
}}catch(...){}}
}
extern "C" __declspec(dllexport) int32_t SnapGo_GetApi(uint32_t version,uint32_t capacity,SnapGoApi* out){
    if(version!=SNAP_GO_ABI||capacity<sizeof(SnapGoApi)||!out)return 0;
    *out={sizeof(SnapGoApi),SNAP_GO_ABI,Create,Destroy,Enabled,Bind,Initialize,Reset,Pair,Cancel,Game,Suspend,Status,Addresses,Input,Rumble,Demand,Video,Audio,CloseAudio,BlockInput};return 1;
}
extern "C" __declspec(dllexport) uint32_t SnapGo_GetMediaState(void* p) noexcept {
    try {
        if(!p||!static_cast<Context*>(p)->runtime.MediaActive())return 0;
        auto& audio=Media::CemuAudioFrames();
        return (audio.Enabled()?SNAP_GO_MEDIA_AUDIO:0u)
            |(audio.RemotePlayback()?SNAP_GO_MEDIA_PLAYBACK:0u)
            |(Media::CemuVideoFrames().Enabled()?SNAP_GO_MEDIA_VIDEO:0u);
    }catch(...){return 0;}
}
