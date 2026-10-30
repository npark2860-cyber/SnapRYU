// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <numeric>
#include <bit>
#include <optional>
#include "common/cpu_features.h"
#include "common/alignment.h"
#include "common/literals.h"
#include "core/arm/nce/arm_nce.h"
#include "core/arm/nce/guest_context.h"
#include "core/arm/nce/instructions.h"
#include "core/arm/nce/x18_fallback.h"
#include "core/arm/nce/patcher.h"
#if defined(_WIN32)
#include "core/arm/nce/windows_generated_context.h"
#include "core/arm/nce/windows_nce_transition.h"
#include "core/arm/nce/windows_patch_code_metadata.h"
#include "core/arm/nce/windows_x18_exclusive.h"
#include "core/arm/nce/windows_x18_lse.h"
#include "core/arm/nce/windows_x18_fallback_runner.h"
#endif
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/svc.h"
#include "core/memory.h"
#include "core/hle/kernel/k_thread.h"

namespace Core::NCE {

using namespace Common::Literals;
using namespace oaknut::util;

using NativeExecutionParameters = Kernel::KThread::NativeExecutionParameters;

constexpr size_t MaxRelativeBranch = 128_MiB;
constexpr u32 ModuleCodeIndex = 0x24 / sizeof(u32);
constexpr u32 NzcvMask = 0xF0000000U;

#if defined(_WIN32)
struct WindowsX18BitfieldPlan {
    u32 rewritten_instruction{};
    u32 value_scratch{};
    u32 context_scratch{};
    bool writes_x18{};
};

[[nodiscard]] constexpr u32 ReplaceWindowsX18BitfieldRegisterField(
    u32 raw, u32 shift, u32 reg) noexcept {
    constexpr u32 RegisterMask = 0x1FU;
    const u32 mask = RegisterMask << shift;
    return (raw & ~mask) | ((reg & RegisterMask) << shift);
}

struct WindowsX18SubstitutionPlan {
    u32 rewritten_instruction;
    u32 value_reg;
    u32 context_reg;
};

// The x18 exclusive/LSE trampolines below keep their save frame allocated while the native atomic
// runs. Windows writes its exception record below SP, so with the frame already released a fault on
// that atomic would overwrite the parked scratch pair at [SP-0x20] and resume with two corrupted guest
// registers. With the frame kept, an SP-based address is taken from a scratch register that holds
// the guest SP instead.
[[nodiscard]] constexpr bool X18AtomicUsesSpBase(u32 instruction) noexcept {
    return ((instruction >> 5) & 0x1FU) == 31U;
}

[[nodiscard]] constexpr u32 X18AtomicWithBaseRegister(u32 instruction, u32 reg) noexcept {
    return (instruction & ~(0x1FU << 5)) | ((reg & 0x1FU) << 5);
}

static_assert(X18AtomicUsesSpBase(0xC85F7FE0U));                            // LDXR X0, [SP]
static_assert(!X18AtomicUsesSpBase(0xC85F7E40U));                           // LDXR X0, [X18]
static_assert(X18AtomicWithBaseRegister(0xC85F7FE0U, 20) == 0xC85F7E80U);   // -> LDXR X0, [X20]
static_assert(X18AtomicWithBaseRegister(0xF83203FFU, 21) == 0xF83202BFU);   // STADD X18, [SP] -> [X21]

#if SNAPRYU_DIAGNOSTICS
// X18_ATOMIC_PATCH (diagnostic builds only): x18 exclusive/LSE trampolines per patched module.
struct X18AtomicPatchCounts {
    u64 exclusive{};
    u64 lse{};
    u64 sp_base{};
    u64 pair{};

