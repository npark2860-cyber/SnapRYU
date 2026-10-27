// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "common/common_types.h"

namespace Vulkan {

class MasterSemaphore;

/**
 * Handles a pool of resources protected by fences. Manages resource overflow allocating more
 * resources.
 */
class ResourcePool {
public:
    explicit ResourcePool() = default;
    explicit ResourcePool(MasterSemaphore& master_semaphore, size_t grow_step);

    virtual ~ResourcePool() = default;

    ResourcePool& operator=(ResourcePool&&) noexcept = default;
    ResourcePool(ResourcePool&&) noexcept = default;

    ResourcePool& operator=(const ResourcePool&) = default;
    ResourcePool(const ResourcePool&) = default;

protected:
    size_t CommitResource();

    /// Called when a chunk of resources have to be allocated.
    virtual void Allocate(size_t begin, size_t end) = 0;

    /// Returns true when the GPU has finished with every resource in [begin, end).
    [[nodiscard]] bool IsIdle(size_t begin, size_t end) const;

    /// Updates the known GPU tick used by IsIdle.
    void RefreshGpuTick();

    /// Stops the pool from growing past max_resources entries (0 means no limit). When the pool
    /// is full and every entry is busy, CommitResource waits for the oldest entry instead, as long
    /// as its tick is not newer than the one passed to SetSubmittedTick; otherwise it still grows.
    void SetCapacityLimit(size_t max_resources_) noexcept {
        max_resources = max_resources_;
    }

    /// Highest tick the committing thread has submitted to the GPU.
    void SetSubmittedTick(u64 tick) noexcept {
        submitted_tick = tick;
    }

    /// Lowers the tick of a committed entry once the tick it was really submitted with is known.
    void LowerTick(size_t index, u64 tick) noexcept {
        if (tick < ticks[index]) {
            ticks[index] = tick;
        }
    }

    /// Waits until the GPU is done with every entry in [begin, end). Returns false without
    /// waiting if one of them is newer than the submitted tick, or if the device is lost.
    bool WaitIdle(size_t begin, size_t end);

    /// Number of times CommitResource waited for the GPU instead of growing.
    [[nodiscard]] u64 OverflowWaits() const noexcept {
        return overflow_waits;
    }

    /// Number of times the pool grew past its capacity limit.
    [[nodiscard]] u64 OverflowGrows() const noexcept {
        return overflow_grows;
    }

private:
    /// Manages pool overflow allocating new resources.
    size_t ManageOverflow();

    /// Allocates a new page of resources.
    void Grow();

    MasterSemaphore* master_semaphore{};
    size_t grow_step = 0;     ///< Number of new resources created after an overflow
    size_t hint_iterator = 0; ///< Hint to where the next free resources is likely to be found
    std::vector<u64> ticks;   ///< Ticks for each resource
    size_t max_resources = 0; ///< Capacity limit, 0 when unlimited
    u64 submitted_tick = 0;   ///< Highest tick known to be submitted
    u64 overflow_waits = 0;   ///< Waits done instead of growing
    u64 overflow_grows = 0;   ///< Growths past the capacity limit
};

} // namespace Vulkan
