// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

#include "common/common_funcs.h"
#include "common/common_types.h"
#include "core/arm/arm_interface.h"
#include "core/arm/nce/arm_nce_asm_definitions.h"

namespace Core {

class ArmNce;
class System;

struct HostContext {
    alignas(16) std::array<u64, 12> host_saved_regs{};
    alignas(16) std::array<u128, 8> host_saved_vregs{};
    u64 host_sp{};
    void* host_tpidr_el0{};
#if defined(_WIN32)
    u32 host_fpcr{};
#endif
};

struct GuestContext {
    std::array<u64, 31> cpu_registers{};
    u64 sp{};
    u64 pc{};
    u32 fpcr{};
    u32 fpsr{};
    std::array<u128, 32> vector_registers{};
    u32 pstate{};
    alignas(16) HostContext host_ctx{};
    u64 tpidrro_el0{};
    u64 tpidr_el0{};
    std::atomic<u64> esr_el1{};
    u32 nzcv{};
    u32 svc{};
    System* system{};
    ArmNce* parent{};
#if defined(_WIN32)
    // Windows NCE transition metadata. These are prepared while host SP is active and consumed
    // only by leaf host<->guest stack handoff code; guest architectural state does not own them.
    u64 windows_guest_stack_base{};
    u64 windows_guest_stack_limit{};
    u64 windows_host_stack_base{};
    u64 windows_host_stack_limit{};
    void* windows_native_execution_parameters{};
#endif
};

// Verify assembly offsets.
static_assert(offsetof(GuestContext, sp) == GuestContextSp);
static_assert(offsetof(GuestContext, host_ctx) == GuestContextHostContext);
static_assert(offsetof(HostContext, host_sp) == HostContextSpTpidrEl0);
static_assert(offsetof(HostContext, host_tpidr_el0) - 8 == HostContextSpTpidrEl0);
static_assert(offsetof(HostContext, host_tpidr_el0) == HostContextTpidrEl0);
static_assert(offsetof(HostContext, host_saved_regs) == HostContextRegs);
static_assert(offsetof(HostContext, host_saved_vregs) == HostContextVregs);
#if defined(_WIN32)
static_assert(offsetof(HostContext, host_fpcr) == HostContextFpcr);
static_assert(offsetof(GuestContext, windows_guest_stack_base) == 0x450);
static_assert(offsetof(GuestContext, windows_guest_stack_limit) == 0x458);
static_assert(offsetof(GuestContext, windows_host_stack_base) == 0x460);
static_assert(offsetof(GuestContext, windows_host_stack_limit) == 0x468);
static_assert(offsetof(GuestContext, windows_native_execution_parameters) == 0x470);
#endif

} // namespace Core
