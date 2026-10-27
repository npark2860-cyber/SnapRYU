// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 Skyline Team and Contributors
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "common/bit_field.h"
#include "common/common_types.h"

namespace Core::NCE {

enum SystemRegister : u32 {
    CtrEl0 = 0x5801,
    TpidrEl0 = 0x5E82,
    TpidrroEl0 = 0x5E83,
    CntfrqEl0 = 0x5F00,
    CntpctEl0 = 0x5F01,
};

// https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/SVC--Supervisor-Call-
union SVC {
    constexpr explicit SVC(u32 raw_) : raw{raw_} {}

    constexpr bool Verify() {
        return (this->GetSig0() == 0x1 && this->GetSig1() == 0x6A0);
    }

    constexpr u32 GetSig0() {
        return decltype(sig0)::ExtractValue(raw);
    }

    constexpr u32 GetValue() {
        return decltype(value)::ExtractValue(raw);
    }

    constexpr u32 GetSig1() {
        return decltype(sig1)::ExtractValue(raw);
    }

    u32 raw;

private:
    BitField<0, 5, u32> sig0;   // 0x1
    BitField<5, 16, u32> value; // 16-bit immediate
    BitField<21, 11, u32> sig1; // 0x6A0
};
static_assert(sizeof(SVC) == sizeof(u32));
static_assert(SVC(0xD40000C1).Verify());
static_assert(SVC(0xD40000C1).GetValue() == 0x6);

// https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/MRS--Move-System-Register-
union MRS {
    constexpr explicit MRS(u32 raw_) : raw{raw_} {}

    constexpr bool Verify() {
        return (this->GetSig() == 0xD53);
    }

    constexpr u32 GetRt() {
        return decltype(rt)::ExtractValue(raw);
    }

    constexpr u32 GetSystemReg() {
        return decltype(system_reg)::ExtractValue(raw);
    }

    constexpr u32 GetSig() {
        return decltype(sig)::ExtractValue(raw);
    }

    u32 raw;

private:
    BitField<0, 5, u32> rt;          // destination register
    BitField<5, 15, u32> system_reg; // source system register
    BitField<20, 12, u32> sig;       // 0xD53
};
static_assert(sizeof(MRS) == sizeof(u32));
static_assert(MRS(0xD53BE020).Verify());
static_assert(MRS(0xD53BE020).GetSystemReg() == CntpctEl0);
static_assert(MRS(0xD53BE020).GetRt() == 0x0);
static_assert(MRS(0xD53B0029).Verify());
static_assert(MRS(0xD53B0029).GetSystemReg() == CtrEl0);
static_assert(MRS(0xD53B0029).GetRt() == 0x9);

// https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/MSR--register---Move-general-purpose-register-to-System-Register-
union MSR {
    constexpr explicit MSR(u32 raw_) : raw{raw_} {}

    constexpr bool Verify() {
        return this->GetSig() == 0xD51;
    }

    constexpr u32 GetRt() {
        return decltype(rt)::ExtractValue(raw);
    }

    constexpr u32 GetSystemReg() {
        return decltype(system_reg)::ExtractValue(raw);
    }

    constexpr u32 GetSig() {
        return decltype(sig)::ExtractValue(raw);
    }

    u32 raw;

private:
    BitField<0, 5, u32> rt;          // source register
    BitField<5, 15, u32> system_reg; // destination system register
    BitField<20, 12, u32> sig;       // 0xD51
};
static_assert(sizeof(MSR) == sizeof(u32));
static_assert(MSR(0xD51BD040).Verify());
static_assert(MSR(0xD51BD040).GetSystemReg() == TpidrEl0);
static_assert(MSR(0xD51BD040).GetRt() == 0x0);

// https://developer.arm.com/documentation/ddi0596/latest/Base-Instructions/DC-CIVAC--Data-Cache-Clean-and-Invalidate-by-VA-to-PoC-
union DCCIVAC {
    constexpr explicit DCCIVAC(u32 raw_) : raw{raw_} {}

    constexpr bool Verify() const {
        return (raw & 0xFFFFFFE0U) == 0xD50B7E20U;
    }

    constexpr u32 GetRt() const {
        return raw & 0x1FU;
    }

    u32 raw;
};
static_assert(sizeof(DCCIVAC) == sizeof(u32));
static_assert(DCCIVAC(0xD50B7E2A).Verify());
static_assert(DCCIVAC(0xD50B7E2A).GetRt() == 0xA);

// https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/LDXR--Load-Exclusive-Register-
// https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/LDXP--Load-Exclusive-Pair-of-Registers-
// https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/STXR--Store-Exclusive-Register-
// https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/STXP--Store-Exclusive-Pair-of-registers-
union Exclusive {
    constexpr explicit Exclusive(u32 raw_) : raw{raw_} {}

