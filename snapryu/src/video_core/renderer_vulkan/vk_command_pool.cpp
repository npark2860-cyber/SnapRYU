// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <numeric>

#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_command_pool.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

constexpr size_t COMMAND_BUFFER_POOL_SIZE = 4;

// The Qualcomm Windows driver allocates a ~2.2 KiB object on every vkCmdEndRenderPass (and on
// vkCmdBeginRenderPass after other commands). A command buffer holds its objects until it is
// reset; the reset moves them into a per-VkCommandPool cache that later recordings draw from
// first and that keeps up to 1024 of them. Only vkDestroyCommandPool empties that cache. All of
// these objects come from one device-wide size class with 32768 entries, and once that class is
// full the driver's allocator leaks the class lock, so the next thread that touches it deadlocks
// (TotK froze after a few minutes with ~45 pools, 32 of them caching 1024 objects each).
//
// So on that driver the pool count is capped: once every command buffer is busy, committing waits
// for the GPU instead of creating another pool, which bounds the caches to MAX_POOLS * 1024. On top
// of that an upper bound of the objects each pool holds is tracked, and the fullest idle pools are
// recreated if the total ever gets high anyway (very long recordings). Past HARD_LIMIT the fullest
// pool is recreated even if that means waiting for the GPU.
constexpr size_t MAX_POOLS = 8;
constexpr u64 DRIVER_POOL_CACHE = 1024;
constexpr u64 RECYCLE_LIMIT = 20480;
constexpr u64 RECYCLE_TARGET = 12288;
constexpr u64 HARD_LIMIT = 28672;
constexpr size_t MAX_RECYCLE_PER_CALL = 2;

struct CommandPool::Pool {
    vk::CommandPool handle;
    vk::CommandBuffers cmdbufs;
    std::array<u64, COMMAND_BUFFER_POOL_SIZE> held{}; ///< Objects held by each command buffer
    u64 driver_objects = 0; ///< Upper bound of driver render pass objects held by the pool
};

CommandPool::CommandPool(MasterSemaphore& master_semaphore_, const Device& device_)
    : ResourcePool(master_semaphore_, COMMAND_BUFFER_POOL_SIZE), device{device_} {
    const char* const env = std::getenv("EDEN_CMDPOOL_RECYCLE");
    if (env != nullptr && env[0] != '\0') {
        driver_workaround = env[0] != '0';
    } else {
#ifdef _WIN32
        // Only the Windows driver was shown to behave like this.
        driver_workaround = device.GetDriverID() == VK_DRIVER_ID_QUALCOMM_PROPRIETARY;
#endif
    }
    // Waiting relies on timeline semaphores; the fence fallback wakes a single waiter.
    can_wait = driver_workaround && device.HasTimelineSemaphore();
    if (can_wait) {
        SetCapacityLimit(MAX_POOLS * COMMAND_BUFFER_POOL_SIZE);
    }
    LOG_INFO(Render_Vulkan, "Command pool driver workaround {}", driver_workaround ? "on" : "off");
}

CommandPool::~CommandPool() = default;

vk::CommandPool CommandPool::CreateHandle() const {
    // Command buffers are going to be committed, recorded, executed every single usage cycle.
    // They are also going to be reset when committed.
    return device.GetLogical().CreateCommandPool({
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags =
            VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = device.GetGraphicsFamily(),
    });
}

void CommandPool::Allocate(size_t begin, size_t end) {
    Pool& pool = pools.emplace_back();
    pool.handle = CreateHandle();
    pool.cmdbufs = pool.handle.Allocate(COMMAND_BUFFER_POOL_SIZE);
}

VkCommandBuffer CommandPool::Commit() {
    const size_t index = CommitResource();
    last_committed[0] = last_committed[1];
    last_committed[1] = index;
    ++num_committed;
    const auto pool_index = index / COMMAND_BUFFER_POOL_SIZE;
    const auto sub_index = index % COMMAND_BUFFER_POOL_SIZE;
    return pools[pool_index].cmdbufs[sub_index];
}

void CommandPool::SetHeld(size_t index, u64 objects) {
    Pool& pool = pools[index / COMMAND_BUFFER_POOL_SIZE];
    pool.held[index % COMMAND_BUFFER_POOL_SIZE] = objects;
    const u64 in_use = std::accumulate(pool.held.begin(), pool.held.end(), u64{0});
    // New recordings take cached objects before allocating, and a reset keeps at most
    // DRIVER_POOL_CACHE of the objects it releases.
    pool.driver_objects =
        std::min(std::max(pool.driver_objects, in_use), DRIVER_POOL_CACHE + in_use);
}

