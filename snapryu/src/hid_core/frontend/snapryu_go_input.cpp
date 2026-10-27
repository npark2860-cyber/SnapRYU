// SPDX-License-Identifier: GPL-3.0-or-later
#include "hid_core/frontend/snapryu_go_input.h"
#include <numbers>
#include "common/settings.h"
#include "hid_core/hid_core.h"
#include "hid_core/hid_util.h"

namespace Core::HID {
namespace {
ControllerMotion NeutralMotion() {
    ControllerMotion value{};
    value.accel = {0, 0, -1};
    value.orientation = {Common::Vec3f{1,0,0}, Common::Vec3f{0,1,0}, Common::Vec3f{0,0,1}};
    value.is_at_rest = true;
    return value;
}
}
SnapGoInputBridge::SnapGoInputBridge(HIDCore& core_) : core{core_}, motion{NeutralMotion()} {}

void SnapGoInputBridge::Update() {
    const auto next = SnapGo::ReadInput();
    const auto styles = core.GetSupportedStyleTag();
    auto* p1 = core.GetEmulatedController(NpadIdType::Player1);
    auto* handheld = core.GetEmulatedController(NpadIdType::Handheld);
    const bool configuring = p1->IsConfiguring() || handheld->IsConfiguring();
    const bool next_blocked = configuring || SnapGo::IsInputBlocked();
    bool next_session = next.remote_session_active && !configuring;
    NpadIdType next_target = NpadIdType::Invalid;
    NpadStyleIndex style = NpadStyleIndex::Fullkey;
    if (next_session) {
        switch (GoMath::SelectStyle(Settings::IsDockedMode(), styles.fullkey,
                                    styles.handheld, styles.joycon_dual)) {
        case GoMath::ControllerStyle::Pro:
            // A single phone is one Pro Controller in either console mode.
            next_target = NpadIdType::Player1;
            break;
        case GoMath::ControllerStyle::Handheld:
            next_target = NpadIdType::Handheld;
            style = NpadStyleIndex::Handheld;
            break;
        case GoMath::ControllerStyle::Dual:
            // Match the emulator's existing Pro -> paired Joy-Con fallback.
            // It is still one logical player and uses the same phone sensors.
            next_target = NpadIdType::Player1;
            style = NpadStyleIndex::JoyconDual;
            break;
        case GoMath::ControllerStyle::None:
            next_session = false;
            break;
        }
    }
    {
        std::lock_guard lock{mutex};
        input = next;
        blocked = next_blocked;
        session = next_session;
        target = next_target;
        const bool active = session && !blocked && input.motion_active && !input.release_required;
        const auto rotations = rotation.Update(input.ownership_epoch, input.motion_sample_sequence,
                                                input.motion_delta_us, input.gyro_rad_s, active);
        motion = NeutralMotion();
        if (active) {
            motion.accel = {input.accel_g[0], input.accel_g[1], input.accel_g[2]};
            constexpr float tau = 2.0f * std::numbers::pi_v<float>;
            motion.gyro = {input.gyro_rad_s[0]/tau, input.gyro_rad_s[1]/tau, input.gyro_rad_s[2]/tau};
            motion.rotation = {rotations[0], rotations[1], rotations[2]};
            const auto converted = GoMath::ConvertQuaternion(input.quaternion_wxyz);
            MotionInput representation;
            representation.SetQuaternion(Common::Quaternion<f32>{{converted[1],converted[2],converted[3]},converted[0]});
            motion.orientation = representation.GetOrientation();
            motion.euler = representation.GetEulerAngles();
            const auto magnitude = motion.accel.Length();
            motion.is_at_rest = motion.gyro.Length() < MotionInput::IsAtRestStandard &&
                                magnitude > 0.9f && magnitude < 1.1f;
        }
    }
    // Never hold the snapshot mutex while callbacks update HID services.
    p1->SetRemoteConnectionOverride(next_session, next_target == NpadIdType::Player1, style);
    handheld->SetRemoteConnectionOverride(next_session, next_target == NpadIdType::Handheld,
                                           NpadStyleIndex::Handheld);
}

void SnapGoInputBridge::Reset() {
    {
        std::lock_guard lock{mutex};
        input = {};
        session = blocked = false;
        target = NpadIdType::Invalid;
        motion = NeutralMotion();
        rotation = {};
    }
    core.GetEmulatedController(NpadIdType::Player1)->SetRemoteConnectionOverride(false, false, NpadStyleIndex::Fullkey);
    core.GetEmulatedController(NpadIdType::Handheld)->SetRemoteConnectionOverride(false, false, NpadStyleIndex::Handheld);
}

void SnapGoInputBridge::Pad(NpadIdType id, NpadButtonState& buttons, AnalogSticks& sticks) {
    const auto index = Service::HID::NpadIdTypeToIndex(id);
    if (index >= gates.size()) return;
    std::lock_guard lock{mutex};
    const GoMath::Pad local{static_cast<u64>(buttons.raw),
                            {sticks.left.x,sticks.left.y,sticks.right.x,sticks.right.y}};
    const bool owned = session && id == target;
    GoMath::Pad remote{};
    if (owned && !blocked && input.pad_owned && input.pad_valid && !input.release_required)
        remote = GoMath::MapPad(input.buttons_wire17, input.sticks);
    const auto out = gates[index].Apply(local, remote, owned, owned && input.release_required);
    buttons.raw = static_cast<NpadButton>(out.buttons);
    sticks.left = {out.sticks[0],out.sticks[1]};
    sticks.right = {out.sticks[2],out.sticks[3]};
}

MotionState SnapGoInputBridge::Motions(NpadIdType id, MotionState local) const {
    std::lock_guard lock{mutex};
    if (session && id == target) return {motion, motion};
    if (input.motion_neutralize && (id == NpadIdType::Player1 || id == NpadIdType::Handheld))
        return {NeutralMotion(), NeutralMotion()};
    return local;
}

TouchFingerState SnapGoInputBridge::Touch(TouchFingerState local) {
    std::lock_guard lock{mutex};
    const bool owned = session;
    if (owned != touch_owned || input.release_required) {
        for (unsigned i = 0; i < local.size(); ++i) touch_suppressed[i] = local[i].pressed;
        touch_owned = owned;
        if (!owned || input.release_required) return {};
    }
    if (owned) {
        TouchFingerState out{};
        if (!blocked && input.touch_valid && input.touch_down) {
            out[0].pressed = true;
            out[0].id = input.touch_id;
            out[0].position = {std::clamp(input.touch_x, 0.0f, 1279.0f/1280.0f),
                               std::clamp(input.touch_y, 0.0f, 719.0f/720.0f)};
        }
        return out;
    }
    for (unsigned i = 0; i < local.size(); ++i) {
        touch_suppressed[i] = touch_suppressed[i] && local[i].pressed;
        if (touch_suppressed[i]) local[i].pressed = false;
    }
    return local;
}

HomeButtonState SnapGoInputBridge::Home(HomeButtonState local) {
    std::lock_guard lock{mutex};
    if (session != home_owned || input.release_required) {
        home_suppressed = local.home;
        home_owned = session;
        if (!session || input.release_required) return {};
    }
    if (session) {
        HomeButtonState out{};
        out.home.Assign(!blocked && input.pad_valid && input.pad_owned && (input.buttons_wire17 & (1u<<16)));
        return out;
    }
    home_suppressed = home_suppressed && local.home;
    if (home_suppressed) local.home.Assign(false);
    return local;
}
} // namespace Core::HID