    constexpr bool Verify() const {
        if (GetSig() != 0x10) {
            return false;
        }
        return IsPair() || GetRt2() == 0b11111;
    }

    constexpr u32 GetRt() const {
        return decltype(rt)::ExtractValue(raw);
    }

    constexpr u32 GetRn() const {
        return decltype(rn)::ExtractValue(raw);
    }

    constexpr u32 GetRt2() const {
        return decltype(rt2)::ExtractValue(raw);
    }

    constexpr u32 GetRs() const {
        return decltype(rs)::ExtractValue(raw);
    }

    constexpr u32 GetOperation() const {
        return decltype(l)::ExtractValue(raw);
    }

    constexpr u32 GetSig() const {
        return decltype(sig)::ExtractValue(raw);
    }

    constexpr bool IsLoad() const {
        return (GetOperation() & 0b10U) != 0;
    }

    constexpr bool IsPair() const {
        return (GetOperation() & 0b01U) != 0;
    }

    constexpr bool HasRegisterInput(u32 reg) const {
        if (GetRn() == reg) {
            return true;
        }
        if (IsLoad()) {
            return false;
        }
        return GetRt() == reg || (IsPair() && GetRt2() == reg);
    }

    constexpr bool HasRegisterOutput(u32 reg) const {
        if (IsLoad()) {
            return GetRt() == reg || (IsPair() && GetRt2() == reg);
        }
        return GetRs() == reg;
    }

    constexpr bool TouchesRegister(u32 reg) const {
        return HasRegisterInput(reg) || HasRegisterOutput(reg);
    }

    constexpr u32 AsOrdered() const {
        return raw | decltype(o0)::FormatValue(1);
    }

    // Rewrite only architecturally active register operands. This is used by the Windows x18
    // exclusive trampoline so physical x18 remains the platform TEB while the guest's virtual
    // x18 value is carried in a non-conflicting scratch register.
    constexpr u32 RewriteRegister(u32 from, u32 to) const {
        u32 rewritten = AsOrdered();
        if (GetRn() == from) {
            rewritten = ReplaceRegisterField(rewritten, 5, to);
        }
        if (GetRt() == from) {
            rewritten = ReplaceRegisterField(rewritten, 0, to);
        }
        if (IsPair() && GetRt2() == from) {
            rewritten = ReplaceRegisterField(rewritten, 10, to);
        }
        if (!IsLoad() && GetRs() == from) {
            rewritten = ReplaceRegisterField(rewritten, 16, to);
        }
        return rewritten;
    }

    u32 raw;

private:
    static constexpr u32 ReplaceRegisterField(u32 value, u32 shift, u32 reg) {
        constexpr u32 RegisterMask = 0x1FU;
        const u32 mask = RegisterMask << shift;
        return (value & ~mask) | ((reg & RegisterMask) << shift);
    }

    BitField<0, 5, u32> rt;    // memory operand
    BitField<5, 5, u32> rn;    // register operand 1
    BitField<10, 5, u32> rt2;  // register operand 2
    BitField<15, 1, u32> o0;   // ordered
    BitField<16, 5, u32> rs;   // status register
    BitField<21, 2, u32> l;    // operation type
    BitField<23, 7, u32> sig;  // 0x10
    BitField<30, 2, u32> size; // size
};
static_assert(Exclusive(0xC85FFC00).Verify());
static_assert(Exclusive(0xC85F7C00).Verify()); // LDXR
static_assert(Exclusive(0xC8200440).Verify()); // STXP uses Rt2
static_assert(Exclusive(0xC85F7E40).Verify()); // Windows virtual x18 base remains valid
static_assert(!Exclusive(0xC85F0000).Verify()); // Reserved Rt2 in a single-register encoding
static_assert(!Exclusive(0xD503201F).Verify()); // NOP is not exclusive memory access
static_assert(Exclusive(0xC85FFC00).AsOrdered() == 0xC85FFC00);
static_assert(Exclusive(0xC85F7C00).AsOrdered() == 0xC85FFC00);
static_assert(Exclusive(0xC8200440).AsOrdered() == 0xC8208440);
static_assert(Exclusive(0xC85F7E40).HasRegisterInput(18));
static_assert(Exclusive(0xC85F7C12).HasRegisterOutput(18));
static_assert(Exclusive(0xC85F7E40).RewriteRegister(18, 19) == 0xC85FFE60);
static_assert(Exclusive(0xC8200452).HasRegisterInput(18));

} // namespace Core::NCE
