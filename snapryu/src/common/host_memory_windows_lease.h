// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if !defined(_WIN32)
#error host_memory_windows_lease.h is only available on Windows.
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <utility>

#include "common/host_memory.h"
#include "common/logging.h"

#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif

namespace Common {

namespace detail {
// A downward-growing stack only holds live data at and above SP. Copying just that part avoids
// touching (and demand-faulting) every page of a large private stack on each lease transition.
// One extra page below SP is kept as a conservative margin.
inline constexpr std::uintptr_t PrivateLeaseLiveMargin = 0x1000;

[[nodiscard]] inline std::uintptr_t PrivateLeaseCopyBegin(std::uintptr_t begin, std::uintptr_t end,
                                                          u64 live_begin) noexcept {
    const auto live = static_cast<std::uintptr_t>(live_begin);
    if (live_begin == 0 || live <= begin || live > end) {
        return begin;
    }
    const auto page = (live - 1) & ~static_cast<std::uintptr_t>(0xFFF);
    return page - begin >= PrivateLeaseLiveMargin ? page - PrivateLeaseLiveMargin : begin;
}
} // namespace detail

inline HostMemory::PrivateMappingLease::PrivateMappingLease(HostMemory* owner_,
                                                            u8* virtual_address_,
                                                            size_t host_offset_, size_t length_,
                                                            MemoryPermission perms_) noexcept
    : owner{owner_}, virtual_address{virtual_address_}, host_offset{host_offset_}, length{length_},
      perms{perms_}, active{true} {}

inline HostMemory::PrivateMappingLease::~PrivateMappingLease() {
    if (active) {
        (void)Restore();
    }
}

inline HostMemory::PrivateMappingLease::PrivateMappingLease(PrivateMappingLease&& other) noexcept
    : owner{std::exchange(other.owner, nullptr)},
      virtual_address{std::exchange(other.virtual_address, nullptr)},
      host_offset{std::exchange(other.host_offset, 0)}, length{std::exchange(other.length, 0)},
      perms{other.perms}, active{std::exchange(other.active, false)} {}

inline HostMemory::PrivateMappingLease& HostMemory::PrivateMappingLease::operator=(
    PrivateMappingLease&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (active && !Restore()) {
        return *this;
    }
    owner = std::exchange(other.owner, nullptr);
    virtual_address = std::exchange(other.virtual_address, nullptr);
    host_offset = std::exchange(other.host_offset, 0);
    length = std::exchange(other.length, 0);
    perms = other.perms;
    active = std::exchange(other.active, false);
    return *this;
}

inline bool HostMemory::PrivateMappingLease::ContainsAddress(u64 address) const noexcept {
    if (!active || virtual_address == nullptr || length == 0) {
        return false;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(virtual_address);
    const auto end = begin + length;
    return end >= begin && address >= begin && address < end;
}

inline bool HostMemory::PrivateMappingLease::SyncToBacking() noexcept {
    if (!active || owner == nullptr || virtual_address == nullptr || length == 0) {
        return false;
    }
    std::memcpy(owner->BackingBasePointer() + host_offset, virtual_address, length);
    std::atomic_thread_fence(std::memory_order_release);
    return true;
}

inline bool HostMemory::PrivateMappingLease::SyncFromBacking() noexcept {
    if (!active || owner == nullptr || virtual_address == nullptr || length == 0) {
        return false;
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    std::memcpy(virtual_address, owner->BackingBasePointer() + host_offset, length);
    return true;
}

inline bool HostMemory::PrivateMappingLease::Restore(u64 live_begin) noexcept {
    if (!active) {
        return true;
    }
    if (owner == nullptr || virtual_address == nullptr || length == 0) {
        active = false;
        return false;
    }

    {
        const auto begin = reinterpret_cast<std::uintptr_t>(virtual_address);
        const auto copy_begin = detail::PrivateLeaseCopyBegin(begin, begin + length, live_begin);
        const size_t skip = static_cast<size_t>(copy_begin - begin);
        std::memcpy(owner->BackingBasePointer() + host_offset + skip, virtual_address + skip,
                    length - skip);
    }

    if (!VirtualFreeEx(GetCurrentProcess(), virtual_address, length,
                       MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
        LOG_ERROR(HW_Memory, "Failed to return private HostMemory lease to placeholder: {}",
                  GetLastError());
        return false;
    }

    active = false;
    owner->Map(reinterpret_cast<size_t>(virtual_address), host_offset, length, perms, false);

    MEMORY_BASIC_INFORMATION restored{};
    const bool restored_mapped =
        VirtualQuery(virtual_address, &restored, sizeof(restored)) != 0 &&
        restored.Type == MEM_MAPPED;
    if (!restored_mapped) {
        LOG_ERROR(HW_Memory, "Failed to restore section-backed HostMemory lease mapping");
    }

    owner = nullptr;
    virtual_address = nullptr;
    host_offset = 0;
    length = 0;
    return restored_mapped;
}

inline std::optional<HostMemory::PrivateMappingLease> HostMemory::AcquireDirectMappedPrivateLease(
    void* virtual_address_, size_t host_offset_, size_t length_, MemoryPermission perms_,
    u64 live_begin) {
    constexpr size_t WindowsPageSize = 0x1000;

    if (virtual_address_ == nullptr || length_ == 0 ||
        (reinterpret_cast<std::uintptr_t>(virtual_address_) & (WindowsPageSize - 1)) != 0 ||
        (length_ & (WindowsPageSize - 1)) != 0 || host_offset_ > backing_size ||
        length_ > backing_size - host_offset_) {
        return std::nullopt;
    }

    MEMORY_BASIC_INFORMATION initial{};
    if (VirtualQuery(virtual_address_, &initial, sizeof(initial)) == 0 ||
        initial.State != MEM_COMMIT || initial.Type != MEM_MAPPED ||
        initial.AllocationBase != virtual_address_) {
        return std::nullopt;
    }

    const auto begin = reinterpret_cast<std::uintptr_t>(virtual_address_);
    const auto end = begin + length_;
    if (end < begin) {
        return std::nullopt;
    }
    for (auto cursor = begin; cursor < end;) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) == 0 ||
            region.State != MEM_COMMIT || region.Type != MEM_MAPPED ||
            region.AllocationBase != initial.AllocationBase) {
            return std::nullopt;
        }
        const auto region_end = reinterpret_cast<std::uintptr_t>(region.BaseAddress) +
                                static_cast<std::uintptr_t>(region.RegionSize);
        if (region_end <= cursor) {
            return std::nullopt;
        }
        cursor = (std::min)(region_end, end);
    }

    using PfnVirtualAlloc2 = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG,
                                            MEM_EXTENDED_PARAMETER*, ULONG);
    auto* const kernelbase = GetModuleHandleW(L"KernelBase.dll");
    if (kernelbase == nullptr) {
        return std::nullopt;
    }
    auto* const virtual_alloc2 = reinterpret_cast<PfnVirtualAlloc2>(
        GetProcAddress(kernelbase, "VirtualAlloc2"));
    if (virtual_alloc2 == nullptr) {
        return std::nullopt;
    }

