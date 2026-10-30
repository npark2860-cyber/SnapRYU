// SPDX-FileCopyrightText: Copyright 2026 SnapRYU contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <memory>
#include <vector>

#include "common/common_types.h"
#include "core/hle/kernel/k_page_group.h"

namespace Kernel {
class KernelCore;
class KProcess;
} // namespace Kernel

namespace Service::RO {

struct NroNcePatch {
    bool active{};
    u64 text_address{};
    u64 text_size{};
    u64 patch_address{};
    u64 patch_size{};
    std::vector<u8> original_text;
    // Physical pages of the patch section (System pool). Mapped while active.
    std::unique_ptr<Kernel::KPageGroup> pages;
};

// Windows NCE: patch an NRO mapped by ldr:ro the way the NSO loader patches static modules.
// Runs between ValidateNro and SetNroPerms. On any precondition miss the NRO stays unpatched.
bool ApplyNroNcePatch(NroNcePatch& out, Kernel::KernelCore& kernel, Kernel::KProcess* process,
                      u64 base_address, u64 nro_size, u64 bss_size, u64 rx_size,
                      const std::array<u8, 0x20>& module_id);

// Restores the original text, retires the NRO's metadata, unmaps and frees its patch pages. Runs
// before UnmapNro.
void RemoveNroNcePatch(NroNcePatch& patch, Kernel::KernelCore& kernel, Kernel::KProcess* process);

// Drops the page reference of a patch whose process context is being finalized. The mapping, if
// it still exists, keeps its own reference until the process page table is torn down.
void ReleaseNroNcePatch(NroNcePatch& patch, Kernel::KernelCore& kernel);

} // namespace Service::RO