void CommandPool::Recreate(Pool& pool) {
    // Destroying the old pool frees its command buffers together with the driver objects they
    // cached. The slots keep their ticks, which are already signaled.
    pool.cmdbufs = {};
    pool.handle = CreateHandle();
    pool.cmdbufs = pool.handle.Allocate(COMMAND_BUFFER_POOL_SIZE);
    pool.driver_objects = 0;
    pool.held = {};
}

void CommandPool::Maintain(u64 render_pass_calls, u64 submitted_tick) {
    if (!driver_workaround) {
        return;
    }
    SetSubmittedTick(submitted_tick);
    if (num_committed < 2) {
        return;
    }
    // Commit stamped the pair with CurrentTick(), which runs ahead of this thread by every
    // submission the GPU thread has recorded but this thread has not executed yet. The pair was
    // really submitted with submitted_tick (or dropped during shutdown, leaving nothing pending),
    // so the pool cap and IsIdle can use that tick.
    LowerTick(last_committed[0], submitted_tick);
    LowerTick(last_committed[1], submitted_tick);
    // The pair is committed main buffer first, then upload buffer, which never records render
    // passes. A recording holds at most one object per render pass call plus the one it starts
    // with.
    SetHeld(last_committed[0], render_pass_calls + 1);
    SetHeld(last_committed[1], 1);
    max_recording_calls = std::max(max_recording_calls, render_pass_calls);

    u64 total = 0;
    for (const Pool& pool : pools) {
        total += pool.driver_objects;
    }
    const u64 total_before = total;
    size_t recycled = 0;
    if (total > RECYCLE_LIMIT) {
        bool refreshed = true;
        try {
            RefreshGpuTick();
        } catch (const vk::Exception&) {
            // Device lost; nothing to recycle.
            refreshed = false;
        }
        std::vector<size_t> order(pools.size());
        std::iota(order.begin(), order.end(), size_t{0});
        std::sort(order.begin(), order.end(), [this](size_t lhs, size_t rhs) {
            return pools[lhs].driver_objects > pools[rhs].driver_objects;
        });
        for (const size_t index : order) {
            if (!refreshed || recycled >= MAX_RECYCLE_PER_CALL || total <= RECYCLE_TARGET) {
                break;
            }
            Pool& pool = pools[index];
            if (pool.driver_objects == 0) {
                break;
            }
            const size_t begin = index * COMMAND_BUFFER_POOL_SIZE;
            if (!IsIdle(begin, begin + COMMAND_BUFFER_POOL_SIZE)) {
                continue;
            }
            total -= pool.driver_objects;
            Recreate(pool);
            ++recycled;
        }
        if (refreshed && can_wait && total > HARD_LIMIT) {
            // Every pool that matters is still in flight. Wait for the fullest one rather than
            // let the driver run out; all of its command buffers are already submitted.
            const auto fullest = std::ranges::max_element(pools, {}, &Pool::driver_objects);
            const size_t begin =
                static_cast<size_t>(fullest - pools.begin()) * COMMAND_BUFFER_POOL_SIZE;
            if (WaitIdle(begin, begin + COMMAND_BUFFER_POOL_SIZE)) {
                total -= fullest->driver_objects;
                Recreate(*fullest);
                ++recycled;
                ++hard_waits;
            }
        }
    }
    recycled_pools += recycled;
    recycled_since_log += recycled;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_log >= std::chrono::seconds{60}) {
        LOG_INFO(Render_Vulkan,
                 "CMDPOOL pools={} waits={} grows_past_cap={} driver_objects_est={}->{} "
                 "max_recording_calls={} recycled={} total_recycled={} hard_waits={}",
                 pools.size(), OverflowWaits() - waits_at_log, OverflowGrows() - grows_at_log,
                 total_before, total, max_recording_calls, recycled_since_log, recycled_pools,
                 hard_waits);
        waits_at_log = OverflowWaits();
        grows_at_log = OverflowGrows();
        max_recording_calls = 0;
        recycled_since_log = 0;
        last_log = now;
    }
}

} // namespace Vulkan
