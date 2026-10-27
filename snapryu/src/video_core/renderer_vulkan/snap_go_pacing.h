// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>

namespace Vulkan::SnapGoPacing {
constexpr std::uint64_t PeriodUs(unsigned fps) { return 1'000'000 / (fps == 30 ? 30 : 60); }
constexpr std::uint64_t NextDue(std::uint64_t ready_us, std::uint64_t previous_us, unsigned fps) {
    return (std::max)(ready_us, previous_us ? previous_us + PeriodUs(fps) : ready_us);
}
constexpr std::uint64_t OriginalDue(std::uint64_t generated_us, std::int64_t half_ns, unsigned fps) {
    return generated_us + (std::max)(PeriodUs(fps), static_cast<std::uint64_t>((std::max)(half_ns, std::int64_t{0}) / 1000));
}
// A pair is admitted as a unit: 30 Hz originals -> 60 Hz remote output. At higher
// source rates pairs are sampled rather than submitting twice the encoder budget.
constexpr std::uint64_t AdmissionPeriod(unsigned fps, bool pair) { return PeriodUs(fps) * (pair ? 2 : 1); }
// Capture PTS is independent of GPU completion and dispatch pacing. A generated
// image belongs before its original; invalid timing must never wrap unsigned time.
constexpr std::uint64_t GeneratedSourcePts(std::uint64_t captured_us, std::int64_t half_ns,
                                         unsigned fps) {
    if (half_ns <= 0) return 0;
    // Twelve microseconds survives 90 kHz RTP timestamp quantization.
    const auto half_us = std::clamp(static_cast<std::uint64_t>(half_ns / 1000),
                                    std::uint64_t{12}, PeriodUs(fps));
    return captured_us > half_us ? captured_us - half_us : 0;
}
constexpr bool IsNewSourcePts(std::uint64_t source_us, std::uint64_t previous_us) {
    return source_us != 0 && source_us > previous_us;
}
}
