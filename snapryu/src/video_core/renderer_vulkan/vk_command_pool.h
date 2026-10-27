// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <vector>

#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class MasterSemaphore;

class CommandPool final : public ResourcePool {
public:
    explicit CommandPool(MasterSemaphore& master_semaphore_, const Device& device_);
    ~CommandPool() override;

    void Allocate(size_t begin, size_t end) override;

    VkCommandBuffer Commit();

    /// Records the render pass calls of the pair of command buffers committed last, which was
    /// just submitted, and recreates idle pools if the driver objects they hold could get too
    /// many (see the .cpp). submitted_tick is the highest tick submitted so far. Must be called
    /// on the thread that commits, records and submits the command buffers.
    void Maintain(u64 render_pass_calls, u64 submitted_tick);

private:
    struct Pool;

    vk::CommandPool CreateHandle() const;

    /// Sets the objects held by the command buffer at index and updates its pool's estimate.
    void SetHeld(size_t index, u64 objects);

    /// Replaces a pool whose command buffers the GPU is done with by a new one.
    void Recreate(Pool& pool);

    const Device& device;
    std::vector<Pool> pools;
    std::array<size_t, 2> last_committed{};
    size_t num_committed = 0;
    bool driver_workaround = false;
    bool can_wait = false; ///< Waiting for the GPU from Maintain is safe
    u64 recycled_pools = 0;
    u64 recycled_since_log = 0;
    u64 waits_at_log = 0;
    u64 grows_at_log = 0;
    u64 hard_waits = 0;
    u64 max_recording_calls = 0;
    std::chrono::steady_clock::time_point last_log{};
};

} // namespace Vulkan
