// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>
#include <vector>
#include <ankerl/unordered_dense.h>

#include "common/common_types.h"
#include "core/hle/kernel/code_set.h"
#include "core/hle/kernel/k_typed_address.h"

namespace Core::NCE {

struct X18FallbackSite {
    u32 text_word_index{};
    u32 instruction{};
    bool may_access_memory{};
};

// Reuse the existing process-owned post-handler map without colliding with ordinary guest PCs.
// Switch user VAs are below bit 63, so tagged keys are never looked up by the normal RunThread
// post-handler path. The upper value word identifies IMP-006 metadata; the lower word preserves the
// original guest instruction replaced by BRK #0xF000.
using X18FallbackMetadata = ankerl::unordered_dense::map<u64, u64>;

class X18SitePatcher {
public:
    static constexpr u32 BreakpointImmediate = 0xF000u;
    static constexpr u32 BreakpointInstruction =
        0xD4200000u | (BreakpointImmediate << 5);
    static constexpr u64 MetadataKeyBit = 1ull << 63;
    static constexpr u32 MetadataMagic = 0x58313836u; // "X186"
    static constexpr u64 MetadataMayAccessMemoryBit = 1ull << 63;

    [[nodiscard]] static constexpr u64 MetadataKey(u64 runtime_pc) noexcept {
        return runtime_pc | MetadataKeyBit;
    }

    [[nodiscard]] static constexpr u64 MetadataValue(u32 instruction,
                                                     bool may_access_memory) noexcept {
        return (static_cast<u64>(MetadataMagic) << 32) | instruction |
               (may_access_memory ? MetadataMayAccessMemoryBit : 0);
    }

    [[nodiscard]] static std::vector<X18FallbackSite> Collect(
        std::span<const u8> program_image, const Kernel::CodeSet::Segment& code);

    static void Apply(Common::ProcessAddress load_base, const Kernel::CodeSet::Segment& code,
                      std::vector<u8>& program_image, std::span<const X18FallbackSite> sites,
                      X18FallbackMetadata& metadata);
};

static_assert(X18SitePatcher::BreakpointInstruction == 0xD43E0000u);

} // namespace Core::NCE
