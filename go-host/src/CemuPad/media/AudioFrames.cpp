#include "AudioFrames.h"
#include <algorithm>
#include <bit>

namespace CemuPad::Media {
std::uint64_t AudioFrames::Start(){
    enabled=false;remotePlayback=false;std::lock_guard lock(mutex);head=count=0;sample=0;interleavedStarted=false;
    auto next=++generation;enabled.store(true,std::memory_order_release);return next;
}
void AudioFrames::Stop(std::uint64_t token){
    std::lock_guard lock(mutex);if(generation!=token)return;
    enabled=false;remotePlayback=false;++generation;head=count=0;
}
bool AudioFrames::Publish(std::span<const std::int32_t> source,unsigned samples,DrcAudioMode mode,std::uint64_t timeUs){
    if(!Enabled())return false;
    auto token=generation.load();auto at=sample.fetch_add(samples);
    if(samples!=AudioBlockSamples||source.size()<samples*(mode==DrcAudioMode::Mono?1u:2u)){++dropped;return false;}
    std::unique_lock lock(mutex,std::try_to_lock);if(!lock||!enabled||generation!=token){++dropped;return false;}
    if(count==queue.size()){head=(head+1)%queue.size();--count;++dropped;}
    auto& block=queue[(head+count)%queue.size()];block.generation=token;block.sample=at;block.timeUs=timeUs;
    auto pcm=[](std::int32_t be){
        auto u=std::bit_cast<std::uint32_t>(be);u=(u>>24)|((u>>8)&0xff00)|((u<<8)&0xff0000)|(u<<24);
        return static_cast<std::int16_t>(std::clamp(std::bit_cast<std::int32_t>(u),-32768,32767));
    };
    for(unsigned i=0;i<samples;++i){block.pcm[i*2]=pcm(source[i]);block.pcm[i*2+1]=mode==DrcAudioMode::Mono?block.pcm[i*2]:pcm(source[samples+i]);}
    ++count;return true;
}
bool AudioFrames::PublishInterleaved(std::span<const std::int16_t> pcm,std::uint64_t timeUs,std::uint64_t token){
    if(!Enabled()||Generation()!=token||pcm.size()!=AudioBlockSamples*2)return false;
    std::unique_lock lock(mutex,std::try_to_lock);
    if(!lock){++dropped;return false;}
    if(!enabled||generation!=token)return false;
    if(!interleavedStarted){interleavedOriginUs=timeUs;interleavedStarted=true;}
    if(timeUs<interleavedOriginUs)return false;
    const auto at=(timeUs-interleavedOriginUs)*AudioRate/1000000;
    if(at<sample.load())return false;
    if(count==queue.size()){head=(head+1)%queue.size();--count;++dropped;}
    auto& block=queue[(head+count)%queue.size()];
    block.generation=token;block.sample=at;sample=at+AudioBlockSamples;block.timeUs=timeUs;
    std::copy(pcm.begin(),pcm.end(),block.pcm.begin());++count;return true;
}
std::optional<AudioBlock> AudioFrames::Take(std::uint64_t token){
    std::lock_guard lock(mutex);if(!enabled||generation!=token||!count)return {};
    auto result=queue[head];head=(head+1)%queue.size();--count;return result;
}
AudioFrames& CemuAudioFrames(){static AudioFrames frames;return frames;}
}
