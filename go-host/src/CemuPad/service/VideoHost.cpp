#include "VideoHost.h"
#include "CemuPad/media/VideoTrace.h"
#include "CemuPad/store/Identity.h"
#include "CemuPad/third_party/nlohmann/json.hpp"
#include <openssl/rand.h>

namespace CemuPad::Service {
namespace T=Transport;namespace C=Control;using Json=nlohmann::json;
namespace {
void Need(bool value){if(!value)throw T::Error(T::Failure::Protocol);}
T::Deadline End(){return T::Clock::now()+std::chrono::seconds(2);}
template<class V> V Random(){V value{};do{Need(RAND_bytes(reinterpret_cast<unsigned char*>(&value),sizeof(value))==1);}while(value==V{});return value;}
}
VideoHost::VideoHost(T::ControlTransport& c,const Store::Identity& identity,Crypto::Digest pin,T::Cancellation& cancel,T::Lifecycle& lifecycle,Media::VideoQuality q,std::uint64_t epoch,std::uint64_t streamRequestId)
    :control(c),stop(cancel),effects(lifecycle),quality(q),clientPin(pin),requestId(streamRequestId){
    session=Random<SessionId>();ssrc=Random<std::uint32_t>();rtpBase=Random<std::uint32_t>();
    auto local=control.LocalAddress().ip;
    input=std::make_unique<T::DtlsChannel>(identity,pin,C::Role::Host,C::Channel::InputHaptics,local,stop);
    video=std::make_unique<T::DtlsChannel>(identity,pin,C::Role::Host,C::Channel::Video,local,stop);
    if(control.Snapshot().negotiated.audio)audio=std::make_unique<T::DtlsChannel>(identity,pin,C::Role::Host,C::Channel::Audio,local,stop);
    std::vector<T::DtlsChannel*> channels{input.get(),video.get()};if(audio)channels.push_back(audio.get());
    Json endpoints=Json::array();
    for(auto* channel:channels){auto e=channel->Endpoint();endpoints.push_back({{"channel",C::Name(e.channel)},{"address",e.address},{"port",e.port},{"dtls_role","server"},{"host_spki_sha256",e.hostPin}});}
    control.Send(T::MakeMessage(C::Type::SessionStart,0,Json{{"session_id",C::Hex(session)},{"game_epoch",std::to_string(epoch)},{"channels",endpoints}}.dump()),End(),T::Now());
    for(auto* channel:channels){
        auto endpoint=channel->Endpoint();auto work=control.BeginChannel(endpoint.channel);Need(work.has_value());
        auto evidence=channel->Handshake(session,endpoint,control.PeerAddress(),T::Clock::now()+std::chrono::seconds(5),channel!=input.get()?std::function<void()>([&]{input->Pump();}):std::function<void()>{});
        Need(bool(control.CompleteChannel(*work,evidence)));
        control.Send(T::MakeMessage(C::Type::ChannelReady,0,Json{{"session_id",C::Hex(session)},{"channel",C::Name(endpoint.channel)},{"dtls_exporter_sha256",C::Hex(evidence.Binding())}}.dump()),End(),T::Now());
    }
    lastPeer=T::Clock::now();
    if(effects.motionControls)effects.motionControls->Bind(session);
}
VideoHost::~VideoHost(){if(effects.motionControls)effects.motionControls->Unbind(session);if(effects.rumble)effects.rumble->Cancel();audioEncoder.reset();encoder.reset();}
void VideoHost::HapticConfiguration(const T::Received& received){
    const auto& message=received.message;
    Need(message.Kind()==C::Type::HapticConfig&&received.decision.work.has_value()&&C::Session(message.Text("session_id"))==session);
    outputGeneration=static_cast<std::uint32_t>(message.Integer("output_generation"));
    outputAvailable=message.Flag("available");rumbleSent=false;lastRumble=false;
    if(effects.rumble)effects.rumble->Cancel();
    Need(bool(control.CompleteApplication(*received.decision.work)));
    control.Send(T::MakeMessage(C::Type::HapticReady,message.Id(),Json{{"session_id",C::Hex(session)},{"output_generation",outputGeneration}}.dump()),End(),T::Now());
}
void VideoHost::RumbleTick(const C::ControlSnapshot& snapshot){
    if(!effects.rumble||!snapshot.negotiated.haptics||!snapshot.hapticReady||!outputAvailable)return;
    auto now=T::Clock::now();
    bool on=effects.rumble->Poll(T::Now(),effects.core.State()==SessionState::Active);
    if(rumbleSent&&on==lastRumble&&(!on||now-lastRumbleTime<std::chrono::milliseconds(40)))return;
    RumblePacket packet;packet.header.session=session;packet.header.sequence=rumbleSequence++;Need(rumbleSequence!=0);
    packet.header.senderTimeUs=T::Now();packet.outputGeneration=outputGeneration;packet.on=on;packet.leaseMs=on?100:0;
    control.WithAuthorizedHapticOn(session,outputGeneration,[&]{
        if(input->SendData(EncodeRumble(packet))){rumbleSent=true;lastRumble=on;lastRumbleTime=now;}
    });
}
void VideoHost::Received(const C::Message& message){
    if(message.Kind()==C::Type::Heartbeat||message.Kind()==C::Type::ChannelReady||message.Kind()==C::Type::StreamReady)lastPeer=T::Clock::now();
    if(message.Kind()==C::Type::KeyframeRequest&&encoder){
        auto now=T::Clock::now();if(now-lastIdrRequest>=std::chrono::milliseconds(500)){encoder->RequestIdr();lastIdrRequest=now;awaitIdr=true;}
    }
    if(message.Kind()==C::Type::Error){
        if(message.Text("code")=="AUDIO_UNAVAILABLE"){audioEncoder.reset();audioFailed=true;}
        else{pending.clear();pendingAt=0;encoder.reset();failed=true;}
        lastPeer=T::Clock::now();
    }
}
void VideoHost::AudioTick(const C::ControlSnapshot& snapshot){
    if(!audio||audioFailed||!snapshot.dataReady[2]||!configured||!snapshot.streamReady[0])return;
    audio->Pump();
    try{
        if(!audioEncoder)audioEncoder=std::make_unique<Media::AudioEncoder>();
        if(!audioEncoder->Failure().empty())throw std::runtime_error("Audio encode failed");
        for(unsigned i=0;i<3;++i){
            auto packet=audioEncoder->Poll();if(!packet)break;
            if(!audioConfigured){
                audioSsrc=Random<std::uint32_t>();audioRtpBase=Random<std::uint32_t>();audioSequence=Random<std::uint16_t>();audioSampleBase=packet->sample;
                audio->SetSsrc(audioSsrc);
                Json payload{{"session_id",C::Hex(session)},{"stream","audio"},{"generation",1},{"ssrc",audioSsrc},{"codec","Opus"},{"rtp_payload_type",111},
                    {"channels",2},{"sample_rate",48000},{"rtp_rate",48000},{"packet_ms",10},{"sample_base",std::to_string(audioSampleBase)},
                    {"rtp_base",audioRtpBase},{"host_time_us",std::to_string(packet->timeUs)}};
                auto decision=control.Send(T::MakeMessage(C::Type::StreamConfig,requestId+1,payload.dump()),End(),T::Now());Need(decision.work.has_value());
                Need(bool(control.CompleteApplication(*decision.work)));audioConfigured=true;continue;
            }
            if(!snapshot.streamReady[1])continue;
            auto bytes=Media::AudioRtp(packet->payload,audioSsrc,audioSequence++,audioRtpBase+static_cast<std::uint32_t>(packet->sample-audioSampleBase),!audioSent||packet->sample!=audioLastSample+480);
            bool sent=false;control.WithAuthorizedStream(session,C::Channel::Audio,1,audioSsrc,[&]{sent=audio->SendRtp(bytes);});
            if(sent){audioSent=true;audioLastSample=packet->sample;Media::CemuAudioFrames().RemotePlayback(true);}
        }
    }catch(const T::Error&){throw;}catch(...){
        audioEncoder.reset();audioFailed=true;
        control.Send(T::MakeMessage(C::Type::Error,0,R"({"code":"AUDIO_UNAVAILABLE","message":"GamePad audio encoder unavailable","fatal":false})"),End(),T::Now());
    }
}
void VideoHost::Tick(){
    auto now=T::Clock::now();
    input->Pump();video->Pump();auto snapshot=control.Snapshot();
    if(Media::VideoTraceEnabled()){++Media::VideoTrace().videoTicks;Media::VideoTrace().readyBits=(snapshot.dataReady[0]?1u:0u)|(snapshot.dataReady[1]?2u:0u)|(snapshot.streamReady[0]?4u:0u);}
    if(!coreBound&&snapshot.dataReady[0]){Need(effects.core.OnAuthenticatedSession(C::Hex(clientPin),session,T::Now()));coreBound=true;}
    for(unsigned i=0;i<32;++i){auto bytes=input->ReceiveData();if(!bytes)break;
        control.WithAuthorizedInput(session,[&]{
            if(auto command=MotionWire::Request(*bytes)){
                if(command->session==session&&snapshot.negotiated.gyro&&snapshot.negotiated.accel&&effects.motionControls)
                    effects.motionControls->Submit(*command,T::Now());
                return;
            }
            auto decoded=DecodeInput(*bytes,{static_cast<float>(snapshot.negotiated.gyroAbsMax),static_cast<float>(snapshot.negotiated.accelAbsMax)});
            if(!decoded||(!snapshot.negotiated.gyro&&(decoded.value->state.validity&GyroValid))||(!snapshot.negotiated.accel&&(decoded.value->state.validity&AccelValid)))return;
            if(effects.core.AcceptInput(*bytes,T::Now())==AcceptResult::Accepted)lastPeer=now;
        });}
    if(effects.motionControls&&snapshot.dataReady[0]&&now-motionReplyTime>=std::chrono::milliseconds(25)){
        if(auto reply=effects.motionControls->Reply(T::Now()))input->SendData(MotionWire::Encode(*reply));
        motionReplyTime=now;
    }
    if(now-lastPeer>std::chrono::seconds(3))throw T::Error(T::Failure::Timeout);
    if(now-lastHeartbeat>=std::chrono::seconds(1)){
        control.Send(T::MakeMessage(C::Type::Heartbeat,0,Json{{"session_id",C::Hex(session)},{"monotonic_us",std::to_string(T::Now())}}.dump()),End(),T::Now());lastHeartbeat=now;
    }
    RumbleTick(snapshot);AudioTick(snapshot);
    if(failed||!snapshot.dataReady[0]||!snapshot.dataReady[1])return;
    if(!encoder)encoder=std::make_unique<Media::HardwareEncoder>(quality);
    if(!encoder->Status().failure.empty()){
        control.Send(T::MakeMessage(C::Type::Error,0,R"({"code":"VIDEO_UNAVAILABLE","message":"Hardware video encoder unavailable","fatal":false})"),End(),T::Now());
        encoder.reset();failed=true;return;
    }
    // Service input/control before each bounded batch, including large IDR bursts.
    if(!pending.empty()){
        if(now-pendingSince>std::chrono::milliseconds(100)){pending.clear();pendingAt=0;awaitIdr=true;encoder->RequestIdr();}
        else{
            bool dropped=false;
            control.WithAuthorizedStream(session,C::Channel::Video,1,ssrc,[&]{
                for(unsigned n=0;n<32&&pendingAt<pending.size();++n){if(!video->SendRtp(pending[pendingAt++])){dropped=true;break;}if(Media::VideoTraceEnabled())++Media::VideoTrace().packetsSent;}
            });
            if(dropped){pending.clear();pendingAt=0;awaitIdr=true;encoder->RequestIdr();}
            else if(pendingAt==pending.size()){pending.clear();pendingAt=0;++sentFrames;if(Media::VideoTraceEnabled())++Media::VideoTrace().framesSent;}
        }
        if(!pending.empty())return;
    }
    for(unsigned i=0;i<1;++i){
        auto frame=encoder->Poll();if(!frame)break;
        if(!frame->sps.empty())sps=frame->sps;if(!frame->pps.empty())pps=frame->pps;
        for(const auto& nal:frame->nals){if((nal[0]&31)==7)sps=nal;if((nal[0]&31)==8)pps=nal;}
        if(!configured){
            if(sps.empty()||pps.empty())continue;
            auto format=Media::ValidateH264(sps,pps);Need(format.width==quality.width&&format.height==quality.height);
            Json payload{{"session_id",C::Hex(session)},{"stream","video"},{"generation",1},{"ssrc",ssrc},{"codec","H264"},{"rtp_payload_type",96},{"packetization_mode",1},
                {"profile","baseline"},{"level",format.level},{"sps",Store::Base64(sps)},{"pps",Store::Base64(pps)},{"coded_width",format.codedWidth},{"coded_height",format.codedHeight},
                {"crop",{{"left",format.left},{"top",format.top},{"width",format.width},{"height",format.height}}},{"color_matrix","BT709"},{"color_range","limited"},
                {"rtp_rate",90000},{"rtp_base",rtpBase},{"host_time_us",std::to_string(frame->timestampUs)}};
            video->SetSsrc(ssrc);packetizer=std::make_unique<Media::H264Packetizer>(ssrc,Random<std::uint16_t>());
            firstTimestamp=frame->timestampUs;
            auto decision=control.Send(T::MakeMessage(C::Type::StreamConfig,requestId,payload.dump()),End(),T::Now());Need(decision.work.has_value());
            Need(bool(control.CompleteApplication(*decision.work)));configured=true;if(Media::VideoTraceEnabled())++Media::VideoTrace().configSent;continue;
        }
        if(!snapshot.streamReady[0]){encoder->RequestIdr();continue;}
        if(awaitIdr&&!Media::HasIdr(frame->nals)){encoder->RequestIdr();continue;}
        awaitIdr=false;
        auto timestamp=rtpBase+static_cast<std::uint32_t>((frame->timestampUs-firstTimestamp)*90/1000);
        control.WithAuthorizedStream(session,C::Channel::Video,1,ssrc,[&]{packetizer->Send(frame->nals,timestamp,[&](Media::Bytes bytes){pending.push_back(std::move(bytes));});});
        Need(pending.size()<=2048);pendingSince=now;pendingAt=0;
    }
}
}
