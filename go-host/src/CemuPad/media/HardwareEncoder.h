#pragma once
#include "H264.h"
#include <memory>
#include <optional>
#include <string>

namespace CemuPad::Media {
struct VideoQuality {unsigned width=1920,height=1080,fps=60;};
struct EncodedFrame {std::vector<Bytes> nals;std::uint64_t timestampUs=0;Bytes sps,pps;};
struct EncoderStatus {bool running=false;std::string failure;std::uint64_t inputs=0,outputs=0,dropped=0;};
// Owns COM, D3D11 and the hardware MFT on one worker. No renderer or socket calls.
class HardwareEncoder {
public:
    explicit HardwareEncoder(VideoQuality);
    ~HardwareEncoder();
    HardwareEncoder(const HardwareEncoder&)=delete;HardwareEncoder& operator=(const HardwareEncoder&)=delete;
    std::optional<EncodedFrame> Poll();
    EncoderStatus Status() const;
    void RequestIdr();
private:
    struct Impl;std::unique_ptr<Impl> m;
};
}
