#include "MotionGate.h"

namespace CemuPad
{
void MotionGate::Sensor::Observe(std::array<float, 3> v, std::uint64_t t, bool enabled, LocalTimeUs now)
{
    if (!enabled) { Invalidate(); return; }
    if (observed && time == t) return; // Keep this watermark even after expiry/invalidity.
    if (observed && (t < time || !IsFresh(now, received, InputLeaseUs))) { consumed = false; reset = true; }
    values = v; time = t; received = now; observed = valid = true;
}
void MotionGate::Sensor::Invalidate() { valid = consumed = false; reset = true; }
SensorRead MotionGate::Sensor::Consume(LocalTimeUs now)
{
    if (!valid || !IsFresh(now, received, InputLeaseUs)) { Invalidate(); return {}; }
    SensorRead out{MotionAction::NoChange, values, time, 0};
    if (!consumed) out.action = reset ? MotionAction::Reset : MotionAction::Prime;
    else if (time != consumedTime)
    {
        if (time < consumedTime || time - consumedTime >= InputLeaseUs) out.action = MotionAction::Reset;
        else { out.action = MotionAction::Integrate; out.deltaUs = time - consumedTime; }
    }
    consumed = true; consumedTime = time; reset = false; return out;
}
void MotionGate::Observe(const InputState& s, LocalTimeUs now)
{
    m_gyro.Observe(s.gyro, s.gyroTimeUs, (s.validity & GyroValid) != 0, now);
    m_accel.Observe(s.accel, s.accelTimeUs, (s.validity & AccelValid) != 0, now);
}
MotionRead MotionGate::Consume(LocalTimeUs now)
{
    auto gyro = m_gyro.Consume(now), accel = m_accel.Consume(now);
    // Timestamps share the sender's monotonic origin; do not fuse a distant pair.
    if (gyro.action != MotionAction::Invalid && accel.action != MotionAction::Invalid)
    {
        auto skew = gyro.timeUs > accel.timeUs ? gyro.timeUs - accel.timeUs : accel.timeUs - gyro.timeUs;
        if (skew >= InputLeaseUs) { m_gyro.Invalidate(); m_accel.Invalidate(); return {}; }
    }
    return {gyro, accel};
}
void MotionGate::Reset() { m_gyro = {}; m_accel = {}; }
void MotionGate::Invalidate() { m_gyro.Invalidate(); m_accel.Invalidate(); }
}
