// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"
#include "core/arm/arm_interface.h"

namespace Kernel {
class KThread;
}

namespace Core {

class ArmDynarmic64;
struct GuestContext;

namespace NCE {

enum class X18InstructionClass : u8 {
    NoX18,
    SupportedOrdinary,
    ExcludedExclusive,
    ExcludedUnsupportedAtomic,
    Unsupported,
};

struct X18FallbackStepResult {
    bool completed{};
    HaltReason halt_reason{};
};

class X18Fallback {
public:
    static constexpr u32 NzcvMask = 0xF0000000u;

    [[nodiscard]] static X18InstructionClass ClassifyInstruction(u32 instruction);
    [[nodiscard]] static bool MayAccessGuestMemory(u32 instruction);
    [[nodiscard]] static bool CanUseDirectTrampoline(u32 instruction);
    // True when `rewritten` decodes to exactly the IR of `original` with every A64 register
    // reference to `from` replaced by `to` (and `original` references `from` at least once, never
    // `to`). Used to validate x18 -> scratch register-field substitution.
    [[nodiscard]] static bool IsRegisterSubstitution(u32 original, u32 rewritten, u32 from,
                                                     u32 to);

    [[nodiscard]] static constexpr X18FallbackStepResult DecodeStepReason(
        HaltReason step_reason) noexcept {
        const u64 raw_reason = static_cast<u64>(step_reason);
        const u64 step_mask = static_cast<u64>(HaltReason::StepThread);
        return {
            .completed = (raw_reason & step_mask) != 0,
            .halt_reason = static_cast<HaltReason>(raw_reason & ~step_mask),
        };
    }

    [[nodiscard]] static X18FallbackStepResult Step(ArmDynarmic64& backend,
                                                    Kernel::KThread* thread,
                                                    GuestContext& guest,
                                                    u32 instruction);
};

} // namespace NCE
} // namespace Core
