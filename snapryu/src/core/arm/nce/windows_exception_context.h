// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if !defined(_WIN32)
#error windows_exception_context.h is only available on Windows.
#endif

#include <windows.h>

#include <cstddef>
#include <cstring>

#include "core/arm/nce/current_nce_context.h"
#include "core/arm/nce/guest_context.h"

namespace Core::NCE {

// Windows keeps physical x18 for the user-mode TEB. Guest architectural x18 is therefore not
// copied to or from ARM64_NT_CONTEXT by this adapter; its virtualized value remains in GuestContext.
class WindowsExceptionContext {
public:
    enum class AccessType {
        Read,
        Write,
        Execute,
        Unknown,
    };

    [[nodiscard]] static GuestContext* CurrentGuestContext() noexcept {
        auto* const params = CurrentNceContext::Get();
        if (params == nullptr) {
            return nullptr;
        }
        return static_cast<GuestContext*>(params->native_context);
    }

    [[nodiscard]] static bool IsAccessViolation(const EXCEPTION_RECORD& record) noexcept {
        return record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2;
    }

    [[nodiscard]] static AccessType GetAccessType(const EXCEPTION_RECORD& record) noexcept {
        if (!IsAccessViolation(record)) {
            return AccessType::Unknown;
        }

        switch (record.ExceptionInformation[0]) {
        case 0:
            return AccessType::Read;
        case 1:
            return AccessType::Write;
        case 8:
            return AccessType::Execute;
        default:
            return AccessType::Unknown;
        }
    }

    [[nodiscard]] static void* GetFaultAddress(const EXCEPTION_RECORD& record) noexcept {
        if (!IsAccessViolation(record)) {
            return nullptr;
        }
        return reinterpret_cast<void*>(record.ExceptionInformation[1]);
    }

    static void SaveGuestState(GuestContext& guest, const ARM64_NT_CONTEXT& context) noexcept {
        CopyGeneralRegistersFromWindows(guest, context);
        guest.sp = context.Sp;
        guest.pc = context.Pc;
        guest.pstate = context.Cpsr;
        guest.nzcv = context.Cpsr & 0xF0000000U;
        guest.fpcr = context.Fpcr;
        guest.fpsr = context.Fpsr;

        static_assert(sizeof(context.V) == sizeof(guest.vector_registers));
        std::memcpy(guest.vector_registers.data(), context.V, sizeof(context.V));
    }

    static void LoadGuestState(const GuestContext& guest, ARM64_NT_CONTEXT& context) noexcept {
        CopyGeneralRegistersToWindows(guest, context);
        context.Sp = guest.sp;
        context.Pc = guest.pc;
        context.Cpsr = guest.pstate;
        context.Fpcr = guest.fpcr;
        context.Fpsr = guest.fpsr;

        static_assert(sizeof(context.V) == sizeof(guest.vector_registers));
        std::memcpy(context.V, guest.vector_registers.data(), sizeof(context.V));
    }

private:
    static constexpr std::size_t GuestX18 = 18;
    static constexpr std::size_t RegisterCount = 31;

    static void CopyGeneralRegistersFromWindows(GuestContext& guest,
                                                const ARM64_NT_CONTEXT& context) noexcept {
        for (std::size_t i = 0; i < GuestX18; ++i) {
            guest.cpu_registers[i] = context.X[i];
        }
        for (std::size_t i = GuestX18 + 1; i < RegisterCount; ++i) {
            guest.cpu_registers[i] = context.X[i];
        }
    }

    static void CopyGeneralRegistersToWindows(const GuestContext& guest,
                                              ARM64_NT_CONTEXT& context) noexcept {
        for (std::size_t i = 0; i < GuestX18; ++i) {
            context.X[i] = guest.cpu_registers[i];
        }
        for (std::size_t i = GuestX18 + 1; i < RegisterCount; ++i) {
            context.X[i] = guest.cpu_registers[i];
        }
    }
};

} // namespace Core::NCE
