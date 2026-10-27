#pragma once
#include "H264.h"
#include <functional>
#include <optional>

namespace CemuPad::Media {
class H264Packetizer {
public:
    explicit H264Packetizer(std::uint32_t ssrc,std::uint16_t sequence=0):ssrc(ssrc),sequence(sequence){}
    void Send(const std::vector<Bytes>& nals,std::uint32_t timestamp,const std::function<void(Bytes)>& output);
private:
    std::uint32_t ssrc;std::uint16_t sequence;
};
struct AccessUnit {Bytes annexB;std::uint32_t timestamp=0;bool idr=false;};
class H264Depacketizer {
public:
    explicit H264Depacketizer(std::uint32_t ssrc):ssrc(ssrc){}
    std::optional<AccessUnit> Push(std::span<const std::uint8_t> rtp);
    void Reset();
    std::uint64_t Dropped() const{return dropped;}
private:
    std::uint32_t ssrc,timestamp=0;std::uint16_t next=0;
    bool active=false,damaged=false,fragment=false,idr=false,haveSequence=false;
    std::uint8_t fragmentHeader=0;Bytes bytes;std::uint64_t dropped=0;
};
}
