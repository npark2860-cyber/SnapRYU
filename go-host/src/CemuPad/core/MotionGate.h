#pragma once
#include "Types.h"

namespace CemuPad
{
enum class MotionAction { Invalid, Prime, Reset, Integrate, NoChange };
struct SensorRead
{
    MotionAction action = MotionAction::Invalid;
    std::array<float, 3> values{};
    std::uint64_t timeUs = 0, deltaUs = 0;
};
struct MotionRead { SensorRead gyro, accel; };
class MotionGate
{
public:
    void Observe(const InputState& state, LocalTimeUs now);
    MotionRead Consume(LocalTimeUs now);
    void Invalidate();
    void Reset();
private:
    struct Sensor
    {
        std::array<float, 3> values{};
        std::uint64_t time = 0, consumedTime = 0;
        LocalTimeUs received = 0;
        bool observed = false, valid = false, consumed = false, reset = false;
        void Invalidate();
        void Observe(std::array<float, 3> v, std::uint64_t t, bool enabled, LocalTimeUs now);
        SensorRead Consume(LocalTimeUs now);
    } m_gyro, m_accel;
};
}
