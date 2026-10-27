#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>

namespace CemuPad::Media {
constexpr unsigned AudioRate=48000,AudioBlockSamples=144,AudioPacketSamples=480;
enum class DrcAudioMode { Mono,Stereo,SurroundLROnly };
struct AudioBlock {
    std::array<std::int16_t,AudioBlockSamples*2> pcm{};
    std::uint64_t generation=0,sample=0,timeUs=0;
};
// One 3ms copy per AX update. The producer never waits or allocates.
class AudioFrames {
public:
    std::uint64_t Start();
    void Stop(std::uint64_t);
    bool Enabled() const {return enabled.load(std::memory_order_acquire);}
    std::uint64_t Generation() const {return generation.load(std::memory_order_acquire);}
    void RemotePlayback(bool value) {remotePlayback.store(value,std::memory_order_release);}
    bool RemotePlayback() const {return remotePlayback.load(std::memory_order_acquire);}
    bool Publish(std::span<const std::int32_t> bigEndianPlanar,unsigned samples,DrcAudioMode,std::uint64_t timeUs);
    bool PublishInterleaved(std::span<const std::int16_t> pcm,std::uint64_t timeUs,std::uint64_t expectedGeneration);
    std::optional<AudioBlock> Take(std::uint64_t);
    std::uint64_t Dropped() const {return dropped.load();}
private:
    std::atomic<bool> enabled{false},remotePlayback{false};
    std::atomic<std::uint64_t> generation{0},sample{0},dropped{0};
    std::mutex mutex;std::array<AudioBlock,10> queue{};unsigned head=0,count=0;
    std::uint64_t interleavedOriginUs=0;bool interleavedStarted=false;
};
AudioFrames& CemuAudioFrames();
}
