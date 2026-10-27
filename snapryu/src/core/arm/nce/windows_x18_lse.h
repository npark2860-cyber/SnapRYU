// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if !defined(_WIN32)
#error windows_x18_lse.h is only available on Windows.
#endif

#include <optional>

#include "common/common_types.h"
#include "core/arm/nce/windows_x18_exclusive.h"

namespace Core::NCE {

enum class WindowsX18LseKind : u8 {
    ScalarCas,
    Casp,
    AtomicMemory,
};

struct WindowsX18LsePlan {
    WindowsX18LseKind kind{};
    u32 rewritten_instruction{};
    u32 value_scratch{};
    u32 pair_scratch_second{};
    u32 context_scratch{};
    bool reads_x18{};
    bool writes_x18{};
    bool casp_pair_uses_x18{};
    bool writes_x19{};
};

[[nodiscard]] constexpr u32 WindowsLseRegisterField(u32 raw, u32 shift) noexcept {
    return (raw >> shift) & 0x1FU;
}

[[nodiscard]] constexpr u32 ReplaceWindowsLseRegisterField(u32 raw, u32 shift,
                                                            u32 reg) noexcept {
    constexpr u32 RegisterMask = 0x1FU;
    const u32 mask = RegisterMask << shift;
    return (raw & ~mask) | ((reg & RegisterMask) << shift);
}

// CAS/CASA/CASL/CASAL, including byte/halfword/word/dword forms. Ordering bits are deliberately
// excluded from the mask so the original acquire/release semantics remain untouched.
[[nodiscard]] constexpr bool IsWindowsScalarCasInstruction(u32 raw) noexcept {
    return (raw & 0x3FA07C00U) == 0x08A07C00U;
}

// CASP/CASPA/CASPL/CASPAL. This encoding overlaps Eden's broad Exclusive::Verify() signature and
// therefore must be intercepted before the legacy AsOrdered() relocation pass.
[[nodiscard]] constexpr bool IsWindowsCaspInstruction(u32 raw) noexcept {
    return (raw & 0xBFA07C00U) == 0x08207C00U;
}

// LDADD/LDCLR/LDEOR/LDSET/LDSMAX/LDSMIN/LDUMAX/LDUMIN/SWP, all exposed element sizes and A/R
// ordering variants. Opcodes 0..8 are the bounded IMP-007 set; later atomic encodings stay out.
[[nodiscard]] constexpr bool IsWindowsAtomicMemoryInstruction(u32 raw) noexcept {
    return (raw & 0x3F200C00U) == 0x38200000U && ((raw >> 12) & 0xFU) <= 8U;
}

[[nodiscard]] constexpr bool IsWindowsLseInstruction(u32 raw) noexcept {
    return IsWindowsScalarCasInstruction(raw) || IsWindowsCaspInstruction(raw) ||
           IsWindowsAtomicMemoryInstruction(raw);
}

[[nodiscard]] constexpr bool WindowsCaspPairIsArchitecturallyBounded(u32 first) noexcept {
    return first <= 30U && (first & 1U) == 0;
}

[[nodiscard]] constexpr bool WindowsCaspPairContains(u32 first, u32 reg) noexcept {
    return first <= 30U && (first == reg || first + 1U == reg);
}

[[nodiscard]] constexpr bool WindowsLseTouchesRegister(u32 raw, u32 reg) noexcept {
    const u32 rt = WindowsLseRegisterField(raw, 0);
    const u32 rn = WindowsLseRegisterField(raw, 5);
    const u32 rs = WindowsLseRegisterField(raw, 16);

    if (rn == reg) {
        return true;
    }
    if (IsWindowsCaspInstruction(raw)) {
        return WindowsCaspPairContains(rs, reg) || WindowsCaspPairContains(rt, reg);
    }
    return rs == reg || rt == reg;
}

[[nodiscard]] constexpr bool WindowsLseReadsRegister(u32 raw, u32 reg) noexcept {
    const u32 rt = WindowsLseRegisterField(raw, 0);
    const u32 rn = WindowsLseRegisterField(raw, 5);
    const u32 rs = WindowsLseRegisterField(raw, 16);

    if (rn == reg) {
        return true;
    }
    if (IsWindowsCaspInstruction(raw)) {
        return WindowsCaspPairContains(rs, reg) || WindowsCaspPairContains(rt, reg);
    }
    if (IsWindowsScalarCasInstruction(raw)) {
        return rs == reg || rt == reg;
    }
    // Atomic-memory operations read Rs and write the old value to Rt.
    return rs == reg;
}

[[nodiscard]] constexpr bool WindowsLseWritesRegister(u32 raw, u32 reg) noexcept {
    const u32 rt = WindowsLseRegisterField(raw, 0);
    const u32 rs = WindowsLseRegisterField(raw, 16);

    if (IsWindowsCaspInstruction(raw)) {
        return WindowsCaspPairContains(rs, reg);
    }
    if (IsWindowsScalarCasInstruction(raw)) {
        return rs == reg;
    }
    return rt != 31U && rt == reg;
}

[[nodiscard]] constexpr std::optional<WindowsX18LsePlan>
BuildWindowsX18LsePlan(u32 raw) noexcept {
    if (!IsWindowsLseInstruction(raw) || !WindowsLseTouchesRegister(raw, GuestX18Register)) {
        return std::nullopt;
    }

    WindowsX18LsePlan plan{};
    plan.kind = IsWindowsCaspInstruction(raw)
                    ? WindowsX18LseKind::Casp
                    : (IsWindowsScalarCasInstruction(raw) ? WindowsX18LseKind::ScalarCas
                                                           : WindowsX18LseKind::AtomicMemory);
    plan.rewritten_instruction = raw;
    plan.reads_x18 = WindowsLseReadsRegister(raw, GuestX18Register);
    plan.writes_x18 = WindowsLseWritesRegister(raw, GuestX18Register);

    const u32 rs = WindowsLseRegisterField(raw, 16);
    const u32 rt = WindowsLseRegisterField(raw, 0);

    if (plan.kind == WindowsX18LseKind::Casp) {
        if (!WindowsCaspPairIsArchitecturallyBounded(rs) ||
            !WindowsCaspPairIsArchitecturallyBounded(rt)) {
            return std::nullopt;
        }

        plan.casp_pair_uses_x18 = rs == GuestX18Register || rt == GuestX18Register;
        plan.writes_x19 = rs == GuestX18Register;
    }

    if (plan.casp_pair_uses_x18) {
        // CASP register pairs are even/odd. Guest x18 therefore participates as the first member
        // of x18/x19. Select one unused even/odd scratch pair entirely within x19-x28.
        for (u32 candidate = 20; candidate <= 26; candidate += 2) {
            if (WindowsLseTouchesRegister(raw, candidate) ||
                WindowsLseTouchesRegister(raw, candidate + 1U)) {
                continue;
            }
            plan.value_scratch = candidate;
            plan.pair_scratch_second = candidate + 1U;
            break;
        }
    } else {
        for (u32 candidate = 19; candidate <= 28; ++candidate) {
            if (!WindowsLseTouchesRegister(raw, candidate)) {
                plan.value_scratch = candidate;
                break;
            }
        }
    }

    if (plan.value_scratch == 0) {
        return std::nullopt;
    }

    for (u32 candidate = 19; candidate <= 28; ++candidate) {
        if (WindowsLseTouchesRegister(raw, candidate) || candidate == plan.value_scratch ||
            candidate == plan.pair_scratch_second) {
            continue;
        }
        plan.context_scratch = candidate;
        break;
    }
    if (plan.context_scratch == 0) {
        return std::nullopt;
    }

    // Rewrite only architectural register fields. Never alter L/A/R ordering bits.
    const u32 rn = WindowsLseRegisterField(raw, 5);
    if (rn == GuestX18Register) {
        plan.rewritten_instruction =
            ReplaceWindowsLseRegisterField(plan.rewritten_instruction, 5, plan.value_scratch);
    }

    if (plan.kind == WindowsX18LseKind::Casp) {
        if (rs == GuestX18Register) {
            plan.rewritten_instruction = ReplaceWindowsLseRegisterField(
                plan.rewritten_instruction, 16, plan.value_scratch);
        }
        if (rt == GuestX18Register) {
            plan.rewritten_instruction =
                ReplaceWindowsLseRegisterField(plan.rewritten_instruction, 0, plan.value_scratch);
        }
    } else {
        if (rs == GuestX18Register) {
            plan.rewritten_instruction = ReplaceWindowsLseRegisterField(
                plan.rewritten_instruction, 16, plan.value_scratch);
        }
        if (rt == GuestX18Register) {
            plan.rewritten_instruction =
                ReplaceWindowsLseRegisterField(plan.rewritten_instruction, 0, plan.value_scratch);
        }
    }

    return plan;
}

static_assert(IsWindowsScalarCasInstruction(0xC8B27C01U));
static_assert(IsWindowsScalarCasInstruction(0x08A07C41U));
static_assert(IsWindowsCaspInstruction(0x48327C02U));
static_assert(IsWindowsCaspInstruction(0x08207C82U));
static_assert(IsWindowsAtomicMemoryInstruction(0xF8320001U));
static_assert(IsWindowsAtomicMemoryInstruction(0x38200041U));
static_assert(IsWindowsAtomicMemoryInstruction(0xF8328001U));
static_assert(!IsWindowsAtomicMemoryInstruction(0xF8209001U));
static_assert(BuildWindowsX18LsePlan(0xC8B27C01U).has_value());
static_assert(BuildWindowsX18LsePlan(0xC8B27C01U)->reads_x18);
static_assert(BuildWindowsX18LsePlan(0xC8B27C01U)->writes_x18);
static_assert(BuildWindowsX18LsePlan(0x48327C02U).has_value());
static_assert(BuildWindowsX18LsePlan(0x48327C02U)->casp_pair_uses_x18);
static_assert(BuildWindowsX18LsePlan(0x48327C02U)->writes_x19);
static_assert(BuildWindowsX18LsePlan(0xF8320001U).has_value());
static_assert(BuildWindowsX18LsePlan(0xF8320001U)->reads_x18);
static_assert(!BuildWindowsX18LsePlan(0xF8320001U)->writes_x18);

} // namespace Core::NCE