    void Note(bool is_lse, bool sp) noexcept {
        ++(is_lse ? lse : exclusive);
        sp_base += sp ? 1 : 0;
    }
};
#endif

[[nodiscard]] std::optional<WindowsX18SubstitutionPlan>
BuildWindowsX18SubstitutionPlan(u32 instruction) {
    if (!CurrentNceContext::HasTebSlot() || !CurrentNceContext::HasScratchSlots()) {
        return std::nullopt;
    }
    // PC-relative forms (ADR/ADRP, literal loads) depend on the executing address.
    if ((instruction & 0x1F000000U) == 0x10000000U || (instruction & 0x3B000000U) == 0x18000000U) {
        return std::nullopt;
    }
    if (!X18Fallback::CanUseDirectTrampoline(instruction)) {
        return std::nullopt;
    }

    constexpr std::array<u32, 4> FieldShifts{0, 5, 10, 16};
    u32 used_mask = 0;
    u32 x18_fields = 0;
    for (std::size_t k = 0; k < FieldShifts.size(); ++k) {
        const u32 field = (instruction >> FieldShifts[k]) & 0x1FU;
        used_mask |= 1U << field;
        if (field == GuestX18Register) {
            x18_fields |= 1U << k;
        }
    }
    if (x18_fields == 0) {
        return std::nullopt;
    }

    constexpr std::array<u32, 27> Preference{16, 17, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4,
                                             3,  2,  1,  0,  19, 20, 21, 22, 23, 24, 25, 26, 27};
    std::optional<u32> value_reg;
    std::optional<u32> context_reg;
    for (const u32 reg : Preference) {
        if ((used_mask & (1U << reg)) != 0) {
            continue;
        }
        if (!value_reg) {
            value_reg = reg;
        } else if (!context_reg) {
            context_reg = reg;
            break;
        }
    }
    if (!value_reg || !context_reg) {
        return std::nullopt;
    }

    // Prefer substituting every field that holds 18; fall back to subsets so a coincidental
    // immediate equal to 18 is left untouched. The decoder comparison is the authority.
    for (u32 subset = x18_fields; subset != 0; subset = (subset - 1) & x18_fields) {
        u32 rewritten = instruction;
        for (std::size_t k = 0; k < FieldShifts.size(); ++k) {
            if ((subset & (1U << k)) != 0) {
                rewritten = (rewritten & ~(0x1FU << FieldShifts[k])) |
                            (*value_reg << FieldShifts[k]);
            }
        }
        if (X18Fallback::IsRegisterSubstitution(instruction, rewritten, GuestX18Register,
                                                *value_reg)) {
            return WindowsX18SubstitutionPlan{
                .rewritten_instruction = rewritten,
                .value_reg = *value_reg,
                .context_reg = *context_reg,
            };
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<WindowsX18BitfieldPlan>
BuildWindowsX18BitfieldPlan(u32 instruction) {
    // SBFM/BFM/UBFM (including aliases such as ASR/SXT/BFI/BFXIL) are pure register
    // data-processing instructions: they cannot fault, branch, touch memory or use SP/PC.
    // Keep P2A deliberately bounded to this family because a real hot fallback site was
    // recovered as BFI W15,W18,#1,#31 (0x331F7A4F).
    constexpr u32 BitfieldMask = 0x1F800000U;
    constexpr u32 BitfieldPattern = 0x13000000U;
    if ((instruction & BitfieldMask) != BitfieldPattern ||
        X18Fallback::ClassifyInstruction(instruction) !=
            X18InstructionClass::SupportedOrdinary) {
        return std::nullopt;
    }

    constexpr u32 RegisterMask = 0x1FU;
    const u32 rn = (instruction >> 5) & RegisterMask;
    const u32 rd = instruction & RegisterMask;
    if (rn != GuestX18Register && rd != GuestX18Register) {
        return std::nullopt;
    }

    u32 value_scratch = 0;
    u32 context_scratch = 0;
    const auto is_architectural_operand = [&](u32 candidate) {
        return candidate == rn || candidate == rd;
    };

    for (u32 candidate = 19; candidate <= 28; ++candidate) {
        if (is_architectural_operand(candidate)) {
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

    u32 rewritten = instruction;
    if (rn == GuestX18Register) {
        rewritten =
            ReplaceWindowsX18BitfieldRegisterField(rewritten, 5, value_scratch);
    }
    if (rd == GuestX18Register) {
        rewritten =
            ReplaceWindowsX18BitfieldRegisterField(rewritten, 0, value_scratch);
    }

    return WindowsX18BitfieldPlan{
        .rewritten_instruction = rewritten,
        .value_scratch = value_scratch,
        .context_scratch = context_scratch,
        .writes_x18 = rd == GuestX18Register,
    };
}
#endif

Patcher::Patcher() : c(m_patch_instructions), c_pre(m_patch_instructions_pre) {
    // The first word of the patch section is always a branch to the first instruction of the
    // module.
    c.dw(0);
    c_pre.dw(0);

    // Write save context helper function.
    c.l(m_save_context);
    WriteSaveContext();
    c_pre.l(m_save_context_pre);
    WriteSaveContext(c_pre);

    // Write load context helper function.
    c.l(m_load_context);
    WriteLoadContext();
    c_pre.l(m_load_context_pre);
    WriteLoadContext(c_pre);
}

Patcher::~Patcher() = default;

bool Patcher::PatchText(std::span<const u8> program_image, const Kernel::CodeSet::Segment& code) {
    // If we have patched modules but cannot reach the new module, then it needs its own patcher.
    const size_t image_size = program_image.size();

    // Check if we need split mode for large modules. A64 max takes 128MB
    // tests showed that, with update, some are larger. (In this case 208MB)
    bool use_split = false;
    if (image_size > MaxRelativeBranch) {
        if (total_program_size > 0) {
            return false;
        }
        use_split = true;
    } else if (total_program_size + image_size > MaxRelativeBranch && total_program_size > 0) {
        return false;
    }

    // Add a new module patch to our list
    modules.emplace_back();
    curr_patch = &modules.back();

    // The first word of the patch section is always a branch to the first instruction of the
    // module.
    if (use_split) {
        // curr_patch->m_branch_to_module_relocations.push_back({0, 0});
        curr_patch->m_branch_to_module_relocations_pre.push_back({0, 0});
    } else {
        curr_patch->m_branch_to_module_relocations.push_back({0, 0});
    }

    // Retrieve text segment data.
    const auto text = std::span{program_image}.subspan(code.offset, code.size);
    const auto text_words =
        std::span<const u32>{reinterpret_cast<const u32*>(text.data()), text.size() / sizeof(u32)};

#if defined(_WIN32)
#if SNAPRYU_DIAGNOSTICS
    X18AtomicPatchCounts x18_atomic_counts{};
#endif
    const auto WriteWindowsX18ExclusiveTrampoline =
        [&](ModuleDestLabel module_dest, const WindowsX18ExclusivePlan& plan,
            oaknut::VectorCodeGenerator& cg) {
            constexpr u32 FullSaveFrameSize = 0x130;
            constexpr u32 VolatileVectorOffset = 0x90;
            constexpr u32 ScratchPairOffset = 0x110;
            constexpr u32 LinkRegisterOffset = 0x120;
            constexpr u32 BlrBase = 0xD63F0000U;
            constexpr size_t GuestX18Offset =
                offsetof(GuestContext, cpu_registers) + sizeof(u64) * GuestX18Register;

            const oaknut::XReg value_reg{static_cast<int>(plan.value_scratch)};
            const oaknut::XReg context_reg{static_cast<int>(plan.context_scratch)};
            oaknut::Label getter_address;

            // Preserve the production-like footprint already proven to coexist with a live native
            // reservation on Windows ARM64. The selected nonvolatile scratches are saved because
            // they temporarily carry virtual guest x18 and the GuestContext pointer.
            cg.SUB(SP, SP, FullSaveFrameSize);
            for (int i = 0; i <= 16; i += 2) {
                cg.STP(oaknut::XReg{i}, oaknut::XReg{i + 1}, SP, 8 * i);
            }
            for (int i = 0; i <= 6; i += 2) {
                cg.STP(oaknut::QReg{i}, oaknut::QReg{i + 1}, SP,
                       VolatileVectorOffset + 16 * i);
            }
            cg.STP(value_reg, context_reg, SP, ScratchPairOffset);
            cg.STR(X30, SP, LinkRegisterOffset);

            // Generated Windows code must use the stable C-linkage getter. Never repurpose
            // physical x18 or TPIDR_EL0 as guest metadata.
            cg.LDR(context_reg, getter_address);
            cg.dw(BlrBase | (plan.context_scratch << 5));
            cg.MOV(context_reg, X0);
            cg.LDR(context_reg, context_reg,
                   offsetof(NativeExecutionParameters, native_context));
            if (plan.reads_x18) {
                cg.LDR(value_reg, context_reg, GuestX18Offset);
            }

            // Restore every unrelated guest volatile register before the actual memory operation.
            // The two selected nonvolatile scratches intentionally remain live until the rewritten
            // exclusive completes.
            cg.LDR(X30, SP, LinkRegisterOffset);
            for (int i = 6; i >= 0; i -= 2) {
                cg.LDP(oaknut::QReg{i}, oaknut::QReg{i + 1}, SP,
                       VolatileVectorOffset + 16 * i);
            }
            for (int i = 16; i >= 0; i -= 2) {
                cg.LDP(oaknut::XReg{i}, oaknut::XReg{i + 1}, SP, 8 * i);
            }
            // The save frame stays allocated while the atomic runs (see X18AtomicUsesSpBase), so an
            // exception there is written below the parked scratch pair instead of over it.
            const bool sp_base = X18AtomicUsesSpBase(plan.rewritten_instruction);
#if SNAPRYU_DIAGNOSTICS
            x18_atomic_counts.Note(false, sp_base);
#endif

            // Keep the real LDXR/STXR-family instruction in the physical ARM reservation domain.
            // Only architectural x18 operand fields have been rewritten to value_reg; an SP base
            // becomes context_reg holding the guest SP (context_reg is reloaded before any use).
            if (sp_base) {
                cg.ADD(context_reg, SP, FullSaveFrameSize);
                cg.dw(X18AtomicWithBaseRegister(plan.rewritten_instruction, plan.context_scratch));
            } else {
                cg.dw(plan.rewritten_instruction);
            }

            if (plan.writes_x18) {
                // A fault on the atomic can end the native epoch and resume the thread on another
                // core, so the GuestContext is looked up again instead of trusting context_reg.
                WriteWindowsCurrentNceParametersLookup(cg, context_reg);
                cg.LDR(context_reg, context_reg,
                       offsetof(NativeExecutionParameters, native_context));
                cg.STR(value_reg, context_reg, GuestX18Offset);
            }

            // Restore the two guest nonvolatile registers used as scratches without perturbing a
            // possible x18 output already committed to virtual GuestContext state.
            cg.LDP(value_reg, context_reg, SP, ScratchPairOffset);
            cg.ADD(SP, SP, FullSaveFrameSize);

            if (&cg == &c_pre) {
                this->BranchToModulePre(module_dest);
            } else {
                this->BranchToModule(module_dest);
            }

            // The branch above skips this literal at runtime. LDR-literal resolves it while the
            // patch is generated, avoiding any compiler/TEB TLS-offset dependency.
            cg.l(getter_address);
            cg.dx(static_cast<u64>(reinterpret_cast<uintptr_t>(
                &GetCurrentNceContextForGeneratedCode)));
        };

    // Same frame as the single-instruction exclusive trampoline above, but the load-exclusive and
    // the store-exclusive run back to back with no memory access between them (see
    // BuildWindowsX18ExclusivePairPlan).
    const auto WriteWindowsX18ExclusivePairTrampoline =
        [&](ModuleDestLabel module_dest, const WindowsX18ExclusivePairPlan& plan,
            oaknut::VectorCodeGenerator& cg) {
            constexpr u32 FullSaveFrameSize = 0x130;
            constexpr u32 VolatileVectorOffset = 0x90;
            constexpr u32 ScratchPairOffset = 0x110;
            constexpr u32 LinkRegisterOffset = 0x120;
            constexpr u32 BlrBase = 0xD63F0000U;
            constexpr size_t GuestX18Offset =
                offsetof(GuestContext, cpu_registers) + sizeof(u64) * GuestX18Register;

            const oaknut::XReg value_reg{static_cast<int>(plan.value_scratch)};
            const oaknut::XReg context_reg{static_cast<int>(plan.context_scratch)};
            oaknut::Label getter_address;

            cg.SUB(SP, SP, FullSaveFrameSize);
            for (int i = 0; i <= 16; i += 2) {
                cg.STP(oaknut::XReg{i}, oaknut::XReg{i + 1}, SP, 8 * i);
            }
            for (int i = 0; i <= 6; i += 2) {
                cg.STP(oaknut::QReg{i}, oaknut::QReg{i + 1}, SP,
                       VolatileVectorOffset + 16 * i);
            }
            cg.STP(value_reg, context_reg, SP, ScratchPairOffset);
            cg.STR(X30, SP, LinkRegisterOffset);

            cg.LDR(context_reg, getter_address);
            cg.dw(BlrBase | (plan.context_scratch << 5));
            cg.MOV(context_reg, X0);
            cg.LDR(context_reg, context_reg,
                   offsetof(NativeExecutionParameters, native_context));
            if (plan.reads_x18) {
                cg.LDR(value_reg, context_reg, GuestX18Offset);
            }

            cg.LDR(X30, SP, LinkRegisterOffset);
            for (int i = 6; i >= 0; i -= 2) {
                cg.LDP(oaknut::QReg{i}, oaknut::QReg{i + 1}, SP,
                       VolatileVectorOffset + 16 * i);
            }
            for (int i = 16; i >= 0; i -= 2) {
                cg.LDP(oaknut::XReg{i}, oaknut::XReg{i + 1}, SP, 8 * i);
            }

            // Both instructions see the guest SP through context_reg when they use an SP base.
            const bool load_sp = X18AtomicUsesSpBase(plan.rewritten_load);
            const bool store_sp = X18AtomicUsesSpBase(plan.rewritten_store);
#if SNAPRYU_DIAGNOSTICS
            x18_atomic_counts.Note(false, load_sp || store_sp);
            ++x18_atomic_counts.pair;
#endif
            if (load_sp || store_sp) {
                cg.ADD(context_reg, SP, FullSaveFrameSize);
            }
            cg.dw(load_sp ? X18AtomicWithBaseRegister(plan.rewritten_load, plan.context_scratch)
                          : plan.rewritten_load);
            cg.dw(store_sp ? X18AtomicWithBaseRegister(plan.rewritten_store, plan.context_scratch)
                           : plan.rewritten_store);

            if (plan.writes_x18) {
                WriteWindowsCurrentNceParametersLookup(cg, context_reg);
                cg.LDR(context_reg, context_reg,
                       offsetof(NativeExecutionParameters, native_context));
                cg.STR(value_reg, context_reg, GuestX18Offset);
            }

            cg.LDP(value_reg, context_reg, SP, ScratchPairOffset);
            cg.ADD(SP, SP, FullSaveFrameSize);

            if (&cg == &c_pre) {
                this->BranchToModulePre(module_dest);
            } else {
                this->BranchToModule(module_dest);
            }

            cg.l(getter_address);
            cg.dx(static_cast<u64>(reinterpret_cast<uintptr_t>(
                &GetCurrentNceContextForGeneratedCode)));
        };

    const auto WriteWindowsX18LseTrampoline =
        [&](ModuleDestLabel module_dest, const WindowsX18LsePlan& plan,
            oaknut::VectorCodeGenerator& cg) {
            constexpr u32 FullSaveFrameSize = 0x130;
            constexpr u32 VolatileVectorOffset = 0x90;
            constexpr u32 ScratchPairOffset = 0x110;
            constexpr u32 ContextScratchOffset = 0x120;
            constexpr u32 PairLinkRegisterOffset = 0x128;
            constexpr u32 ScalarLinkRegisterOffset = 0x120;
            constexpr u32 BlrBase = 0xD63F0000U;
            constexpr size_t GuestX18Offset =
                offsetof(GuestContext, cpu_registers) + sizeof(u64) * GuestX18Register;

            const oaknut::XReg value_reg{static_cast<int>(plan.value_scratch)};
            const oaknut::XReg context_reg{static_cast<int>(plan.context_scratch)};
            const bool has_pair_scratch = plan.pair_scratch_second != 0;
            const oaknut::XReg pair_reg{
                static_cast<int>(has_pair_scratch ? plan.pair_scratch_second : plan.value_scratch)};
            oaknut::Label getter_address;

            // Use the same conservative guest volatile save footprint as the proven classic
            // exclusive seam. LSE atomics are single instructions, so no reservation state crosses
            // this getter call; the native atomic instruction still owns the complete memory
            // transaction and its original A/R ordering bits.
            cg.SUB(SP, SP, FullSaveFrameSize);
            for (int i = 0; i <= 16; i += 2) {
                cg.STP(oaknut::XReg{i}, oaknut::XReg{i + 1}, SP, 8 * i);
            }
            for (int i = 0; i <= 6; i += 2) {
                cg.STP(oaknut::QReg{i}, oaknut::QReg{i + 1}, SP,
                       VolatileVectorOffset + 16 * i);
            }
            if (has_pair_scratch) {
                cg.STP(value_reg, pair_reg, SP, ScratchPairOffset);
                cg.STR(context_reg, SP, ContextScratchOffset);
                cg.STR(X30, SP, PairLinkRegisterOffset);
            } else {
                cg.STP(value_reg, context_reg, SP, ScratchPairOffset);
                cg.STR(X30, SP, ScalarLinkRegisterOffset);
            }

            cg.LDR(context_reg, getter_address);
            cg.dw(BlrBase | (plan.context_scratch << 5));
            cg.MOV(context_reg, X0);
            cg.LDR(context_reg, context_reg,
                   offsetof(NativeExecutionParameters, native_context));
            if (plan.reads_x18) {
                cg.LDR(value_reg, context_reg, GuestX18Offset);
            }
            if (plan.casp_pair_uses_x18) {
                cg.MOV(pair_reg, X19);
            }

            cg.LDR(X30, SP,
                   has_pair_scratch ? PairLinkRegisterOffset : ScalarLinkRegisterOffset);
            for (int i = 6; i >= 0; i -= 2) {
                cg.LDP(oaknut::QReg{i}, oaknut::QReg{i + 1}, SP,
                       VolatileVectorOffset + 16 * i);
            }
            for (int i = 16; i >= 0; i -= 2) {
                cg.LDP(oaknut::XReg{i}, oaknut::XReg{i + 1}, SP, 8 * i);
            }
            // Same frame rule as the exclusive trampoline above.
            const bool sp_base = X18AtomicUsesSpBase(plan.rewritten_instruction);
#if SNAPRYU_DIAGNOSTICS
            x18_atomic_counts.Note(true, sp_base);
#endif

            // Execute the real host LSE instruction. Only guest-x18 register fields were rewritten;
            // acquire/release bits and the complete atomic operation remain native and unchanged.
            // An SP base becomes context_reg holding the guest SP (reloaded before any use).
            if (sp_base) {
                cg.ADD(context_reg, SP, FullSaveFrameSize);
                cg.dw(X18AtomicWithBaseRegister(plan.rewritten_instruction, plan.context_scratch));
            } else {
                cg.dw(plan.rewritten_instruction);
            }

            if (plan.writes_x18) {
                // As above: the GuestContext may belong to another core after a fault.
                WriteWindowsCurrentNceParametersLookup(cg, context_reg);
                cg.LDR(context_reg, context_reg,
                       offsetof(NativeExecutionParameters, native_context));
                cg.STR(value_reg, context_reg, GuestX18Offset);
            }
            if (plan.writes_x19) {
                cg.MOV(X19, pair_reg);
            }

            if (has_pair_scratch) {
                cg.LDP(value_reg, pair_reg, SP, ScratchPairOffset);
                cg.LDR(context_reg, SP, ContextScratchOffset);
            } else {
                cg.LDP(value_reg, context_reg, SP, ScratchPairOffset);
            }
            cg.ADD(SP, SP, FullSaveFrameSize);

            if (&cg == &c_pre) {
                this->BranchToModulePre(module_dest);
            } else {
                this->BranchToModule(module_dest);
            }

            cg.l(getter_address);
            cg.dx(static_cast<u64>(reinterpret_cast<uintptr_t>(
                &GetCurrentNceContextForGeneratedCode)));
        };

    const auto WriteWindowsX18BitfieldTrampoline =
        [&](ModuleDestLabel module_dest, u32 original, const WindowsX18BitfieldPlan& plan,
            oaknut::VectorCodeGenerator& cg) {
            constexpr size_t GuestX18Offset =
                offsetof(GuestContext, cpu_registers) + sizeof(u64) * GuestX18Register;

            const oaknut::XReg value_reg{static_cast<int>(plan.value_scratch)};
            const oaknut::XReg context_reg{static_cast<int>(plan.context_scratch)};

            // R2b-B2x1: one label per instruction. A recipe is recorded only for the one-load TEB
            // slot lookup and only if every label is at index * 4 (WindowsX18BitfieldSteps).
            const bool record_recipe =
                CurrentNceContext::HasTebSlot() && CurrentNceContext::HasScratchSlots();
            std::array<oaknut::Label, WindowsX18BitfieldMaxSteps + 1> labels;
            std::size_t label_index = 0;
            const auto bind = [&] { cg.l(labels[label_index++]); };

            // Keep physical x18 Windows/TEB-owned at all times. Reuse the already-proven
            // Windows generated-context lookup, then substitute virtual guest x18 into a
            // non-conflicting nonvolatile scratch only for the original bitfield instruction.
            // Saving both scratch registers on the guest stack preserves unrelated architectural
            // state while avoiding BRK/VEH/Dynarmic/NtContinue for this instruction.
            bind();
            cg.STP(value_reg, context_reg, SP, PRE_INDEXED, -16);
            bind();
            WriteWindowsCurrentNceParametersLookup(cg, context_reg);
            bind();
            cg.LDR(context_reg, context_reg,
                   offsetof(NativeExecutionParameters, native_context));
            bind();
            cg.LDR(value_reg, context_reg, GuestX18Offset);

            // Bitfield instructions are non-faulting, non-branching and do not modify NZCV.
            // All x18 register fields were rewritten to value_reg by the bounded plan above.
            bind();
            cg.dw(plan.rewritten_instruction);

            if (plan.writes_x18) {
                bind();
                cg.STR(value_reg, context_reg, GuestX18Offset);
            }

            bind();
            cg.LDP(value_reg, context_reg, SP, POST_INDEXED, 16);

            bind();
            if (&cg == &c_pre) {
                this->BranchToModulePre(module_dest);
            } else {
                this->BranchToModule(module_dest);
            }
            const std::size_t word_count = label_index;
            bind();

            if (!record_recipe) {
                return;
            }
            const auto steps = WindowsX18BitfieldSteps(plan.writes_x18);
            WindowsPatchRecoveryRecipe recipe{};
            recipe.kind = WindowsPatchRecoveryKind::X18BitfieldV1;
            recipe.word_count = static_cast<u8>(word_count);
            for (std::size_t i = 0; i <= word_count; ++i) {
                if (static_cast<std::size_t>(labels[i].offset() - labels[0].offset()) !=
                    i * sizeof(u32)) {
                    return; // A different emitted shape stays Unknown.
                }
            }
            for (std::size_t i = 0; i < word_count; ++i) {
                recipe.steps[i] = steps[i];
            }
            recipe.guest_pc = module_dest - sizeof(u32); // Relocated with this module later.
            recipe.original_instruction = original;
            recipe.rewritten_instruction = plan.rewritten_instruction;
            recipe.params_tls_offset = CurrentNceContext::TebSlotOffset();
            recipe.scratch_tls_offset = CurrentNceContext::ScratchSlotOffset(0);
            recipe.spill_context_offset = offsetof(NativeExecutionParameters, x18_scratch);
            recipe.spill_value_offset = recipe.spill_context_offset + sizeof(u64);
            recipe.native_context_offset = offsetof(NativeExecutionParameters, native_context);
            recipe.guest_x18_offset = GuestX18Offset;
            recipe.value_reg = static_cast<u8>(plan.value_scratch);
            recipe.context_reg = static_cast<u8>(plan.context_scratch);
            recipe.begin = static_cast<u64>(labels[0].offset());
            recipe.end = static_cast<u64>(labels[word_count].offset());
            recipe.tail_offset = recipe.steps[word_count - 1].offset;
            recipe.tail_bytes = sizeof(u32);
            curr_patch->m_windows_recovery.push_back({recipe, &cg == &c_pre});
        };

    // Conditional branches that only test guest x18 (CBZ/CBNZ/TBZ/TBNZ Xt|Wt=18). They do not
    // write registers, memory or NZCV, so the virtual x18 value can be tested in a scratch register
    // and the branch resolved natively, instead of taking the BRK/VEH/NtContinue fallback cycle
    // for every evaluation of a hot polling loop.
    // General x18 register-field substitution. The rewritten instruction (validated against the
    // Dynarmic decoder in BuildWindowsX18SubstitutionPlan) runs natively with scratch S standing in
    // for guest x18; P holds the GuestContext pointer. Both scratches are spilled to per-OS-thread
    // TEB TLS slots through physical x18, so neither the guest stack, SP-relative addressing nor
    // NZCV is affected.
    const auto WriteWindowsX18SubstitutionTrampoline =
        [&](ModuleDestLabel module_dest, u32 original, const WindowsX18SubstitutionPlan& plan,
            oaknut::VectorCodeGenerator& cg) {
            constexpr size_t GuestX18Offset =
                offsetof(GuestContext, cpu_registers) + sizeof(u64) * GuestX18Register;
            const oaknut::XReg value_reg{static_cast<int>(plan.value_reg)};
            const oaknut::XReg context_reg{static_cast<int>(plan.context_reg)};
            const u32 teb_spill = CurrentNceContext::ScratchSlotOffset(0);
            const u32 params_slot = CurrentNceContext::TebSlotOffset();
            constexpr size_t SpillContext = offsetof(NativeExecutionParameters, x18_scratch);
            constexpr size_t SpillValue = SpillContext + sizeof(u64);
            constexpr size_t NativeContext = offsetof(NativeExecutionParameters, native_context);

            using Source = WindowsPatchScratchSource;
            using X18Source = WindowsPatchX18Source;
            WindowsPatchRecoveryRecipe recipe{};
            recipe.guest_pc = module_dest - sizeof(u32); // Relocated with this module later.
            recipe.original_instruction = original;
            recipe.rewritten_instruction = plan.rewritten_instruction;
            recipe.params_tls_offset = params_slot;
            recipe.scratch_tls_offset = teb_spill;
            recipe.spill_context_offset = SpillContext;
            recipe.spill_value_offset = SpillValue;
            recipe.native_context_offset = NativeContext;
            recipe.guest_x18_offset = GuestX18Offset;
            recipe.value_reg = static_cast<u8>(plan.value_reg);
            recipe.context_reg = static_cast<u8>(plan.context_reg);
            std::array<oaknut::Label, WindowsPatchRecoveryRecipe::StepCount + 1> labels;
            std::size_t step_index = 0;
            const auto mark = [&](Source value, Source context, X18Source x18, u8 pc_delta) {
                cg.l(labels[step_index]);
                recipe.steps[step_index] = {0, value, context, x18, pc_delta};
                ++step_index;
            };

            // Spill both scratches into the current guest thread's parameters. The TEB slot only
            // bridges the first instructions, which cannot fault or be interrupted (patch window).
            mark(Source::CapturedSelf, Source::CapturedSelf, X18Source::GuestContext, 0);
            cg.STR(context_reg, X18, teb_spill);
            mark(Source::CapturedSelf, Source::CapturedSelf, X18Source::GuestContext, 0);
            cg.LDR(context_reg, X18, params_slot);
            mark(Source::CapturedSelf, Source::TargetTebScratch, X18Source::GuestContext, 0);
            cg.STR(value_reg, context_reg, SpillValue);
            mark(Source::CapturedSelf, Source::TargetTebScratch, X18Source::GuestContext, 0);
            cg.LDR(value_reg, X18, teb_spill);
            mark(Source::ParamsSpillValue, Source::CapturedValueRegister, X18Source::GuestContext, 0);
            cg.STR(value_reg, context_reg, SpillContext);
            mark(Source::ParamsSpillValue, Source::ParamsSpillContext, X18Source::GuestContext, 0);
            cg.LDR(context_reg, context_reg, NativeContext);
            mark(Source::ParamsSpillValue, Source::ParamsSpillContext, X18Source::GuestContext, 0);
            cg.LDR(value_reg, context_reg, GuestX18Offset);

            // The only instruction here that can fault. If the fault ends the epoch and the thread
            // later resumes on another core, everything after it re-derives its pointers.
            mark(Source::ParamsSpillValue, Source::ParamsSpillContext, X18Source::GuestContext, 0);
            cg.dw(plan.rewritten_instruction);

            mark(Source::ParamsSpillValue, Source::ParamsSpillContext, X18Source::CapturedValueRegister, 4);
            cg.LDR(context_reg, X18, params_slot);
            mark(Source::ParamsSpillValue, Source::ParamsSpillContext, X18Source::CapturedValueRegister, 4);
            cg.LDR(context_reg, context_reg, NativeContext);
            mark(Source::ParamsSpillValue, Source::ParamsSpillContext, X18Source::CapturedValueRegister, 4);
            cg.STR(value_reg, context_reg, GuestX18Offset);
            mark(Source::ParamsSpillValue, Source::ParamsSpillContext, X18Source::GuestContext, 4);
            cg.LDR(context_reg, X18, params_slot);
            mark(Source::ParamsSpillValue, Source::ParamsSpillContext, X18Source::GuestContext, 4);
            cg.LDR(value_reg, context_reg, SpillValue);
            mark(Source::CapturedSelf, Source::ParamsSpillContext, X18Source::GuestContext, 4);
            cg.LDR(context_reg, context_reg, SpillContext);

            mark(Source::CapturedSelf, Source::CapturedSelf, X18Source::GuestContext, 4);
            if (&cg == &c_pre) {
                this->BranchToModulePre(module_dest);
            } else {
                this->BranchToModule(module_dest);
            }
            cg.l(labels.back());
            ASSERT(step_index == WindowsPatchRecoveryRecipe::StepCount);
            recipe.begin = static_cast<u64>(labels.front().offset());
            recipe.end = static_cast<u64>(labels.back().offset());
            for (std::size_t i = 0; i < recipe.steps.size(); ++i) {
                recipe.steps[i].offset = static_cast<u16>(labels[i].offset() - labels[0].offset());
            }
            recipe.tail_offset = recipe.steps.back().offset;
            recipe.tail_bytes = static_cast<u16>(labels.back().offset() - labels[14].offset());
            curr_patch->m_windows_recovery.push_back({recipe, &cg == &c_pre});
        };

    const auto WriteWindowsX18CondBranchTrampoline =
        [&](uintptr_t this_offset, ModuleDestLabel fallthrough, u32 instruction,
            oaknut::VectorCodeGenerator& cg) {
            constexpr size_t GuestX18Offset =
                offsetof(GuestContext, cpu_registers) + sizeof(u64) * GuestX18Register;
            const bool is_tb = (instruction & 0x7E000000U) == 0x36000000U;
            const bool is_nz = ((instruction >> 24) & 1U) != 0;
            const bool is_64 = ((instruction >> 31) & 1U) != 0;
            s64 imm{};
            u32 bit{};
            if (is_tb) {
                imm = static_cast<s64>(static_cast<s32>(((instruction >> 5) & 0x3FFFU) << 18) >> 18);
                bit = (is_64 ? 32U : 0U) | ((instruction >> 19) & 0x1FU);
            } else {
                imm = static_cast<s64>(static_cast<s32>(((instruction >> 5) & 0x7FFFFU) << 13) >> 13);
            }
            const auto target = static_cast<ModuleDestLabel>(
                static_cast<s64>(this_offset) + imm * static_cast<s64>(sizeof(u32)));

            // R2b-B2x2: one label per instruction (WindowsX18CondBranchStepTable).
            std::array<oaknut::Label, WindowsX18CondBranchSteps + 1> labels;
            std::size_t label_index = 0;
            const auto bind = [&] { cg.l(labels[label_index++]); };

            oaknut::Label taken;
            bind();
            cg.STR(X16, SP, PRE_INDEXED, -16);
            bind();
            WriteWindowsCurrentNceParametersLookup(cg, X16);
            bind();
            cg.LDR(X16, X16, offsetof(NativeExecutionParameters, native_context));
            bind();
            cg.LDR(X16, X16, GuestX18Offset);
            bind();
            if (is_tb) {
                if (is_nz) {
                    cg.TBNZ(X16, bit, taken);
                } else {
                    cg.TBZ(X16, bit, taken);
                }
            } else if (is_64) {
                if (is_nz) {
                    cg.CBNZ(X16, taken);
                } else {
                    cg.CBZ(X16, taken);
                }
            } else {
                if (is_nz) {
                    cg.CBNZ(W16, taken);
                } else {
                    cg.CBZ(W16, taken);
                }
            }
            bind();
            cg.LDR(X16, SP, POST_INDEXED, 16);
            bind();
            if (&cg == &c_pre) {
                this->BranchToModulePre(fallthrough);
            } else {
                this->BranchToModule(fallthrough);
            }
            cg.l(taken);
            bind();
            cg.LDR(X16, SP, POST_INDEXED, 16);
            bind();
            if (&cg == &c_pre) {
                this->BranchToModulePre(target);
            } else {
                this->BranchToModule(target);
            }
            bind();

            // The recovery derives the branch target from the original instruction and guest_pc,
            // so guest_pc must be the instruction itself.
            if (!CurrentNceContext::HasTebSlot() || fallthrough != this_offset + sizeof(u32) ||
                labels[7].offset() != taken.offset()) {
                return;
            }
            for (std::size_t i = 0; i <= WindowsX18CondBranchSteps; ++i) {
                if (static_cast<std::size_t>(labels[i].offset() - labels[0].offset()) !=
                    i * sizeof(u32)) {
                    return; // A different emitted shape stays Unknown.
                }
            }
            WindowsPatchRecoveryRecipe recipe{};
            recipe.kind = WindowsPatchRecoveryKind::X18CondBranchV1;
            recipe.word_count = static_cast<u8>(WindowsX18CondBranchSteps);
            const auto steps = WindowsX18CondBranchStepTable();
            std::copy(steps.begin(), steps.end(), recipe.steps.begin());
            recipe.guest_pc = this_offset; // Relocated with this module later.
            recipe.original_instruction = instruction;
            recipe.rewritten_instruction = WindowsX18CondBranchRewrite(instruction);
            recipe.params_tls_offset = CurrentNceContext::TebSlotOffset();
            recipe.scratch_tls_offset = CurrentNceContext::ScratchSlotOffset(0);
            recipe.spill_context_offset = offsetof(NativeExecutionParameters, x18_scratch);
            recipe.spill_value_offset = recipe.spill_context_offset + sizeof(u64);
            recipe.native_context_offset = offsetof(NativeExecutionParameters, native_context);
            recipe.guest_x18_offset = GuestX18Offset;
            recipe.value_reg = WindowsX18CondBranchScratch;
            recipe.context_reg = WindowsX18CondBranchScratch;
            recipe.begin = static_cast<u64>(labels[0].offset());
            recipe.end = static_cast<u64>(labels[WindowsX18CondBranchSteps].offset());
            recipe.tail_offset = recipe.steps[WindowsX18CondBranchSteps - 1].offset;
            recipe.tail_bytes = sizeof(u32);
            curr_patch->m_windows_recovery.push_back({recipe, &cg == &c_pre});
        };

    const auto WriteWindowsX18DirectTrampoline =
        [&](ModuleDestLabel instruction_pc, ModuleDestLabel module_dest, u32 instruction,
            oaknut::VectorCodeGenerator& cg, oaknut::Label& save_ctx,
            oaknut::Label& load_ctx) {
            const bool is_pre = (&cg == &c_pre);
            (void)load_ctx;

            constexpr u32 BlrBase = 0xD63F0000U;
            constexpr size_t WindowsTebStackBaseOffset = 0x08;
            constexpr size_t HostRegsOffset = offsetof(HostContext, host_saved_regs);
            constexpr size_t HostVregsOffset = offsetof(HostContext, host_saved_vregs);
            constexpr size_t GuestVregsOffset = offsetof(GuestContext, vector_registers);

            oaknut::Label guest_pc;
            oaknut::Label helper_address;
            oaknut::Label direct_success;

            // Snapshot live guest architectural state while the guest stack is still active.
            // WriteSaveContext deliberately keeps physical x18 Windows/TEB-owned and preserves
            // virtual guest x18 in GuestContext.
            cg.STR(X30, SP, PRE_INDEXED, -16);
            cg.BL(save_ctx);
            cg.LDR(X30, SP, POST_INDEXED, 16);

            WriteWindowsCurrentNceParametersLookup(cg, X2);
            cg.LDR(X2, X2, offsetof(NativeExecutionParameters, native_context));
            cg.LDR(X3, guest_pc);
            cg.STR(X3, X2, offsetof(GuestContext, pc));

            // P3B: no Windows/C++ helper may run on the guest stack. Restore the host TEB bounds
            // first and immediately switch to the already-captured host SP before entering the
            // Dynarmic single-step helper.
            cg.LDR(X3, X2, offsetof(GuestContext, windows_host_stack_base));
            cg.LDR(X4, X2, offsetof(GuestContext, windows_host_stack_limit));
            cg.LDR(X5, X2, offsetof(GuestContext, host_ctx) + offsetof(HostContext, host_sp));
            cg.STP(X3, X4, X18, WindowsTebStackBaseOffset);
            cg.MOV(SP, X5);

            // The helper, and RunThread after a non-zero return, are host code. Load the host FPCR
            // saved at guest entry, as the normal exit does (WriteSaveContext); otherwise the next
            // guest entry saves the guest FPCR as the host FPCR. direct_success reloads the guest
            // FPCR below.
            cg.LDR(W3, X2, offsetof(GuestContext, host_ctx) + offsetof(HostContext, host_fpcr));
            cg.MSR(oaknut::SystemReg::FPCR, X3);

            cg.MOV(X0, X2);
            cg.MOV(W1, instruction);
            cg.LDR(X16, helper_address);
            cg.dw(BlrBase | (16U << 5));
            cg.CBZ(X0, direct_success);

            // A real halt/fault is already on the host stack with host TEB bounds installed.
            // Acquire scheduler ownership and return through the original RunThread continuation.
            this->LockContext(cg);
            WriteWindowsCurrentNceParametersLookup(cg, X1);
            cg.LDR(X1, X1, offsetof(NativeExecutionParameters, native_context));
            cg.ADD(X1, X1, offsetof(GuestContext, host_ctx));

            cg.LDP(X19, X20, X1, HostRegsOffset);
            cg.LDP(X21, X22, X1, HostRegsOffset + 2 * sizeof(u64));
            cg.LDP(X23, X24, X1, HostRegsOffset + 4 * sizeof(u64));
            cg.LDP(X25, X26, X1, HostRegsOffset + 6 * sizeof(u64));
            cg.LDP(X27, X28, X1, HostRegsOffset + 8 * sizeof(u64));
            cg.LDP(X29, X30, X1, HostRegsOffset + 10 * sizeof(u64));
            cg.LDP(Q8, Q9, X1, HostVregsOffset);
            cg.LDP(Q10, Q11, X1, HostVregsOffset + 2 * sizeof(u128));
            cg.LDP(Q12, Q13, X1, HostVregsOffset + 4 * sizeof(u128));
            cg.LDP(Q14, Q15, X1, HostVregsOffset + 6 * sizeof(u128));
            cg.RET();

            cg.l(direct_success);

            // Stay on the host stack while locating GuestContext. From this point onward restore
            // guest state directly, mirroring WindowsNceEnterGuest, so no C ABI call executes
            // after the guest SP is reinstalled.
            WriteWindowsCurrentNceParametersLookup(cg, X17);
            cg.LDR(X17, X17, offsetof(NativeExecutionParameters, native_context));

            cg.LDR(W9, X17, offsetof(GuestContext, fpcr));
            cg.MSR(oaknut::SystemReg::FPCR, X9);
            cg.LDR(W9, X17, offsetof(GuestContext, fpsr));
            cg.MSR(oaknut::SystemReg::FPSR, X9);
            cg.LDR(W9, X17, offsetof(GuestContext, nzcv));
            cg.MSR(oaknut::SystemReg::NZCV, X9);

            for (int i = 0; i <= 30; i += 2) {
                cg.LDP(oaknut::QReg{i}, oaknut::QReg{i + 1}, X17,
                       GuestVregsOffset + 16 * i);
            }

            // x30 temporarily carries guest SP. x14/x15 are reserved until the TEB pair handoff;
            // x16/x17 remain patch scratch until the final two guest-register reloads.
            cg.LDR(X30, X17, offsetof(GuestContext, sp));
            cg.LDP(X0, X1, X17, 0x000);
            cg.LDP(X2, X3, X17, 0x010);
            cg.LDP(X4, X5, X17, 0x020);
            cg.LDP(X6, X7, X17, 0x030);
            cg.LDP(X8, X9, X17, 0x040);
            cg.LDP(X10, X11, X17, 0x050);
            cg.LDP(X12, X13, X17, 0x060);
            cg.LDP(X19, X20, X17, 0x098);
            cg.LDP(X21, X22, X17, 0x0A8);
            cg.LDP(X23, X24, X17, 0x0B8);
            cg.LDP(X25, X26, X17, 0x0C8);
            cg.LDP(X27, X28, X17, 0x0D8);
            cg.LDR(X29, X17, 0x0E8);

            // Match the D2 leaf stack handoff: publish a complete guest TEB interval and switch SP
            // immediately, with no call/probe/stack access in between.
            cg.LDR(X14, X17, offsetof(GuestContext, windows_guest_stack_base));
            cg.LDR(X15, X17, offsetof(GuestContext, windows_guest_stack_limit));
            cg.STP(X14, X15, X18, WindowsTebStackBaseOffset);
            cg.MOV(SP, X30);

            cg.LDP(X14, X15, X17, 0x070);
            cg.LDR(X30, X17, 0x0F0);
            cg.LDR(X16, X17, 0x080);
            cg.LDR(X17, X17, 0x088);

            if (is_pre) {
                this->BranchToModulePre(module_dest);
            } else {
                this->BranchToModule(module_dest);
            }

            cg.l(guest_pc);
            if (is_pre) {
                this->WriteModulePcPre(instruction_pc);
            } else {
                this->WriteModulePc(instruction_pc);
            }
            cg.l(helper_address);
            cg.dx(static_cast<u64>(reinterpret_cast<uintptr_t>(
                &WindowsNceExecuteDirectX18)));
        };
#endif

    // Loop through instructions, patching as needed.
    for (u32 i = ModuleCodeIndex; i < static_cast<u32>(text_words.size()); i++) {
        const u32 inst = text_words[i];

        const auto AddRelocations = [&](bool& pre_buffer) {
            const uintptr_t this_offset = i * sizeof(u32);
            const uintptr_t next_offset = this_offset + sizeof(u32);

            pre_buffer = use_split && (this_offset < MaxRelativeBranch);

            // Relocate to pre- or post-patch
            if (pre_buffer) {
                this->BranchToPatchPre(this_offset);
            } else {
                this->BranchToPatch(this_offset);
            }

            // Relocate from patch to next instruction.
            return next_offset;
        };

        // SVC
        if (auto svc = SVC{inst}; svc.Verify()) {
            bool pre_buffer = false;
            auto ret = AddRelocations(pre_buffer);
            if (pre_buffer) {
                WriteSvcTrampoline(ret, svc.GetValue(), c_pre, m_save_context_pre, m_load_context_pre);
            } else {
                WriteSvcTrampoline(ret, svc.GetValue(), c, m_save_context, m_load_context);
            }
            continue;
        }

        // MRS Xn, TPIDR_EL0
        // MRS Xn, TPIDRRO_EL0
        if (auto mrs = MRS{inst};
            mrs.Verify() && (mrs.GetSystemReg() == TpidrroEl0 || mrs.GetSystemReg() == TpidrEl0)) {
            const auto src_reg = mrs.GetSystemReg() == TpidrroEl0 ? oaknut::SystemReg::TPIDRRO_EL0
                                                                  : oaknut::SystemReg::TPIDR_EL0;
            const auto dest_reg = oaknut::XReg{static_cast<int>(mrs.GetRt())};
            bool pre_buffer = false;
            auto ret = AddRelocations(pre_buffer);
            if (pre_buffer) {
                WriteMrsHandler(ret, dest_reg, src_reg, c_pre);
            } else {
                WriteMrsHandler(ret, dest_reg, src_reg, c);
            }
            continue;
        }

#if defined(_WIN32)
        // Windows ARM64 does not expose CTR_EL0 to user mode. Match Dynarmic's guest-visible
        // CTR_EL0 value instead of executing the guest MRS natively and taking STATUS_ILLEGAL_INSTRUCTION.
        if (auto mrs = MRS{inst}; mrs.Verify() && mrs.GetSystemReg() == CtrEl0) {
            bool pre_buffer = false;
            const auto ret = AddRelocations(pre_buffer);
            if (pre_buffer) {
                WriteCtrEl0Handler(ret, oaknut::XReg{static_cast<int>(mrs.GetRt())}, c_pre);
            } else {
                WriteCtrEl0Handler(ret, oaknut::XReg{static_cast<int>(mrs.GetRt())}, c);
            }
            continue;
        }
#endif

#if defined(_WIN32)
        // Windows ARM64 does not permit guest DC CIVAC in user mode. Eden's current Dynarmic
        // configuration leaves hook_data_cache_operations disabled, so ordinary data-cache
        // maintenance instructions have no emulator-visible side effect there. Match that guest
        // contract by skipping only the observed DC CIVAC instruction on Windows NCE.
        if (DCCIVAC{inst}.Verify()) {
            bool pre_buffer = false;
            const auto ret = AddRelocations(pre_buffer);
            if (pre_buffer) {
                BranchToModulePre(ret);
            } else {
                BranchToModule(ret);
            }
            continue;
        }
#endif

        // MRS Xn, CNTPCT_EL0
        if (auto mrs = MRS{inst}; mrs.Verify() && mrs.GetSystemReg() == CntpctEl0) {
            bool pre_buffer = false;
            auto ret = AddRelocations(pre_buffer);
            if (pre_buffer) {
                WriteCntpctHandler(ret, oaknut::XReg{static_cast<int>(mrs.GetRt())}, c_pre);
            } else {
                WriteCntpctHandler(ret, oaknut::XReg{static_cast<int>(mrs.GetRt())}, c);
            }
            continue;
        }

        // MRS Xn, CNTFRQ_EL0
        if (auto mrs = MRS{inst}; mrs.Verify() && mrs.GetSystemReg() == CntfrqEl0) {
            bool pre_buffer = false;
            auto ret = AddRelocations(pre_buffer);
            if (pre_buffer) {
                WriteCntfrqHandler(ret, oaknut::XReg{static_cast<int>(mrs.GetRt())}, c_pre);
            } else {
                WriteCntfrqHandler(ret, oaknut::XReg{static_cast<int>(mrs.GetRt())}, c);
            }
            continue;
        }

        // MSR TPIDR_EL0, Xn
        if (auto msr = MSR{inst}; msr.Verify() && msr.GetSystemReg() == TpidrEl0) {
            bool pre_buffer = false;
            auto ret = AddRelocations(pre_buffer);
             if (pre_buffer) {
                WriteMsrHandler(ret, oaknut::XReg{static_cast<int>(msr.GetRt())}, c_pre);
            } else {
                WriteMsrHandler(ret, oaknut::XReg{static_cast<int>(msr.GetRt())}, c);
            }
            continue;
        }

#if defined(_WIN32)
        if (const bool is_cb = (inst & 0x7E000000U) == 0x34000000U,
                       is_tb = (inst & 0x7E000000U) == 0x36000000U;
            (is_cb || is_tb) && (inst & 0x1FU) == GuestX18Register) {
            const uintptr_t this_offset = i * sizeof(u32);
            bool pre_buffer = false;
            const auto ret = AddRelocations(pre_buffer);
            if (pre_buffer) {
                WriteWindowsX18CondBranchTrampoline(this_offset, ret, inst, c_pre);
            } else {
                WriteWindowsX18CondBranchTrampoline(this_offset, ret, inst, c);
            }
            continue;
        }

        // P2A: eliminate the expensive BRK/VEH/host-return/NtContinue cycle for the directly
        // observed ordinary-x18 bitfield family. The generated helper keeps physical x18 as TEB,
        // substitutes virtual guest x18 through a scratch register, executes the original
        // instruction natively, and branches straight back to the following guest instruction.
        if (const auto plan = BuildWindowsX18BitfieldPlan(inst)) {
            bool pre_buffer = false;
            const auto ret = AddRelocations(pre_buffer);
            if (pre_buffer) {
                WriteWindowsX18BitfieldTrampoline(ret, inst, *plan, c_pre);
            } else {
                WriteWindowsX18BitfieldTrampoline(ret, inst, *plan, c);
            }
            continue;
        }

        // Intercept the bounded single-instruction LSE families before Eden's broad Exclusive
        // signature. CASP overlaps Exclusive::Verify(); allowing it to fall through would set its
        // release bit via AsOrdered() and silently change guest ordering semantics.
        if (IsWindowsLseInstruction(inst)) {
            if (WindowsLseTouchesRegister(inst, GuestX18Register)) {
                const auto plan = BuildWindowsX18LsePlan(inst);
                if (!plan) {
                    // Never allow a recognized LSE touching guest x18 to execute with physical
                    // Windows x18 when a bounded non-conflicting rewrite cannot be constructed.
                    return false;
                }

                bool pre_buffer = false;
                const auto ret = AddRelocations(pre_buffer);
                if (pre_buffer) {
                    WriteWindowsX18LseTrampoline(ret, *plan, c_pre);
                } else {
                    WriteWindowsX18LseTrampoline(ret, *plan, c);
                }
            }
            // Non-x18 LSE instructions already execute correctly as native single instructions.
            // Continue here so CASP never reaches the legacy broad-exclusive AsOrdered pass.
            continue;
        }
#endif

        if (auto exclusive = Exclusive{inst}; exclusive.Verify()) {
#if defined(_WIN32)
            if (i + 1 < static_cast<u32>(text_words.size())) {
                if (const auto pair =
                        BuildWindowsX18ExclusivePairPlan(exclusive, Exclusive{text_words[i + 1]})) {
                    bool pre_buffer = false;
                    const auto ret = AddRelocations(pre_buffer);
                    // Resume after the store. The store still gets its own patch on the next
                    // iteration for code that branches to it directly.
                    if (pre_buffer) {
                        WriteWindowsX18ExclusivePairTrampoline(ret + sizeof(u32), *pair, c_pre);
                    } else {
                        WriteWindowsX18ExclusivePairTrampoline(ret + sizeof(u32), *pair, c);
                    }
                    continue;
                }
            }
            if (const auto plan = BuildWindowsX18ExclusivePlan(exclusive)) {
                bool pre_buffer = false;
                const auto ret = AddRelocations(pre_buffer);
                if (pre_buffer) {
                    WriteWindowsX18ExclusiveTrampoline(ret, *plan, c_pre);
                } else {
                    WriteWindowsX18ExclusiveTrampoline(ret, *plan, c);
                }
                continue;
            }
#endif
            curr_patch->m_exclusives.push_back(i);
        }

#if defined(_WIN32)
        // Native register substitution for linear x18 instructions; the Dynarmic single-step direct
        // trampoline below remains the fallback for forms that cannot be validated.
        if (const auto plan = BuildWindowsX18SubstitutionPlan(inst)) {
            bool pre_buffer = false;
            const auto ret = AddRelocations(pre_buffer);
            if (pre_buffer) {
                WriteWindowsX18SubstitutionTrampoline(ret, inst, *plan, c_pre);
            } else {
                WriteWindowsX18SubstitutionTrampoline(ret, inst, *plan, c);
            }
            continue;
        }

        // P3A: keep decoder-supported linear/non-SP ordinary x18 inside the current guest epoch.
        // Branch/terminal/SP-changing forms remain on the established BRK fallback.
        if (X18Fallback::CanUseDirectTrampoline(inst)) {
            const uintptr_t this_offset = i * sizeof(u32);
            bool pre_buffer = false;
            const auto ret = AddRelocations(pre_buffer);
            if (pre_buffer) {
                WriteWindowsX18DirectTrampoline(this_offset, ret, inst, c_pre,
                                                m_save_context_pre, m_load_context_pre);
            } else {
                WriteWindowsX18DirectTrampoline(this_offset, ret, inst, c,
                                                m_save_context, m_load_context);
            }
            continue;
        }
#endif
    }

#if SNAPRYU_DIAGNOSTICS
#if defined(_WIN32)
    // One line per patched module, also when it has no x18 atomic: the line proves NCE patching ran.
    LOG_INFO(Render_Vulkan, "X18_ATOMIC_PATCH exclusive={} lse={} sp_base={} pair={} text_bytes={:#x}",
             x18_atomic_counts.exclusive, x18_atomic_counts.lse, x18_atomic_counts.sp_base,
             x18_atomic_counts.pair,
             code.size);
#endif
#endif

    // Determine patching mode for the final relocation step
    total_program_size += image_size;
    if (use_split) {
        this->mode = PatchMode::Split;
    } else {
        this->mode = image_size > MaxRelativeBranch ? PatchMode::PreText : PatchMode::PostData;
    }
    return true;
}

bool Patcher::RelocateAndCopy(Common::ProcessAddress load_base, const Kernel::CodeSet::Segment& code, std::vector<u8>& program_image, EntryTrampolines* out_trampolines
#if defined(_WIN32)
                              , WindowsPatchRecoveryTable* out_recovery
#endif
                              ) {
    const size_t patch_size = GetSectionSize();
    const size_t pre_patch_size = GetPreSectionSize();

    const size_t image_size = (mode == PatchMode::Split) ? program_image.size() - pre_patch_size : program_image.size();

    // Retrieve text segment data.
    const auto text = std::span{program_image}.subspan(code.offset, code.size);
    const auto text_words =
        std::span<u32>{reinterpret_cast<u32*>(text.data()), text.size() / sizeof(u32)};

    const auto ApplyBranchToPatchRelocation = [&](u32* target, const Relocation& rel) {
        oaknut::CodeGenerator rc{target};
        if (mode == PatchMode::PreText) {
            rc.B(rel.patch_offset - patch_size - rel.module_offset);
        } else {
            rc.B(total_program_size - rel.module_offset + rel.patch_offset);
        }
    };

    const auto ApplyBranchToPatchRelocationPre = [&](u32* target, const Relocation& rel) {
        oaknut::CodeGenerator rc{target};
        rc.B(static_cast<ptrdiff_t>(rel.patch_offset) - static_cast<ptrdiff_t>(pre_patch_size) - static_cast<ptrdiff_t>(rel.module_offset));
    };

    const auto ApplyBranchToPatchRelocationPostSplit = [&](u32* target, const Relocation& rel) {
        oaknut::CodeGenerator rc{target};
        rc.B(static_cast<ptrdiff_t>(image_size) + static_cast<ptrdiff_t>(rel.patch_offset) - static_cast<ptrdiff_t>(rel.module_offset));
    };

    const auto ApplyBranchToModuleRelocation = [&](u32* target, const Relocation& rel) {
        oaknut::CodeGenerator rc{target};
        if (mode == PatchMode::PreText) {
            rc.B(patch_size - rel.patch_offset + rel.module_offset);
        } else {
            rc.B(rel.module_offset - total_program_size - rel.patch_offset);
        }
    };

    const auto ApplyBranchToModuleRelocationPre = [&](u32* target, const Relocation& rel) {
        oaknut::CodeGenerator rc{target};
        rc.B(static_cast<ptrdiff_t>(pre_patch_size) + static_cast<ptrdiff_t>(rel.module_offset) - static_cast<ptrdiff_t>(rel.patch_offset));
    };

     const auto ApplyBranchToModuleRelocationPostSplit = [&](u32* target, const Relocation& rel) {
        oaknut::CodeGenerator rc{target};
        rc.B(static_cast<ptrdiff_t>(rel.module_offset) - static_cast<ptrdiff_t>(image_size) - static_cast<ptrdiff_t>(rel.patch_offset));
    };

    const auto RebasePatch = [&](ptrdiff_t patch_offset) {
        if (mode == PatchMode::PreText) {
            return GetInteger(load_base) + patch_offset;
        } else {
            return GetInteger(load_base) + total_program_size + patch_offset;
        }
    };

    const auto RebasePc = [&](uintptr_t module_offset) {
        if (mode == PatchMode::PreText) {
            return GetInteger(load_base) + patch_size + module_offset;
        }
        if (mode == PatchMode::Split) {
            return GetInteger(load_base) + pre_patch_size + module_offset;
        }

        return GetInteger(load_base) + module_offset;
    };

    // We are now ready to relocate!
    auto& patch = modules[m_relocate_module_index++];

    if (mode == PatchMode::Split) {
        for (const Relocation& rel : patch.m_branch_to_pre_patch_relocations) {
            ApplyBranchToPatchRelocationPre(text_words.data() + rel.module_offset / sizeof(u32), rel);
        }
        LOG_DEBUG(Core_ARM, "applied Pre: {}", patch.m_branch_to_pre_patch_relocations.size());

        for (const Relocation& rel : patch.m_branch_to_patch_relocations) {
            ApplyBranchToPatchRelocationPostSplit(text_words.data() + rel.module_offset / sizeof(u32), rel);
        }
        LOG_DEBUG(Core_ARM, "applied Post: {}", patch.m_branch_to_patch_relocations.size());

        for (const Relocation& rel : patch.m_branch_to_module_relocations_pre) {
            ApplyBranchToModuleRelocationPre(m_patch_instructions_pre.data() + rel.patch_offset / sizeof(u32), rel);
        }
        LOG_DEBUG(Core_ARM, "aplied Pre-module {}", patch.m_branch_to_module_relocations_pre.size());

        for (const Relocation& rel : patch.m_branch_to_module_relocations) {
            ApplyBranchToModuleRelocationPostSplit(m_patch_instructions.data() + rel.patch_offset / sizeof(u32), rel);
        }
        LOG_DEBUG(Core_ARM, "applied Post-module {}", patch.m_branch_to_module_relocations.size());

        // Pre
        for (const Relocation& rel : patch.m_write_module_pc_relocations_pre) {
            oaknut::CodeGenerator rc{m_patch_instructions_pre.data() + rel.patch_offset / sizeof(u32)};
            rc.dx(RebasePc(rel.module_offset));
        }
        // Post
        for (const Relocation& rel : patch.m_write_module_pc_relocations) {
            oaknut::CodeGenerator rc{m_patch_instructions.data() + rel.patch_offset / sizeof(u32)};
            rc.dx(RebasePc(rel.module_offset));
        }

        // Trampolines (split pre + post)
        for (const Trampoline& rel : patch.m_trampolines_pre) {
            out_trampolines->insert({RebasePc(rel.module_offset),
                                     GetInteger(load_base) + rel.patch_offset});
        }
        for (const Trampoline& rel : patch.m_trampolines) {
            out_trampolines->insert({RebasePc(rel.module_offset),
                                     GetInteger(load_base) + pre_patch_size + image_size + rel.patch_offset});
        }

        if (!m_patch_instructions_pre.empty()) {
            u32 insn = m_patch_instructions_pre[0];
            if ((insn & 0xFC000000) == 0x14000000) {
                s32 imm26 = insn & 0x3FFFFFF;
                // Sign extend
                if (imm26 & 0x2000000) imm26 |= 0xFC000000;
            }
        }
    } else {
        for (const Relocation& rel : patch.m_branch_to_patch_relocations) {
            ApplyBranchToPatchRelocation(text_words.data() + rel.module_offset / sizeof(u32), rel);
        }
        for (const Relocation& rel : patch.m_branch_to_module_relocations) {
            ApplyBranchToModuleRelocation(m_patch_instructions.data() + rel.patch_offset / sizeof(u32),
                                          rel);
        }

        // Rewrite PC constants
        for (const Relocation& rel : patch.m_write_module_pc_relocations) {
            oaknut::CodeGenerator rc{m_patch_instructions.data() + rel.patch_offset / sizeof(u32)};
            rc.dx(RebasePc(rel.module_offset));
        }
    }

    if (mode != PatchMode::Split) {
        for (const Trampoline& rel : patch.m_trampolines) {
            out_trampolines->insert({RebasePc(rel.module_offset), RebasePatch(rel.patch_offset)});
        }
    }

#if defined(_WIN32)
    // Export recipes only after branch relocation, and before total_program_size is decremented.
    // Split pre/post have different bases; ordinary PostData shares a section across modules.
    if (out_recovery != nullptr) {
        for (const auto& pending : patch.m_windows_recovery) {
            auto recipe = pending.recipe;
            const auto& words = pending.pre_buffer ? m_patch_instructions_pre : m_patch_instructions;
            const u64 offset = recipe.begin;
            const u64 size = recipe.end - recipe.begin;
            if (recipe.word_count == 0 || recipe.word_count > recipe.code_words.size() ||
                size != recipe.word_count * sizeof(u32) ||
                offset > words.size() * sizeof(u32) ||
                size > words.size() * sizeof(u32) - offset) {
                continue; // Changed emitter shape stays Unknown until a new recipe is reviewed.
            }
            std::copy_n(words.data() + offset / sizeof(u32), recipe.word_count,
                        recipe.code_words.begin());
            const u64 section_base = mode == PatchMode::Split
                ? GetInteger(load_base) + (pending.pre_buffer ? 0 : pre_patch_size + image_size)
                : RebasePatch(0);
            recipe.begin += section_base;
            recipe.end += section_base;
            recipe.guest_pc = RebasePc(recipe.guest_pc);
            (void)out_recovery->AppendForLoad(std::move(recipe));
        }
    }
#endif

#if defined(_WIN32)
    // Cross-thread breaks may arrive while generated patch helpers temporarily own guest-stack
    // scratch frames/registers. Record the complete generated-code regions as tagged process
    // metadata so the Windows host can defer architectural capture until execution returns to a
    // real guest instruction. Tagged keys cannot collide with ordinary post-handler lookups.
    if (mode == PatchMode::Split) {
        WindowsPatchCodeMetadata::RegisterRange(*out_trampolines, GetInteger(load_base),
                                                pre_patch_size);
        WindowsPatchCodeMetadata::RegisterRange(
            *out_trampolines, GetInteger(load_base) + pre_patch_size + image_size, patch_size);
    } else {
        WindowsPatchCodeMetadata::RegisterRange(*out_trampolines, RebasePatch(0), patch_size);
    }
#endif

    // Cortex-A57 seems to treat all exclusives as ordered, but newer processors do not.
    // Convert to ordered to preserve this assumption.
    for (const ModuleTextAddress i : patch.m_exclusives) {
        auto exclusive = Exclusive{text_words[i]};
        text_words[i] = exclusive.AsOrdered();
    }

    // Remove the patched module size from the total. This is done so total_program_size
    // always represents the distance from the currently patched module to the patch section.
    total_program_size -= image_size;

    // Only copy to the program image of the last module
    if (m_relocate_module_index == modules.size()) {
        if (this->mode == PatchMode::PreText) {
            ASSERT(image_size == total_program_size);
            std::memcpy(program_image.data(), m_patch_instructions.data(),
                        m_patch_instructions.size() * sizeof(u32));
        } else if (this->mode == PatchMode::Split) {
            const size_t current_size = program_image.size();
            program_image.resize(current_size + patch_size);
            // Copy pre-patch buffer to the beginning
            std::memcpy(program_image.data(), m_patch_instructions_pre.data(),
                        m_patch_instructions_pre.size() * sizeof(u32));
            // Same for post-patch buffer to the end
            std::memcpy(program_image.data() + current_size, m_patch_instructions.data(),
                        m_patch_instructions.size() * sizeof(u32));
        } else {
            program_image.resize(image_size + patch_size);
            std::memcpy(program_image.data() + image_size, m_patch_instructions.data(),
                        m_patch_instructions.size() * sizeof(u32));
        }
        return true;
    }

    return false;
}

size_t Patcher::GetSectionSize() const noexcept {
    return Common::AlignUp(m_patch_instructions.size() * sizeof(u32), Core::Memory::YUZU_PAGESIZE);
}

size_t Patcher::GetPreSectionSize() const noexcept {
    return Common::AlignUp(m_patch_instructions_pre.size() * sizeof(u32), Core::Memory::YUZU_PAGESIZE);
}

void Patcher::WriteLoadContext(oaknut::VectorCodeGenerator& cg) {
    // This function was called, which modifies X30, so use that as a scratch register.
    // SP contains the guest X30, so save our return X30 to SP + 8, since we have allocated 16 bytes
    // of stack.
    cg.STR(X30, SP, 8);
#if defined(_WIN32)
    WriteWindowsCurrentNceParametersLookup(cg, X30);
#else
    cg.MRS(X30, oaknut::SystemReg::TPIDR_EL0);
#endif
    cg.LDR(X30, X30, offsetof(NativeExecutionParameters, native_context));

    // Load system registers.
    cg.LDR(W0, X30, offsetof(GuestContext, fpsr));
    cg.MSR(oaknut::SystemReg::FPSR, X0);
    cg.LDR(W0, X30, offsetof(GuestContext, fpcr));
    cg.MSR(oaknut::SystemReg::FPCR, X0);
    cg.LDR(W0, X30, offsetof(GuestContext, nzcv));
    cg.MSR(oaknut::SystemReg::NZCV, X0);

    // Load all vector registers.
    static constexpr size_t VEC_OFF = offsetof(GuestContext, vector_registers);
    for (int i = 0; i <= 30; i += 2) {
        cg.LDP(oaknut::QReg{i}, oaknut::QReg{i + 1}, X30, VEC_OFF + 16 * i);
    }

#if defined(_WIN32)
    // Restore guest GPRs without assigning architectural x18 to the Windows TEB register.
    for (int i = 0; i <= 16; i += 2) {
        cg.LDP(oaknut::XReg{i}, oaknut::XReg{i + 1}, X30, 8 * i);
    }
    for (int i = 19; i <= 27; i += 2) {
        cg.LDP(oaknut::XReg{i}, oaknut::XReg{i + 1}, X30, 8 * i);
    }
    cg.LDR(X29, X30, 8 * 29);
#else
    // Load all general-purpose registers except X30.
    for (int i = 0; i <= 28; i += 2) {
        cg.LDP(oaknut::XReg{i}, oaknut::XReg{i + 1}, X30, 8 * i);
    }
#endif

    // Reload our return X30 from the stack and return.
    // The patch code will reload the guest X30 for us.
    cg.LDR(X30, SP, 8);
    cg.RET();
}

void Patcher::WriteSaveContext(oaknut::VectorCodeGenerator& cg) {
    // This function was called, which modifies X30, so use that as a scratch register.
    // SP contains the guest X30, so save our X30 to SP + 8, since we have allocated 16 bytes of
    // stack.
    cg.STR(X30, SP, 8);
#if defined(_WIN32)
    WriteWindowsCurrentNceParametersLookup(cg, X30);
#else
    cg.MRS(X30, oaknut::SystemReg::TPIDR_EL0);
#endif
    cg.LDR(X30, X30, offsetof(NativeExecutionParameters, native_context));

#if defined(_WIN32)
    // Save guest GPRs without copying the physical Windows x18/TEB value into virtual guest x18.
    for (int i = 0; i <= 16; i += 2) {
        cg.STP(oaknut::XReg{i}, oaknut::XReg{i + 1}, X30, 8 * i);
    }
    for (int i = 19; i <= 27; i += 2) {
        cg.STP(oaknut::XReg{i}, oaknut::XReg{i + 1}, X30, 8 * i);
    }
    cg.STR(X29, X30, 8 * 29);
#else
    // Store all general-purpose registers except X30.
    for (int i = 0; i <= 28; i += 2) {
        cg.STP(oaknut::XReg{i}, oaknut::XReg{i + 1}, X30, 8 * i);
    }
#endif

    // Store all vector registers.
    static constexpr size_t VEC_OFF = offsetof(GuestContext, vector_registers);
    for (int i = 0; i <= 30; i += 2) {
        cg.STP(oaknut::QReg{i}, oaknut::QReg{i + 1}, X30, VEC_OFF + 16 * i);
    }

    // Store guest system registers, X30 and SP, using X0 as a scratch register.
    cg.STR(X0, SP, PRE_INDEXED, -16);
    cg.LDR(X0, SP, 16);
    cg.STR(X0, X30, 8 * 30);
    cg.ADD(X0, SP, 32);
    cg.STR(X0, X30, offsetof(GuestContext, sp));
    cg.MRS(X0, oaknut::SystemReg::FPSR);
    cg.STR(W0, X30, offsetof(GuestContext, fpsr));
    cg.MRS(X0, oaknut::SystemReg::FPCR);
    cg.STR(W0, X30, offsetof(GuestContext, fpcr));
    cg.MRS(X0, oaknut::SystemReg::NZCV);
    cg.STR(W0, X30, offsetof(GuestContext, nzcv));
#if defined(_WIN32)
    // Keep the canonical architectural NZCV bits coherent across the Windows SVC round trip.
    // Windows native re-entry and scheduler context exchange consume GuestContext::pstate, while
    // x18 fallback consumes GuestContext::nzcv. Preserve non-NZCV PSTATE bits and mirror the live
    // flags into both fields at the producer seam.
    cg.LDR(W1, X30, offsetof(GuestContext, pstate));
    cg.AND(W1, W1, ~NzcvMask);
    cg.AND(W0, W0, NzcvMask);
    cg.ORR(W1, W1, W0);
    cg.STR(W1, X30, offsetof(GuestContext, pstate));
#endif
    cg.LDR(X0, SP, POST_INDEXED, 16);

    // Reload our return X30 from the stack, and return.
    cg.LDR(X30, SP, 8);
    cg.RET();
}

void Patcher::WriteSvcTrampoline(ModuleDestLabel module_dest, u32 svc_id,
                                 oaknut::VectorCodeGenerator& cg, oaknut::Label& save_ctx,
                                 oaknut::Label& load_ctx) {
    // Determine if we're writing to the pre-patch buffer.
    const bool is_pre = (&cg == &c_pre);
#if defined(_WIN32)
    // Windows re-entry uses WindowsNceEnterGuest instead of the Linux load-context trampoline.
    (void)load_ctx;
#endif

    // We are about to start saving state, so we need to lock the context.
    this->LockContext(cg);

    // Store guest X30 to the stack. Then, save the context and restore the stack.
    // This will save all registers except PC, but we know PC at patch time.
    cg.STR(X30, SP, PRE_INDEXED, -16);
    cg.BL(save_ctx);
    cg.LDR(X30, SP, POST_INDEXED, 16);

    // Now that we've saved all guest registers, ordinary scratch registers are available.
    oaknut::Label pc_after_svc;
#if defined(_WIN32)
    WriteWindowsCurrentNceParametersLookup(cg, X1);
#else
    cg.MRS(X1, oaknut::SystemReg::TPIDR_EL0);
#endif
    cg.LDR(X1, X1, offsetof(NativeExecutionParameters, native_context));
    cg.LDR(X2, pc_after_svc);
    cg.STR(X2, X1, offsetof(GuestContext, pc));

    // Store SVC number to execute when we return.
    cg.MOV(X2, svc_id);
    cg.STR(W2, X1, offsetof(GuestContext, svc));

    // We are calling a SVC. Clear esr_el1 and return it.
    static_assert(std::is_same_v<std::underlying_type_t<HaltReason>, u64>);
    oaknut::Label retry;
    cg.ADD(X2, X1, offsetof(GuestContext, esr_el1));
    cg.l(retry);
    cg.LDAXR(X0, X2);
    cg.STLXR(W3, XZR, X2);
    cg.CBNZ(W3, retry);

    // Add the calling-SVC flag. X0 is the host return value.
    cg.ORR(X0, X0, static_cast<u64>(HaltReason::SupervisorCall));

    // Offset the GuestContext pointer to the HostContext member.
    // STP/LDP have limited ranges, so all offsets below are from HostContext.
    cg.ADD(X1, X1, offsetof(GuestContext, host_ctx));

#if defined(_WIN32)
    // D2: x1 points at GuestContext::host_ctx. Restore the complete host TEB interval before the
    // host SP becomes active. The pair store and MOV SP form a leaf transition: no call/probe or
    // stack access is permitted between them.
    static_assert(offsetof(GuestContext, windows_host_stack_base) >
                  offsetof(GuestContext, host_ctx));
    constexpr size_t HostStackBaseFromHostContext =
        offsetof(GuestContext, windows_host_stack_base) - offsetof(GuestContext, host_ctx);
    constexpr size_t HostStackLimitFromHostContext =
        offsetof(GuestContext, windows_host_stack_limit) - offsetof(GuestContext, host_ctx);
    constexpr size_t WindowsTebStackBaseOffset = 0x08;
    cg.LDR(W5, X1, offsetof(HostContext, host_fpcr));
    cg.MSR(oaknut::SystemReg::FPCR, X5);
    cg.LDR(X3, X1, HostStackBaseFromHostContext);
    cg.LDR(X4, X1, HostStackLimitFromHostContext);
    cg.LDR(X2, X1, offsetof(HostContext, host_sp));
    cg.STP(X3, X4, X18, WindowsTebStackBaseOffset);
    cg.MOV(SP, X2);
#else
    // Reload host TPIDR_EL0 and SP on the Linux/Android path.
    static_assert(offsetof(HostContext, host_sp) + 8 == offsetof(HostContext, host_tpidr_el0));
    cg.LDP(X2, X3, X1, offsetof(HostContext, host_sp));
    cg.MOV(SP, X2);
    cg.MSR(oaknut::SystemReg::TPIDR_EL0, X3);
#endif

    // Load callee-saved host registers and return to host.
    static constexpr size_t HOST_REGS_OFF = offsetof(HostContext, host_saved_regs);
    static constexpr size_t HOST_VREGS_OFF = offsetof(HostContext, host_saved_vregs);
    cg.LDP(X19, X20, X1, HOST_REGS_OFF);
    cg.LDP(X21, X22, X1, HOST_REGS_OFF + 2 * sizeof(u64));
    cg.LDP(X23, X24, X1, HOST_REGS_OFF + 4 * sizeof(u64));
    cg.LDP(X25, X26, X1, HOST_REGS_OFF + 6 * sizeof(u64));
    cg.LDP(X27, X28, X1, HOST_REGS_OFF + 8 * sizeof(u64));
    cg.LDP(X29, X30, X1, HOST_REGS_OFF + 10 * sizeof(u64));
    cg.LDP(Q8, Q9, X1, HOST_VREGS_OFF);
    cg.LDP(Q10, Q11, X1, HOST_VREGS_OFF + 2 * sizeof(u128));
    cg.LDP(Q12, Q13, X1, HOST_VREGS_OFF + 4 * sizeof(u128));
    cg.LDP(Q14, Q15, X1, HOST_VREGS_OFF + 6 * sizeof(u128));
    cg.RET();

    // This address is the post-SVC entry point selected by RunThread when GuestContext::pc equals
    // the instruction after the emulated SVC.
    if (is_pre) {
        curr_patch->m_trampolines_pre.push_back({cg.offset(), module_dest});
    } else {
        curr_patch->m_trampolines.push_back({cg.offset(), module_dest});
    }

#if defined(_WIN32)
    // WindowsNceEnterGuest has already restored guest x0-x15, x19-x30, SIMD/status and guest SP.
    // It intentionally carries GuestContext in x17 and the selected trampoline in x16. Unlock the
    // scheduler-owned NCE context, restore the two guest scratch registers, then use a direct branch
    // so no architectural guest register is consumed as a PC carrier.
    this->UnlockContextFromGuestContext(cg);
    cg.LDR(X16, X17, offsetof(GuestContext, cpu_registers) + sizeof(u64) * 16);
    cg.LDR(X17, X17, offsetof(GuestContext, cpu_registers) + sizeof(u64) * 17);

    if (is_pre)
        this->BranchToModulePre(module_dest);
    else
        this->BranchToModule(module_dest);
#else
    // Linux/Android host called this location. Save the return address so the existing assembly
    // entry path can unwind the stack properly when jumping back.
    cg.MRS(X2, oaknut::SystemReg::TPIDR_EL0);
    cg.LDR(X2, X2, offsetof(NativeExecutionParameters, native_context));
    cg.ADD(X0, X2, offsetof(GuestContext, host_ctx));
    cg.STR(X30, X0, offsetof(HostContext, host_saved_regs) + 11 * sizeof(u64));

    // Reload all guest registers except X30 and PC.
    // The function also expects 16 bytes of stack already allocated.
    cg.STR(X30, SP, PRE_INDEXED, -16);
    cg.BL(load_ctx);
    cg.LDR(X30, SP, POST_INDEXED, 16);

    // Use X1 as a scratch register to restore X30.
    cg.STR(X1, SP, PRE_INDEXED, -16);
    cg.MRS(X1, oaknut::SystemReg::TPIDR_EL0);
    cg.LDR(X1, X1, offsetof(NativeExecutionParameters, native_context));
    cg.LDR(X30, X1, offsetof(GuestContext, cpu_registers) + sizeof(u64) * 30);
    cg.LDR(X1, SP, POST_INDEXED, 16);

    // Unlock the context.
    this->UnlockContext(cg);

    // Jump back to the instruction after the emulated SVC.
    if (is_pre)
        this->BranchToModulePre(module_dest);
    else
        this->BranchToModule(module_dest);
#endif

    // Store PC after call.
    cg.l(pc_after_svc);
    if (is_pre)
        this->WriteModulePcPre(module_dest);
    else
        this->WriteModulePc(module_dest);
}

void Patcher::WriteMrsHandler(ModuleDestLabel module_dest, oaknut::XReg dest_reg,
                              oaknut::SystemReg src_reg, oaknut::VectorCodeGenerator& cg) {
#if defined(_WIN32)
    if (dest_reg.index() == GuestX18Register) {
        // Architectural x18 is virtual state on Windows. Never write the TEB platform register.
        cg.STP(X0, X1, SP, PRE_INDEXED, -16);
        WriteWindowsCurrentNceParametersLookup(cg, X0);
        if (src_reg == oaknut::SystemReg::TPIDRRO_EL0) {
            cg.LDR(X1, X0, offsetof(NativeExecutionParameters, tpidrro_el0));
        } else {
            cg.LDR(X1, X0, offsetof(NativeExecutionParameters, tpidr_el0));
        }
        cg.LDR(X0, X0, offsetof(NativeExecutionParameters, native_context));
        cg.STR(X1, X0, offsetof(GuestContext, cpu_registers) + sizeof(u64) * GuestX18Register);
        cg.LDP(X0, X1, SP, POST_INDEXED, 16);
    } else if (dest_reg.index() != 31) {
        // R2b-B2x2: one label per instruction. A recipe is recorded only for the one-load TEB
        // slot lookup and only if every label is at index * 4 (WindowsMrsTpidrStepTable).
        const bool is_ro = src_reg == oaknut::SystemReg::TPIDRRO_EL0;
        const u32 field = is_ro ? offsetof(NativeExecutionParameters, tpidrro_el0)
                                : offsetof(NativeExecutionParameters, tpidr_el0);
        std::array<oaknut::Label, WindowsMrsTpidrSteps + 1> labels;
        std::size_t label_index = 0;
        const auto bind = [&] { cg.l(labels[label_index++]); };
        bind();
        WriteWindowsCurrentNceParametersLookup(cg, dest_reg);
        bind();
        cg.LDR(dest_reg, dest_reg, field);
        bind();
        if (&cg == &c_pre)
            this->BranchToModulePre(module_dest);
        else
            this->BranchToModule(module_dest);
        bind();
        if (!CurrentNceContext::HasTebSlot()) {
            return;
        }
        for (std::size_t i = 0; i <= WindowsMrsTpidrSteps; ++i) {
            if (static_cast<std::size_t>(labels[i].offset() - labels[0].offset()) !=
                i * sizeof(u32)) {
                return; // A different emitted shape stays Unknown.
            }
        }
        const u32 d = static_cast<u32>(dest_reg.index());
        WindowsPatchRecoveryRecipe recipe{};
        recipe.kind = WindowsPatchRecoveryKind::MrsTpidrV1;
        recipe.word_count = static_cast<u8>(WindowsMrsTpidrSteps);
        const auto steps = WindowsMrsTpidrStepTable();
        std::copy(steps.begin(), steps.end(), recipe.steps.begin());
        recipe.guest_pc = module_dest - sizeof(u32); // Relocated with this module later.
        recipe.original_instruction = (is_ro ? 0xD53BD060U : 0xD53BD040U) | d;
        recipe.params_tls_offset = CurrentNceContext::TebSlotOffset();
        recipe.scratch_tls_offset = CurrentNceContext::ScratchSlotOffset(0);
        recipe.spill_context_offset = offsetof(NativeExecutionParameters, x18_scratch);
        recipe.spill_value_offset = recipe.spill_context_offset + sizeof(u64);
        recipe.native_context_offset = offsetof(NativeExecutionParameters, native_context);
        recipe.guest_x18_offset =
            offsetof(GuestContext, cpu_registers) + sizeof(u64) * GuestX18Register;
        recipe.params_field_offset = field;
        recipe.value_reg = static_cast<u8>(d);
        recipe.context_reg = static_cast<u8>(d);
        recipe.begin = static_cast<u64>(labels[0].offset());
        recipe.end = static_cast<u64>(labels[WindowsMrsTpidrSteps].offset());
        recipe.tail_offset = recipe.steps[WindowsMrsTpidrSteps - 1].offset;
        recipe.tail_bytes = sizeof(u32);
        curr_patch->m_windows_recovery.push_back({recipe, &cg == &c_pre});
        return;
    }
#else
    // Retrieve emulated TLS register from GuestContext.
    cg.MRS(dest_reg, oaknut::SystemReg::TPIDR_EL0);
    if (src_reg == oaknut::SystemReg::TPIDRRO_EL0) {
        cg.LDR(dest_reg, dest_reg, offsetof(NativeExecutionParameters, tpidrro_el0));
    } else {
        cg.LDR(dest_reg, dest_reg, offsetof(NativeExecutionParameters, tpidr_el0));
    }
#endif

    // Jump back to the instruction after the emulated MRS.
    if (&cg == &c_pre)
        this->BranchToModulePre(module_dest);
    else
        this->BranchToModule(module_dest);
}

void Patcher::WriteMsrHandler(ModuleDestLabel module_dest, oaknut::XReg src_reg, oaknut::VectorCodeGenerator& cg) {
    const auto scratch_reg = src_reg.index() == 0 ? X1 : X0;
    cg.STR(scratch_reg, SP, PRE_INDEXED, -16);

#if defined(_WIN32)
    WriteWindowsCurrentNceParametersLookup(cg, scratch_reg);
    if (src_reg.index() == GuestX18Register) {
        // Source x18 is the virtual guest value, never the live physical Windows TEB register.
        const auto value_reg = scratch_reg.index() == 0 ? X1 : X0;
        cg.STR(value_reg, SP, PRE_INDEXED, -16);
        cg.LDR(value_reg, scratch_reg, offsetof(NativeExecutionParameters, native_context));
        cg.LDR(value_reg, value_reg,
               offsetof(GuestContext, cpu_registers) + sizeof(u64) * GuestX18Register);
        cg.STR(value_reg, scratch_reg, offsetof(NativeExecutionParameters, tpidr_el0));
        cg.LDR(value_reg, SP, POST_INDEXED, 16);
    } else {
        cg.STR(src_reg, scratch_reg, offsetof(NativeExecutionParameters, tpidr_el0));
    }
#else
    // Save guest value to NativeExecutionParameters::tpidr_el0.
    cg.MRS(scratch_reg, oaknut::SystemReg::TPIDR_EL0);
    cg.STR(src_reg, scratch_reg, offsetof(NativeExecutionParameters, tpidr_el0));
#endif

    // Restore scratch register.
    cg.LDR(scratch_reg, SP, POST_INDEXED, 16);

    // Jump back to the instruction after the emulated MSR.
    if (&cg == &c_pre)
        this->BranchToModulePre(module_dest);
    else
        this->BranchToModule(module_dest);
}

void Patcher::WriteCtrEl0Handler(ModuleDestLabel module_dest, oaknut::XReg dest_reg,
                                  oaknut::VectorCodeGenerator& cg) {
    // Keep NCE fallback state coherent with Dynarmic::A64::UserConfig::ctr_el0.
    static constexpr u64 GuestCtrEl0 = 0x8444C004;

#if defined(_WIN32)
    if (dest_reg.index() == GuestX18Register) {
        // Architectural x18 is virtual guest state on Windows; never overwrite the live TEB.
        cg.STP(X0, X1, SP, PRE_INDEXED, -16);
        WriteWindowsCurrentNceParametersLookup(cg, X0);
        cg.LDR(X0, X0, offsetof(NativeExecutionParameters, native_context));
        cg.MOV(X1, GuestCtrEl0);
        cg.STR(X1, X0,
               offsetof(GuestContext, cpu_registers) + sizeof(u64) * GuestX18Register);
        cg.LDP(X0, X1, SP, POST_INDEXED, 16);
    } else if (dest_reg.index() != 31) {
        cg.MOV(dest_reg, GuestCtrEl0);
    }
#else
    if (dest_reg.index() != 31) {
        cg.MOV(dest_reg, GuestCtrEl0);
    }
#endif

    if (&cg == &c_pre)
        this->BranchToModulePre(module_dest);
    else
        this->BranchToModule(module_dest);
}

void Patcher::WriteCntfrqHandler(ModuleDestLabel module_dest, oaknut::XReg dest_reg, oaknut::VectorCodeGenerator& cg) {
#if defined(_WIN32)
    if (dest_reg.index() == GuestX18Register) {
        // P3D: X18 is the Windows TEB platform register. CNTFRQ_EL0 -> X18 must update the
        // virtual guest register in GuestContext instead of clobbering the live TEB pointer.
        cg.STP(X0, X1, SP, PRE_INDEXED, -16);
        WriteWindowsCurrentNceParametersLookup(cg, X0);
        cg.LDR(X0, X0, offsetof(NativeExecutionParameters, native_context));
        cg.MOV(X1, Common::WallClock::CNTFRQ);
        cg.STR(X1, X0,
               offsetof(GuestContext, cpu_registers) + sizeof(u64) * GuestX18Register);
        cg.LDP(X0, X1, SP, POST_INDEXED, 16);
    } else {
        cg.MOV(dest_reg, Common::WallClock::CNTFRQ);
    }
#else
    cg.MOV(dest_reg, Common::WallClock::CNTFRQ);
#endif

    // Jump back to the instruction after the emulated MRS.
    if (&cg == &c_pre)
        this->BranchToModulePre(module_dest);
    else
        this->BranchToModule(module_dest);
}

void Patcher::WriteCntpctHandler(ModuleDestLabel module_dest, oaknut::XReg dest_reg, oaknut::VectorCodeGenerator& cg) {
#if defined(HAS_NCE)
    static Common::WallClock clock(false, 1);
#else
    static Common::WallClock clock(true, 1);
#endif
    const auto factor = clock.GetGuestCNTFRQFactor();
    const auto raw_factor = std::bit_cast<std::array<u64, 2>>(factor);

    oaknut::Label factorlo;
    oaknut::Label factorhi;

#if defined(_WIN32)
    if (dest_reg.index() == GuestX18Register) {
        // P3D: compute the guest counter in ordinary scratch registers, then commit it to the
        // virtual guest x18 slot. Physical x18 must remain the Windows TEB pointer throughout.
        cg.STP(X0, X1, SP, PRE_INDEXED, -16);
        cg.STP(X2, X3, SP, PRE_INDEXED, -16);

        cg.MRS(X0, oaknut::SystemReg::CNTVCT_EL0);
        cg.LDR(X1, factorlo);
        cg.LDR(X3, factorhi);
        cg.UMULH(X1, X0, X1);
        cg.MADD(X0, X0, X3, X1);

        WriteWindowsCurrentNceParametersLookup(cg, X2);
        cg.LDR(X2, X2, offsetof(NativeExecutionParameters, native_context));
        cg.STR(X0, X2,
               offsetof(GuestContext, cpu_registers) + sizeof(u64) * GuestX18Register);

        cg.LDP(X2, X3, SP, POST_INDEXED, 16);
        cg.LDP(X0, X1, SP, POST_INDEXED, 16);
    } else
#endif
    {
        const auto use_x2_x3 = dest_reg.index() == 0 || dest_reg.index() == 1;
        oaknut::XReg scratch0 = use_x2_x3 ? X2 : X0;
        oaknut::XReg scratch1 = use_x2_x3 ? X3 : X1;

        cg.STP(scratch0, scratch1, SP, PRE_INDEXED, -16);
        cg.MRS(dest_reg, oaknut::SystemReg::CNTVCT_EL0);
        cg.LDR(scratch0, factorlo);
        cg.LDR(scratch1, factorhi);
        cg.UMULH(scratch0, dest_reg, scratch0);
        cg.MADD(dest_reg, dest_reg, scratch1, scratch0);
        cg.LDP(scratch0, scratch1, SP, POST_INDEXED, 16);
    }

    if (&cg == &c_pre)
        this->BranchToModulePre(module_dest);
    else
        this->BranchToModule(module_dest);

    cg.l(factorlo);
    cg.dx(raw_factor[0]);
    cg.l(factorhi);
    cg.dx(raw_factor[1]);
}

void Patcher::LockContext(oaknut::VectorCodeGenerator& cg) {
    oaknut::Label retry;

    // Save scratches.
    cg.STP(X0, X1, SP, PRE_INDEXED, -16);

    // Reload lock pointer.
    cg.l(retry);
    cg.CLREX();
#if defined(_WIN32)
    WriteWindowsCurrentNceParametersLookup(cg, X0);
#else
    cg.MRS(X0, oaknut::SystemReg::TPIDR_EL0);
#endif
    cg.ADD(X0, X0, offsetof(NativeExecutionParameters, lock));

    static_assert(SpinLockLocked == 0);

    // Load-linked with acquire ordering.
    cg.LDAXR(W1, X0);

    // If the value was SpinLockLocked, clear monitor and retry.
    cg.CBZ(W1, retry);

    // Store-conditional SpinLockLocked with relaxed ordering.
    cg.STXR(W1, WZR, X0);

    // If we failed to store, retry.
    cg.CBNZ(W1, retry);

    // We succeeded! Reload scratches.
    cg.LDP(X0, X1, SP, POST_INDEXED, 16);
}

void Patcher::UnlockContext(oaknut::VectorCodeGenerator& cg) {
    // Save scratches.
    cg.STP(X0, X1, SP, PRE_INDEXED, -16);

    // Load lock pointer.
#if defined(_WIN32)
    WriteWindowsCurrentNceParametersLookup(cg, X0);
#else
    cg.MRS(X0, oaknut::SystemReg::TPIDR_EL0);
#endif
    cg.ADD(X0, X0, offsetof(NativeExecutionParameters, lock));

    // Load SpinLockUnlocked.
    cg.MOV(W1, SpinLockUnlocked);

    // Store value with release ordering.
    cg.STLR(W1, X0);

    // Load scratches.
    cg.LDP(X0, X1, SP, POST_INDEXED, 16);
}

#if defined(_WIN32)
void Patcher::UnlockContextFromGuestContext(oaknut::VectorCodeGenerator& cg) {
    // The Windows post-SVC trampoline enters with its owning GuestContext in X17. Use the
    // NativeExecutionParameters pointer captured by that context's RunThread epoch rather than
    // looking it up through thread-local state after an OS context transition.
    cg.STP(X0, X1, SP, PRE_INDEXED, -16);
    cg.LDR(X0, X17, offsetof(GuestContext, windows_native_execution_parameters));
    cg.ADD(X0, X0, offsetof(NativeExecutionParameters, lock));
    cg.MOV(W1, SpinLockUnlocked);
    cg.STLR(W1, X0);
    cg.LDP(X0, X1, SP, POST_INDEXED, 16);
}
#endif
} // namespace Core::NCE
