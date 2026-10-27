// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/arm/nce/x18_fallback.h"

#include "core/arm/nce/instructions.h"

#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/frontend/A64/a64_types.h"
#include "dynarmic/frontend/A64/translate/a64_translate.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/type.h"
#include "dynarmic/ir/value.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace Core::NCE {

namespace {

constexpr u32 RegisterMask = 0x1Fu;
constexpr u32 X18 = 18;

[[nodiscard]] u32 GetRs(u32 instruction) {
    return (instruction >> 16) & RegisterMask;
}

[[nodiscard]] u32 GetRn(u32 instruction) {
    return (instruction >> 5) & RegisterMask;
}

[[nodiscard]] u32 GetRt(u32 instruction) {
    return instruction & RegisterMask;
}

[[nodiscard]] bool IsX18(u32 reg) {
    return reg == X18;
}

[[nodiscard]] bool PairTouchesX18(u32 first_reg) {
    return first_reg == X18 || first_reg + 1 == X18;
}

[[nodiscard]] bool KnownDecoderDisabledAtomicTouchesX18(u32 instruction) {
    // These masks come directly from the decoder-disabled ARMv8.1 patterns in tools/gendynarm.cpp.
    // Keep this list bounded to the families explicitly deferred to IMP-007.
    const bool casp = (instruction & 0xBFA07C00u) == 0x08207C00u;
    if (casp) {
        return IsX18(GetRn(instruction)) || PairTouchesX18(GetRs(instruction)) ||
               PairTouchesX18(GetRt(instruction));
    }

    const bool scalar_cas =
        (instruction & 0xFFA07C00u) == 0x08A07C00u ||
        (instruction & 0xFFA07C00u) == 0x48A07C00u ||
        (instruction & 0xBFA07C00u) == 0x88A07C00u;
    if (scalar_cas) {
        return IsX18(GetRs(instruction)) || IsX18(GetRn(instruction)) ||
               IsX18(GetRt(instruction));
    }

    const bool atomic_memory_group =
        (instruction & 0xFF200000u) == 0x38200000u ||
        (instruction & 0xFF200000u) == 0x78200000u ||
        (instruction & 0xBF200000u) == 0xB8200000u;
    if (!atomic_memory_group) {
        return false;
    }

    const u32 operation = (instruction >> 10) & 0x3Fu;
    const bool deferred_operation = operation <= 0x20u && (operation & 0x3u) == 0;
    if (!deferred_operation) {
        return false;
    }

    return IsX18(GetRs(instruction)) || IsX18(GetRn(instruction)) ||
           IsX18(GetRt(instruction));
}

[[nodiscard]] bool IsGuestMemoryAccess(const Dynarmic::IR::Inst& inst) {
    using Dynarmic::IR::Opcode;

    switch (inst.GetOpcode()) {
    case Opcode::A64ReadMemory8:
    case Opcode::A64ReadMemory16:
    case Opcode::A64ReadMemory32:
    case Opcode::A64ReadMemory64:
    case Opcode::A64ReadMemory128:
    case Opcode::A64ExclusiveReadMemory8:
    case Opcode::A64ExclusiveReadMemory16:
    case Opcode::A64ExclusiveReadMemory32:
    case Opcode::A64ExclusiveReadMemory64:
    case Opcode::A64ExclusiveReadMemory128:
    case Opcode::A64WriteMemory8:
    case Opcode::A64WriteMemory16:
    case Opcode::A64WriteMemory32:
    case Opcode::A64WriteMemory64:
    case Opcode::A64WriteMemory128:
    case Opcode::A64ExclusiveWriteMemory8:
    case Opcode::A64ExclusiveWriteMemory16:
    case Opcode::A64ExclusiveWriteMemory32:
    case Opcode::A64ExclusiveWriteMemory64:
    case Opcode::A64ExclusiveWriteMemory128:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool IsX18RegisterReference(const Dynarmic::IR::Inst& inst) {
    using Dynarmic::IR::Opcode;

    switch (inst.GetOpcode()) {
    case Opcode::A64GetW:
    case Opcode::A64GetX:
    case Opcode::A64SetW:
    case Opcode::A64SetX:
        return inst.GetArg(0).GetA64RegRef() == Dynarmic::A64::Reg::R18;
    default:
        return false;
    }
}

} // namespace

bool X18Fallback::MayAccessGuestMemory(u32 instruction) {
    const Dynarmic::A64::LocationDescriptor descriptor{0, Dynarmic::FP::FPCR{0}, true};
    Dynarmic::IR::Block block{static_cast<Dynarmic::IR::LocationDescriptor>(descriptor)};

    const bool should_continue =
        Dynarmic::A64::TranslateSingleInstruction(block, descriptor, instruction);

    bool emitted_ir = false;
    for (const auto& inst : block.Instructions()) {
        emitted_ir = true;
        if (IsGuestMemoryAccess(inst)) {
            return true;
        }
    }

    // Unknown/decoder-rejected instructions stay conservative. Valid terminal ordinary
    // instructions (for example BR) emit IR even though should_continue is false.
    return !should_continue && !emitted_ir;
}


bool X18Fallback::IsRegisterSubstitution(u32 original, u32 rewritten, u32 from, u32 to) {
    using Dynarmic::IR::Type;
    const Dynarmic::A64::LocationDescriptor descriptor{0, Dynarmic::FP::FPCR{0}, true};
    Dynarmic::IR::Block block_a{static_cast<Dynarmic::IR::LocationDescriptor>(descriptor)};
    Dynarmic::IR::Block block_b{static_cast<Dynarmic::IR::LocationDescriptor>(descriptor)};
    const bool continue_a =
        Dynarmic::A64::TranslateSingleInstruction(block_a, descriptor, original);
    const bool continue_b =
        Dynarmic::A64::TranslateSingleInstruction(block_b, descriptor, rewritten);
    if (continue_a != continue_b) {
        return false;
    }

    std::vector<const Dynarmic::IR::Inst*> insts_a;
    std::vector<const Dynarmic::IR::Inst*> insts_b;
    for (const auto& inst : block_a.Instructions()) {
        insts_a.push_back(&inst);
    }
    for (const auto& inst : block_b.Instructions()) {
        insts_b.push_back(&inst);
    }
    if (insts_a.empty() || insts_a.size() != insts_b.size()) {
        return false;
    }

    const auto index_of = [](const std::vector<const Dynarmic::IR::Inst*>& list,
                             const Dynarmic::IR::Inst* inst) -> std::ptrdiff_t {
        const auto it = std::find(list.begin(), list.end(), inst);
        return it == list.end() ? -1 : it - list.begin();
    };

    const auto from_reg = static_cast<Dynarmic::A64::Reg>(from);
    const auto to_reg = static_cast<Dynarmic::A64::Reg>(to);
    bool referenced_from = false;

    for (std::size_t k = 0; k < insts_a.size(); ++k) {
        const auto& a = *insts_a[k];
        const auto& b = *insts_b[k];
        if (a.GetOpcode() != b.GetOpcode() || a.NumArgs() != b.NumArgs()) {
            return false;
        }
        for (std::size_t n = 0; n < a.NumArgs(); ++n) {
            const auto va = a.GetArg(n);
            const auto vb = b.GetArg(n);
            if (va.IsEmpty() != vb.IsEmpty()) {
                return false;
            }
            if (va.IsEmpty()) {
                continue;
            }
            if (va.IsImmediate() != vb.IsImmediate()) {
                return false;
            }
            if (!va.IsImmediate()) {
                const auto ia = index_of(insts_a, va.GetInst());
                if (ia < 0 || ia != index_of(insts_b, vb.GetInst())) {
                    return false;
                }
                continue;
            }
            if (va.GetType() != vb.GetType()) {
                return false;
            }
            switch (va.GetType()) {
            case Type::A64Reg: {
                const auto ra = va.GetA64RegRef();
                const auto rb = vb.GetA64RegRef();
                if (ra == to_reg || rb == from_reg) {
                    return false;
                }
                if (ra == from_reg) {
                    referenced_from = true;
                    if (rb != to_reg) {
                        return false;
                    }
                } else if (ra != rb) {
                    return false;
                }
                break;
            }
            case Type::U1:
            case Type::U8:
            case Type::U16:
            case Type::U32:
            case Type::U64:
                if (va.GetImmediateAsU64() != vb.GetImmediateAsU64()) {
                    return false;
                }
                break;
            case Type::A64Vec:
                if (va.GetA64VecRef() != vb.GetA64VecRef()) {
                    return false;
                }
                break;
            case Type::Cond:
                if (va.GetCond() != vb.GetCond()) {
                    return false;
                }
                break;
            case Type::AccType:
                if (va.GetAccType() != vb.GetAccType()) {
                    return false;
                }
                break;
            case Type::NZCVFlags:
                break;
            default:
                // Unknown immediate kinds stay on the conservative direct/BRK paths.
                return false;
            }
        }
    }
    return referenced_from;
}

bool X18Fallback::CanUseDirectTrampoline(u32 instruction) {
    if (ClassifyInstruction(instruction) != X18InstructionClass::SupportedOrdinary) {
        return false;
    }

    const Dynarmic::A64::LocationDescriptor descriptor{0, Dynarmic::FP::FPCR{0}, true};
    Dynarmic::IR::Block block{static_cast<Dynarmic::IR::LocationDescriptor>(descriptor)};
    const bool should_continue =
        Dynarmic::A64::TranslateSingleInstruction(block, descriptor, instruction);

    if (!should_continue) {
        return false;
    }

    bool emitted_ir = false;
    for (const auto& inst : block.Instructions()) {
        emitted_ir = true;
        using Dynarmic::IR::Opcode;
        switch (inst.GetOpcode()) {
        case Opcode::A64SetSP:
        case Opcode::A64SetPC:
        case Opcode::A64CallSupervisor:
        case Opcode::A64ExceptionRaised:
        case Opcode::A64ClearExclusive:
        case Opcode::A64ExclusiveReadMemory8:
        case Opcode::A64ExclusiveReadMemory16:
        case Opcode::A64ExclusiveReadMemory32:
        case Opcode::A64ExclusiveReadMemory64:
        case Opcode::A64ExclusiveReadMemory128:
        case Opcode::A64ExclusiveWriteMemory8:
        case Opcode::A64ExclusiveWriteMemory16:
        case Opcode::A64ExclusiveWriteMemory32:
        case Opcode::A64ExclusiveWriteMemory64:
        case Opcode::A64ExclusiveWriteMemory128:
            return false;
        default:
            break;
        }
    }

    return emitted_ir;
}
X18InstructionClass X18Fallback::ClassifyInstruction(u32 instruction) {
    if (KnownDecoderDisabledAtomicTouchesX18(instruction)) {
        return X18InstructionClass::ExcludedUnsupportedAtomic;
    }

    const Dynarmic::A64::LocationDescriptor descriptor{0, Dynarmic::FP::FPCR{0}, true};
    Dynarmic::IR::Block block{static_cast<Dynarmic::IR::LocationDescriptor>(descriptor)};

    // TranslateSingleInstruction returns the translator's should_continue result, not a pure
    // decode-success flag. Valid terminal instructions such as BR therefore return false while
    // still emitting IR. Inspect the emitted IR before using that boolean as a conservative
    // unsupported signal.
    const bool should_continue =
        Dynarmic::A64::TranslateSingleInstruction(block, descriptor, instruction);

    bool touches_x18 = false;
    for (const auto& inst : block.Instructions()) {
        touches_x18 |= IsX18RegisterReference(inst);
    }

    if (touches_x18) {
        if (Exclusive{instruction}.Verify()) {
            return X18InstructionClass::ExcludedExclusive;
        }
        return X18InstructionClass::SupportedOrdinary;
    }

    if (!should_continue) {
        return X18InstructionClass::Unsupported;
    }

    return X18InstructionClass::NoX18;
}

} // namespace Core::NCE
