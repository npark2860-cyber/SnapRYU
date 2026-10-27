// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <iterator>
#include <span>
#include <utility>
#include <vector>
#include "common/common_types.h"

namespace Core::NCE {

// B1 describes generated code. It does not authorize a CONTEXT transform or read target memory.
// B2 must first validate the suspended target's physical x18/TEB and its params TLS slot against
// the pinned epoch params; only then may it read the target TEB scratch slot or params spills.
enum class WindowsPatchScratchSource : u8 {
    CapturedSelf,
    CapturedValueRegister,
    TargetTebScratch,
    ParamsSpillContext,
    ParamsSpillValue,
    GuestStackSlot0, // R2b-B2x1: [captured SP + 0], saved by the bitfield helper's STP
    GuestStackSlot8, // R2b-B2x1: [captured SP + 8]
    ParamsField,     // R2b-B2x2: the params field at params_field_offset (TPIDR_EL0/TPIDRRO_EL0)
};

enum class WindowsPatchX18Source : u8 { GuestContext, CapturedValueRegister };
enum class WindowsPatchRecoveryKind : u8 {
    X18SubstitutionV1,
    X18BitfieldV1,
    MrsTpidrV1,      // R2b-B2x2
    X18CondBranchV1, // R2b-B2x2
};

struct WindowsPatchRecoveryStep {
    u16 offset{}; // Bound emitter label, relative to begin; never an independently assumed offset.
    WindowsPatchScratchSource value{};
    WindowsPatchScratchSource context{};
    WindowsPatchX18Source x18{};
    u8 guest_pc_delta{};
    u8 sp_restore{}; // R2b-B2x1: bytes the helper has pushed below the guest SP at this step
    bool to_branch_target{}; // R2b-B2x2: resume at the guest branch target, not guest_pc + delta
};

[[nodiscard]] constexpr bool SameWindowsPatchRecoveryStep(const WindowsPatchRecoveryStep& a,
                                                          const WindowsPatchRecoveryStep& b) {
    return a.offset == b.offset && a.value == b.value && a.context == b.context &&
           a.x18 == b.x18 && a.guest_pc_delta == b.guest_pc_delta && a.sp_restore == b.sp_restore &&
           a.to_branch_target == b.to_branch_target;
}

// R2b-B2x1. One step per instruction of the bitfield helper (patcher.cpp
// WriteWindowsX18BitfieldTrampoline, TEB-slot lookup only). The patcher binds a label per step and
// records the recipe only if every label sits at index * 4, so this table is the emitter's layout:
//   STP V,C,[SP,#-16]! / LDR C,[X18,#params] / LDR C,[C,#native_context] / LDR V,[C,#guest x18] /
//   <rewritten> / STR V,[C,#guest x18] (writes_x18 only) / LDP V,C,[SP],#16 / B back
// V and C keep their guest values until the STP has saved them; after that the originals are on
// the guest stack until the LDP. The rewritten instruction is the only guest-visible effect.
inline constexpr std::size_t WindowsX18BitfieldMaxSteps = 8;
[[nodiscard]] constexpr std::array<WindowsPatchRecoveryStep, WindowsX18BitfieldMaxSteps>
WindowsX18BitfieldSteps(bool writes_x18) {
    using S = WindowsPatchScratchSource;
    using X = WindowsPatchX18Source;
    std::array<WindowsPatchRecoveryStep, WindowsX18BitfieldMaxSteps> s{};
    std::size_t i = 0;
    const auto add = [&](S value, S context, X x18, u8 pc_delta, u8 sp_restore) {
        s[i] = {static_cast<u16>(i * sizeof(u32)), value, context, x18, pc_delta, sp_restore};
        ++i;
    };
    add(S::CapturedSelf, S::CapturedSelf, X::GuestContext, 0, 0);          // STP
    add(S::CapturedSelf, S::CapturedSelf, X::GuestContext, 0, 16);         // LDR C, params
    add(S::CapturedSelf, S::GuestStackSlot8, X::GuestContext, 0, 16);      // LDR C, native_context
    add(S::CapturedSelf, S::GuestStackSlot8, X::GuestContext, 0, 16);      // LDR V, guest x18
    add(S::GuestStackSlot0, S::GuestStackSlot8, X::GuestContext, 0, 16);   // rewritten
    if (writes_x18) {
        add(S::GuestStackSlot0, S::GuestStackSlot8, X::CapturedValueRegister, 4, 16); // STR V
    }
    add(S::GuestStackSlot0, S::GuestStackSlot8, X::GuestContext, 4, 16);   // LDP
    add(S::CapturedSelf, S::CapturedSelf, X::GuestContext, 4, 0);          // B back
    return s;
}

// R2b-B2x2. MRS Xd, TPIDR_EL0 / TPIDRRO_EL0 handler (patcher.cpp WriteMrsHandler, d != 18, 31):
//   LDR Xd,[X18,#params] / LDR Xd,[Xd,#field] / B back
// Xd is the instruction's destination, so its old value is dead. Before the field load the MRS has
// not happened; at the field load the recovery completes it with the field value.
inline constexpr std::size_t WindowsMrsTpidrSteps = 3;
[[nodiscard]] constexpr std::array<WindowsPatchRecoveryStep, WindowsMrsTpidrSteps>
WindowsMrsTpidrStepTable() {
    using S = WindowsPatchScratchSource;
    using X = WindowsPatchX18Source;
    return {{
        {0, S::CapturedSelf, S::CapturedSelf, X::GuestContext, 0, 0},  // LDR Xd, params
        {4, S::ParamsField, S::ParamsField, X::GuestContext, 4, 0},    // LDR Xd, field
        {8, S::CapturedSelf, S::CapturedSelf, X::GuestContext, 4, 0},  // B back
    }};
}

// R2b-B2x2. CBZ/CBNZ/TBZ/TBNZ on guest x18 (patcher.cpp WriteWindowsX18CondBranchTrampoline,
// TEB-slot lookup only):
//   STR X16,[SP,#-16]! / LDR X16,[X18,#params] / LDR X16,[X16,#native_context] /
//   LDR X16,[X16,#guest x18] / <CB|TB X16 -> +12> / LDR X16,[SP],#16 / B next /
//   LDR X16,[SP],#16 / B target
// X16 keeps its guest value until the STR; after that it is on the guest stack until a reload.
// Nothing guest-visible happens before the branch is resolved, so until then the guest restarts
// the instruction; after it, the guest is at the next instruction or at the branch target.
inline constexpr std::size_t WindowsX18CondBranchSteps = 9;
inline constexpr u8 WindowsX18CondBranchScratch = 16;
[[nodiscard]] constexpr std::array<WindowsPatchRecoveryStep, WindowsX18CondBranchSteps>
WindowsX18CondBranchStepTable() {
    using S = WindowsPatchScratchSource;
    using X = WindowsPatchX18Source;
    return {{
        {0, S::CapturedSelf, S::CapturedSelf, X::GuestContext, 0, 0, false},       // STR X16
        {4, S::GuestStackSlot0, S::GuestStackSlot0, X::GuestContext, 0, 16, false}, // LDR params
        {8, S::GuestStackSlot0, S::GuestStackSlot0, X::GuestContext, 0, 16, false}, // LDR native
        {12, S::GuestStackSlot0, S::GuestStackSlot0, X::GuestContext, 0, 16, false}, // LDR x18
        {16, S::GuestStackSlot0, S::GuestStackSlot0, X::GuestContext, 0, 16, false}, // CB/TB
        {20, S::GuestStackSlot0, S::GuestStackSlot0, X::GuestContext, 4, 16, false}, // LDR (not taken)
        {24, S::CapturedSelf, S::CapturedSelf, X::GuestContext, 4, 0, false},       // B next
        {28, S::GuestStackSlot0, S::GuestStackSlot0, X::GuestContext, 0, 16, true},  // LDR (taken)
        {32, S::CapturedSelf, S::CapturedSelf, X::GuestContext, 0, 0, true},        // B target
    }};
}

[[nodiscard]] constexpr bool IsWindowsX18CondBranch(u32 instruction) {
    const u32 op = instruction & 0x7E000000U;
    return (op == 0x34000000U || op == 0x36000000U) && (instruction & 0x1FU) == 18;
}

// The helper's own branch: same opcode, size and tested bit, register X16, offset +12.
[[nodiscard]] constexpr u32 WindowsX18CondBranchRewrite(u32 instruction) {
    const bool is_tb = (instruction & 0x7E000000U) == 0x36000000U;
    const u32 keep = is_tb ? 0xFFF80000U : 0xFF000000U;
    return (instruction & keep) | (3U << 5) | WindowsX18CondBranchScratch;
}

[[nodiscard]] constexpr u64 WindowsX18CondBranchTarget(u32 instruction, u64 guest_pc) {
    const bool is_tb = (instruction & 0x7E000000U) == 0x36000000U;
    const s64 imm = is_tb
        ? static_cast<s64>(static_cast<s32>(((instruction >> 5) & 0x3FFFU) << 18) >> 18)
        : static_cast<s64>(static_cast<s32>(((instruction >> 5) & 0x7FFFFU) << 13) >> 13);
    return guest_pc + static_cast<u64>(imm * 4);
}

[[nodiscard]] constexpr u32 WindowsRecoveryLdrX(u32 rt, u32 rn, u32 offset) {
    return 0xF9400000U | ((offset / 8) << 10) | (rn << 5) | rt;
}
[[nodiscard]] constexpr u32 WindowsRecoveryStrX(u32 rt, u32 rn, u32 offset) {
    return 0xF9000000U | ((offset / 8) << 10) | (rn << 5) | rt;
}

struct WindowsPatchRecoveryRecipe {
    static constexpr u16 Version = 1;
    static constexpr std::size_t StepCount = 15;

