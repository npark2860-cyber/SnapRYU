#pragma once
#include "OpusAudio.h"
#include <deque>
#include <string>
#include <thread>

namespace CemuPad::Media {
class AudioEncoder {
public:
    AudioEncoder();~AudioEncoder();
    std::optional<EncodedAudio> Poll();
    std::string Failure() const;
private:
    std::uint64_t generation;std::atomic<bool> stop{false};std::thread worker;
    mutable std::mutex mutex;std::deque<EncodedAudio> packets;std::string failure;
};
}
