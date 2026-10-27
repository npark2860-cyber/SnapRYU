// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if !defined(_WIN32)
#error windows_nce_transition.h is only available on Windows.
#endif

#include <cstdint>
#include <windows.h>

#include "core/arm/nce/guest_context.h"

namespace Core::NCE {

// Fixed Windows ARM64 trampoline entry. The assembly function saves the Windows ABI nonvolatile
// host state, restores guest architectural state except physical x18, and tail-enters
// entry_trampoline. The trampoline owns the final restoration of guest x16/x17 and branches to the
// selected guest PC.
extern "C" std::uint64_t WindowsNceEnterGuest(GuestContext* guest,
                                               const void* entry_trampoline) noexcept;

// Windows ARM64 arbitrary-PC entry. The assembly wrapper saves the same host continuation consumed
// by generated SVC/exception returns, then WindowsNceRestoreGuestContext builds a Windows CONTEXT
// from GuestContext and resumes GuestContext::pc. Physical x18 remains the captured Windows/TEB
// value and is never loaded from guest architectural state.
extern "C" std::uint64_t WindowsNceEnterGuestContext(GuestContext* guest) noexcept;
extern "C" [[noreturn]] void WindowsNceRestoreGuestContext(GuestContext* guest) noexcept;

class WindowsNceTransition {
public:
    // Resolve ntdll!NtContinue while still on an ordinary Windows host stack. Exception-handler
    // resume paths can run with a guest SP, so they must not enter the loader to resolve it.
    [[nodiscard]] static bool Initialize() noexcept;

    // Resume exactly the supplied Windows ARM64 context through the pre-resolved ntdll!NtContinue.
    // A successful call never returns. Missing initialization or an unexpected NtContinue return
    // is fatal because
    // there is no safe partially-completed context transition to resume from.
    [[noreturn]] static void ContinueContext(ARM64_NT_CONTEXT& context) noexcept;

    // Convert an externally suspended target back to the saved host ABI continuation. If the
    // interrupted PC/SP is known to be native guest execution, save that guest state first.
    // return_value becomes the x0 result of the Windows guest-entry call.
    static void RedirectToHost(ARM64_NT_CONTEXT& interrupted, GuestContext& guest,
                               bool save_guest_state, std::uint64_t return_value) noexcept;
};

// Stable C-linkage metadata locator for generated Windows NCE helpers. Generated code must call
// this fixed getter rather than embedding compiler/TEB TLS offsets.
extern "C" void* GetCurrentNceContextForGeneratedCode() noexcept;

} // namespace Core::NCE
