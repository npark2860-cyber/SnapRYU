#include "VideoFrames.h"
#include <stdexcept>

namespace CemuPad::Media {
std::uint64_t VideoFrames::Start(unsigned rate){
    if(rate!=30&&rate!=60)throw std::invalid_argument("Video frame rate");
    Stop();fps=rate;nextCapture=0;auto token=++generation;enabled.store(true,std::memory_order_release);return token;
}
void VideoFrames::Stop(){enabled.store(false,std::memory_order_release);++generation;std::lock_guard lock(mutex);latest.reset();}
std::uint64_t VideoFrames::CaptureAt(std::uint64_t now){
    if(!Enabled())return 0;auto token=Generation();auto next=nextCapture.load();
    auto period=1'000'000/fps.load();
    // Keep the cadence across small render jitter; now+period drifts and skips
    // alternate frames whenever a nominal 60 Hz callback arrives slightly early.
    if(now+period/8<next)return 0;
    nextCapture=now>next+period?now+period:next+period;return Enabled()?token:0;
}
bool VideoFrames::Publish(std::shared_ptr<RgbaFrame> frame){
    if(!frame||!Enabled()||frame->generation!=Generation()||frame->width<2||frame->width>4096||frame->height<2||frame->height>2160
        ||frame->rgba.size()!=static_cast<std::size_t>(frame->width)*frame->height*4)return false;
    std::unique_lock lock(mutex,std::try_to_lock);if(!lock||!Enabled()||frame->generation!=Generation())return false;
    latest=std::move(frame);return true;
}
std::shared_ptr<RgbaFrame> VideoFrames::Take(std::uint64_t token){
    std::lock_guard lock(mutex);if(!Enabled()||token!=Generation())return {};
    auto out=std::move(latest);latest.reset();return out&&out->generation==token?out:nullptr;
}
VideoFrames& CemuVideoFrames(){static VideoFrames frames;return frames;}
}
