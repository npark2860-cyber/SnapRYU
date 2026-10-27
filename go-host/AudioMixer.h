#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>

namespace SnapGoHost {
// Callbacks copy into fixed storage using try_lock. The worker consumes the
// capture timeline 24 ms later; no lock here covers I/O or encoding.
class AudioMixer {
public:
    static constexpr std::uint32_t Frames=144, Slots=128, MaxStreams=16;
    static constexpr std::uint32_t MaxPushFrames=16384, FadeFrames=96;
    static constexpr std::uint64_t DelayUs=24000, MaxWorkerLateUs=12000;
    struct Block {std::array<std::int16_t,Frames*2> samples{};std::uint64_t time_us=0;};
    struct Stats {
        std::uint64_t push_drops{},late_frames{},reanchors{},concealed_frames{};
        std::uint64_t future_frames{},worker_skipped_frames{};
    };
    Stats GetStats()const {
        return {push_drops.load(std::memory_order_relaxed),late_frames.load(std::memory_order_relaxed),
            reanchors.load(std::memory_order_relaxed),concealed_frames.load(std::memory_order_relaxed),
            future_frames.load(std::memory_order_relaxed),worker_skipped_frames.load(std::memory_order_relaxed)};
    }
    bool Push(std::uint64_t generation,std::uint64_t id,std::uint64_t time_us,
              unsigned channels,unsigned count,const std::int16_t* data) {
        if(!generation||!id||!data||!count||count>MaxPushFrames||!(channels==1||channels==2||channels==6||channels==8)){
            ++push_drops;return false;
        }
        std::unique_lock lock(mutex,std::try_to_lock);if(!lock){++push_drops;return false;}
        if(epoch!=generation)ResetLocked(generation);
        Stream* stream=nullptr;
        for(auto& s:streams)if(s.id==id){stream=&s;break;}
        if(!stream)for(auto& s:streams)if(!s.id||time_us>s.last_us+500000){stream=&s;*stream={};s.id=id;break;}
        if(!stream||(stream->last_us&&time_us<stream->last_us)){++push_drops;return false;}
        const auto observed=time_us*48/1000;
        auto first=stream->next_sample;
        const bool initialized=first!=0;
        const bool stale_callback=(time_us>stream->last_us+100000||observed>first+4800)&&first+count<=observed;
        if(!initialized||stream->reanchor_next||stale_callback){
            first=observed;if(initialized)++reanchors;
        }
        stream->reanchor_next=false;
        if(!started){next_block=first/Frames;started=true;}
        const auto head=next_block*Frames;
        // A whole obsolete callback gets a fresh capture anchor. A catch-up
        // batch spanning the head keeps its original sample positions instead.
        if(first+count<=head&&observed>first){first=observed;++reanchors;}
        const auto end=first+count;
        stream->last_us=time_us;stream->next_sample=end;
        const auto begin=std::max(first,head);
        const auto limit=std::min(end,(next_block+Slots)*Frames);
        const auto late=std::min<std::uint64_t>(count,head>first?head-first:0);
        late_frames.fetch_add(late,std::memory_order_relaxed);
        if(end>(next_block+Slots)*Frames)
            future_frames.fetch_add(end-std::max(first,(next_block+Slots)*Frames),std::memory_order_relaxed);
        // Partial catch-up must not strand all subsequent ordinary callbacks
        // across the consumed boundary. The next fresh callback reanchors.
        stream->reanchor_next=late&&end<=observed;
        if(begin>=limit){++push_drops;return false;}
        for(auto at=begin;at<limit;++at){
            const auto block=at/Frames;
            auto& slot=ring[block%Slots];if(slot.index!=block){slot={};slot.index=block;}
            const auto* in=data+static_cast<std::size_t>(at-first)*channels;
            std::int32_t left=in[0],right=channels==1?left:in[1];
            if(channels>=6){
                // Standard interleaved FL FR FC LFE SL SR [BL BR].
                left+=in[2]*181/256+in[3]/2+in[4]*181/256;
                right+=in[2]*181/256+in[3]/2+in[5]*181/256;
                if(channels==8){left+=in[6]/2;right+=in[7]/2;}
            }
            const auto sample=static_cast<unsigned>(at%Frames)*2;
            slot.mix[sample]+=left;slot.mix[sample+1]+=right;
            slot.covered[sample/2]=true;
        }
        return true;
    }
    std::optional<Block> Pop(std::uint64_t generation,std::uint64_t now_us){
        std::lock_guard lock(mutex);
        if(epoch!=generation){ResetLocked(generation);return {};}
        if(!generation||!started)return {};
        const auto due=next_block*Frames*1000/48+DelayUs;
        if(now_us<due)return {};
        // Drain at most 12 ms of worker backlog. After a longer stall, the first
        // returned capture timestamp is 24..27 ms old, never seconds old.
        if(now_us>due+MaxWorkerLateUs){
            const auto target=(now_us-DelayUs)*48/1000/Frames;
            worker_skipped_frames.fetch_add((target-next_block)*Frames,std::memory_order_relaxed);
            next_block=target;fade_from=last_output;fade_position=0;
        }
        Block out;out.time_us=next_block*Frames*1000/48;
        auto& slot=ring[next_block%Slots];
        unsigned missing_frames=0;
        for(unsigned frame=0;frame<Frames;++frame){
            const bool covered=slot.index==next_block&&slot.covered[frame];
            if(covered!=previous_covered){fade_from=last_output;fade_position=0;}
            if(!covered)++missing_frames;
            if(fade_position<FadeFrames)++fade_position;
            for(unsigned channel=0;channel<2;++channel){
                const auto target=covered?std::clamp(slot.mix[frame*2+channel],-32768,32767):0;
                // A 2 ms transition conceals only the boundary, not the gap's
                // duration. It never marks missing samples as covered.
                const auto value=fade_from[channel]+(target-fade_from[channel])*
                    static_cast<std::int32_t>(fade_position)/static_cast<std::int32_t>(FadeFrames);
                out.samples[frame*2+channel]=static_cast<std::int16_t>(value);last_output[channel]=value;
            }
            previous_covered=covered;
        }
        concealed_frames.fetch_add(missing_frames,std::memory_order_relaxed);
        slot={};++next_block;return out;
    }
    // Lifecycle close must not be lost when a callback is copying. All work
    // under this mutex is bounded memory processing, never I/O or encoding.
    void Close(std::uint64_t id){std::lock_guard lock(mutex);for(auto& s:streams)if(s.id==id)s={};}
    void Reset(std::uint64_t generation){std::lock_guard lock(mutex);ResetLocked(generation);}
private:
    struct Stream{std::uint64_t id=0,next_sample=0,last_us=0;bool reanchor_next=false;};
    struct Slot{std::uint64_t index=~std::uint64_t{};std::array<std::int32_t,Frames*2> mix{};std::array<bool,Frames> covered{};};
    void ResetLocked(std::uint64_t generation){
        epoch=generation;started=false;next_block=0;streams={};ring={};
        last_output={};fade_from={};fade_position=0;previous_covered=false;
    }
    std::mutex mutex;std::array<Stream,MaxStreams> streams{};std::array<Slot,Slots> ring{};
    std::uint64_t epoch=0,next_block=0;bool started=false;
    bool previous_covered=false;unsigned fade_position=0;
    std::array<std::int32_t,2> last_output{},fade_from{};
    std::atomic<std::uint64_t> push_drops{},late_frames{},reanchors{},concealed_frames{};
    std::atomic<std::uint64_t> future_frames{},worker_skipped_frames{};
};
}
