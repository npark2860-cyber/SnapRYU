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

struct WindowsX18ExclusivePairPlan {
    u32 rewritten_load{};
    u32 rewritten_store{};
    u32 value_scratch{};
    u32 context_scratch{};
    bool reads_x18{};
    bool writes_x18{};
};

// A load-exclusive directly followed by its store-exclusive, where either one touches guest x18.
// Patching only the x18 instruction puts the trampoline's save-frame stores and context lookup
// between the two. That clears the reservation, so the store-exclusive fails on every retry and
// the guest loop never ends (FATAL FRAME: Maiden of Black Water hangs while loading). Both
// instructions run back to back in one trampoline instead; all bookkeeping happens before the
// load or after the store.
[[nodiscard]] constexpr std::optional<WindowsX18ExclusivePairPlan>
BuildWindowsX18ExclusivePairPlan(Exclusive load, Exclusive store) noexcept {
    if (!IsClassicExclusiveForWindowsX18(load) || !IsClassicExclusiveForWindowsX18(store) ||
        !load.IsLoad() || store.IsLoad()) {
        return std::nullopt;
    }
    const bool load_x18 = load.TouchesRegister(GuestX18Register);
    const bool store_x18 = store.TouchesRegister(GuestX18Register);
    if (!load_x18 && !store_x18) {
        return std::nullopt;
    }

    u32 value_scratch = 0;
    u32 context_scratch = 0;
    for (u32 candidate = 19; candidate <= 28; ++candidate) {
        if (load.TouchesRegister(candidate) || store.TouchesRegister(candidate)) {
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

    return WindowsX18ExclusivePairPlan{
        .rewritten_load = load_x18 ? load.RewriteRegister(GuestX18Register, value_scratch)
                                   : load.AsOrdered(),
        .rewritten_store = store_x18 ? store.RewriteRegister(GuestX18Register, value_scratch)
                                     : store.AsOrdered(),
        .value_scratch = value_scratch,
        .context_scratch = context_scratch,
        .reads_x18 = load.HasRegisterInput(GuestX18Register) ||
                     (store.HasRegisterInput(GuestX18Register) &&
                      !load.HasRegisterOutput(GuestX18Register)),
        .writes_x18 = load.HasRegisterOutput(GuestX18Register) ||
                      store.HasRegisterOutput(GuestX18Register),
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
// LDAXR WZR, [X11] ; STLXR W18, W17, [X11]  (the hanging FATAL FRAME pair)
static_assert(BuildWindowsX18ExclusivePairPlan(Exclusive{0x885FFD7F}, Exclusive{0x8812FD71})
                  ->rewritten_store == 0x8813FD71);
static_assert(BuildWindowsX18ExclusivePairPlan(Exclusive{0x885FFD7F}, Exclusive{0x8812FD71})
                  ->rewritten_load == 0x885FFD7F);
static_assert(BuildWindowsX18ExclusivePairPlan(Exclusive{0x885FFD7F}, Exclusive{0x8812FD71})
                  ->context_scratch == 20);
static_assert(BuildWindowsX18ExclusivePairPlan(Exclusive{0x885FFD7F}, Exclusive{0x8812FD71})
                  ->writes_x18);
static_assert(!BuildWindowsX18ExclusivePairPlan(Exclusive{0x885FFD7F}, Exclusive{0x8812FD71})
                   ->reads_x18);
// LDXR X18, [X0] ; STXR W1, X18, [X0]: the loaded value is stored again through the scratch
static_assert(BuildWindowsX18ExclusivePairPlan(Exclusive{0xC85F7C12}, Exclusive{0xC8017C12})
                  ->writes_x18);
static_assert(!BuildWindowsX18ExclusivePairPlan(Exclusive{0xC85F7C12}, Exclusive{0xC8017C12})
                   ->reads_x18);
// No x18 in either instruction: left to the native path
static_assert(!BuildWindowsX18ExclusivePairPlan(Exclusive{0x885FFD7F}, Exclusive{0x880EFD6D})
                   .has_value());
// Two loads are not a pair
static_assert(!BuildWindowsX18ExclusivePairPlan(Exclusive{0x885FFD7F}, Exclusive{0xC85F7C12})
                   .has_value());
#endif

} // namespace Core::NCE
