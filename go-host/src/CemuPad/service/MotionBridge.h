#pragma once
#include "CemuPad/core/SessionCore.h"
#include "CemuPad/core/MotionControl.h"
#include <memory>

namespace CemuPad::Service {
struct MotionOutput {
    bool active=false,neutralize=false;
    std::array<float,3> gyro{},orientation{},accel{};
    std::array<float,3> physicalGyro{},physicalAccel{};
    std::array<float,4> physicalQuaternion{1,0,0,0};
    std::uint64_t sensorTimeUs=0,deltaUs=0;
    float accMagnitude=0,accAcceleration=0;std::array<float,2> accXY{1,0};
    std::array<float,9> attitude{1,0,0,0,1,0,0,0,1};
};
class MotionBridge {
public:
    MotionBridge();~MotionBridge();
    MotionOutput Apply(const std::optional<ConsumedInput>&);
    MotionOutput Apply(const std::optional<ConsumedInput>&,MotionControlMailbox*,LocalTimeUs now);
    std::uint64_t Integrations() const;
private:struct Impl;std::unique_ptr<Impl> m;
};
}