    u64 begin{};
    u64 end{}; // Exclusive, including the final BranchToModule/BranchToModulePre emission.
    u64 guest_pc{};
    u32 original_instruction{};
    u32 rewritten_instruction{};
    u32 params_tls_offset{};
    u32 scratch_tls_offset{};
    u32 spill_context_offset{};
    u32 spill_value_offset{};
    u32 native_context_offset{};
    u32 guest_x18_offset{};
    u32 params_field_offset{}; // R2b-B2x2: MrsTpidrV1 only
    u16 version{Version};
    u16 tail_offset{};
    u16 tail_bytes{};
    u8 value_reg{};
    u8 context_reg{};
    WindowsPatchRecoveryKind kind{WindowsPatchRecoveryKind::X18SubstitutionV1};
    u8 word_count{StepCount}; // R2b-B2x1: instructions in this helper; steps beyond it are unused
    std::array<WindowsPatchRecoveryStep, StepCount> steps{};
    // Filled after relocation. B2 can compare against actual target bytes without interpreting
    // instruction encodings or trusting an offline game-address list.
    std::array<u32, StepCount> code_words{};

    [[nodiscard]] bool Valid() const noexcept {
        if (kind == WindowsPatchRecoveryKind::X18BitfieldV1) {
            return ValidBitfield();
        }
        if (kind == WindowsPatchRecoveryKind::MrsTpidrV1) {
            return ValidMrsTpidr();
        }
        if (kind == WindowsPatchRecoveryKind::X18CondBranchV1) {
            return ValidX18CondBranch();
        }
        if (word_count != StepCount) {
            return false;
        }
        // V1 covers the current fifteen single-instruction steps and one-word direct B tail.
        // A future wider branch must get a reviewed recipe version, not implicit coverage.
        if (kind != WindowsPatchRecoveryKind::X18SubstitutionV1 ||
            version != Version || (begin & 3) || (guest_pc & 3) ||
            end <= begin || end - begin != StepCount * sizeof(u32) ||
            value_reg >= 31 || context_reg >= 31 || value_reg == context_reg ||
            value_reg == 18 || context_reg == 18 ||
            tail_bytes != sizeof(u32) || tail_offset + tail_bytes != end - begin ||
            tail_offset != steps.back().offset ||
            (code_words.back() & 0xfc000000U) != 0x14000000U ||
            code_words[7] != rewritten_instruction) {
            return false;
        }
        for (std::size_t i = 0; i < steps.size(); ++i) {
            if (steps[i].offset != i * sizeof(u32) ||
                steps[i].guest_pc_delta != (i <= 7 ? 0 : 4)) {
                return false;
            }
        }
        return true;
    }

