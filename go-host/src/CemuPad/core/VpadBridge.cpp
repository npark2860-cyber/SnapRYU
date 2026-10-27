#include "VpadBridge.h"
#include <cmath>

namespace CemuPad {
namespace {
constexpr std::uint32_t repeatFlag=0x80000000;
constexpr std::uint32_t flags[17]={0x8000,0x4000,0x2000,0x1000,0x200,0x100,0x800,0x400,0x20,0x10,0x80,0x40,8,4,0x40000,0x20000,2};
bool Centred(const std::array<float,4>& axes,unsigned i){return std::hypot(axes[i*2],axes[i*2+1])<=0.25f;}
void GateAxes(std::array<float,4>& axes,std::array<bool,2>& gates){
    for(unsigned i=0;i<2;++i){if(Centred(axes,i))gates[i]=false;if(gates[i])axes[i*2]=axes[i*2+1]=0;}
}
std::uint32_t StickFlags(const std::array<float,4>& axes,std::uint32_t last){
    constexpr std::uint32_t left[2]={0x40000000,0x04000000},right[2]={0x20000000,0x02000000},up[2]={0x10000000,0x01000000},down[2]={0x08000000,0x00800000};
    std::uint32_t bits=0;
    for(unsigned i=0;i<2;++i){float x=axes[i*2],y=axes[i*2+1];
        if(x<=-0.5f||((last&left[i])&&x<=-0.1f))bits|=left[i];else if(x>=0.5f||((last&right[i])&&x>=0.1f))bits|=right[i];
        if(y<=-0.5f||((last&down[i])&&y<=-0.1f))bits|=down[i];else if(y>=0.5f||((last&up[i])&&y>=0.1f))bits|=up[i];
    }return bits;
}
}
std::uint32_t VpadBridge::Buttons(std::uint32_t wire){std::uint32_t result=0;for(unsigned i=0;i<17;++i)if(wire&(1u<<i))result|=flags[i];return result;}
VpadSample VpadBridge::Apply(VpadSample local,const std::optional<ConsumedInput>& input,LocalTimeUs now,int delayMs,int pulseMs){
    bool valid=input&&(input->state.validity&PadValid)&&!input->handoverRelease;
    VpadSample output=local;bool filter=false;
    if(valid){
        bool acquiring=!owned||epoch!=input->ownershipEpoch;
        if(acquiring){
            remoteSuppressed=input->state.buttons;
            for(unsigned i=0;i<2;++i)remoteAxes[i]=std::hypot(static_cast<float>(input->state.sticks[i*2]),static_cast<float>(input->state.sticks[i*2+1]))>32767.0f*0.25f;
            localSuppressed=local.hold&~repeatFlag;localAxes={!Centred(local.sticks,0),!Centred(local.sticks,1)};
            owned=true;epoch=input->ownershipEpoch;output={};filter=true;
            // An already delivered local hold must receive release before takeover.
            if(!tracked)previous=(local.hold|local.release)&~repeatFlag;
        }else{
            output={};remoteSuppressed&=input->state.buttons;
            output.hold=Buttons(input->state.buttons&~remoteSuppressed);
            for(unsigned i=0;i<4;++i)output.sticks[i]=static_cast<float>(input->state.sticks[i])/32767.0f;
            GateAxes(output.sticks,remoteAxes);output.hold|=StickFlags(output.sticks,previous);filter=true;
        }
    }else if(owned){
        owned=false;output={};filter=true;localSuppressed=local.hold&~repeatFlag;
        localAxes={!Centred(local.sticks,0),!Centred(local.sticks,1)};
    }else if(localSuppressed||localAxes[0]||localAxes[1]){
        localSuppressed&=local.hold;output.hold&=~localSuppressed;GateAxes(output.sticks,localAxes);
        // Remove direction flags while the corresponding local stick is gated.
        if(localAxes[0])output.hold&=~0x78000000u;if(localAxes[1])output.hold&=~0x07800000u;filter=true;
    }
    if(filter){
        auto hold=output.hold&~repeatFlag;
        if(hold!=previous){changed=pulsed=now;}
        if(owned&&hold&&pulseMs>0&&delayMs>=0&&now>=changed&&now-changed>=static_cast<LocalTimeUs>(delayMs)*1000
            &&now>=pulsed&&now-pulsed>=static_cast<LocalTimeUs>(pulseMs)*1000){output.hold|=repeatFlag;pulsed=now;}
        output.trig=hold&~previous;output.release=previous&~hold;
    }
    previous=output.hold&~repeatFlag;tracked=true;return output;
}
}
