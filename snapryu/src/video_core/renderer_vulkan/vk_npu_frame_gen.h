// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include "common/common_types.h"

namespace Layout {
struct FramebufferLayout;
}

namespace Vulkan {
class Device;
class MemoryAllocator;
class Scheduler;
class PresentManager;
class Swapchain;
struct Frame;

// Owns only presentation-side images. No guest query, UB_KEEP, or cache ownership
// semantics are changed by frame generation.
class NpuFrameGenerator {
  public:
    NpuFrameGenerator(const Device& device, MemoryAllocator& allocator, Scheduler& scheduler,
                      PresentManager& present, Swapchain& swapchain);
    ~NpuFrameGenerator();
    NpuFrameGenerator(const NpuFrameGenerator&) = delete;
    NpuFrameGenerator& operator=(const NpuFrameGenerator&) = delete;

    // current has been rendered normally but not submitted for presentation.
    // true means this module has flushed and presented it; false is passthrough.
    bool Process(Frame* current, const Layout::FramebufferLayout& layout);
    void Reset();
    void Shutdown();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Vulkan
