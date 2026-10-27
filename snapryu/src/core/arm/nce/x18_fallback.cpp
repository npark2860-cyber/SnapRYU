// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/arm/nce/x18_fallback.h"

#include <cstddef>

#include "core/arm/dynarmic/arm_dynarmic_64.h"
#include "core/arm/nce/guest_context.h"
#include "core/hle/kernel/k_thread.h"
#include "core/memory.h"

namespace Core {

void ArmDynarmic64::SetSingleInstructionCodeOverride(u64 pc, u32 instruction) {
    // P3E: Dynarmic's single-stepping translator performs exactly one MemoryReadCode(pc).
    // Publish the original guest opcode directly instead of pre-reading/mutating a full 4 KiB
    // page. A compiled single-step block remains keyed by (PC, semantic instruction) below.
    m_cb->m_single_instruction_override_pc = pc;
    m_cb->m_single_instruction_override_instruction = instruction;
    m_cb->m_single_instruction_override_active = true;

    const auto [it, inserted] =
        m_single_instruction_override_cache.try_emplace(pc, instruction);
    if (!inserted && it->second != instruction) {
        // The private x18 fallback backend can safely reuse a single-step block only while both
        // the guest PC and the original patched instruction are unchanged. If a later code/module
        // epoch reuses the same PC with different semantics, invalidate before the next step.
        it->second = instruction;
        m_jit->InvalidateCacheRange(pc, sizeof(u32));
    }
}

void ArmDynarmic64::ClearSingleInstructionCodeOverride() {
    // Do not invalidate the ordinary code-page cache here. The override never mutates that page.
    // ISB/IC callbacks retain ownership of real code-cache/page-cache invalidation.
    m_cb->m_single_instruction_override_active = false;
}

} // namespace Core

namespace Core::NCE {

namespace {

void CopyGuestToThreadContext(const GuestContext& guest, Kernel::Svc::ThreadContext& context) {
    for (std::size_t i = 0; i < 29; ++i) {
        context.r[i] = guest.cpu_registers[i];
    }
    context.fp = guest.cpu_registers[29];
    context.lr = guest.cpu_registers[30];
    context.sp = guest.sp;
    context.pc = guest.pc;
    context.pstate = (guest.pstate & ~X18Fallback::NzcvMask) |
                     (guest.nzcv & X18Fallback::NzcvMask);
    context.v = guest.vector_registers;
    context.fpcr = guest.fpcr;
    context.fpsr = guest.fpsr;
    context.tpidr = guest.tpidr_el0;
}

void CopyThreadContextToGuest(const Kernel::Svc::ThreadContext& context, GuestContext& guest) {
    for (std::size_t i = 0; i < 29; ++i) {
        guest.cpu_registers[i] = context.r[i];
    }
    guest.cpu_registers[29] = context.fp;
    guest.cpu_registers[30] = context.lr;
    guest.sp = context.sp;
    guest.pc = context.pc;
    guest.pstate = (guest.pstate & ~X18Fallback::NzcvMask) |
                   (context.pstate & X18Fallback::NzcvMask);
    guest.nzcv = context.pstate & X18Fallback::NzcvMask;
    guest.vector_registers = context.v;
    guest.fpcr = context.fpcr;
    guest.fpsr = context.fpsr;
    guest.tpidr_el0 = context.tpidr;
}

} // namespace

X18FallbackStepResult X18Fallback::Step(ArmDynarmic64& backend, Kernel::KThread* thread,
                                        GuestContext& guest, u32 instruction) {
    Kernel::Svc::ThreadContext context{};
    CopyGuestToThreadContext(guest, context);

    backend.SetContext(context);
    backend.SetTpidrroEl0(guest.tpidrro_el0);
    backend.SetSingleInstructionCodeOverride(guest.pc, instruction);

    const HaltReason step_reason = backend.StepThread(thread);
    backend.ClearSingleInstructionCodeOverride();

    backend.GetContext(context);
    CopyThreadContextToGuest(context, guest);

    return DecodeStepReason(step_reason);
}

} // namespace Core::NCE
