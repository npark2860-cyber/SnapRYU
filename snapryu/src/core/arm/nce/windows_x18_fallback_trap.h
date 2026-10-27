// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if !defined(_WIN32)
#error windows_x18_fallback_trap.h is only available on Windows.
#endif

#include <optional>
#include <windows.h>

#include "core/arm/nce/x18_site_patcher.h"

namespace Core {

struct GuestContext;

namespace NCE {

struct WindowsX18FallbackSiteInfo {
    u32 instruction{};
    bool may_access_memory{};
};

class WindowsX18FallbackTrap {
public:
    // Internal RunThread-only marker. It is deliberately outside the public Core::HaltReason bits
    // and must never be returned to PhysicalCore as an emulator halt reason.
    static constexpr u64 ReturnMarker = 0x5846313846414C4Cull; // "XF18FALL"

    [[nodiscard]] static bool TryRedirect(PEXCEPTION_POINTERS exception, GuestContext& guest,
                                          const X18FallbackMetadata& metadata) noexcept;

    [[nodiscard]] static std::optional<WindowsX18FallbackSiteInfo> FindSiteInfo(
        u64 pc, const X18FallbackMetadata& metadata) noexcept;

    [[nodiscard]] static std::optional<u32> FindOriginalInstruction(
        u64 pc, const X18FallbackMetadata& metadata) noexcept;
};

} // namespace NCE
} // namespace Core
