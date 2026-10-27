#include "OpusAudio.h"
#include <opus.h>
#include <algorithm>
#include <stdexcept>

namespace CemuPad::Media {
namespace {void Need(bool value){if(!value)throw std::runtime_error("Opus audio operation failed");}
std::uint32_t U32(std::span<const std::uint8_t> v,unsigned at){return (std::uint32_t(v[at])<<24)|(std::uint32_t(v[at+1])<<16)|(std::uint32_t(v[at+2])<<8)|v[at+3];}}
void AudioAccumulator::Push(const AudioBlock& block,const std::function<void(const AudioPcm&)>& emit){
    if(generation!=block.generation||next!=block.sample){filled=0;generation=block.generation;}
    next=block.sample+AudioBlockSamples;
    for(unsigned offset=0;offset<AudioBlockSamples;){
        if(!filled){pending.sample=block.sample+offset;pending.timeUs=block.timeUs+std::uint64_t(offset)*1'000'000/AudioRate;}
        auto count=std::min(AudioBlockSamples-offset,AudioPacketSamples-filled);
        std::copy_n(block.pcm.begin()+offset*2,count*2,pending.pcm.begin()+filled*2);offset+=count;filled+=count;
        if(filled==AudioPacketSamples){emit(pending);filled=0;}
    }
}
OpusEncode::OpusEncode(){
    int error=0;encoder=opus_encoder_create(AudioRate,2,OPUS_APPLICATION_RESTRICTED_LOWDELAY,&error);Need(encoder&&error==OPUS_OK);
    try{Need(opus_encoder_ctl(encoder,OPUS_SET_BITRATE(96000))==OPUS_OK);Need(opus_encoder_ctl(encoder,OPUS_SET_VBR_CONSTRAINT(1))==OPUS_OK);
        Need(opus_encoder_ctl(encoder,OPUS_SET_COMPLEXITY(5))==OPUS_OK);Need(opus_encoder_ctl(encoder,OPUS_SET_INBAND_FEC(0))==OPUS_OK);
    }catch(...){opus_encoder_destroy(encoder);encoder=nullptr;throw;}
}
OpusEncode::~OpusEncode(){if(encoder)opus_encoder_destroy(encoder);}
EncodedAudio OpusEncode::Encode(const AudioPcm& pcm){
    EncodedAudio result;result.payload.resize(1000);result.sample=pcm.sample;result.timeUs=pcm.timeUs;
    int n=opus_encode(encoder,pcm.pcm.data(),AudioPacketSamples,result.payload.data(),static_cast<opus_int32>(result.payload.size()));Need(n>0);result.payload.resize(n);return result;
}
std::vector<std::uint8_t> AudioRtp(std::span<const std::uint8_t> payload,std::uint32_t ssrc,std::uint16_t sequence,std::uint32_t timestamp,bool marker){
    Need(!payload.empty()&&payload.size()<=1000&&ssrc);std::vector<std::uint8_t> out(12+payload.size());out[0]=0x80;out[1]=111|(marker?0x80:0);
    out[2]=static_cast<std::uint8_t>(sequence>>8);out[3]=static_cast<std::uint8_t>(sequence);
    auto put=[&](unsigned at,std::uint32_t v){for(unsigned i=0;i<4;++i)out[at+i]=static_cast<std::uint8_t>(v>>(24-i*8));};put(4,timestamp);put(8,ssrc);
    std::copy(payload.begin(),payload.end(),out.begin()+12);return out;
}
OpusJitter::OpusJitter(std::uint32_t value):ssrc(value){Need(ssrc!=0);int error=0;decoder=opus_decoder_create(AudioRate,2,&error);Need(decoder&&error==OPUS_OK);}
OpusJitter::OpusJitter(std::uint32_t value,std::uint32_t rtp,std::uint64_t host,std::uint64_t sample):OpusJitter(value){
    sourceClock=true;base=rtp;hostBase=host;sampleBase=sample;
}
OpusJitter::~OpusJitter(){if(decoder)opus_decoder_destroy(decoder);}
bool OpusJitter::Push(std::span<const std::uint8_t> packet,std::uint64_t now){
    if(packet.size()<13||packet.size()>1012||packet[0]!=0x80||(packet[1]&127)!=111||U32(packet,8)!=ssrc||opus_packet_get_nb_samples(packet.data()+12,static_cast<opus_int32>(packet.size()-12),AudioRate)!=AudioPacketSamples){++dropped;return false;}
    auto timestamp=U32(packet,4);
    if(!started){started=true;played=false;
        if(sourceClock)expected+=static_cast<std::int32_t>(timestamp-base-static_cast<std::uint32_t>(expected));
        else{base=timestamp;expected=0;}
        due=now+20'000;queue.clear();concealed=0;Need(opus_decoder_ctl(decoder,OPUS_RESET_STATE)==OPUS_OK);}
    auto index=expected+static_cast<std::int32_t>(timestamp-base-static_cast<std::uint32_t>(expected));
    if(!played&&index<expected&&index>=expected-960)expected=index;
    if(index<expected){++dropped;return false;}
    // A marker describes a source discontinuity, not an instruction to discard
    // earlier audio still queued for playback. Keep it with its packet until
    // Poll reaches that boundary. Large jumps still use the bounded re-prime below.
    // Receive batches are drained before Poll. Preserve packets whose playout
    // deadlines are already due: they are backlog, not extra future latency.
    // Poll still rejects stalls over 30ms; no new startup delay is introduced.
    const auto overdue=now>due?std::min<std::uint64_t>((now-due)/10'000,3)*AudioPacketSamples:0;
    if(index>expected+1440+static_cast<std::int64_t>(overdue)){dropped+=queue.size();queue.clear();expected=index;due=now+20'000;concealed=0;Need(opus_decoder_ctl(decoder,OPUS_RESET_STATE)==OPUS_OK);}
    if(!queue.emplace(index,Packet{std::vector<std::uint8_t>(packet.begin()+12,packet.end()),(packet[1]&0x80)!=0}).second){++dropped;return false;}
    while(queue.size()>6){auto first=queue.begin();expected=first->first+AudioPacketSamples;queue.erase(first);++dropped;due=now;}
    return true;
}
std::optional<AudioPcm> OpusJitter::Poll(std::uint64_t now){
    if(!started||now<due)return {};
    if(now-due>30'000){
        if(queue.empty()){started=false;return {};}
        auto latest=std::prev(queue.end());auto keep=std::move(latest->second);expected=latest->first;dropped+=queue.size()-1;queue.clear();queue.emplace(expected,std::move(keep));due=now;concealed=0;
        Need(opus_decoder_ctl(decoder,OPUS_RESET_STATE)==OPUS_OK);
    }
    // Source capture uses 3ms blocks whereas Opus uses 10ms packets. After
    // concealing any whole missing packets, align the remaining sub-packet gap
    // at playback time. Do not add a new 20ms startup wait or reset a continuing
    // encoder's decoder state for each small capture discontinuity.
    if(!queue.empty()){
        const auto& first=*queue.begin();
        if(first.first>expected&&first.first<expected+AudioPacketSamples&&first.second.discontinuity){
            // Advance both timelines. Advancing only the source cursor consumes
            // the entire jitter reserve after several small gaps and makes
            // timely packets look late, causing avoidable PLC and rejection.
            due+=static_cast<std::uint64_t>(first.first-expected)*1'000'000/AudioRate;
            expected=first.first;
            if(now<due)return {};
        }
    }
    AudioPcm pcm;pcm.sample=expected>=0?static_cast<std::uint64_t>(expected):0;pcm.timeUs=due;
    // Keep capture time independent of packet arrival/jitter deadlines. The same
    // host clock also timestamps video; resetting a jitter queue must not reset it.
    if(sourceClock){
        const auto offsetUs=expected*1'000'000/AudioRate;
        Need(offsetUs>=0||hostBase>=static_cast<std::uint64_t>(-offsetUs));
        pcm.timeUs=static_cast<std::uint64_t>(static_cast<std::int64_t>(hostBase)+offsetUs);
        pcm.sample=static_cast<std::uint64_t>(static_cast<std::int64_t>(sampleBase)+expected);
    }
    auto found=queue.find(expected);int count;
    if(found!=queue.end()){count=opus_decode(decoder,found->second.payload.data(),static_cast<opus_int32>(found->second.payload.size()),pcm.pcm.data(),AudioPacketSamples,0);queue.erase(found);concealed=0;}
    else{
        if(concealed>=5){started=false;queue.clear();return {};}
        count=opus_decode(decoder,nullptr,0,pcm.pcm.data(),AudioPacketSamples,0);++lost;++concealed;
    }
    Need(count==AudioPacketSamples);expected+=AudioPacketSamples;due+=10'000;played=true;return pcm;
}
}
