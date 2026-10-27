// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Build switch for the stall and guest write diagnostics (CMake option EDEN_HANG_DIAG, default ON).
// With it OFF, the diagnostic helpers compile to nothing and their environment switches are ignored;
// rendering and emulation behave as in the diagnostic build with every diagnostic switched off.
namespace Common::HangDiag {

#if defined(EDEN_HANG_DIAG_DISABLED) || !SNAPRYU_DIAGNOSTICS
inline constexpr bool COMPILED = false;
#else
inline constexpr bool COMPILED = true;
#endif

} // namespace Common::HangDiag
