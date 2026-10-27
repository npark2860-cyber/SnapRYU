// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if !defined(_WIN32)
#error windows_x18_fallback_runner.h is only available on Windows.
#endif

#include <cstddef>
#include <memory>

#include "common/common_types.h"
#include "core/arm/nce/x18_fallback.h"
#include "core/arm/nce/x18_site_patcher.h"

namespace Kernel {
class KProcess;
class KThread;
}

namespace Core {

class ArmDynarmic64;
class DynarmicExclusiveMonitor;
class System;
struct GuestContext;

namespace NCE {

struct WindowsX18FallbackDispatchResult {
    bool handled{};
    bool metadata_found{};
    X18FallbackStepResult step{};
};

// Windows-only owner for the selective Dynarmic backend used by IMP-006. The private exclusive
// monitor exists only because ArmDynarmic64 requires one; IMP-006 never routes exclusive/LSE
// instructions here, so native NCE reservation state is not shared or transferred.
//
// The integrated ArmNce loop remains an IMP-008 concern. This class owns only the ordinary
// guest-x18 fallback seam and can later be embedded by the Windows ArmNce implementation.
class WindowsX18FallbackRunner {
public:
    static constexpr u64 DirectStackRefreshMarker = 0x5831385354524546ull; // "X18STREF"

    WindowsX18FallbackRunner(System& system, bool uses_wall_clock, Kernel::KProcess* process,
                             std::size_t core_index);
    ~WindowsX18FallbackRunner();

    WindowsX18FallbackRunner(const WindowsX18FallbackRunner&) = delete;
    WindowsX18FallbackRunner& operator=(const WindowsX18FallbackRunner&) = delete;

    [[nodiscard]] WindowsX18FallbackDispatchResult Dispatch(
        u64 transition_result, Kernel::KThread* thread, GuestContext& guest,
        const X18FallbackMetadata& metadata, u64 private_stack_base,
        u64 private_stack_size);

    void SetDirectPrivateStackView(u64 base, u64 size) noexcept;
    void ClearDirectPrivateStackView() noexcept;

    [[nodiscard]] X18FallbackStepResult ExecuteDirect(Kernel::KThread* thread,
                                                      GuestContext& guest,
                                                      u32 instruction);

private:
    std::unique_ptr<DynarmicExclusiveMonitor> m_exclusive_monitor;
    std::unique_ptr<ArmDynarmic64> m_backend;
    u64 m_direct_private_stack_base{};
    u64 m_direct_private_stack_size{};
};

extern "C" u64 WindowsNceExecuteDirectX18(GuestContext* guest, u32 instruction) noexcept;

} // namespace NCE
} // namespace Core
