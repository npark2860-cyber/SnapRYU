#pragma once
#include "SessionCore.h"

namespace CemuPad {
// Plain values at the VPADRead boundary. No profile or physical provider mutation.
struct VpadSample {
    std::uint32_t hold=0,trig=0,release=0;
    std::array<float,4> sticks{};
};
class VpadBridge {
public:
    VpadSample Apply(VpadSample local,const std::optional<ConsumedInput>& remote,LocalTimeUs now,int delayMs,int pulseMs);
    bool RemotePad() const{return owned;}
    static std::uint32_t Buttons(std::uint32_t wire);
private:
    bool owned=false,tracked=false;
    std::uint32_t previous=0,remoteSuppressed=0,localSuppressed=0;
    std::array<bool,2> remoteAxes{},localAxes{};
    LocalTimeUs changed=0,pulsed=0;
    std::uint64_t epoch=0;
};
}
