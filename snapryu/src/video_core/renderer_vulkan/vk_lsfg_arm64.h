// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>
#include "video_core/vulkan_common/vulkan_wrapper.h"
namespace Layout { struct FramebufferLayout; }
namespace Vulkan {
class Device;
class MemoryAllocator;
class Scheduler;
class PresentManager;
class Swapchain;
class BlitScreen;
class SnapGoCapture;
struct Frame;
class Arm64Lsfg {
public:
    Arm64Lsfg(VkInstance instance, const Device& device, MemoryAllocator& allocator,
              Scheduler& scheduler, PresentManager& present, Swapchain& swapchain);
    ~Arm64Lsfg();
    bool Process(Frame* frame, const Layout::FramebufferLayout& layout, BlitScreen& blit,
                 VkExtent2D guest, SnapGoCapture* capture = nullptr, bool remote_only = false);
    void Reset();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Vulkan
