// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// EDEN_NCE_IMAGE_READBACK: keep GPU-drawn images across an NCE page invalidation.
//
// A native (NCE) guest store to a tracked page faults, and Memory::InvalidateNCE invalidates the
// whole 4 KiB page. TextureCache::WriteMemory then marks every image on that page CpuModified, and
// the next PrepareImage -> RefreshContents uploads guest RAM over what the GPU drew there.
// Dynarmic reports only the stored bytes (OnCPUWrite), so it reaches only images the store touches.
// The existing EDEN_NCE_GPU_READBACK (#4490) downloads buffers only.
//
//   unset or any other value: off, unchanged.
//   "audit": count page invalidations whose page holds a GPU-modified image; nothing is downloaded.
//   "1":     same count, and download those images into guest RAM before the invalidation
//            (TextureCache::DownloadMemory, as FlushRegion does).

#pragma once

#include <atomic>
#include <cstdlib>
#include <string_view>

#include "common/common_types.h"
#include "common/logging.h"

namespace Tegra::NceImageReadback {

enum class Mode : u32 {
    Off = 0,
    Audit = 1,
    Readback = 2,
};

[[nodiscard]] inline Mode ParseMode(const char* value) {
    if (value == nullptr) {
        return Mode::Off;
    }
    const std::string_view text{value};
    if (text == "1") {
        return Mode::Readback;
    }
    if (text == "audit") {
        return Mode::Audit;
    }
    return Mode::Off;
}

[[nodiscard]] inline const char* ModeName(Mode mode) {
    switch (mode) {
    case Mode::Audit:
        return "audit";
    case Mode::Readback:
        return "readback";
    default:
        return "off";
    }
}

[[nodiscard]] inline Mode GetMode() {
    static const Mode mode = [] {
        const char* const value = std::getenv("EDEN_NCE_IMAGE_READBACK");
        const Mode parsed = ParseMode(value);
        LOG_INFO(HW_GPU, "NCE_IMAGE_READBACK mode={} value={}", ModeName(parsed),
                 value != nullptr ? value : "");
        return parsed;
    }();
    return mode;
}

// Log the first four events, then every power of two.
[[nodiscard]] inline bool Sample(u64 count) {
    return count <= 4 || (count & (count - 1)) == 0;
}

struct Counters {
    std::atomic<u64> checks{0};    // preserving page invalidations looked at
    std::atomic<u64> hits{0};      // page held an image with GPU-written contents
    std::atomic<u64> completed{0}; // downloads finished ("1" only)
    std::atomic<u64> wait_us{0};   // CPU time spent waiting for those downloads
};

[[nodiscard]] inline Counters& Get() {
    static Counters counters;
    return counters;
}

} // namespace Tegra::NceImageReadback
