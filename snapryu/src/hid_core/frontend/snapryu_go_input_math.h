// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace Core::HID::GoMath {
enum class ControllerStyle { None, Pro, Handheld, Dual };
inline ControllerStyle SelectStyle(bool docked, bool pro, bool handheld, bool dual) {
    if (pro) return ControllerStyle::Pro;
    if (!docked && handheld) return ControllerStyle::Handheld;
    if (dual) return ControllerStyle::Dual;
    return ControllerStyle::None;
}
struct Pad {
    std::uint64_t buttons{};
    std::array<std::int32_t, 4> sticks{};
};
inline Pad MapPad(std::uint32_t wire, const float* sticks) {
    // Wire follows the existing phone app; bit 16 is handled by HomeButton.
    constexpr std::array<unsigned, 16> native{0,1,2,3,13,15,12,14,6,7,8,9,10,11,4,5};
    Pad out;
    for (unsigned i = 0; i < native.size(); ++i)
        if (wire & (1u << i)) out.buttons |= std::uint64_t{1} << native[i];
    for (unsigned i = 0; i < 4; ++i)
        out.sticks[i] = static_cast<std::int32_t>(std::clamp(sticks[i], -1.0f, 1.0f) * 32767.0f);
    for (unsigned i = 0; i < 2; ++i) {
        const auto base = 16 + i * 4;
        if (sticks[i*2] < -0.5f) out.buttons |= std::uint64_t{1} << base;
        if (sticks[i*2+1] > 0.5f) out.buttons |= std::uint64_t{1} << (base+1);
        if (sticks[i*2] > 0.5f) out.buttons |= std::uint64_t{1} << (base+2);
        if (sticks[i*2+1] < -0.5f) out.buttons |= std::uint64_t{1} << (base+3);
    }
    return out;
}

// Local input stays observed while remote input owns the game. A held local
// button/stick must return to neutral before it can resume controlling the game.
class RecoveryGate {
public:
    Pad Apply(Pad local, Pad remote, bool remote_owned, bool release) {
        if (remote_owned != owned || release) {
            suppressed = local.buttons;
            for (unsigned i = 0; i < 2; ++i) axes[i] = !Centred(local, i);
            owned = remote_owned;
            if (!remote_owned || release) return {};
        }
        if (remote_owned) return remote;
        suppressed &= local.buttons;
        local.buttons &= ~suppressed;
        for (unsigned i = 0; i < 2; ++i) {
            if (Centred(local, i)) axes[i] = false;
            if (axes[i]) {
                local.sticks[i*2] = local.sticks[i*2+1] = 0;
                local.buttons &= ~(std::uint64_t{15} << (16+i*4));
            }
        }
        return local;
    }
private:
    static bool Centred(const Pad& pad, unsigned i) {
        return std::hypot(static_cast<float>(pad.sticks[i*2]),
                          static_cast<float>(pad.sticks[i*2+1])) <= 32767.0f * 0.25f;
    }
    bool owned{};
    std::uint64_t suppressed{};
    std::array<bool, 2> axes{};
};

inline std::array<float,4> ConvertQuaternion(const float* q) {
    // CemuPad physical/recentered quaternion C=(w,x,y,z). The Switch internal
    // convention is E=C*(0,0,1,0). Only the representation changes; no new fusion.
    return {-q[2], -q[3], q[0], q[1]};
}
class RotationAccumulator {
public:
    std::array<float,3> Update(std::uint64_t epoch, std::uint64_t sequence,
                               std::uint64_t delta_us, const float* gyro, bool active) {
        if (!active) { primed = false; return rotation; }
        const bool prime = !primed || previous_epoch != epoch;
        // A device-generation/full-reset sample may reuse the last sensor time.
        // The host explicitly marks that sample with zero delta.
        if (prime || delta_us == 0) {
            rotation = {};
            previous_epoch = epoch;
            previous_sequence = sequence;
            primed = true;
            return rotation;
        }
        if (sequence <= previous_sequence) return rotation;
        if (!prime && sequence != previous_sequence && delta_us > 0 && delta_us < 100000) {
            const float dt = static_cast<float>(delta_us) / 1000000.0f;
            for (unsigned i = 0; i < 3; ++i)
                rotation[i] += gyro[i] * dt / (2.0f * std::numbers::pi_v<float>);
        }
        previous_epoch = epoch;
        previous_sequence = sequence;
        primed = true;
        return rotation;
    }
private:
    bool primed{};
    std::uint64_t previous_epoch{}, previous_sequence{};
    std::array<float,3> rotation{};
};
} // namespace Core::HID::GoMath
