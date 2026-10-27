// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// EDEN_NCE_WRITE_STEP: give NCE write faults on GPU-cached pages the Dynarmic memory path.
//
// A guest store to a page the GPU caches track (Common::PageType::RasterizerCachedMemory) faults
// under NCE. RunThread (arm_nce_windows.cpp) then calls Memory::InvalidateNCE on the whole 4 KiB
// page, on the CPU thread: the buffer and texture caches drop their GPU-modified state for all of
// it and mark all of it CPU-modified. Dynarmic reports each store with its exact range instead
// (Memory::Write -> HandleRasterizerWrite -> GPU::OnCPUWrite). A store into a GPU-modified buffer
// page is collected and reaches the caches at the next gather (fence, or a texture data cache
// invalidation); any other store invalidates only the bytes written.
//
//   unset or any other value: off, unchanged.
//   "1": a data-abort fault on a RasterizerCachedMemory page runs the faulting instruction once
//        through the callback-only fallback runner (WindowsX18FallbackRunner::ExecuteDirect), so
//        its stores take the Dynarmic path above. The page keeps the protection the caches decide,
//        as the Dynarmic page type does. Exclusive, ordered, compare-and-swap, atomic and system
//        instructions keep InvalidateNCE (see Classify).

#pragma once

#if SNAPRYU_DIAGNOSTICS

#include <atomic>
#include <cstdlib>
#include <string_view>

#include "common/common_types.h"
#include "common/logging.h"

namespace Core::NCE::WriteStep {

enum class Mode : u32 {
    Off = 0,
    Step = 1,
};

[[nodiscard]] inline Mode ParseMode(const char* value) {
    if (value == nullptr) {
        return Mode::Off;
    }
    return std::string_view{value} == "1" ? Mode::Step : Mode::Off;
}

// Read once per process (the launcher sets it before start). RunThread reads it on entry, so the
// mode line also shows that the NCE backend ran.
[[nodiscard]] inline Mode GetMode() {
    static const Mode mode = [] {
        const char* const value = std::getenv("EDEN_NCE_WRITE_STEP");
        const Mode parsed = ParseMode(value);
        LOG_WARNING(Core_ARM, "NCE_WRITE_STEP mode={} value={}",
                    parsed == Mode::Step ? "step" : "off", value != nullptr ? value : "");
        return parsed;
    }();
    return mode;
}

enum class Kind : u32 {
    Step,      // runs through the fallback runner
    NotCached, // fault page is not RasterizerCachedMemory
    Fetch,     // instruction fetch abort (pc == fault address)
    NoCode,    // instruction word not readable
    Exclusive, // load/store exclusive, load-acquire/store-release, CAS/CASP
    Atomic,    // LSE atomic memory operation (LDADD, SWP, ...)
    System,    // system instruction (DC ZVA, ...)
};

// The fallback runner has its own exclusive monitor, so a store-exclusive after a native
// load-exclusive would fail every time and the guest would retry forever. Atomics and system
// instructions stay on the unchanged path too.
[[nodiscard]] constexpr Kind Classify(u32 instruction) noexcept {
    if ((instruction & 0x3F000000U) == 0x08000000U) {
        return Kind::Exclusive;
    }
    if ((instruction & 0x3F200C00U) == 0x38200000U) {
        return Kind::Atomic;
    }
    if ((instruction & 0xFFC00000U) == 0xD5000000U) {
        return Kind::System;
    }
    return Kind::Step;
}

// Log the first four faults, then every power of two.
[[nodiscard]] inline bool Sample(u64 count) {
    return count <= 4 || (count & (count - 1)) == 0;
}

struct Counters {
    std::atomic<u64> faults{0};       // pending NCE faults seen with the switch on
    std::atomic<u64> stepped{0};      // ran through the fallback runner and retired
    std::atomic<u64> still_cached{0}; // page still RasterizerCachedMemory after the step
    std::atomic<u64> incomplete{0};   // step did not retire; InvalidateNCE as before
    std::atomic<u64> halts{0};        // step retired and raised a halt
    std::atomic<u64> step_ns{0};      // time inside ExecuteDirect
    std::atomic<u64> not_cached{0};
    std::atomic<u64> fetch{0};
    std::atomic<u64> no_code{0};
    std::atomic<u64> exclusive{0};
    std::atomic<u64> atomic{0};
    std::atomic<u64> system{0};
};

[[nodiscard]] inline Counters& Get() {
    static Counters counters;
    return counters;
}

inline void LogSummary(u64 fault) {
    const auto& c = Get();
    const auto load = [](const std::atomic<u64>& counter) {
        return counter.load(std::memory_order_relaxed);
    };
    LOG_WARNING(Core_ARM,
                "NCE_WRITE_STEP faults={} stepped={} still_cached={} incomplete={} halts={} "
                "step_us={} not_cached={} fetch={} no_code={} exclusive={} atomic={} system={}",
                fault, load(c.stepped), load(c.still_cached), load(c.incomplete), load(c.halts),
                load(c.step_ns) / 1000, load(c.not_cached), load(c.fetch), load(c.no_code),
                load(c.exclusive), load(c.atomic), load(c.system));
}

// Returns the fault number.
[[nodiscard]] inline u64 NoteFault() {
    return Get().faults.fetch_add(1, std::memory_order_relaxed) + 1;
}

inline void NoteFallback(u64 fault, Kind kind) {
    auto& c = Get();
    switch (kind) {
    case Kind::NotCached:
        c.not_cached.fetch_add(1, std::memory_order_relaxed);
        break;
    case Kind::Fetch:
        c.fetch.fetch_add(1, std::memory_order_relaxed);
        break;
    case Kind::NoCode:
        c.no_code.fetch_add(1, std::memory_order_relaxed);
        break;
    case Kind::Exclusive:
        c.exclusive.fetch_add(1, std::memory_order_relaxed);
        break;
    case Kind::Atomic:
        c.atomic.fetch_add(1, std::memory_order_relaxed);
        break;
    case Kind::System:
        c.system.fetch_add(1, std::memory_order_relaxed);
        break;
    case Kind::Step:
        break;
    }
    if (Sample(fault)) {
        LogSummary(fault);
    }
}

inline void NoteStep(u64 fault, bool completed, bool halted, bool still_cached, u64 ns, u64 pc,
                     u32 instruction, u64 fault_address) {
    auto& c = Get();
    c.step_ns.fetch_add(ns, std::memory_order_relaxed);
    u64 stepped = 0;
    u64 incomplete = 0;
    if (!completed) {
        incomplete = c.incomplete.fetch_add(1, std::memory_order_relaxed) + 1;
    } else {
        stepped = c.stepped.fetch_add(1, std::memory_order_relaxed) + 1;
        if (still_cached) {
            c.still_cached.fetch_add(1, std::memory_order_relaxed);
        }
        if (halted) {
            c.halts.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if ((incomplete != 0 && Sample(incomplete)) || (stepped != 0 && stepped <= 4)) {
        LOG_WARNING(Core_ARM,
                    "NCE_WRITE_STEP step pc={:#x} insn={:08x} fault={:#x} completed={} halted={} "
                    "still_cached={} ns={}",
                    pc, instruction, fault_address, completed, halted, still_cached, ns);
    }
    if (Sample(fault)) {
        LogSummary(fault);
    }
}

} // namespace Core::NCE::WriteStep

#endif // SNAPRYU_DIAGNOSTICS
