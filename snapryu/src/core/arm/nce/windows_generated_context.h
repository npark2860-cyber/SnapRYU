// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if !defined(_WIN32)
#error windows_generated_context.h is only available on Windows.
#endif

#include <cstdint>

#include <oaknut/oaknut.hpp>

#include "common/common_types.h"
#include "core/arm/nce/current_nce_context.h"
#include "core/arm/nce/windows_nce_transition.h"

namespace Core::NCE {

// Emit a Windows-safe lookup of the current NativeExecutionParameters pointer.
//
// The stable C-linkage getter is an ordinary Windows ABI call and can therefore clobber volatile
// integer/SIMD state. Generated NCE helpers may execute with arbitrary guest state live, so save the
// conservative footprint already proven by IMP-005/007 before calling it. Physical x18 is never
// touched and physical TPIDR_EL0 is never used as a metadata locator.
//
// On return every saved guest register/status value is restored, except output_reg which contains
// the NativeExecutionParameters pointer returned by GetCurrentNceContextForGeneratedCode(). Callers
// must treat output_reg as an explicit scratch/destination register.
inline void WriteWindowsCurrentNceParametersLookup(oaknut::VectorCodeGenerator& cg,
                                                    oaknut::XReg output_reg) {
    using namespace oaknut::util;

    // Fast path: physical x18 is the Windows TEB inside every NCE epoch, and RunThread mirrors the
    // NativeExecutionParameters pointer into a TEB TLS slot. One load, no registers or flags
    // touched, no C call on the guest stack.
    if (CurrentNceContext::HasTebSlot()) {
        cg.LDR(output_reg, X18, CurrentNceContext::TebSlotOffset());
        return;
    }

    constexpr u32 FrameSize = 0x140;
    constexpr u32 VolatileVectorOffset = 0x090;
    constexpr u32 LinkRegisterOffset = 0x110;
    constexpr u32 NzcvOffset = 0x118;
    constexpr u32 FpcrOffset = 0x120;
    constexpr u32 FpsrOffset = 0x128;
    constexpr u32 ResultOffset = 0x130;
    constexpr u32 BlrBase = 0xD63F0000U;

    static_assert((FrameSize & 0xF) == 0);

    oaknut::Label getter_address;
    oaknut::Label done;

    cg.SUB(SP, SP, FrameSize);

    // Windows ABI volatile integer registers. x18 is intentionally absent because it is the TEB
    // platform register and is never guest-owned on this path.
    for (int i = 0; i <= 16; i += 2) {
        cg.STP(oaknut::XReg{i}, oaknut::XReg{i + 1}, SP, 8 * i);
    }
    cg.STR(X30, SP, LinkRegisterOffset);

    // The verified getter does not currently use SIMD state, but preserving q0-q7 keeps this helper
    // aligned with the conservative generated-call footprint already proven in IMP-007.
    for (int i = 0; i <= 6; i += 2) {
        cg.STP(oaknut::QReg{i}, oaknut::QReg{i + 1}, SP,
               VolatileVectorOffset + 16 * i);
    }

    // Preserve guest-visible status/control state across the C ABI call as well.
    cg.MRS(X9, oaknut::SystemReg::NZCV);
    cg.STR(X9, SP, NzcvOffset);
    cg.MRS(X9, oaknut::SystemReg::FPCR);
    cg.STR(X9, SP, FpcrOffset);
    cg.MRS(X9, oaknut::SystemReg::FPSR);
    cg.STR(X9, SP, FpsrOffset);

    cg.LDR(X16, getter_address);
    cg.dw(BlrBase | (16U << 5));
    cg.STR(X0, SP, ResultOffset);

    cg.LDR(X9, SP, FpsrOffset);
    cg.MSR(oaknut::SystemReg::FPSR, X9);
    cg.LDR(X9, SP, FpcrOffset);
    cg.MSR(oaknut::SystemReg::FPCR, X9);
    cg.LDR(X9, SP, NzcvOffset);
    cg.MSR(oaknut::SystemReg::NZCV, X9);

    for (int i = 6; i >= 0; i -= 2) {
        cg.LDP(oaknut::QReg{i}, oaknut::QReg{i + 1}, SP,
               VolatileVectorOffset + 16 * i);
    }
    for (int i = 16; i >= 0; i -= 2) {
        cg.LDP(oaknut::XReg{i}, oaknut::XReg{i + 1}, SP, 8 * i);
    }
    cg.LDR(X30, SP, LinkRegisterOffset);

    // Load the result only after restoring the original value of output_reg.
    cg.LDR(output_reg, SP, ResultOffset);
    cg.ADD(SP, SP, FrameSize);

    // Keep the embedded function pointer out of the executed instruction stream.
    cg.B(done);
    cg.l(getter_address);
    cg.dx(static_cast<u64>(reinterpret_cast<std::uintptr_t>(
        &GetCurrentNceContextForGeneratedCode)));
    cg.l(done);
}

} // namespace Core::NCE
