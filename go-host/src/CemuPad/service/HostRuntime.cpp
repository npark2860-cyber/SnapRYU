#include "HostRuntime.h"
#include "NetworkBinding.h"
#include "Discovery.h"
#include <windows.h>

namespace CemuPad::Service {
struct HostRuntime::Owner {
    Store::WindowsStore store;
    SessionCore core;
    VpadBridge bridge;
    MotionBridge motion;
    MotionControlMailbox motionControls;
    HapticLease haptic;
    RumbleState rumble;
    Transport::Lifecycle effects{core,haptic,{},nullptr};
    HostService service;
    Owner(const std::filesystem::path& path,const std::string& address):store(path),service(store,effects,address,DefaultControlPort,true){effects.rumble=&rumble;effects.motionControls=&motionControls;}
};
HostRuntime::HostRuntime()=default;
HostRuntime::~HostRuntime(){Shutdown();}
void HostRuntime::Configure(std::filesystem::path path){
    std::lock_guard lock(mutex);
    if(state.shutdown||state.configured)throw std::logic_error("Cemu host runtime already configured or shut down");
    if(!path.is_absolute())throw std::invalid_argument("Cemu host user data path must be absolute");
    root=std::move(path)/"snapryugo";state.configured=true;
}
bool HostRuntime::SetEnabled(bool value) noexcept{
    std::lock_guard lock(mutex);
    if(state.shutdown)return !value;
    state.enabled=value;
    try{
        if(value){
            if(!state.configured)throw std::logic_error("Cemu host runtime is not configured");
            state.bindingFault=invalidBindSelection||!BindAddressAvailable(state.bindAddress);
            if(state.bindingFault)throw std::logic_error("Selected Cemu Pad address unavailable");
            if(!owner){
                owner=std::make_unique<Owner>(root,state.bindAddress);state.ownerCreated=true;
                if(state.gameRunning)owner->service.SetGameRunning(true);
                if(state.suspended)owner->service.Suspend();
            }
            owner->service.SetEnabled(true);
        }else if(owner)owner->service.SetEnabled(false);
        state.fault=false;return true;
    }catch(...){state.fault=true;if(owner)try{owner->service.SetEnabled(false);}catch(...){}return false;}
}
bool HostRuntime::SetBindAddress(std::string address) noexcept{
    std::lock_guard lock(mutex);
    if(!state.configured||state.enabled||state.suspended||state.shutdown)return false;
    if(!ParseBindAddress(address)){invalidBindSelection=true;state.bindingFault=true;return false;}
    if(address!=state.bindAddress){owner.reset();state.ownerCreated=false;state.bindAddress=std::move(address);}
    invalidBindSelection=false;state.bindingFault=false;state.fault=false;return true;
}
Store::StoreResult HostRuntime::InitializeIdentity() noexcept{
    std::lock_guard lock(mutex);
    if(!state.configured||state.shutdown||state.enabled||state.suspended)
        return {Control::CommitOutcome::NotCommitted,Store::StoreError::Busy};
    try{
        Store::WindowsStore store(root);auto existing=store.Load();
        if(existing.snapshot){state.fault=false;return {Control::CommitOutcome::Committed,Store::StoreError::None};}
        if(existing.error!=Store::StoreError::Missing){state.fault=true;return {Control::CommitOutcome::NotCommitted,existing.error};}
        auto result=store.Initialize();state.fault=!result.Committed();return result;
    }
    catch(...){state.fault=true;return {Control::CommitOutcome::NotCommitted,Store::StoreError::Io};}
}
Store::StoreResult HostRuntime::ResetPairing() noexcept{
    std::lock_guard lock(mutex);
    if(!state.configured||state.shutdown||state.suspended||state.gameRunning)
        return {Control::CommitOutcome::NotCommitted,Store::StoreError::Busy};
    state.enabled=false;
    try{
        // No storage mutation until all control/media owners have joined. Drop
        // the old bridge as well, so a new pairing cannot inherit held input.
        if(owner)owner->service.SetEnabled(false);
        owner.reset();state.ownerCreated=false;
        Store::WindowsStore store(root);auto current=store.Load();
        if(!current.snapshot){state.fault=true;return {Control::CommitOutcome::NotCommitted,current.error};}
        if(!current.snapshot->peer){state.fault=false;return {Control::CommitOutcome::Committed,Store::StoreError::None};}
        auto result=store.ResetTrust(current.snapshot->revision);state.fault=!result.Committed();return result;
    }catch(...){state.fault=true;return {Control::CommitOutcome::NotCommitted,Store::StoreError::Io};}
}
std::optional<PairingCode> HostRuntime::OpenPairing() noexcept{
    std::lock_guard lock(mutex);
    if(!state.configured||!state.enabled||state.suspended||state.shutdown||!owner)return {};
    try{auto result=owner->service.OpenPairing();if(result)state.fault=false;return result;}
    catch(...){state.fault=true;return {};}
}
void HostRuntime::CancelPairing() noexcept{
    std::lock_guard lock(mutex);
    try{if(owner)owner->service.CancelPairing();}catch(...){state.fault=true;}
}
bool HostRuntime::SetGameRunning(bool value) noexcept{
    std::lock_guard lock(mutex);
    if(state.shutdown)return false;
    if(state.gameRunning!=value&&owner)owner->bridge=VpadBridge{};
    state.gameRunning=value;
    try{if(owner)owner->service.SetGameRunning(value);return true;}
    catch(...){state.fault=true;if(owner)try{owner->service.SetEnabled(false);}catch(...){}return false;}
}
void HostRuntime::Suspend() noexcept{
    std::lock_guard lock(mutex);
    if(state.shutdown||state.suspended)return;
    state.suspended=true;
    try{if(owner)owner->service.Suspend();}catch(...){state.fault=true;}
}
void HostRuntime::Resume() noexcept{
    std::lock_guard lock(mutex);
    if(state.shutdown||!state.suspended)return;
    state.suspended=false;
    try{
        if(state.enabled){state.bindingFault=invalidBindSelection||!BindAddressAvailable(state.bindAddress);
            if(state.bindingFault)throw std::logic_error("Selected Cemu Pad address unavailable");}
        if(owner)owner->service.Resume();
    }catch(...){state.fault=true;if(owner)try{owner->service.SetEnabled(false);}catch(...){}}
}
void HostRuntime::Shutdown() noexcept{
    std::lock_guard lock(mutex);
    if(state.shutdown)return;
    state.shutdown=true;state.enabled=false;state.gameRunning=false;
    if(owner){
        try{owner->service.SetGameRunning(false);}catch(...){state.fault=true;}
        try{owner->service.Shutdown();}catch(...){state.fault=true;}
    }
}
bool HostRuntime::HandleWindowsMessage(std::uint32_t message,std::uintptr_t param) noexcept{
    if(message==WM_ENDSESSION&&param){Shutdown();return true;}
    if(message!=WM_POWERBROADCAST)return false;
    switch(param){
    case PBT_APMSUSPEND:Suspend();return true;
    case PBT_APMRESUMEAUTOMATIC:
    case PBT_APMRESUMESUSPEND:
    case PBT_APMRESUMECRITICAL:Resume();return true;
    default:return false;
    }
}
RuntimeStatus HostRuntime::Snapshot() const{
    std::lock_guard lock(mutex);auto result=state;
    if(owner)result.host=owner->service.Snapshot();
    result.fault=result.fault||result.host.phase==Phase::Faulted;return result;
}
bool HostRuntime::MediaActive() const{
    std::lock_guard lock(mutex);
    return state.enabled&&state.gameRunning&&!state.suspended&&!state.fault&&!state.shutdown
        &&owner&&owner->service.Connected();
}
std::string HostRuntime::StatusText() const{
    const auto s=Snapshot();
    if(s.shutdown)return "Cemu Pad: stopped";
    if(!s.enabled)return "Cemu Pad: off";
    if(s.suspended)return "Cemu Pad: suspended";
    if(s.bindingFault)return "Cemu Pad: selected address is invalid or unavailable. Switch OFF and select an address.";
    if(s.fault)return "Cemu Pad: cannot start. Check paired identity and storage.";
    if(s.host.phase==Phase::Pairing)return "Cemu Pad: pairing window open on "+s.bindAddress;
    auto endpoint=s.bindAddress+":"+std::to_string(s.host.endpoint.port);
    if(s.host.phase==Phase::Connected)return s.host.videoStreaming?"Cemu Pad: video + input streaming on "+endpoint+" (frames "+std::to_string(s.host.videoFrames)+")":"Cemu Pad: control connected on "+endpoint+" (waiting for GamePad video)";
    if(s.host.phase==Phase::Listening)return "Cemu Pad: waiting on "+endpoint+" (paired identity required)";
    if(!s.host.worker&&s.host.pairingGeneration)return "Cemu Pad: pairing window closed. Open a new window to retry.";
    return "Cemu Pad: starting control host";
}
HostRuntime& CemuHostRuntime(){static HostRuntime instance;return instance;}
HostRuntime::InputRead HostRuntime::ReadInput(VpadSample local,int delayMs,int pulseMs,bool menu){
    std::lock_guard lock(mutex);if(!owner||state.shutdown)return {local,{},{}};
    auto now=Transport::Now();owner->core.SetConsumer(menu?InputConsumer::AppMenu:InputConsumer::Game,now);
    auto input=owner->core.ConsumeForRead(InputConsumer::Game,now);
    bool wasRemote=owner->bridge.RemotePad();auto pad=owner->bridge.Apply(local,input,now,delayMs,pulseMs);
    owner->rumble.Permit(!menu&&owner->bridge.RemotePad()&&input&&!input->handoverRelease&&owner->core.State()==SessionState::Active,input?input->ownershipEpoch:0);
    return {pad,input,owner->bridge.RemotePad(),!wasRemote&&owner->bridge.RemotePad(),owner->motion.Apply(input,&owner->motionControls,now)};
}
bool HostRuntime::RemotePadOwned() const{
    std::lock_guard lock(mutex);return owner&&owner->bridge.RemotePad();
}
std::optional<bool> HostRuntime::QueueRemoteRumble(std::span<const std::uint8_t> pattern,unsigned bits){
    std::lock_guard lock(mutex);
    if(!owner||!state.enabled||state.shutdown||!owner->bridge.RemotePad())return {};
    if(state.suspended||!state.gameRunning||owner->core.State()!=SessionState::Active){owner->rumble.Cancel();return true;}
    return owner->rumble.Push(pattern,bits,Transport::Now());
}
void HostRuntime::SetRemoteRumbleLevel(bool on){std::lock_guard lock(mutex);if(owner)owner->rumble.Level(on,Transport::Now()+100000);}
void HostRuntime::StopRemoteRumble(){std::lock_guard lock(mutex);if(owner)owner->rumble.Cancel();}
}
