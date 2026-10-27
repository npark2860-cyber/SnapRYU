// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if !defined(_WIN32)
#error windows_x18_exclusive.h is only available on Windows.
#endif

#include <optional>

#include "common/common_types.h"
#include "core/arm/nce/instructions.h"

namespace Core::NCE {

constexpr u32 GuestX18Register = 18;

struct WindowsX18ExclusivePlan {
    u32 rewritten_instruction{};
    u32 value_scratch{};
    u32 context_scratch{};
    bool reads_x18{};
    bool writes_x18{};
};

// CASP shares Eden's broad Exclusive::Verify() signature even though it is a single-instruction
// LSE atomic. Keep it outside the classic load-exclusive/store-exclusive production seam; bounded
// CASP handling remains a later IMP-007 step.
[[nodiscard]] constexpr bool IsClassicExclusiveForWindowsX18(Exclusive exclusive) noexcept {
    constexpr u32 CaspMask = 0xBFA07C00U;
    constexpr u32 CaspPattern = 0x08207C00U;
    return exclusive.Verify() && (exclusive.raw & CaspMask) != CaspPattern;
}

// Build the bounded native-execution plan for one classic exclusive instruction touching guest
// x18. Both scratches are Windows ABI nonvolatile registers and are guaranteed not to overlap any
// architecturally active operand of the original instruction. All architectural x18 operand fields
// are rewritten to value_scratch, keeping the operation in the host CPU's physical exclusive
// monitor domain while physical x18 remains the Windows platform/TEB register.
[[nodiscard]] constexpr std::optional<WindowsX18ExclusivePlan>
BuildWindowsX18ExclusivePlan(Exclusive exclusive) noexcept {
    if (!IsClassicExclusiveForWindowsX18(exclusive) ||
        !exclusive.TouchesRegister(GuestX18Register)) {
        return std::nullopt;
    }

    u32 value_scratch = 0;
    u32 context_scratch = 0;
    for (u32 candidate = 19; candidate <= 28; ++candidate) {
        if (exclusive.TouchesRegister(candidate)) {
            continue;
        }
        if (value_scratch == 0) {
            value_scratch = candidate;
        } else {
            context_scratch = candidate;
            break;
        }
    }

    if (value_scratch == 0 || context_scratch == 0) {
        return std::nullopt;
    }

    return WindowsX18ExclusivePlan{
        .rewritten_instruction = exclusive.RewriteRegister(GuestX18Register, value_scratch),
        .value_scratch = value_scratch,
        .context_scratch = context_scratch,
        .reads_x18 = exclusive.HasRegisterInput(GuestX18Register),
        .writes_x18 = exclusive.HasRegisterOutput(GuestX18Register),
    };
}

#if defined(_M_ARM64) || defined(__aarch64__)
static_assert(BuildWindowsX18ExclusivePlan(Exclusive{0xC85F7E40}).has_value());
static_assert(BuildWindowsX18ExclusivePlan(Exclusive{0xC85F7E40})->value_scratch == 19);
static_assert(BuildWindowsX18ExclusivePlan(Exclusive{0xC85F7E40})->context_scratch == 20);
static_assert(BuildWindowsX18ExclusivePlan(Exclusive{0xC85F7E40})->reads_x18);
static_assert(!BuildWindowsX18ExclusivePlan(Exclusive{0xC85F7E40})->writes_x18);
static_assert(BuildWindowsX18ExclusivePlan(Exclusive{0xC85F7C12})->writes_x18);
static_assert(!BuildWindowsX18ExclusivePlan(Exclusive{0x48317C20}).has_value());
#endif

} // namespace Core::NCE