    // R2b-B2x1. Exact layout of the bitfield helper, including the encodings of the save/restore
    // pair and the three loads; B2 then compares these words with the target's bytes.
    [[nodiscard]] bool ValidBitfield() const noexcept {
        const bool writes_x18 = word_count == WindowsX18BitfieldMaxSteps;
        if (version != Version || (begin & 3) || (guest_pc & 3) ||
            (word_count != WindowsX18BitfieldMaxSteps &&
             word_count != WindowsX18BitfieldMaxSteps - 1) ||
            end <= begin || end - begin != word_count * sizeof(u32) ||
            value_reg < 19 || value_reg > 28 || context_reg < 19 || context_reg > 28 ||
            value_reg == context_reg || tail_bytes != sizeof(u32) ||
            tail_offset + tail_bytes != end - begin ||
            tail_offset != steps[word_count - 1].offset ||
            (params_tls_offset & 7) || params_tls_offset > 0x7FF8 ||
            (native_context_offset & 7) || native_context_offset > 0x7FF8 ||
            (guest_x18_offset & 7) || guest_x18_offset > 0x7FF8) {
            return false;
        }
        const u32 v = value_reg;
        const u32 c = context_reg;
        std::size_t i = 0;
        if (code_words[i++] != (0xA9BF03E0U | (c << 10) | v) ||
            code_words[i++] != WindowsRecoveryLdrX(c, 18, params_tls_offset) ||
            code_words[i++] != WindowsRecoveryLdrX(c, c, native_context_offset) ||
            code_words[i++] != WindowsRecoveryLdrX(v, c, guest_x18_offset) ||
            code_words[i++] != rewritten_instruction ||
            (writes_x18 && code_words[i++] != WindowsRecoveryStrX(v, c, guest_x18_offset)) ||
            code_words[i++] != (0xA8C103E0U | (c << 10) | v) ||
            (code_words[i++] & 0xfc000000U) != 0x14000000U || i != word_count) {
            return false;
        }
        const auto want = WindowsX18BitfieldSteps(writes_x18);
        for (std::size_t k = 0; k < word_count; ++k) {
            if (!SameWindowsPatchRecoveryStep(steps[k], want[k])) {
                return false;
            }
        }
        return true;
    }

