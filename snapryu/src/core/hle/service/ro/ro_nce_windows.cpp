// SPDX-FileCopyrightText: Copyright 2026 SnapRYU contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(_WIN32) && defined(HAS_NCE)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "common/alignment.h"
#include "common/host_memory.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/arm/nce/patcher.h"
#include "core/arm/nce/windows_patch_recovery_metadata.h"
#include "core/arm/nce/x18_site_patcher.h"
#include "core/hle/kernel/k_memory_manager.h"
#include "core/hle/kernel/k_page_group.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_system_resource.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/ro/ro_nce_windows.h"
#include "core/memory.h"

namespace Service::RO {

namespace {

using PostHandlerMap = ankerl::unordered_dense::map<u64, u64>;

constexpr u64 MetadataKeyBit = 1ull << 63;
constexpr u64 MaxPatchedImageSize = static_cast<u64>(128) << 20;
// Patch pages come from the System pool so the application pool keeps the size the game measures
// through svcGetInfo. The pool must keep this much free after the allocation.
constexpr u64 PatchPoolReserve = static_cast<u64>(32) << 20;
constexpr u32 PatchPoolOption = Kernel::KMemoryManager::EncodeOption(
    Kernel::KMemoryManager::Pool::System, Kernel::KMemoryManager::Direction::FromFront);
// Guest permission None, host Read|Execute: the arrangement KProcess::LoadModule uses for the
// NSO patch segments. The pages do not show up as a readable/executable module to the guest.
constexpr Common::MemoryPermission HostPatchPermission =
    Common::MemoryPermission::Read | Common::MemoryPermission::Execute;

struct NroPatchStats {
    const char* status{"ok"};
    std::size_t x18_sites{};
    u64 patch_size{};
    u64 pool_free{};
};

bool RangeIsFree(Kernel::KProcessPageTable& page_table, u64 address, u64 size) {
    Kernel::KMemoryInfo info;
    Kernel::Svc::PageInfo page_info;
    if (R_FAILED(page_table.QueryInfo(std::addressof(info), std::addressof(page_info), address))) {
        return false;
    }
    return info.GetSvcState() == Kernel::Svc::MemoryState::Free && info.GetAddress() <= address &&
           info.GetEndAddress() >= address + size;
}

void FlushGuestCode(u64 address, u64 size) {
    (void)FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(address), size);
}

bool BuildRecoveryTable(Core::NCE::WindowsPatchRecoveryTable& out,
                        const Core::NCE::WindowsPatchRecoveryTable& live,
                        const Core::NCE::WindowsPatchRecoveryTable& added, u64 drop_begin,
                        u64 drop_end) {
    std::size_t expected = 0;
    for (const auto& recipe : live.Entries()) {
        if (recipe.begin >= drop_begin && recipe.begin < drop_end) {
            continue;
        }
        if (!out.AppendForLoad(recipe)) {
            return false;
        }
        ++expected;
    }
    for (const auto& recipe : added.Entries()) {
        if (!out.AppendForLoad(recipe)) {
            return false;
        }
        ++expected;
    }
    out.Seal();
    return out.Entries().size() == expected;
}

bool ApplyImpl(NroNcePatch& out, Kernel::KernelCore& kernel, Kernel::KProcess* process,
               u64 base_address, u64 nro_size, u64 bss_size, u64 rx_size,
               const std::array<u8, 0x20>& module_id, NroPatchStats& stats) {
    if (process == nullptr || !process->IsApplication() || !Settings::IsNceEnabled()) {
        stats.status = "nce-off";
        return false;
    }
    const u64 image_size = nro_size + bss_size;
    if (rx_size == 0 || rx_size > nro_size || image_size < nro_size ||
        !Common::IsAligned(image_size, Kernel::PageSize) || image_size > MaxPatchedImageSize) {
        stats.status = "size";
        return false;
    }

    auto& memory = process->GetMemory();
    std::vector<u8> image(image_size);
    if (!memory.ReadBlock(Common::ProcessAddress{base_address}, image.data(), nro_size)) {
        stats.status = "read";
        return false;
    }

    Kernel::CodeSet::Segment code{};
    code.size = static_cast<u32>(rx_size);

    Core::NCE::Patcher patcher{};
    patcher.SetModuleID(module_id);
    if (!patcher.PatchText(image, code) ||
        patcher.GetPatchMode() != Core::NCE::PatchMode::PostData) {
        stats.status = "mode";
        return false;
    }
    const u64 patch_size = patcher.GetSectionSize();
    const u64 patch_address = base_address + image_size;
    const size_t patch_pages = patch_size / Kernel::PageSize;
    stats.patch_size = patch_size;
    if (patch_pages == 0 || !Common::IsAligned(patch_size, Kernel::PageSize) ||
        patch_address < base_address) {
        stats.status = "section";
        return false;
    }

    auto& page_table = process->GetPageTable();
    if (!RangeIsFree(page_table, patch_address, patch_size + Kernel::PageSize)) {
        stats.status = "space";
        return false;
    }

    auto& memory_manager = kernel.MemoryManager();
    stats.pool_free = memory_manager.GetFreeSize(Kernel::KMemoryManager::Pool::System);
    if (stats.pool_free < patch_size + PatchPoolReserve) {
        stats.status = "pool";
        return false;
    }

    const auto x18_sites = Core::NCE::X18SitePatcher::Collect(image, code);
    stats.x18_sites = x18_sites.size();
    std::vector<u8> original_text(image.begin(), image.begin() + rx_size);

    std::scoped_lock lk{process->GetNcePatchPublishLock()};
    auto handlers = std::make_unique<PostHandlerMap>(process->GetPostHandlers());
    Core::NCE::WindowsPatchRecoveryTable added{};
    if (!patcher.RelocateAndCopy(Common::ProcessAddress{base_address}, code, image, handlers.get(),
                                 std::addressof(added)) ||
        image.size() != image_size + patch_size) {
        stats.status = "relocate";
        return false;
    }
    Core::NCE::X18SitePatcher::Apply(Common::ProcessAddress{base_address}, code, image, x18_sites,
                                     *handlers);
    added.Seal();
    auto recovery = std::make_unique<Core::NCE::WindowsPatchRecoveryTable>();
    if (!BuildRecoveryTable(*recovery, process->GetWindowsPatchRecovery(), added, 0, 0)) {
        stats.status = "recovery";
        return false;
    }

    auto pages = std::make_unique<Kernel::KPageGroup>(
        kernel, std::addressof(kernel.GetSystemSystemResource().GetBlockInfoManager()));
    if (R_FAILED(memory_manager.AllocateAndOpen(pages.get(), patch_pages, PatchPoolOption))) {
        stats.status = "alloc";
        return false;
    }
    if (R_FAILED(page_table.MapPageGroup(patch_address, *pages, Kernel::KMemoryState::AliasCode,
                                         Kernel::KMemoryPermission::UserReadWrite))) {
        pages->Close(kernel);
        stats.status = "map";
        return false;
    }
    const bool written =
        memory.WriteBlock(Common::ProcessAddress{base_address}, image.data(), rx_size) &&
        memory.WriteBlock(Common::ProcessAddress{patch_address}, image.data() + image_size,
                          patch_size);
    const bool hidden = written && R_SUCCEEDED(page_table.SetProcessMemoryPermission(
                                       patch_address, patch_size, Kernel::Svc::MemoryPermission::None));
    if (!hidden) {
        (void)memory.WriteBlock(Common::ProcessAddress{base_address}, original_text.data(),
                                rx_size);
        if (R_SUCCEEDED(page_table.UnmapPageGroup(patch_address, *pages,
                                                  Kernel::KMemoryState::AliasCode))) {
            pages->Close(kernel);
        } else {
            (void)pages.release();
        }
        stats.status = written ? "perm" : "write";
        return false;
    }
    memory.ProtectRegion(page_table.GetImpl(), Common::ProcessAddress{patch_address}, patch_size,
                         HostPatchPermission);
    FlushGuestCode(base_address, rx_size);
    FlushGuestCode(patch_address, patch_size);

    process->PublishNcePatchTables(std::move(handlers), std::move(recovery));

    out.active = true;
    out.text_address = base_address;
    out.text_size = rx_size;
    out.patch_address = patch_address;
    out.patch_size = patch_size;
    out.original_text = std::move(original_text);
    out.pages = std::move(pages);
    return true;
}

} // namespace

