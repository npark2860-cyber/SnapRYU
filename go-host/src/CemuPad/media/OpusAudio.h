#pragma once
#include "AudioFrames.h"
#include <functional>
#include <map>
#include <memory>
#include <vector>

struct OpusEncoder;struct OpusDecoder;
namespace CemuPad::Media {
struct AudioPcm {std::array<std::int16_t,AudioPacketSamples*2> pcm{};std::uint64_t sample=0,timeUs=0;};
class AudioAccumulator {
public:
    void Push(const AudioBlock&,const std::function<void(const AudioPcm&)>&);
private:
    AudioPcm pending;unsigned filled=0;std::uint64_t next=0,generation=0;
};
struct EncodedAudio {std::vector<std::uint8_t> payload;std::uint64_t sample=0,timeUs=0;};
class OpusEncode {
public:
    OpusEncode();~OpusEncode();
    OpusEncode(const OpusEncode&)=delete;OpusEncode& operator=(const OpusEncode&)=delete;
    EncodedAudio Encode(const AudioPcm&);
private:
    OpusEncoder* encoder=nullptr;
};
std::vector<std::uint8_t> AudioRtp(std::span<const std::uint8_t>,std::uint32_t ssrc,std::uint16_t sequence,std::uint32_t timestamp,bool marker);
// 20ms initial jitter; bounded storage for 30ms future plus 30ms due packets.
// Poll retains its 30ms stale-playout cutoff and 50ms consecutive PLC limit.
// Call Poll with the connection owner's monotonic clock, never a wall clock.
class OpusJitter {
public:
    explicit OpusJitter(std::uint32_t ssrc);~OpusJitter();
    OpusJitter(std::uint32_t ssrc,std::uint32_t rtpBase,std::uint64_t hostUs,std::uint64_t sampleBase);
    OpusJitter(const OpusJitter&)=delete;OpusJitter& operator=(const OpusJitter&)=delete;
    bool Push(std::span<const std::uint8_t>,std::uint64_t nowUs);
    std::optional<AudioPcm> Poll(std::uint64_t nowUs);
    std::uint64_t Lost() const{return lost;}std::uint64_t Dropped() const{return dropped;}
private:
    OpusDecoder* decoder=nullptr;std::uint32_t ssrc,base=0;bool started=false,played=false;
    bool sourceClock=false;std::uint64_t hostBase=0,sampleBase=0;
    std::int64_t expected=0;std::uint64_t due=0,lost=0,dropped=0;unsigned concealed=0;
    struct Packet {std::vector<std::uint8_t> payload;bool discontinuity=false;};
    std::map<std::int64_t,Packet> queue;
};
}