    // R2b-B2x2. Exact layout of the MRS TPIDR handler. The field offset is checked against the
    // instruction's system register by the Windows break worker, which knows the params layout.
    [[nodiscard]] bool ValidMrsTpidr() const noexcept {
        const u32 d = value_reg;
        if (version != Version || (begin & 3) || (guest_pc & 3) ||
            word_count != WindowsMrsTpidrSteps || end <= begin ||
            end - begin != WindowsMrsTpidrSteps * sizeof(u32) || d >= 31 || d == 18 ||
            context_reg != d || tail_bytes != sizeof(u32) || tail_offset != 8 ||
            (original_instruction != (0xD53BD040U | d) &&
             original_instruction != (0xD53BD060U | d)) ||
            (params_tls_offset & 7) || params_tls_offset > 0x7FF8 ||
            (params_field_offset & 7) || params_field_offset > 0x7FF8 ||
            code_words[0] != WindowsRecoveryLdrX(d, 18, params_tls_offset) ||
            code_words[1] != WindowsRecoveryLdrX(d, d, params_field_offset) ||
            (code_words[2] & 0xfc000000U) != 0x14000000U) {
            return false;
        }
        const auto want = WindowsMrsTpidrStepTable();
        for (std::size_t k = 0; k < word_count; ++k) {
            if (!SameWindowsPatchRecoveryStep(steps[k], want[k])) {
                return false;
            }
        }
        return true;
    }

