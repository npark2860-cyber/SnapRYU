#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include "FramePixels.h"

namespace CemuPad::Media {
struct RgbaFrame {
    unsigned width=0,height=0;std::uint64_t timestampUs=0,generation=0;
    std::vector<std::uint8_t> rgba;
    PixelOrder order=PixelOrder::Rgba;
};
// One latest frame between the GPU and encoder owners. GPU producer never waits.
class VideoFrames {
public:
    std::uint64_t Start(unsigned fps);
    void Stop();
    bool Enabled() const{return enabled.load(std::memory_order_acquire);}
    unsigned Rate() const{return fps.load(std::memory_order_acquire);}
    std::uint64_t CaptureAt(std::uint64_t timestampUs);
    bool Publish(std::shared_ptr<RgbaFrame>);
    std::shared_ptr<RgbaFrame> Take(std::uint64_t generation);
    std::uint64_t Generation() const{return generation.load(std::memory_order_acquire);}
private:
    std::atomic<bool> enabled{false};std::atomic<unsigned> fps{60};
    std::atomic<std::uint64_t> generation{0},nextCapture{0};
    std::mutex mutex;std::shared_ptr<RgbaFrame> latest;
};
VideoFrames& CemuVideoFrames();
}
