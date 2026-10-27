// SPDX-FileCopyrightText: Copyright 2026 SnapRYU contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace VideoCore {

struct PresentationCounts {
    std::uint64_t original{};
    std::uint64_t generated{};
    bool available{};
};

struct RendererPerformanceStats {
    PresentationCounts presents;
    std::uint64_t memory_used{};
    std::uint64_t memory_budget{};
    bool memory_available{};
};

struct PresentationRates {
    double total = -1.0;
    double generated = -1.0;
};

// UI-local reader of cumulative counters. Never resets the renderer's counters.
class PresentationRateSampler {
public:
    PresentationRates Sample(PresentationCounts counts, std::int64_t now_ns) {
        PresentationRates result;
        if (counts.available && previous.available && now_ns > previous_ns &&
            now_ns - previous_ns <= 3'500'000'000 && counts.original >= previous.original &&
            counts.generated >= previous.generated) {
            const double seconds = static_cast<double>(now_ns - previous_ns) / 1.0e9;
            const double originals = static_cast<double>(counts.original - previous.original);
            const double generated = static_cast<double>(counts.generated - previous.generated);
            result = {(originals + generated) / seconds, generated / seconds};
        }
        previous = counts;
        previous_ns = now_ns;
        return result;
    }

    void Reset() {
        previous = {};
        previous_ns = 0;
    }

private:
    PresentationCounts previous;
    std::int64_t previous_ns{};
};

} // namespace VideoCore
