#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace CemuPad::Media {
using Bytes=std::vector<std::uint8_t>;
struct H264Format {
    unsigned codedWidth=0,codedHeight=0,left=0,top=0,width=0,height=0;
    unsigned profile=0,level=0,spsId=0;
};
// Raw NALs include the one-byte header, without Annex B or AVCC framing.
H264Format ValidateH264(std::span<const std::uint8_t> sps,std::span<const std::uint8_t> pps);
std::vector<Bytes> SplitH264(std::span<const std::uint8_t> accessUnit);
bool HasIdr(const std::vector<Bytes>& nals);
Bytes AnnexB(const std::vector<Bytes>& nals);
}
