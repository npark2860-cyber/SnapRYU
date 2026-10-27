#include "AudioEncoder.h"

namespace CemuPad::Media {
AudioEncoder::AudioEncoder():generation(CemuAudioFrames().Start()){
    try{worker=std::thread([this]{try{
        OpusEncode encoder;AudioAccumulator accumulator;
        while(!stop){auto block=CemuAudioFrames().Take(generation);
            if(!block){std::this_thread::sleep_for(std::chrono::milliseconds(1));continue;}
            accumulator.Push(*block,[&](const AudioPcm& pcm){auto packet=encoder.Encode(pcm);std::lock_guard lock(mutex);if(packets.size()==3)packets.pop_front();packets.push_back(std::move(packet));});
        }
    }catch(...){std::lock_guard lock(mutex);failure="Opus encoder unavailable";}});}catch(...){CemuAudioFrames().Stop(generation);throw;}
}
AudioEncoder::~AudioEncoder(){stop=true;CemuAudioFrames().Stop(generation);if(worker.joinable())worker.join();}
std::optional<EncodedAudio> AudioEncoder::Poll(){std::lock_guard lock(mutex);if(packets.empty())return {};auto value=std::move(packets.front());packets.pop_front();return value;}
std::string AudioEncoder::Failure() const{std::lock_guard lock(mutex);return failure;}
}
