// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/arm/nce/x18_site_patcher.h"

#include "core/arm/nce/x18_fallback.h"

namespace Core::NCE {

namespace {

constexpr u32 ModuleCodeIndex = 0x24 / sizeof(u32);

} // namespace

std::vector<X18FallbackSite> X18SitePatcher::Collect(
    std::span<const u8> program_image, const Kernel::CodeSet::Segment& code) {
    std::vector<X18FallbackSite> result;

#if defined(_WIN32)
    const auto text = program_image.subspan(code.offset, code.size);
    const auto words = std::span<const u32>{reinterpret_cast<const u32*>(text.data()),
                                            text.size() / sizeof(u32)};

    for (u32 i = ModuleCodeIndex; i < static_cast<u32>(words.size()); ++i) {
        const u32 instruction = words[i];
        if (X18Fallback::ClassifyInstruction(instruction) ==
            X18InstructionClass::SupportedOrdinary) {
            result.push_back({
                .text_word_index = i,
                .instruction = instruction,
                .may_access_memory = X18Fallback::MayAccessGuestMemory(instruction),
            });
        }
    }
#else
    (void)program_image;
    (void)code;
#endif

    return result;
}

void X18SitePatcher::Apply(Common::ProcessAddress load_base,
                           const Kernel::CodeSet::Segment& code,
                           std::vector<u8>& program_image,
                           std::span<const X18FallbackSite> sites,
                           X18FallbackMetadata& metadata) {
#if defined(_WIN32)
    auto text = std::span{program_image}.subspan(code.offset, code.size);
    auto words = std::span<u32>{reinterpret_cast<u32*>(text.data()), text.size() / sizeof(u32)};

    for (const auto& site : sites) {
        if (site.text_word_index >= words.size()) {
            continue;
        }

        const u64 runtime_pc = GetInteger(load_base) + GetInteger(code.addr) +
                               static_cast<u64>(site.text_word_index) * sizeof(u32);

        // RelocateAndCopy may already have replaced this original x18 instruction with a branch
        // to a generated Windows-safe trampoline. Never overwrite that branch with BRK #0xF000.
        if (words[site.text_word_index] != site.instruction) {
            continue;
        }

        // A dedicated generated NCE trampoline owns this site when RelocateAndCopy already
        // registered an ordinary (untagged) post-handler entry for the same guest PC. P2A uses
        // this precedence for safe bitfield x18 instructions; leave all other ordinary x18
        // instructions on the existing BRK #0xF000 fallback path.
        if (metadata.find(runtime_pc) != metadata.end()) {
            continue;
        }

        words[site.text_word_index] = BreakpointInstruction;
        metadata.insert_or_assign(MetadataKey(runtime_pc),
                                  MetadataValue(site.instruction, site.may_access_memory));
    }
#else
    (void)load_base;
    (void)code;
    (void)program_image;
    (void)sites;
    (void)metadata;
#endif
}

} // namespace Core::NCE
