#include "video_core/query_cache/predicate_trace.h"
// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/device_memory.h"
#include "hle/kernel/board/nintendo/nx/k_system_control.h"

namespace Core {

#ifdef HAS_NCE
constexpr size_t VirtualReserveSize = 1ULL << 38;
#else
constexpr size_t VirtualReserveSize = 1ULL << 39;
#endif

DeviceMemory::DeviceMemory()
    : buffer{Kernel::Board::Nintendo::Nx::KSystemControl::Init::GetIntendedMemorySize(),
             VirtualReserveSize} {
    if (VideoCommon::PredicateTrace::Enabled()) {
        Common::GuestWriteWatch::g_dram_base.store(
            reinterpret_cast<uintptr_t>(buffer.BackingBasePointer()), std::memory_order_relaxed);
        Common::GuestWriteWatch::g_dram_size.store(
            Kernel::Board::Nintendo::Nx::KSystemControl::Init::GetIntendedMemorySize(),
            std::memory_order_relaxed);
        LOG_INFO(HW_Memory, "LIGHT_TRACE old_backend=true query_reports=true "
                 "write_watch=false full_wfi_barrier=false submit_accumulation=false "
                 "min_true_observations=3 max_bursts_per_minute=16");
    }
}

DeviceMemory::~DeviceMemory() = default;

} // namespace Core
