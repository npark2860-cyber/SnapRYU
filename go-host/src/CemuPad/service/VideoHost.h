#pragma once
#include "CemuPad/transport/ControlTransport.h"
#include "CemuPad/transport/DtlsChannel.h"
#include "CemuPad/media/HardwareEncoder.h"
#include "CemuPad/media/RtpH264.h"
#include "CemuPad/media/AudioEncoder.h"

namespace CemuPad::Service {
class VideoHost {
public:
    VideoHost(Transport::ControlTransport&,const Store::Identity&,Crypto::Digest clientPin,
        Transport::Cancellation&,Transport::Lifecycle&,Media::VideoQuality,std::uint64_t gameEpoch,std::uint64_t streamRequestId);
    ~VideoHost();
    void Tick();
    void Received(const Control::Message&);
    void HapticConfiguration(const Transport::Received&);
    SessionId Session() const{return session;}
    bool Streaming() const{return sentFrames!=0&&!failed;}
    std::uint64_t Frames() const{return sentFrames;}
private:
    Transport::ControlTransport& control;Transport::Cancellation& stop;Transport::Lifecycle& effects;
    Media::VideoQuality quality;SessionId session{};Crypto::Digest clientPin;std::uint64_t requestId;
    std::unique_ptr<Transport::DtlsChannel> input,video;
    std::unique_ptr<Transport::DtlsChannel> audio;
    std::unique_ptr<Media::AudioEncoder> audioEncoder;
    std::uint32_t audioSsrc=0,audioRtpBase=0;std::uint16_t audioSequence=0;
    std::uint64_t audioSampleBase=0,audioLastSample=0;bool audioConfigured=false,audioFailed=false,audioSent=false;
    void AudioTick(const Control::ControlSnapshot&);
    void RumbleTick(const Control::ControlSnapshot&);
    std::uint32_t outputGeneration=0;
    std::uint64_t rumbleSequence=1;
    bool outputAvailable=false,rumbleSent=false,lastRumble=false;
    Transport::Deadline lastRumbleTime{};
    std::unique_ptr<Media::HardwareEncoder> encoder;std::unique_ptr<Media::H264Packetizer> packetizer;
    Media::Bytes sps,pps;std::uint32_t ssrc=0,rtpBase=0;std::uint64_t firstTimestamp=0,sentFrames=0;
    std::vector<Media::Bytes> pending;std::size_t pendingAt=0;
    Transport::Deadline pendingSince{};
    Transport::Deadline lastPeer=Transport::Clock::now(),lastHeartbeat=lastPeer,lastIdrRequest=lastPeer;
    bool coreBound=false,configured=false,awaitIdr=true,failed=false;
    Transport::Deadline motionReplyTime{};
};
}
