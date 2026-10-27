#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace CemuPad::Media {
// Explicit per-process diagnosis only. No payload, identity, token or key data.
// Compile-time opt-in for dedicated diagnostic builds only.
#ifdef CEMUPAD_ENABLE_VIDEO_TRACE
inline bool VideoTraceEnabled(){static const bool enabled=[] {const char* value=std::getenv("CEMUPAD_VIDEO_TRACE");return value&&std::string_view(value)=="1";}();return enabled;}
#else
inline constexpr bool VideoTraceEnabled(){return false;}
#endif
struct VideoTraceCounters {
    std::atomic<std::uint64_t> captureCalls{0},copyQueued{0},converted{0},published{0},pendingWait{0},rejected{0},allocationFailed{0};
    std::atomic<std::uint32_t> format{0},dimension{0},mip{0},slice{0},width{0},height{0},rejectReason{0};
    std::atomic<std::uint64_t> encoderStarts{0},encoderInputs{0},encoderOutputs{0},encoderEmpty{0},encoderFailed{0};
    std::atomic<std::uint32_t> encoderStage{0},encoderHresult{0},encoderCredits{0},submitted{0},spsBytes{0},ppsBytes{0};
    // Failure: 1 HRESULT, 2 H264 parser, 3 timestamp association, 4 stalled,
    // 5 other exception. Numeric output metadata only, never coded pixels.
    std::atomic<std::uint32_t> encoderFailure{0},encodedBytes{0},encodedNals{0};
    std::atomic<std::uint64_t> encoderZeroOutputs{0},encoderSkippedInputs{0};
    std::atomic<std::int64_t> outputPts{0},oldestPts{0},newestPts{0};
    std::atomic<std::uint64_t> videoTicks{0},configSent{0},framesSent{0},packetsSent{0};
    std::atomic<std::uint32_t> readyBits{0};
};
inline VideoTraceCounters& VideoTrace(){static VideoTraceCounters counters;return counters;}
}