    // R2b-B2x2. Exact layout of the x18 conditional-branch helper, including its own branch.
    [[nodiscard]] bool ValidX18CondBranch() const noexcept {
        constexpr u32 s = WindowsX18CondBranchScratch;
        constexpr u32 Push = 0xF81F0FE0U | s; // STR X16,[SP,#-16]!
        constexpr u32 Pop = 0xF84107E0U | s;  // LDR X16,[SP],#16
        if (version != Version || (begin & 3) || (guest_pc & 3) ||
            word_count != WindowsX18CondBranchSteps || end <= begin ||
            end - begin != WindowsX18CondBranchSteps * sizeof(u32) || value_reg != s ||
            context_reg != s || tail_bytes != sizeof(u32) || tail_offset != 32 ||
            !IsWindowsX18CondBranch(original_instruction) ||
            rewritten_instruction != WindowsX18CondBranchRewrite(original_instruction) ||
            (params_tls_offset & 7) || params_tls_offset > 0x7FF8 ||
            (native_context_offset & 7) || native_context_offset > 0x7FF8 ||
            (guest_x18_offset & 7) || guest_x18_offset > 0x7FF8 ||
            code_words[0] != Push || code_words[1] != WindowsRecoveryLdrX(s, 18, params_tls_offset) ||
            code_words[2] != WindowsRecoveryLdrX(s, s, native_context_offset) ||
            code_words[3] != WindowsRecoveryLdrX(s, s, guest_x18_offset) ||
            code_words[4] != rewritten_instruction || code_words[5] != Pop ||
            (code_words[6] & 0xfc000000U) != 0x14000000U || code_words[7] != Pop ||
            (code_words[8] & 0xfc000000U) != 0x14000000U) {
            return false;
        }
        const auto want = WindowsX18CondBranchStepTable();
        for (std::size_t k = 0; k < word_count; ++k) {
            if (!SameWindowsPatchRecoveryStep(steps[k], want[k])) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] const WindowsPatchRecoveryStep* FindStep(u64 pc) const noexcept {
        if (pc < begin || pc >= end || ((pc - begin) & 3) || word_count == 0 ||
            word_count > steps.size()) {
            return nullptr;
        }
        const auto last = steps.begin() + word_count;
        const auto it = std::upper_bound(steps.begin(), last, pc - begin,
            [](u64 offset, const auto& step) { return offset < step.offset; });
        return it == steps.begin() ? nullptr : &*std::prev(it);
    }
};

// R2b-B2 inputs read from the stopped target, after its TEB and params were validated.
struct WindowsPatchRecoveryInputs {
    const u64* captured_x{};  // captured X0..X30
    u64 target_teb_scratch{}; // T, read only when a step needs it
    u64 spill_context{};      // params.x18_scratch[0] (A)
    u64 spill_value{};        // params.x18_scratch[1] (B)
    u64 stack_slot0{};        // R2b-B2x1: [captured SP + 0], read only when a step needs it
    u64 stack_slot8{};        // R2b-B2x1: [captured SP + 8]
    u64 params_field{};       // R2b-B2x2: params field at params_field_offset (MRS handler)
};

struct WindowsPatchRecoveryResult {
    u64 value_original{};   // original guest value of the V scratch register
    u64 context_original{}; // original guest value of the C scratch register
    u64 guest_pc{};
    bool publish_x18{};     // guest x18 is held only in V and must be stored to GuestContext
    u64 guest_x18{};
    u8 sp_restore{};        // R2b-B2x1: add to the captured SP
};

[[nodiscard]] inline bool WindowsPatchStepNeedsTebScratch(const WindowsPatchRecoveryStep& step) {
    return step.value == WindowsPatchScratchSource::TargetTebScratch ||
           step.context == WindowsPatchScratchSource::TargetTebScratch;
}

[[nodiscard]] inline bool WindowsPatchStepNeedsGuestStack(const WindowsPatchRecoveryStep& step) {
    const auto stack = [](WindowsPatchScratchSource s) {
        return s == WindowsPatchScratchSource::GuestStackSlot0 ||
               s == WindowsPatchScratchSource::GuestStackSlot8;
    };
    return stack(step.value) || stack(step.context);
}

// Pure recovery of one captured instruction boundary. Shared by the Windows break worker and the
// host-independent boundary tests, so both exercise the same rules.
[[nodiscard]] inline WindowsPatchRecoveryResult RecoverWindowsX18Substitution(
    const WindowsPatchRecoveryRecipe& recipe, const WindowsPatchRecoveryStep& step,
    const WindowsPatchRecoveryInputs& in) noexcept {
    const auto original = [&](WindowsPatchScratchSource source, u8 self) -> u64 {
        switch (source) {
        case WindowsPatchScratchSource::CapturedSelf:
            return in.captured_x[self];
        case WindowsPatchScratchSource::CapturedValueRegister:
            return in.captured_x[recipe.value_reg];
        case WindowsPatchScratchSource::TargetTebScratch:
            return in.target_teb_scratch;
        case WindowsPatchScratchSource::ParamsSpillContext:
            return in.spill_context;
        case WindowsPatchScratchSource::ParamsSpillValue:
            return in.spill_value;
        case WindowsPatchScratchSource::GuestStackSlot0:
            return in.stack_slot0;
        case WindowsPatchScratchSource::GuestStackSlot8:
            return in.stack_slot8;
        case WindowsPatchScratchSource::ParamsField:
            return in.params_field;
        }
        return in.captured_x[self];
    };
    WindowsPatchRecoveryResult result{};
    result.value_original = original(step.value, recipe.value_reg);
    result.context_original = original(step.context, recipe.context_reg);
    result.guest_pc = step.to_branch_target
        ? WindowsX18CondBranchTarget(recipe.original_instruction, recipe.guest_pc)
        : recipe.guest_pc + step.guest_pc_delta;
    result.publish_x18 = step.x18 == WindowsPatchX18Source::CapturedValueRegister;
    result.guest_x18 = in.captured_x[recipe.value_reg];
    result.sp_restore = step.sp_restore;
    return result;
}

// Process owned; only the loader appends. KProcess::Run seals it before starting a guest thread.
// Afterwards only const lookup/span access is exposed, with lifetime pinned by the process epoch.
// This is independent of the existing WNPC/X186/SVC PostHandlers map.
class WindowsPatchRecoveryTable {
public:
    [[nodiscard]] bool AppendForLoad(WindowsPatchRecoveryRecipe recipe) {
        if (sealed || !recipe.Valid()) {
            return false;
        }
        entries.push_back(std::move(recipe));
        return true;
    }

    void Seal() {
        if (sealed) {
            return;
        }
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return a.begin < b.begin; });
        for (std::size_t i = 1; i < entries.size(); ++i) {
            if (entries[i - 1].end > entries[i].begin) {
                // An ambiguous table never licenses recovery. Existing Unknown remains valid.
                entries.clear();
                break;
            }
        }
        sealed = true;
    }

    [[nodiscard]] const WindowsPatchRecoveryRecipe* Find(u64 pc) const noexcept {
        if (!sealed) {
            return nullptr;
        }
        const auto it = std::upper_bound(entries.begin(), entries.end(), pc,
            [](u64 address, const auto& recipe) { return address < recipe.begin; });
        if (it == entries.begin()) {
            return nullptr;
        }
        const auto& candidate = *std::prev(it);
        return candidate.FindStep(pc) != nullptr ? &candidate : nullptr;
    }

    [[nodiscard]] std::span<const WindowsPatchRecoveryRecipe> Entries() const noexcept {
        return sealed ? std::span<const WindowsPatchRecoveryRecipe>{entries} :
                        std::span<const WindowsPatchRecoveryRecipe>{};
    }

private:
    std::vector<WindowsPatchRecoveryRecipe> entries;
    bool sealed{};
};

} // namespace Core::NCE
