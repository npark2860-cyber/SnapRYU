// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/arm/nce/windows_x18_fallback_trap.h"

#include "core/arm/nce/guest_context.h"
#include "core/arm/nce/windows_nce_transition.h"

namespace Core::NCE {

std::optional<WindowsX18FallbackSiteInfo> WindowsX18FallbackTrap::FindSiteInfo(
    u64 pc, const X18FallbackMetadata& metadata) noexcept {
    const auto it = metadata.find(X18SitePatcher::MetadataKey(pc));
    if (it == metadata.end()) {
        return std::nullopt;
    }

    const u64 value = it->second;
    const u32 tagged_magic =
        static_cast<u32>((value & ~X18SitePatcher::MetadataMayAccessMemoryBit) >> 32);
    if (tagged_magic != X18SitePatcher::MetadataMagic) {
        return std::nullopt;
    }

    return WindowsX18FallbackSiteInfo{
        .instruction = static_cast<u32>(value),
        .may_access_memory =
            (value & X18SitePatcher::MetadataMayAccessMemoryBit) != 0,
    };
}

std::optional<u32> WindowsX18FallbackTrap::FindOriginalInstruction(
    u64 pc, const X18FallbackMetadata& metadata) noexcept {
    const auto info = FindSiteInfo(pc, metadata);
    if (!info.has_value()) {
        return std::nullopt;
    }
    return info->instruction;
}

bool WindowsX18FallbackTrap::TryRedirect(PEXCEPTION_POINTERS exception, GuestContext& guest,
                                         const X18FallbackMetadata& metadata) noexcept {
    if (exception == nullptr || exception->ExceptionRecord == nullptr ||
        exception->ContextRecord == nullptr ||
        exception->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) {
        return false;
    }

    auto& context = *reinterpret_cast<ARM64_NT_CONTEXT*>(exception->ContextRecord);
    if (!FindOriginalInstruction(context.Pc, metadata).has_value()) {
        return false;
    }

    // Windows captured the full architectural context before this helper runs. RedirectToHost
    // saves all Windows-representable guest state while deliberately leaving physical x18 alone;
    // guest architectural x18 remains the virtual value already owned by GuestContext.
    WindowsNceTransition::RedirectToHost(context, guest, true, ReturnMarker);
    return true;
}

} // namespace Core::NCE
