// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <iterator>
#include <optional>

#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

ResourcePool::ResourcePool(MasterSemaphore& master_semaphore_, size_t grow_step_)
    : master_semaphore{&master_semaphore_}, grow_step{grow_step_} {}

size_t ResourcePool::CommitResource() {
    const auto search = [this](size_t begin, size_t end, u64 gpu_tick) -> std::optional<size_t> {
        for (size_t iterator = begin; iterator < end; ++iterator) {
            if (gpu_tick >= ticks[iterator]) {
                ticks[iterator] = master_semaphore->CurrentTick();
                return iterator;
            }
        }
        return std::nullopt;
    };
    const auto find_free = [&](u64 gpu_tick) -> std::optional<size_t> {
        std::optional<size_t> result = search(hint_iterator, ticks.size(), gpu_tick);
        if (!result) {
            result = search(0, hint_iterator, gpu_tick);
        }
        return result;
    };
    std::optional<size_t> found = find_free(master_semaphore->KnownGpuTick());
    if (!found) {
        master_semaphore->Refresh();
        found = find_free(master_semaphore->KnownGpuTick());
        if (!found) {
            // Both searches failed, the pool is full; handle it.
            const size_t free_resource = ManageOverflow();

            ticks[free_resource] = master_semaphore->CurrentTick();
            found = free_resource;
        }
    }
    // Free iterator is hinted to the resource after the one that's been committed.
    hint_iterator = (*found + 1) % ticks.size();
    return *found;
}

size_t ResourcePool::ManageOverflow() {
    if (max_resources != 0 && ticks.size() >= max_resources) {
        const auto oldest = std::ranges::min_element(ticks);
        // A tick that is already submitted gets signaled without any work from this thread, so
        // waiting for it cannot deadlock.
        if (*oldest <= submitted_tick) {
            try {
                master_semaphore->Wait(*oldest);
                ++overflow_waits;
                return static_cast<size_t>(std::distance(ticks.begin(), oldest));
            } catch (const vk::Exception&) {
                // Device lost; fall back to growing.
            }
        }
        ++overflow_grows;
    }
    const size_t old_capacity = ticks.size();
    Grow();

    // The last entry is guaranteed to be free, since it's the first element of the freshly
    // allocated resources.
    return old_capacity;
}

bool ResourcePool::IsIdle(size_t begin, size_t end) const {
    const u64 gpu_tick = master_semaphore->KnownGpuTick();
    for (size_t index = begin; index < end && index < ticks.size(); ++index) {
        if (ticks[index] > gpu_tick) {
            return false;
        }
    }
    return true;
}

bool ResourcePool::WaitIdle(size_t begin, size_t end) {
    u64 newest = 0;
    for (size_t index = begin; index < end && index < ticks.size(); ++index) {
        newest = (std::max)(newest, ticks[index]);
    }
    if (newest > submitted_tick) {
        return false;
    }
    try {
        master_semaphore->Wait(newest);
    } catch (const vk::Exception&) {
        return false;
    }
    return true;
}

void ResourcePool::RefreshGpuTick() {
    master_semaphore->Refresh();
}

void ResourcePool::Grow() {
    const size_t old_capacity = ticks.size();
    ticks.resize(old_capacity + grow_step);
    Allocate(old_capacity, old_capacity + grow_step);
}

} // namespace Vulkan
