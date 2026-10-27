// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <mutex>
#include "common/snapryu_go.h"
#include "hid_core/frontend/emulated_console.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/frontend/snapryu_go_input_math.h"

namespace Core::HID {
class HIDCore;
class SnapGoInputBridge {
public:
    explicit SnapGoInputBridge(HIDCore& core);
    void Update();
    void Reset();
    void Pad(NpadIdType id, NpadButtonState& buttons, AnalogSticks& sticks);
    MotionState Motions(NpadIdType id, MotionState local) const;
    TouchFingerState Touch(TouchFingerState local);
    HomeButtonState Home(HomeButtonState local);
private:
    HIDCore& core;
    mutable std::mutex mutex;
    SnapGoInput input{};
    NpadIdType target{NpadIdType::Invalid};
    bool session{}, blocked{}, touch_owned{}, home_owned{}, home_suppressed{};
    std::array<GoMath::RecoveryGate, 10> gates;
    std::array<bool, MaxActiveTouchInputs> touch_suppressed{};
    GoMath::RotationAccumulator rotation;
    ControllerMotion motion{};
};
} // namespace Core::HID
