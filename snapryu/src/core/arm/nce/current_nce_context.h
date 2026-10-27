// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "core/hle/kernel/k_thread.h"

namespace Core::NCE {

class CurrentNceContext {
public:
    using Parameters = Kernel::KThread::NativeExecutionParameters;

    // Per-OS-thread Install/Clear history. Windows generated code resolves the current
    // NativeExecutionParameters through Get(); when that unexpectedly returns null the history is
    // copied into a global crash record so the minidump shows which Install/Clear came last.
    struct Event {
        std::uint64_t tick;
        Parameters* params;
        std::uint32_t kind;
        std::uint32_t site;
    };
    static constexpr std::size_t HistorySize = 16;
    static constexpr std::uint32_t EventInstall = 1;
    static constexpr std::uint32_t EventClear = 2;

#if defined(_WIN32)
    // Generated Windows NCE code runs with physical x18 = TEB. Mirroring the current parameters
    // pointer into a TEB TLS slot lets it be loaded with a single LDR [X18, #slot] instead of a
    // C ABI call. TebTlsSlotsOffset is TEB64::TlsSlots (identical on x64 and ARM64).
    static constexpr std::uint32_t TebTlsSlotsOffset = 0x1480;
    static constexpr std::uint32_t TebTlsSlotCount = 64;

    // Allocated on first use (the patcher asks for it while generating module trampolines).
    // Returns TLS_OUT_OF_INDEXES when no slot is available; generated code then keeps the C-call
    // lookup path.
    [[nodiscard]] static DWORD TebSlotIndex() noexcept {
        static const DWORD index = TlsAlloc();
        return index;
    }

    [[nodiscard]] static bool HasTebSlot() noexcept {
        return TebSlotIndex() < TebTlsSlotCount;
    }

    [[nodiscard]] static std::uint32_t TebSlotOffset() noexcept {
        return TebTlsSlotsOffset + static_cast<std::uint32_t>(TebSlotIndex()) * 8U;
    }

    // Two per-OS-thread spill slots used by generated x18 substitution trampolines to preserve
    // their scratch registers without touching the guest stack or flags.
    [[nodiscard]] static DWORD ScratchSlotIndex(std::uint32_t which) noexcept {
        static const DWORD index0 = TlsAlloc();
        static const DWORD index1 = TlsAlloc();
        return which == 0 ? index0 : index1;
    }

    [[nodiscard]] static bool HasScratchSlots() noexcept {
        return ScratchSlotIndex(0) < TebTlsSlotCount && ScratchSlotIndex(1) < TebTlsSlotCount;
    }

    [[nodiscard]] static std::uint32_t ScratchSlotOffset(std::uint32_t which) noexcept {
        return TebTlsSlotsOffset + static_cast<std::uint32_t>(ScratchSlotIndex(which)) * 8U;
    }
#endif

    static void Install(Parameters* parameters, std::uint32_t site = 0) noexcept {
        current = parameters;
#if defined(_WIN32)
        if (HasTebSlot()) {
            TlsSetValue(TebSlotIndex(), parameters);
        }
        Record(EventInstall, parameters, site);
        Trace("install", parameters);
#else
        (void)site;
#endif
    }

    static Parameters* Get() noexcept {
        return current;
    }

    static void Clear(std::uint32_t site = 0) noexcept {
#if defined(_WIN32)
        if (HasTebSlot()) {
            TlsSetValue(TebSlotIndex(), nullptr);
        }
        Record(EventClear, current, site);
        Trace("clear", current);
#else
        (void)site;
#endif
        current = nullptr;
    }

#if defined(_WIN32)
    [[nodiscard]] static const std::array<Event, HistorySize>& History() noexcept {
        return history;
    }

    [[nodiscard]] static std::uint32_t HistoryPosition() noexcept {
        return history_pos;
    }
#endif

private:
#if defined(_WIN32)
    static void Record(std::uint32_t kind, Parameters* parameters, std::uint32_t site) noexcept {
        LARGE_INTEGER tick{};
        QueryPerformanceCounter(&tick);
        history[history_pos % HistorySize] =
            Event{static_cast<std::uint64_t>(tick.QuadPart), parameters, kind, site};
        ++history_pos;
    }

    static void Trace(const char* event, Parameters* parameters) noexcept {
        if (!IsDebuggerPresent()) {
            return;
        }

        char message[112]{};
        (void)std::snprintf(message, sizeof(message), "NCECTX %s tid=%lu params=%p\n", event,
                            static_cast<unsigned long>(GetCurrentThreadId()),
                            static_cast<void*>(parameters));
        OutputDebugStringA(message);
    }

    inline static thread_local std::array<Event, HistorySize> history{};
    inline static thread_local std::uint32_t history_pos{};
#endif

    inline static thread_local Parameters* current{};
};

} // namespace Core::NCE