    // Keep the exact placeholder boundary created by unmapping this section view. Generic
    // HostMemory::Unmap() may coalesce adjacent placeholders, but MEM_REPLACE_PLACEHOLDER requires
    // BaseAddress and Size to exactly match the placeholder being replaced.
    UnmapForPrivateLease(reinterpret_cast<size_t>(virtual_address_), length_);

    auto restore_mapped_view = [&] {
        Map(reinterpret_cast<size_t>(virtual_address_), host_offset_, length_, perms_, false);
    };

    void* replacement = virtual_alloc2(
        GetCurrentProcess(), virtual_address_, length_,
        MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
    if (replacement != virtual_address_) {
        if (replacement != nullptr) {
            (void)VirtualFreeEx(GetCurrentProcess(), replacement, length_,
                                MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
        }

        if (!VirtualFreeEx(GetCurrentProcess(), virtual_address_, length_,
                           MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            restore_mapped_view();
            return std::nullopt;
        }

        replacement = virtual_alloc2(
            GetCurrentProcess(), virtual_address_, length_,
            MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
        if (replacement != virtual_address_) {
            if (replacement != nullptr) {
                (void)VirtualFreeEx(GetCurrentProcess(), replacement, length_,
                                    MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
            }
            restore_mapped_view();
            return std::nullopt;
        }
    }

    {
        const size_t skip = static_cast<size_t>(
            detail::PrivateLeaseCopyBegin(begin, end, live_begin) - begin);
        std::memcpy(static_cast<u8*>(virtual_address_) + skip, backing_base + host_offset_ + skip,
                    length_ - skip);
    }

    MEMORY_BASIC_INFORMATION private_region{};
    if (VirtualQuery(virtual_address_, &private_region, sizeof(private_region)) == 0 ||
        private_region.State != MEM_COMMIT || private_region.Type != MEM_PRIVATE) {
        (void)VirtualFreeEx(GetCurrentProcess(), virtual_address_, length_,
                            MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
        restore_mapped_view();
        return std::nullopt;
    }

    return PrivateMappingLease{this, static_cast<u8*>(virtual_address_), host_offset_, length_,
                               perms_};
}

} // namespace Common