bool ApplyNroNcePatch(NroNcePatch& out, Kernel::KernelCore& kernel, Kernel::KProcess* process,
                      u64 base_address, u64 nro_size, u64 bss_size, u64 rx_size,
                      const std::array<u8, 0x20>& module_id) {
    out = {};
    NroPatchStats stats{};
    const bool patched = ApplyImpl(out, kernel, process, base_address, nro_size, bss_size, rx_size,
                                   module_id, stats);
#if SNAPRYU_DIAGNOSTICS
    LOG_INFO(Render_Vulkan,
             "NRO_NCE_PATCH base={:#x} text={:#x} bss={:#x} patch={:#x} x18_sites={} "
             "pool=system pool_free={:#x} guest_perm=none status={}",
             base_address, rx_size, bss_size, stats.patch_size, stats.x18_sites, stats.pool_free,
             stats.status);
#else
    (void)stats;
#endif
    return patched;
}

void RemoveNroNcePatch(NroNcePatch& patch, Kernel::KernelCore& kernel, Kernel::KProcess* process) {
    if (!patch.active || process == nullptr) {
        ReleaseNroNcePatch(patch, kernel);
        return;
    }

    auto& memory = process->GetMemory();
    auto& page_table = process->GetPageTable();
    (void)memory.WriteBlock(Common::ProcessAddress{patch.text_address}, patch.original_text.data(),
                            patch.original_text.size());
    FlushGuestCode(patch.text_address, patch.text_size);

    const u64 drop_begin = patch.text_address;
    const u64 drop_end = patch.patch_address + patch.patch_size;
    bool recovery_rebuilt = false;
    {
        std::scoped_lock lk{process->GetNcePatchPublishLock()};
        const auto& live = process->GetPostHandlers();
        auto handlers = std::make_unique<PostHandlerMap>();
        handlers->reserve(live.size());
        for (const auto& [key, value] : live) {
            const u64 address = key & ~MetadataKeyBit;
            if (address >= drop_begin && address < drop_end) {
                continue;
            }
            handlers->insert_or_assign(key, value);
        }
        Core::NCE::WindowsPatchRecoveryTable none{};
        none.Seal();
        auto recovery = std::make_unique<Core::NCE::WindowsPatchRecoveryTable>();
        recovery_rebuilt = BuildRecoveryTable(*recovery, process->GetWindowsPatchRecovery(), none,
                                              drop_begin, drop_end);
        if (!recovery_rebuilt) {
            recovery.reset();
        }
        process->PublishNcePatchTables(std::move(handlers), std::move(recovery));
    }

    bool unmapped = false;
    if (patch.pages != nullptr) {
        unmapped = R_SUCCEEDED(page_table.UnmapPageGroup(patch.patch_address, *patch.pages,
                                                         Kernel::KMemoryState::AliasCode));
        if (unmapped) {
            patch.pages->Close(kernel);
        } else {
            // Still mapped: keep the pages alive rather than freeing memory the process can reach.
            (void)patch.pages.release();
        }
    }
#if SNAPRYU_DIAGNOSTICS
    LOG_INFO(Render_Vulkan, "NRO_NCE_UNPATCH base={:#x} patch={:#x} recovery={} unmap={}",
             patch.text_address, patch.patch_size, recovery_rebuilt, unmapped);
#else
    (void)recovery_rebuilt;
#endif
    patch = {};
}

void ReleaseNroNcePatch(NroNcePatch& patch, Kernel::KernelCore& kernel) {
    if (patch.pages != nullptr) {
        patch.pages->Close(kernel);
    }
    patch = {};
}

} // namespace Service::RO

#endif
