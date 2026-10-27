#include "HostService.h"
#include "VideoHost.h"
#include "Discovery.h"
#include "CemuPad/third_party/nlohmann/json.hpp"
#include <algorithm>
#include <openssl/crypto.h>

namespace CemuPad::Service {
namespace T=Transport;namespace C=Control;
namespace {
using Json=nlohmann::json;
void Need(bool v){if(!v)throw T::Error(T::Failure::Protocol);}
T::Deadline ReplyEnd(){return T::Clock::now()+std::chrono::milliseconds(500);}
C::Message Caps(std::uint64_t id,bool haptics){return T::MakeMessage(C::Type::Capabilities,id,Json{{"input_hz",120},{"gyro",true},{"accel",true},{"gyro_abs_max",100},{"accel_abs_max",200},{"haptics",haptics},{"video_codecs",Json::array({"H264"})},{"audio_codecs",Json::array({"Opus"})}}.dump());}
Store::LoadResult Load(Store::WindowsStore& store,const T::Cancellation* stop=nullptr){
    const auto end=T::Clock::now()+std::chrono::milliseconds(250);
    for(;;){
        if(stop){if(stop->Reason()!=T::StopReason::None)throw T::Error(T::Failure::Cancelled);stop->Pulse();}
        auto result=store.Load();
        if(result.error!=Store::StoreError::Busy||T::Clock::now()>=end)return result;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
}
const char* Name(Phase p){switch(p){case Phase::Disabled:return "DISABLED";case Phase::Starting:return "STARTING";case Phase::Listening:return "LISTENING";case Phase::Negotiating:return "NEGOTIATING";case Phase::Connected:return "CONNECTED";case Phase::Suspended:return "SUSPENDED";case Phase::Faulted:return "FAULTED";case Phase::Pairing:return "PAIRING";default:return "SHUTDOWN";}}
HostService::HostService(Store::WindowsStore& s,T::Lifecycle& e,std::string a,std::uint16_t port,bool discovery):store(s),effects(e),address(std::move(a)),controlPort(port),discoverable(discovery){}
HostService::~HostService(){Shutdown();}
HostStatus HostService::Snapshot() const{std::lock_guard lock(stateMutex);return state;}
bool HostService::Connected() const{std::lock_guard lock(stateMutex);return state.phase==Phase::Connected;}
void HostService::StartLocked(std::optional<Store::Snapshot> expected){
    if(worker.joinable())worker.join();
    auto stop=std::make_shared<T::Cancellation>();
    {std::lock_guard lock(stateMutex);cancellation=stop;state.worker=true;state.phase=Phase::Starting;++state.generation;}
    try{worker=std::thread([this,stop,expected=std::move(expected)]() mutable {Run(stop,std::move(expected));});}
    catch(...){std::lock_guard lock(stateMutex);cancellation.reset();state.worker=false;state.phase=Phase::Faulted;throw;}
}
void HostService::StopLocked(T::StopReason reason){
    std::shared_ptr<T::Cancellation> stop;{std::lock_guard lock(stateMutex);stop=cancellation;}
    if(stop)stop->Request(reason);
    pairing.Close();
    if(worker.joinable())worker.join();
    std::lock_guard lock(stateMutex);cancellation.reset();pairGeneration=0;state.pairing=false;state.pairingDeadline=0;
}
std::optional<PairingCode> HostService::OpenPairing(){
    std::lock_guard operation(operationMutex);
    {std::lock_guard lock(stateMutex);if(!state.enabled||state.suspended||state.shutdown||state.pairing)return {};}
    auto loaded=Load(store);if(!loaded.snapshot||loaded.snapshot->peer)return {};
    StopLocked(T::StopReason::Off);
    auto offer=pairing.Open();
    struct Wipe{C::RawPairToken& value;~Wipe(){OPENSSL_cleanse(value.bytes.data(),value.bytes.size());}} wipe{offer.token};
    {std::lock_guard lock(stateMutex);pairGeneration=offer.generation;state.pairing=true;
        state.pairingGeneration=offer.generation;state.pairingDeadline=offer.deadline;}
    try{
        StartLocked(*loaded.snapshot);
        std::unique_lock lock(stateMutex);
        const bool ready=changed.wait_for(lock,std::chrono::seconds(1),[&]{return state.phase==Phase::Pairing||!state.worker;});
        if(ready&&state.phase==Phase::Pairing&&state.endpoint.port&&state.storeId==loaded.snapshot->identity.StoreId()
            &&state.pin==C::Hex(loaded.snapshot->identity.Pin())&&state.revision==std::to_string(loaded.snapshot->revision))
            return PairingCode::Create(state.endpoint,*loaded.snapshot,offer);
        lock.unlock();StopLocked(T::StopReason::Off);return {};
    }catch(...){StopLocked(T::StopReason::Off);throw;}
}
void HostService::CancelPairing(){
    std::lock_guard operation(operationMutex);
    {std::lock_guard lock(stateMutex);if(!pairGeneration&&!state.pairing)return;}
    StopLocked(T::StopReason::Off);
    // Cancellation must not roll back a durable commit. Recover using the normal
    // pinned path if commit won the race; never reopen a provisional window.
    auto current=Load(store);bool start=false;
    {std::lock_guard lock(stateMutex);state.phase=Phase::Disabled;
        start=state.enabled&&!state.suspended&&!state.shutdown&&current.snapshot&&current.snapshot->peer;}
    if(start)StartLocked();
}
void HostService::SetEnabled(bool value){
    std::lock_guard operation(operationMutex);bool start=false;
    {std::lock_guard lock(stateMutex);
        if(state.shutdown){if(value)throw std::logic_error("Service is shut down");return;}
        state.enabled=value;start=value&&!state.suspended&&!state.worker;
    }
    if(start)StartLocked();
    else if(!value){StopLocked(T::StopReason::Off);std::lock_guard lock(stateMutex);state.phase=state.suspended?Phase::Suspended:Phase::Disabled;}
}
void HostService::Suspend(){
    std::lock_guard operation(operationMutex);
    {std::lock_guard lock(stateMutex);if(state.shutdown)return;state.suspended=true;}
    StopLocked(T::StopReason::Off);std::lock_guard lock(stateMutex);state.phase=Phase::Suspended;
}
void HostService::Resume(){
    std::lock_guard operation(operationMutex);bool start=false;
    {std::lock_guard lock(stateMutex);if(state.shutdown)return;state.suspended=false;start=state.enabled&&!state.worker;if(!start&&!state.worker)state.phase=Phase::Disabled;}
    if(start)StartLocked();
}
void HostService::SetGameRunning(bool value){
    std::lock_guard operation(operationMutex);
    std::unique_lock lock(stateMutex);
    if(state.shutdown||(requestedGame==value&&appliedGameChange==requestedGameChange))return;
    if(requestedGame!=value){requestedGame=value;++requestedGameChange;}
    const auto revision=requestedGameChange;
    if(!state.worker){
        lock.unlock();
        if(value)effects.core.StartGame(T::Now());else effects.core.EndGame(T::Now());
        lock.lock();state.gameRunning=value;++state.gameChanges;appliedGameChange=revision;return;
    }
    if(!changed.wait_for(lock,std::chrono::seconds(1),[&]{return appliedGameChange>=revision||!state.worker;})){
        lock.unlock();StopLocked(T::StopReason::Off);
        if(value)effects.core.StartGame(T::Now());else effects.core.EndGame(T::Now());
        lock.lock();state.gameRunning=value;++state.gameChanges;appliedGameChange=revision;
        throw std::runtime_error("Service game lifecycle barrier timed out");
    }
    // A fault may stop the owner before its next pulse. Drop all game history
    // after join; this does not touch output or I/O from the caller thread.
    if(appliedGameChange<revision){
        lock.unlock();if(worker.joinable())worker.join();
        if(value)effects.core.StartGame(T::Now());else effects.core.EndGame(T::Now());
        lock.lock();state.gameRunning=value;++state.gameChanges;appliedGameChange=revision;
    }
}
void HostService::Shutdown(){
    std::lock_guard operation(operationMutex);
    {std::lock_guard lock(stateMutex);if(state.shutdown)return;state.shutdown=true;state.enabled=false;}
    StopLocked(T::StopReason::Shutdown);std::lock_guard lock(stateMutex);state.phase=Phase::Shutdown;
}
void HostService::Run(std::shared_ptr<T::Cancellation> stop,std::optional<Store::Snapshot> expected) noexcept{
    std::unique_ptr<T::Listener> listener;std::unique_ptr<T::ControlTransport> driver;
    std::unique_ptr<DiscoveryPublisher> discovery;
    bool fault=false;LocalTimeUs last=0;bool outputWasOn=effects.haptic.IsOn(T::Now());
    try{
        stop->SetOwnerPulse([&]{
            bool game=false;std::uint64_t revision=0;
            {std::lock_guard lock(stateMutex);if(appliedGameChange!=requestedGameChange){game=requestedGame;revision=requestedGameChange;}}
            if(revision){
                if(game)effects.core.StartGame(T::Now());else effects.Stop(T::StopReason::GameEnd,T::Now());
                {std::lock_guard lock(stateMutex);state.gameRunning=game;++state.gameChanges;appliedGameChange=revision;}
                changed.notify_all();
            }
            auto now=T::Now();if(last&&now-last<5'000)return;
            effects.Tick(now);bool on=effects.haptic.IsOn(now);
            if(outputWasOn&&!on)effects.CancelOutput();
            if(effects.outputCancelFailed||effects.coreStopFailed)throw std::runtime_error("Service lifecycle effect failed");
            outputWasOn=on;
            std::lock_guard lock(stateMutex);++state.ticks;if(last)state.maxTickGapUs=std::max(state.maxTickGapUs,now-last);
            state.core=effects.core.State();state.outputOn=on;last=now;
        });
        effects.core.SetEnabled(true,T::Now());
        std::uint64_t generation=0;{std::lock_guard lock(stateMutex);generation=pairGeneration;}
        auto loaded=Load(store,stop.get());Need(loaded.snapshot&&(loaded.snapshot->peer||generation));auto initial=*loaded.snapshot;
        if(generation)Need(expected&&!expected->peer&&initial.revision==expected->revision
            &&initial.identity.StoreId()==expected->identity.StoreId()&&initial.identity.Pin()==expected->identity.Pin());
        listener=std::make_unique<T::Listener>(address,controlPort);
        if(discoverable)try{discovery=std::make_unique<DiscoveryPublisher>(address,listener->LocalAddress().port,initial.identity.StoreId());}catch(...){/* Saved endpoint still works if system mDNS is unavailable. */}
        if(generation&&!Pair(*listener,initial,generation,*stop))throw T::Error(T::Failure::Cancelled);
        {std::lock_guard lock(stateMutex);state.endpoint=listener->LocalAddress();state.phase=Phase::Listening;state.storeId=initial.identity.StoreId();state.pin=C::Hex(initial.identity.Pin());state.peerPin=C::Hex(initial.peer->clientPin);state.revision=std::to_string(initial.revision);}
        while(stop->Reason()==T::StopReason::None){
            // Never silently adopt a reset/replaced identity during a live owner.
            auto current=Load(store,stop.get());Need(current.snapshot&&current.snapshot->peer
                &&current.snapshot->identity.StoreId()==initial.identity.StoreId()&&current.snapshot->identity.Pin()==initial.identity.Pin()
                &&current.snapshot->peer->clientPin==initial.peer->clientPin&&current.snapshot->revision==initial.revision);
            try{
                driver=T::AcceptStoredHost(*listener,store,effects,*stop,T::Clock::now()+std::chrono::seconds(5));
                {std::lock_guard lock(stateMutex);++state.accepted;state.phase=Phase::Negotiating;}
                Connection(*driver,initial,*stop);
            }catch(const T::Error& error){
                if(error.code==T::Failure::Cancelled)break;
                std::lock_guard lock(stateMutex);++state.failures;
                if(error.code==T::Failure::PeerRejected)++state.peerRejected;
                if(error.code==T::Failure::Protocol)++state.protocolRejected;
            }
            if(driver){driver->Close(T::StopReason::TransportLost,T::Now());std::lock_guard lock(stateMutex);state.control=driver->Snapshot();}
            driver.reset();
            if(stop->Reason()!=T::StopReason::None)break;
            effects.core.SetEnabled(true,T::Now());
            std::lock_guard lock(stateMutex);state.phase=Phase::Listening;state.videoStreaming=false;state.videoFrames=0;
        }
    }catch(const T::Error& error){fault=error.code!=T::Failure::Cancelled;}
    catch(...){fault=true;}
    const auto reason=stop->Reason()==T::StopReason::None?T::StopReason::Off:stop->Reason();
    if(driver){try{driver->Close(reason,T::Now());}catch(...){fault=true;}driver.reset();}
    discovery.reset();listener.reset();pairing.Close();effects.Stop(reason,T::Now());
    try{stop->SetOwnerPulse({});}catch(...){fault=true;}
    std::lock_guard lock(stateMutex);state.worker=false;state.endpoint={};state.control={};state.outputOn=false;state.videoStreaming=false;state.videoFrames=0;state.core=effects.core.State();state.outputCancelFailed=effects.outputCancelFailed;state.coreStopFailed=effects.coreStopFailed;state.pairing=false;state.pairingDeadline=0;pairGeneration=0;
    state.phase=fault?Phase::Faulted:state.shutdown?Phase::Shutdown:state.suspended?Phase::Suspended:Phase::Disabled;
    changed.notify_all();
}
bool HostService::Pair(T::Listener& listener,Store::Snapshot& initial,std::uint64_t generation,const T::Cancellation& stop){
    Need(!initial.peer);
    {std::lock_guard lock(stateMutex);state.endpoint=listener.LocalAddress();state.phase=Phase::Pairing;
        state.storeId=initial.identity.StoreId();state.pin=C::Hex(initial.identity.Pin());state.peerPin.clear();state.revision=std::to_string(initial.revision);}
    changed.notify_all();
    while(stop.Reason()==T::StopReason::None&&pairing.CanAccept(generation)){
        // Detect external reset/replacement before every provisional connection.
        auto current=Load(store,&stop);Need(current.snapshot&&!current.snapshot->peer&&current.snapshot->revision==initial.revision
            &&current.snapshot->identity.StoreId()==initial.identity.StoreId()&&current.snapshot->identity.Pin()==initial.identity.Pin());
        try{
            LocalTimeUs deadline=0;{std::lock_guard lock(stateMutex);deadline=state.pairingDeadline;}
            const auto end=std::min(T::Clock::now()+std::chrono::seconds(5),T::Deadline(std::chrono::microseconds(deadline)));
            const auto result=T::PairOnce(listener,store,pairing,generation,stop,end,&initial);
            {std::lock_guard lock(stateMutex);++state.pairAttempts;}
            if(result.outcome==C::PairOutcome::Paired){
                auto committed=Load(store,&stop);Need(committed.snapshot&&committed.snapshot->peer
                    &&committed.snapshot->revision==initial.revision+1&&committed.snapshot->identity.StoreId()==initial.identity.StoreId()
                    &&committed.snapshot->identity.Pin()==initial.identity.Pin());
                initial=*committed.snapshot;pairing.Close();
                std::lock_guard lock(stateMutex);++state.pairCommits;pairGeneration=0;state.pairing=false;state.pairingDeadline=0;return true;
            }
            if(result.outcome==C::PairOutcome::Indeterminate)throw std::runtime_error("Pairing store outcome uncertain");
            std::lock_guard lock(stateMutex);++state.pairRejected;
        }catch(const T::Error& error){
            if(error.code==T::Failure::Cancelled)return false;
            std::lock_guard lock(stateMutex);++state.failures;
            if(error.code!=T::Failure::Timeout)++state.pairRejected;
        }
    }
    return false;
}
void HostService::Connection(T::ControlTransport& driver,const Store::Snapshot& identity,T::Cancellation& stop){
    unsigned step=0;auto authEnd=T::Clock::now()+std::chrono::seconds(5);std::optional<T::Deadline> frameEnd;
    std::optional<C::Message> clientHello,hostHello;
    std::unique_ptr<VideoHost> media;std::optional<Media::VideoQuality> quality;std::uint64_t nextStreamRequest=1;
    while(stop.Reason()==T::StopReason::None){
        auto now=T::Now();driver.Tick(now);if(!driver.Snapshot().open)return;
        {std::lock_guard lock(stateMutex);++state.controlTicks;state.control=driver.Snapshot();}
        if(step<5&&T::Clock::now()>=authEnd)throw T::Error(T::Failure::Timeout);
        if(frameEnd&&T::Clock::now()>=*frameEnd){std::lock_guard lock(stateMutex);++state.partialTimeouts;throw T::Error(T::Failure::Timeout);}
        auto received=driver.PollReceive(ReplyEnd(),now);
        if(received)frameEnd.reset();
        if(driver.HasPartialFrame()&&!frameEnd)frameEnd=T::Clock::now()+std::chrono::seconds(2);
        if(received){
            const auto& message=received->message;
            if(received->decision.code==C::DecisionCode::Duplicate||received->decision.code==C::DecisionCode::StaleCallback)continue;
            if(!driver.Snapshot().open)return;
            if(message.Kind()==C::Type::SessionStop){media.reset();continue;}
            if(message.Kind()==C::Type::VideoPreferences){
                Need(step==3&&!quality);quality=Media::VideoQuality{static_cast<unsigned>(message.Integer("width")),static_cast<unsigned>(message.Integer("height")),static_cast<unsigned>(message.Integer("fps"))};Need(quality->width==1280&&quality->height==720&&(quality->fps==30||quality->fps==60));continue;
            }
            if(step==0){Need(message.Kind()==C::Type::Hello);clientHello=message;hostHello=T::Hello(C::Role::Host,message.Id(),C::Session(identity.identity.StoreId()));driver.Send(*hostHello,ReplyEnd(),T::Now());++step;}
            else if(step==1){Need(message.Kind()==C::Type::Auth);driver.Send(T::MakeMessage(C::Type::AuthResult,message.Id(),Json{{"status","authenticated"},{"host_spki_sha256",C::Hex(identity.identity.Pin())},{"client_spki_sha256",C::Hex(identity.peer->clientPin)}}.dump()),ReplyEnd(),T::Now());++step;}
            else if(step==2){Need(message.Kind()==C::Type::Capabilities);driver.Send(Caps(message.Id(),effects.rumble!=nullptr),ReplyEnd(),T::Now());++step;}
            else if(message.Kind()==C::Type::TimeSync){
                if(message.Text("phase")=="request"){
                    Need(step==3||step==5);auto t2=std::to_string(T::Now()),t3=std::to_string(T::Now());
                    driver.Send(T::MakeMessage(C::Type::TimeSync,message.Id(),Json{{"phase","response"},{"t1",std::string(message.Text("t1"))},{"t2",t2},{"t3",t3}}.dump()),ReplyEnd(),T::Now());if(step==3)step=4;
                }else{
                    Need(message.Text("phase")=="complete"&&(step==4||step==5));
                    if(step==4){
                        auto snapshot=driver.Snapshot();Need(snapshot.authenticated&&snapshot.capabilities&&snapshot.timeEstimatesApplied==1
                            &&!snapshot.dataReady[0]&&!snapshot.dataReady[1]&&!snapshot.dataReady[2]&&IsZero(snapshot.session));
                        auto hn=C::Digest(hostHello->Text("nonce")),cn=C::Digest(clientHello->Text("nonce"));auto binding=driver.AuthBinding(hn,cn);
                        std::lock_guard lock(stateMutex);++state.authenticated;state.phase=Phase::Connected;state.control=snapshot;state.hostNonce=C::Hex(hn);state.clientNonce=C::Hex(cn);state.authBinding=C::Hex(Crypto::Sha256(binding));step=5;
                    }
                }
            }else if(step==5&&media&&message.Kind()==C::Type::HapticConfig){
                media->HapticConfiguration(*received);
            }else if(step==5&&media&&(message.Kind()==C::Type::ChannelReady||message.Kind()==C::Type::StreamReady||message.Kind()==C::Type::Heartbeat||message.Kind()==C::Type::KeyframeRequest||message.Kind()==C::Type::Error)){
                media->Received(message);
            }else throw T::Error(T::Failure::Protocol);
        }
        if(step==5&&driver.Snapshot().open){
            bool game=false;std::uint64_t epoch=0;
            {std::lock_guard lock(stateMutex);game=state.gameRunning;epoch=state.gameChanges;}
            if(!game&&media){
                driver.Send(T::MakeMessage(C::Type::SessionStop,0,Json{{"session_id",C::Hex(media->Session())},{"reason","game_end"}}.dump()),ReplyEnd(),T::Now());media.reset();
            }
            if(game&&quality&&driver.Snapshot().negotiated.video&&!media&&IsZero(driver.Snapshot().session)){
                media=std::make_unique<VideoHost>(driver,identity.identity,identity.peer->clientPin,stop,effects,*quality,epoch,nextStreamRequest);nextStreamRequest+=2;
            }
            if(media)media->Tick();
            {std::lock_guard lock(stateMutex);state.videoStreaming=media&&media->Streaming();state.videoFrames=media?media->Frames():0;}
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
}
