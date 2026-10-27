// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// EDEN_NCE_X18_HOST_FPCR: counters for the host FPCR around the direct x18 helper.
//
// The direct x18 trampoline (patcher.cpp, WriteWindowsX18DirectTrampoline) saves guest state and
// calls the C++ helper WindowsNceExecuteDirectX18. It always loads the host FPCR saved at guest
// entry before that call, so the helper, and RunThread after a non-zero return, run with the host
// FPCR as after the normal exit (WriteSaveContext, WindowsNceTransition). Before that fix the guest
// FPCR stayed live there, and the next guest entry (windows_nce_entry.asm) saved it as the host
// FPCR.
//
//   unset or any other value: no counting.
//   "audit" or "1": count helper calls where the guest, saved host and live FPCR differ, and
//                   non-zero returns. With the fix, live_ne_host and host_changed stay 0.

#pragma once

#include <atomic>
#include <cstdlib>
#include <string_view>

#include "common/common_types.h"
#include "common/logging.h"

namespace Core::NCE::X18HostFpcr {

enum class Mode : u32 {
    Off = 0,
    Audit = 1,
};

[[nodiscard]] inline Mode ParseMode(const char* value) {
    if (value == nullptr) {
        return Mode::Off;
    }
    const std::string_view text{value};
    if (text == "1" || text == "audit") {
        return Mode::Audit;
    }
    return Mode::Off;
}

[[nodiscard]] inline const char* ModeName(Mode mode) {
    switch (mode) {
    case Mode::Audit:
        return "audit";
    default:
        return "off";
    }
}

// Read once per process (the launcher sets it before start).
[[nodiscard]] inline Mode GetMode() {
    static const Mode mode = [] {
        const char* const value = std::getenv("EDEN_NCE_X18_HOST_FPCR");
        const Mode parsed = ParseMode(value);
        LOG_WARNING(Core_ARM, "NCE_X18_HOST_FPCR mode={} value={} host_restore=always",
                    ModeName(parsed), value != nullptr ? value : "");
        return parsed;
    }();
    return mode;
}

[[nodiscard]] inline u32 ReadLiveFpcr() noexcept {
#if defined(__aarch64__)
    u64 value;
    asm volatile("mrs %0, fpcr" : "=r"(value));
    return static_cast<u32>(value);
#else
    return 0;
#endif
}

// Log the first four events, then every power of two.
[[nodiscard]] inline bool Sample(u64 count) {
    return count <= 4 || (count & (count - 1)) == 0;
}

constexpr u32 UnsetFpcr = 0xFFFFFFFFU;

struct Counters {
    std::atomic<u64> calls{0};
    std::atomic<u64> guest_ne_host{0}; // guest FPCR differs from the saved host FPCR
    std::atomic<u64> live_ne_host{0};  // FPCR live in the helper differs from the saved host FPCR
    std::atomic<u64> host_changed{0};  // saved host FPCR differs from the first one seen
    std::atomic<u64> halts{0};         // helper returned non-zero (trampoline halt path)
    std::atomic<u32> first_host{UnsetFpcr};
};

[[nodiscard]] inline Counters& Get() {
    static Counters counters;
    return counters;
}

inline void NoteEntry(u32 guest_fpcr, u32 host_fpcr, u32 live_fpcr) {
    auto& c = Get();
    const u64 call = c.calls.fetch_add(1, std::memory_order_relaxed) + 1;
    u32 first = UnsetFpcr;
    if (c.first_host.compare_exchange_strong(first, host_fpcr, std::memory_order_relaxed)) {
        first = host_fpcr;
    }
    const auto bump = [](std::atomic<u64>& counter, bool hit) -> u64 {
        return hit ? counter.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
    };
    const u64 guest_ne = bump(c.guest_ne_host, guest_fpcr != host_fpcr);
    const u64 live_ne = bump(c.live_ne_host, live_fpcr != host_fpcr);
    const u64 changed = bump(c.host_changed, host_fpcr != first);
    if (Sample(call) || guest_ne == 1 || live_ne == 1 || changed == 1) {
        LOG_WARNING(Core_ARM,
                    "NCE_X18_HOST_FPCR calls={} guest_ne_host={} live_ne_host={} host_changed={} "
                    "halts={} guest=0x{:x} host=0x{:x} live=0x{:x} first_host=0x{:x}",
                    call, c.guest_ne_host.load(std::memory_order_relaxed),
                    c.live_ne_host.load(std::memory_order_relaxed),
                    c.host_changed.load(std::memory_order_relaxed),
                    c.halts.load(std::memory_order_relaxed), guest_fpcr, host_fpcr, live_fpcr,
                    first);
    }
}

// result != 0 takes the trampoline halt path back to RunThread with the FPCR live here.
inline void NoteResult(u64 result, u32 host_fpcr, u32 live_fpcr) {
    if (result == 0) {
        return;
    }
    const u64 halt = Get().halts.fetch_add(1, std::memory_order_relaxed) + 1;
    if (Sample(halt)) {
        LOG_WARNING(Core_ARM,
                    "NCE_X18_HOST_FPCR halts={} result=0x{:x} host=0x{:x} live=0x{:x}", halt,
                    result, host_fpcr, live_fpcr);
    }
}

} // namespace Core::NCE::X18HostFpcr
