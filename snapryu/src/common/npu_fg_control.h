// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Settings {
enum class NpuFgMode : std::uint32_t { Off, Auto, Fixed2x };
constexpr NpuFgMode SanitizeNpuFgMode(std::uint32_t value) {
    return value <= 2 ? static_cast<NpuFgMode>(value) : NpuFgMode::Off;
}
} // namespace Settings

namespace VideoCore {
struct NpuFgPlan {
    bool generate{};
    bool multiplier_limited{};
    double base_fps{};
    double achievable_fps{};
    // A single genuine midpoint is the only supported output for a pair.
    static constexpr double timestep = 0.5;
};

class NpuFgControl {
  public:
    NpuFgPlan Plan(Settings::NpuFgMode requested, unsigned target_fps, std::int64_t timestamp_ns,
                   bool capacity_available, double measured_job_ms = 0.0) {
        const auto mode = Settings::SanitizeNpuFgMode(static_cast<std::uint32_t>(requested));
        const unsigned target = target_fps >= 30 && target_fps <= 120 ? target_fps : 60;
        if (mode != last_mode || (mode == Settings::NpuFgMode::Auto && target != last_target)) {
            Reset();
        }
        last_mode = mode;
        last_target = target;
        if (mode == Settings::NpuFgMode::Off) {
            Reset();
            return {};
        }
        if (timestamp_ns < 0) {
            Reset();
            return {};
        }
        if (!have_timestamp) {
            last_ns = timestamp_ns;
            have_timestamp = true;
            return {};
        }
        const double elapsed = static_cast<double>(timestamp_ns - last_ns) / 1e9;
        last_ns = timestamp_ns;
        if (elapsed <= 0 || elapsed > 0.2) {
            interval = 0;
            credit = 0;
            return {};
        }
        // Ignore duplicate/burst timestamps; do not treat them as original frames.
        if (elapsed < 0.001) {
            return {};
        }
        interval = interval == 0 ? elapsed : interval + (elapsed - interval) * 0.2;
        const double base = 1.0 / interval;
        const double desired = mode == Settings::NpuFgMode::Fixed2x
                                   ? 2.0
                                   : std::clamp(static_cast<double>(target) / base, 1.0, 2.0);
        NpuFgPlan plan{false, mode == Settings::NpuFgMode::Auto && target > base * 2.0, base,
                       base * desired};
        // Backpressure drops an admission, never accumulates a burst or reuses stale output.
        if (!capacity_available || !std::isfinite(measured_job_ms) || measured_job_ms < 0 ||
            measured_job_ms * 1.1 > interval * 1000.0) {
            credit = 0;
            plan.achievable_fps = base;
            return plan;
        }
        credit += desired - 1.0;
        if (credit >= 1.0 - 1e-6) {
            plan.generate = true;
            credit -= 1.0;
        }
        credit = std::clamp(credit, 0.0, 1.0);
        return plan;
    }
    void Reset() {
        have_timestamp = false;
        last_ns = 0;
        interval = 0;
        credit = 0;
    }

  private:
    Settings::NpuFgMode last_mode = Settings::NpuFgMode::Off;
    unsigned last_target = 60;
    std::int64_t last_ns{};
    bool have_timestamp{};
    double interval{};
    double credit{};
};
} // namespace VideoCore
