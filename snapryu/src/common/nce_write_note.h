// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NCE_WRITE_NOTE (b64): the guest store behind an NCE page invalidation, for the caches.
//
// Under NCE a guest store to a page the GPU caches track faults, and RunThread
// (arm_nce_windows.cpp) invalidates the whole 4 KiB page (Memory::InvalidateNCE). The caches then
// see only the page, not the store. b63 found a baked terrain tile marked CpuModified by such a
// page invalidation of its last page (only 0x600 of that page is tile), and re-uploaded from zero
// guest memory over its GPU content: the square. To tell a store into the tile from a store next
// to it, RunThread sets this note (exact fault address, guest pc, instruction word) just before
// its InvalidateNCE call and clears it right after; the texture cache reads it in WriteMemory,
// which runs synchronously on the same thread (GPU::InvalidateRegion -> OnCacheInvalidation).
//
// Diagnostic builds only; release builds compile none of this.
#pragma once

#if SNAPRYU_DIAGNOSTICS
// NCE_WRITE_NOTE (b71): b70 logged every cpu_modified on a baked tile as "source=other". They were
// NCE stores: a store into a page that holds GPU-only content takes the write-back path
// (GPU::InvalidateBeforeCpuWrite), which runs the invalidation on the GPU thread, where this
// thread_local note is empty. That path now carries the note there (`carried`), and RunThread
// also records the guest state of the store: its data registers, x0..x7 with the bytes at each
// (does the store copy from memory the game read as zeros?), lr/fp/sp and the frame-pointer chain,
// so the store can be traced to game code.
#include <array>
#include <atomic>
#include "common/common_types.h"

namespace Common::NceWriteNote {

inline constexpr size_t REGS = 8;        // x0..x7
inline constexpr size_t FRAMES = 4;      // return addresses from the frame-pointer chain
inline constexpr u64 READ_BYTES = 64;    // bytes read at each register (not past its page)
inline constexpr u64 GUEST_PAGE = 0x1000;

enum StoreKind : u8 {
    StoreUnknown = 0, // note not filled
    StoreGpr,         // STR/STP/STUR... of general registers (data = their values)
    StoreZr,          // the data registers are all xzr
    StoreVector,      // SIMD&FP store (data = low and high halves of Vt, Vt2)
    StoreZva,         // DC ZVA (zeroes a block)
    StoreOther,       // not a load/store encoding
};

inline const char* StoreName(u8 kind) {
    switch (kind) {
    case StoreGpr:
        return "gpr";
    case StoreZr:
        return "xzr";
    case StoreVector:
        return "vector";
    case StoreZva:
        return "dc_zva";
    case StoreOther:
        return "other";
    default:
        return "unknown";
    }
}

struct Operands {
    u8 kind = StoreOther;
    u8 rt = 0;
    u8 rt2 = 0;
    bool pair = false;
};

// The data registers of a store instruction word (A64 "loads and stores" class, op0 = x1x0).
[[nodiscard]] constexpr Operands Decode(u32 insn) noexcept {
    if ((insn & 0xFFFFFFE0U) == 0xD50B7420U) {
        return Operands{StoreZva, static_cast<u8>(insn & 31U), 0, false};
    }
    if ((insn & 0x0A000000U) != 0x08000000U) {
        return Operands{};
    }
    const bool vector = ((insn >> 26) & 1U) != 0;
    const bool pair = (insn & 0x3A000000U) == 0x28000000U;
    const u8 rt = static_cast<u8>(insn & 31U);
    const u8 rt2 = static_cast<u8>((insn >> 10) & 31U);
    u8 kind = StoreGpr;
    if (vector) {
        kind = StoreVector;
    } else if (rt == 31 && (!pair || rt2 == 31)) {
        kind = StoreZr;
    }
    return Operands{kind, rt, rt2, pair};
}

struct Note {
    u64 fault = 0; // guest virtual address of the faulting access
    u64 pc = 0;    // guest pc of the faulting instruction
    u32 insn = 0;  // its instruction word (0: not readable)
    bool valid = false;
    // b71 (filled by RunThread before the store runs)
    bool carried = false; // the note reached the GPU thread (write-back path)
    bool filled = false;  // the fields below are set
    u8 store = StoreUnknown;
    u8 dst_zeros = 0; // zero bytes among dst_bytes read at the fault address before the store
    u8 dst_bytes = 0; // 0: not read
    u64 seq = 0;      // 1, 2, ... over all filled notes
    u64 thread = 0;   // guest thread id
    u64 lr = 0;
    u64 fp = 0;
    u64 sp = 0;
    std::array<u64, 4> data{}; // stored data: Rt (low, high for a vector), Rt2 (low, high)
    std::array<u64, REGS> x{};
    std::array<u8, REGS> zeros{};  // zero bytes among bytes[i] read at x[i] before the store
    std::array<u8, REGS> bytes{};  // bytes read at x[i] (0: not mapped or not read)
    std::array<u8, REGS> contig{}; // 1: x[i] and the fault address lie as far apart on the host
    std::array<u64, FRAMES> frames{};
};

inline thread_local Note current{};
inline std::atomic<u64> sequence{0};

} // namespace Common::NceWriteNote
#endif
