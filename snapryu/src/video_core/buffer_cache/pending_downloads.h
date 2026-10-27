// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

#include "common/common_types.h"

namespace VideoCommon {

// Pending copies use src_offset as a guest address and dst_offset as a staging
// offset. Retire cancelled bytes from the copies themselves, so a later download
// at the same address cannot reactivate an old copy through shared overlap counts.
// The caller holds the buffer-cache mutex. Keep empty batches in the FIFO: each
// still owns a staging allocation and corresponds to an outstanding fence batch.
template <typename DownloadBatches>
void CancelPendingDownloads(DownloadBatches& batches, DAddr address, u64 size) {
    if (size == 0) {
        return;
    }
    const DAddr cancel_end = address + size;
    for (auto& downloads : batches) {
        const size_t original_count = downloads.size();
        for (size_t i = 0; i < original_count; ++i) {
            // Appending a suffix may reallocate downloads; do not keep a reference.
            const auto copy = downloads[i];
            const DAddr copy_start = static_cast<DAddr>(copy.src_offset);
            const DAddr copy_end = copy_start + copy.size;
            const DAddr cut_start = (std::max)(copy_start, address);
            const DAddr cut_end = (std::min)(copy_end, cancel_end);
            if (cut_start >= cut_end) {
                continue;
            }
            const u64 prefix_size = cut_start - copy_start;
            const u64 suffix_size = copy_end - cut_end;
            if (prefix_size == 0) {
                downloads[i].src_offset = cut_end;
                downloads[i].dst_offset = copy.dst_offset + (cut_end - copy_start);
                downloads[i].size = suffix_size;
            } else {
                downloads[i].size = prefix_size;
                if (suffix_size != 0) {
                    auto suffix = copy;
                    suffix.src_offset = cut_end;
                    suffix.dst_offset += cut_end - copy_start;
                    suffix.size = suffix_size;
                    downloads.push_back(suffix);
                }
            }
        }
        downloads.erase(std::remove_if(downloads.begin(), downloads.end(),
                                       [](const auto& copy) { return copy.size == 0; }),
                        downloads.end());
    }
}

} // namespace VideoCommon
