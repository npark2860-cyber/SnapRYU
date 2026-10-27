// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <deque>
#include <random>
#include <vector>

#include <boost/container/small_vector.hpp>
#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/pending_downloads.h"
#include "video_core/texture_cache/types.h"

namespace {
using Copies = boost::container::small_vector<VideoCommon::BufferCopy, 4>;
using Batches = std::deque<Copies>;
using ByteMap = std::array<s64, 128>;

ByteMap MapBytes(const Copies& copies) {
    ByteMap map;
    map.fill(-1);
    for (const auto& copy : copies) {
        REQUIRE(copy.size > 0);
        REQUIRE(copy.src_offset + copy.size <= map.size());
        for (u64 i = 0; i < copy.size; ++i) {
            REQUIRE(map[copy.src_offset + i] == -1);
            map[copy.src_offset + i] = static_cast<s64>(copy.dst_offset + i);
        }
    }
    return map;
}
} // namespace

TEST_CASE("Cancelled downloads cannot revive when an address is reused", "[video_core][buffer_cache]") {
    Batches batches;
    batches.emplace_back().push_back({32, 256, 16});
    VideoCommon::CancelPendingDownloads(batches, 32, 16);
    REQUIRE(batches.size() == 1);
    REQUIRE(batches.front().empty());

    // A new generation at the same address must not give the old batch a copy
    // which can publish using, and consume, the new generation's overlap count.
    batches.emplace_back().push_back({32, 512, 16});
    REQUIRE(batches.front().empty());
    batches.pop_front();
    REQUIRE(batches.front().size() == 1);
    REQUIRE(batches.front()[0].src_offset == 32);
    REQUIRE(batches.front()[0].dst_offset == 512);
    REQUIRE(batches.front()[0].size == 16);
}

TEST_CASE("Download cancellation preserves unaffected bytes and staging offsets", "[video_core][buffer_cache]") {
    for (u64 address = 16; address < 64; ++address) {
        for (u64 size = 0; size <= 48; ++size) {
            Batches batches;
            batches.emplace_back().push_back({24, 256, 32});
            batches.emplace_back(); // preserve a batch whose copies were already cancelled
            batches.emplace_back().push_back({20, 1024, 40});
            std::vector<ByteMap> expected;
            for (const auto& batch : batches) {
                expected.push_back(MapBytes(batch));
            }
            for (auto& map : expected) {
                for (u64 i = address; i < address + size; ++i) {
                    map[i] = -1;
                }
            }
            VideoCommon::CancelPendingDownloads(batches, address, size);
            REQUIRE(batches.size() == expected.size());
            for (size_t i = 0; i < batches.size(); ++i) {
                REQUIRE(MapBytes(batches[i]) == expected[i]);
            }
        }
    }
}

TEST_CASE("Repeated cancellations and FIFO retirement preserve per-batch identity", "[video_core][buffer_cache]") {
    std::mt19937 random(0xAD12);
    Batches batches;
    std::deque<ByteMap> expected;
    u64 staging_offset = 256;
    for (size_t step = 0; step < 30000; ++step) {
        const u64 address = random() % 96;
        const u64 size = 1 + random() % (128 - address);
        switch (random() % 3) {
        case 0: {
            auto& copies = batches.emplace_back();
            copies.push_back({address, staging_offset, size});
            expected.push_back(MapBytes(copies));
            staging_offset += 256;
            break;
        }
        case 1:
            VideoCommon::CancelPendingDownloads(batches, address, size);
            for (auto& map : expected) {
                for (u64 i = address; i < address + size; ++i) {
                    map[i] = -1;
                }
            }
            break;
        case 2:
            if (!batches.empty()) {
                REQUIRE(MapBytes(batches.front()) == expected.front());
                batches.pop_front();
                expected.pop_front();
            }
            break;
        }
        REQUIRE(batches.size() == expected.size());
        for (size_t i = 0; i < batches.size(); ++i) {
            REQUIRE(MapBytes(batches[i]) == expected[i]);
        }
    }
}
