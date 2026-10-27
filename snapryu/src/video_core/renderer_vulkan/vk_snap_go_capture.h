// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>
#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Layout { struct FramebufferLayout; }
namespace Vulkan {
class Device;
class MemoryAllocator;
class Scheduler;
struct Frame;

// Renderer-owned GPU slots; a separate owner observes completion and paces CPU frames.
// Stop the worker before destroying Scheduler, retain slots until the device is idle.
class SnapGoCapture {
public:
    SnapGoCapture(const Device&, MemoryAllocator&, Scheduler&);
    ~SnapGoCapture();
    void Single(Frame*, VkFormat, const Layout::FramebufferLayout&) noexcept;
    void Pair(Frame* generated, Frame* original, VkFormat,
              const Layout::FramebufferLayout&, s64 half_interval_ns) noexcept;
    void Reset() noexcept;
    void Stop() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
