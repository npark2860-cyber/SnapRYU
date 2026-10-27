// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/vulkan_common/vk_enum_string_helper.h"
#include "video_core/vulkan_common/vma.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

#ifdef _WIN32
#include <malloc.h>
#endif

namespace Vulkan::vk {

namespace {

// Live object counts for leak diagnostics (logged with the GPU memory budget).
template <typename T>
std::atomic<long long> g_live{0};

#ifdef _WIN32
// Diagnostic host allocation callbacks for the logical device (off unless EDEN_VK_HOST_ALLOC=1).
// The Qualcomm Windows driver builds its small-object pool on top of these, so every pool chunk
// allocation passes through here. That lets us see who keeps a pool size class full: when the
// ~2.3 KiB class is full, the driver's allocator never releases the class lock, and the next
// thread allocating from it (a pipeline worker in vkCreateShaderModule) deadlocks.
struct HostAllocHeader {
    u64 size;
    u32 pad;
    u32 tracked;
};
static_assert(sizeof(HostAllocHeader) == 16);

constexpr size_t TRACK_MIN = 2048;
constexpr size_t TRACK_MAX = 2560;
constexpr size_t MAX_SITES = 32;

struct HostAllocSite {
    uintptr_t rva;
    uintptr_t caller_rva;
    long long count;
};

std::atomic<long long> g_host_live{0};
std::atomic<long long> g_host_bytes{0};
std::atomic<long long> g_host_tracked_live{0};
std::atomic<long long> g_host_tracked_total{0};
std::array<std::atomic<long long>, 5> g_host_tracked_scope{};
std::mutex g_host_site_mutex;
std::array<HostAllocSite, MAX_SITES> g_host_sites{};

std::pair<uintptr_t, uintptr_t> ExeRange() {
    static const auto range = [] {
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        return std::pair{base, base + nt->OptionalHeader.SizeOfImage};
    }();
    return range;
}

void RecordTrackedSite() {
    void* frames[48];
    const USHORT count = RtlCaptureStackBackTrace(1, 48, frames, nullptr);
    const auto [base, end] = ExeRange();
    // The first frames are this callback, then the driver. The first Eden frame after the
    // driver is the Vulkan call site; the next one is its caller.
    bool in_driver = false;
    uintptr_t rva = 0;
    uintptr_t caller_rva = 0;
    for (USHORT i = 0; i < count; ++i) {
        const auto addr = reinterpret_cast<uintptr_t>(frames[i]);
        const bool in_exe = addr >= base && addr < end;
        if (!in_driver) {
            in_driver = !in_exe;
            continue;
        }
        if (!in_exe) {
            continue;
        }
        if (rva == 0) {
            rva = addr - base;
        } else {
            caller_rva = addr - base;
            break;
        }
    }
    std::scoped_lock lock{g_host_site_mutex};
    for (auto& site : g_host_sites) {
        if (site.count != 0 && site.rva == rva && site.caller_rva == caller_rva) {
            ++site.count;
            return;
        }
    }
    for (auto& site : g_host_sites) {
        if (site.count == 0) {
            site = {rva, caller_rva, 1};
            return;
        }
    }
}

void* HostAllocate(size_t size, size_t alignment, VkSystemAllocationScope scope) {
    const size_t align = std::max<size_t>(alignment, 16);
    auto* const base = static_cast<u8*>(_aligned_malloc(size + align, align));
    if (base == nullptr) {
        return nullptr;
    }
    u8* const ptr = base + align;
    const bool tracked = size >= TRACK_MIN && size <= TRACK_MAX;
    const HostAllocHeader header{size, static_cast<u32>(align), tracked ? 1U : 0U};
    std::memcpy(ptr - sizeof(HostAllocHeader), &header, sizeof(HostAllocHeader));
    ++g_host_live;
    g_host_bytes += static_cast<long long>(size);
    if (tracked) {
        ++g_host_tracked_live;
        ++g_host_tracked_total;
        if (static_cast<size_t>(scope) < g_host_tracked_scope.size()) {
            ++g_host_tracked_scope[static_cast<size_t>(scope)];
        }
        RecordTrackedSite();
    }
    return ptr;
}

void HostFree(void* memory) {
    if (memory == nullptr) {
        return;
    }
    auto* const ptr = static_cast<u8*>(memory);
    HostAllocHeader header;
    std::memcpy(&header, ptr - sizeof(HostAllocHeader), sizeof(HostAllocHeader));
    --g_host_live;
    g_host_bytes -= static_cast<long long>(header.size);
    if (header.tracked != 0) {
        --g_host_tracked_live;
    }
    _aligned_free(ptr - header.pad);
}

void* VKAPI_PTR HostAllocationCallback(void*, size_t size, size_t alignment,
                                       VkSystemAllocationScope scope) {
    return HostAllocate(size, alignment, scope);
}

void* VKAPI_PTR HostReallocationCallback(void*, void* original, size_t size, size_t alignment,
                                         VkSystemAllocationScope scope) {
    if (original == nullptr) {
        return HostAllocate(size, alignment, scope);
    }
    if (size == 0) {
        HostFree(original);
        return nullptr;
    }
    HostAllocHeader header;
    std::memcpy(&header, static_cast<u8*>(original) - sizeof(HostAllocHeader),
                sizeof(HostAllocHeader));
    void* const result = HostAllocate(size, alignment, scope);
    if (result == nullptr) {
        return nullptr;
    }
    std::memcpy(result, original, std::min<size_t>(header.size, size));
    HostFree(original);
    return result;
}

void VKAPI_PTR HostFreeCallback(void*, void* memory) {
    HostFree(memory);
}

const VkAllocationCallbacks* DeviceHostAllocator() {
    static const VkAllocationCallbacks callbacks{
        .pUserData = nullptr,
        .pfnAllocation = HostAllocationCallback,
        .pfnReallocation = HostReallocationCallback,
        .pfnFree = HostFreeCallback,
        .pfnInternalAllocation = nullptr,
        .pfnInternalFree = nullptr,
    };
    static const bool enabled = [] {
        // Off by default: stack capture on every tracked allocation slows the Vulkan worker,
        // which made the driver pool fill several times faster in testing.
        const char* const v = std::getenv("EDEN_VK_HOST_ALLOC");
        return v != nullptr && v[0] == '1';
    }();
    return enabled ? &callbacks : nullptr;
}

std::string HostAllocSummary() {
    if (DeviceHostAllocator() == nullptr) {
        return {};
    }
    std::array<HostAllocSite, MAX_SITES> sites;
    {
        std::scoped_lock lock{g_host_site_mutex};
        sites = g_host_sites;
    }
    std::sort(sites.begin(), sites.end(),
              [](const HostAllocSite& a, const HostAllocSite& b) { return a.count > b.count; });
    std::string top;
    for (size_t i = 0; i < 6 && sites[i].count != 0; ++i) {
        top += fmt::format(" {:x}<{:x}:{}", sites[i].rva, sites[i].caller_rva, sites[i].count);
    }
    return fmt::format(" host_live={} host_mib={} host2k_live={} host2k_total={} "
                       "host2k_scope={}/{}/{}/{}/{} host2k_sites=[{} ]",
                       g_host_live.load(), g_host_bytes.load() >> 20, g_host_tracked_live.load(),
                       g_host_tracked_total.load(), g_host_tracked_scope[0].load(),
                       g_host_tracked_scope[1].load(), g_host_tracked_scope[2].load(),
                       g_host_tracked_scope[3].load(), g_host_tracked_scope[4].load(), top);
}
#else
const VkAllocationCallbacks* DeviceHostAllocator() {
    return nullptr;
}

std::string HostAllocSummary() {
    return {};
}
#endif


template <typename Func>
void SortPhysicalDevices(std::vector<VkPhysicalDevice>& devices, const InstanceDispatch& dld,
                         Func&& func) {
    // Calling GetProperties calls Vulkan more than needed. But they are supposed to be cheap
    // functions.
    std::stable_sort(devices.begin(), devices.end(),
                     [&dld, &func](VkPhysicalDevice lhs, VkPhysicalDevice rhs) {
                         return func(vk::PhysicalDevice(lhs, dld).GetProperties(),
                                     vk::PhysicalDevice(rhs, dld).GetProperties());
                     });
}

void SortPhysicalDevicesPerVendor(std::vector<VkPhysicalDevice>& devices,
                                  const InstanceDispatch& dld,
                                  std::initializer_list<u32> vendor_ids) {
    for (auto it = vendor_ids.end(); it != vendor_ids.begin();) {
        --it;
        SortPhysicalDevices(devices, dld, [id = *it](const auto& lhs, const auto& rhs) {
            return lhs.vendorID == id && rhs.vendorID != id;
        });
    }
}

bool IsMicrosoftDozen(const char* device_name) {
    return std::strstr(device_name, "Microsoft") != nullptr;
}

void SortPhysicalDevices(std::vector<VkPhysicalDevice>& devices, const InstanceDispatch& dld) {
    // Sort by name, this will set a base and make GPUs with higher numbers appear first
    // (e.g. GTX 1650 will intentionally be listed before a GTX 1080).
    SortPhysicalDevices(devices, dld, [](const auto& lhs, const auto& rhs) {
        return std::string_view{lhs.deviceName} > std::string_view{rhs.deviceName};
    });
    // Prefer discrete over non-discrete
    SortPhysicalDevices(devices, dld, [](const auto& lhs, const auto& rhs) {
        return lhs.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
               rhs.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    });
    // Prefer Nvidia over AMD, AMD over Intel, Intel over the rest.
    SortPhysicalDevicesPerVendor(devices, dld, {0x10DE, 0x1002, 0x8086});
    // Demote Microsoft's Dozen devices to the bottom.
    SortPhysicalDevices(
        devices, dld,
        [](const VkPhysicalDeviceProperties& lhs, const VkPhysicalDeviceProperties& rhs) {
            return IsMicrosoftDozen(rhs.deviceName) && !IsMicrosoftDozen(lhs.deviceName);
        });
}

template <typename T>
bool Proc(T& result, const InstanceDispatch& dld, const char* proc_name,
          VkInstance instance = nullptr) noexcept {
    result = reinterpret_cast<T>(dld.vkGetInstanceProcAddr(instance, proc_name));
    return result != nullptr;
}

template <typename T>
void Proc(T& result, const DeviceDispatch& dld, const char* proc_name, VkDevice device) noexcept {
    result = reinterpret_cast<T>(dld.vkGetDeviceProcAddr(device, proc_name));
}

void Load(VkDevice device, DeviceDispatch& dld) noexcept {
#define X(name) Proc(dld.name, dld, #name, device)
    X(vkAcquireNextImageKHR);
    X(vkAllocateCommandBuffers);
    X(vkAllocateDescriptorSets);
    X(vkAllocateMemory);
    X(vkBeginCommandBuffer);
    X(vkBindBufferMemory);
    X(vkBindImageMemory);
    X(vkCmdBeginConditionalRenderingEXT);
    X(vkCmdBeginQuery);
    X(vkCmdBeginRenderPass);
    X(vkCmdBeginTransformFeedbackEXT);
    X(vkCmdBeginDebugUtilsLabelEXT);
    X(vkCmdBindDescriptorSets);
    X(vkCmdBindIndexBuffer);
    X(vkCmdBindPipeline);
    X(vkCmdBindTransformFeedbackBuffersEXT);
    X(vkCmdBindVertexBuffers);
    X(vkCmdBlitImage);
    X(vkCmdClearAttachments);
    X(vkCmdClearColorImage);
    X(vkCmdCopyBuffer);
    X(vkCmdCopyBufferToImage);
    X(vkCmdCopyImage);
    X(vkCmdCopyImageToBuffer);
    X(vkCmdCopyQueryPoolResults);
    X(vkCmdDispatch);
    X(vkCmdDispatchIndirect);
    X(vkCmdDraw);
    X(vkCmdDrawIndexed);
    X(vkCmdDrawIndirect);
    X(vkCmdDrawIndexedIndirect);
    X(vkCmdDrawIndirectCount);
    X(vkCmdDrawIndexedIndirectCount);
    X(vkCmdDrawIndirectByteCountEXT);
    X(vkCmdEndConditionalRenderingEXT);
    X(vkCmdEndQuery);
    X(vkCmdEndRenderPass);
    X(vkCmdEndTransformFeedbackEXT);
    X(vkCmdEndDebugUtilsLabelEXT);
    X(vkCmdFillBuffer);
    X(vkCmdPipelineBarrier);
    X(vkCmdPipelineBarrier2);
    X(vkCmdPushConstants);
    X(vkCmdPushDescriptorSetWithTemplateKHR);
    X(vkCmdSetBlendConstants);
    X(vkCmdSetDepthBias);
    X(vkCmdSetDepthBias2EXT);
    X(vkCmdSetDepthBounds);
    X(vkCmdSetEvent);
    X(vkCmdSetScissor);
    X(vkCmdSetStencilCompareMask);
    X(vkCmdSetStencilReference);
    X(vkCmdSetStencilWriteMask);
    X(vkCmdSetViewport);
    X(vkCmdWaitEvents);
    X(vkCmdBindVertexBuffers2EXT);
    X(vkCmdSetCullModeEXT);
    X(vkCmdSetDepthBoundsTestEnableEXT);
    X(vkCmdSetDepthCompareOpEXT);
    X(vkCmdSetDepthTestEnableEXT);
    X(vkCmdSetDepthWriteEnableEXT);
    X(vkCmdSetPrimitiveRestartEnableEXT);
    X(vkCmdSetRasterizerDiscardEnableEXT);
    X(vkCmdSetAlphaToCoverageEnableEXT);
    X(vkCmdSetAlphaToOneEnableEXT);
    X(vkCmdSetConservativeRasterizationModeEXT);
    X(vkCmdSetLineRasterizationModeEXT);
    X(vkCmdSetLineStippleEnableEXT);
    X(vkCmdSetDepthBiasEnableEXT);
    X(vkCmdSetLogicOpEnableEXT);
    X(vkCmdSetDepthClampEnableEXT);
    X(vkCmdSetFrontFaceEXT);
    X(vkCmdSetLogicOpEXT);
    X(vkCmdSetPatchControlPointsEXT);
    X(vkCmdSetLineWidth);
    X(vkCmdSetPrimitiveTopologyEXT);
    X(vkCmdSetStencilOpEXT);
    X(vkCmdSetStencilTestEnableEXT);
    X(vkCmdSetVertexInputEXT);
    X(vkCmdSetColorWriteMaskEXT);
    X(vkCmdSetColorWriteEnableEXT);
    X(vkCmdSetColorBlendEnableEXT);
    X(vkCmdSetColorBlendEquationEXT);
    X(vkCmdResetQueryPool);
    X(vkCmdResolveImage);
    X(vkCreateBuffer);
    X(vkCreateBufferView);
    X(vkCreateCommandPool);
    X(vkCreateComputePipelines);
    X(vkCreateDescriptorPool);
    X(vkCreateDescriptorSetLayout);
    X(vkCreateDescriptorUpdateTemplate);
    X(vkCreateEvent);
    X(vkCreateFence);
    X(vkCreateFramebuffer);
    X(vkCreateGraphicsPipelines);
    X(vkCreateImage);
    X(vkCreateImageView);
    X(vkCreatePipelineCache);
    X(vkCreatePipelineLayout);
    X(vkCreateQueryPool);
    X(vkCreateRenderPass);
    X(vkCreateRenderPass2);
    X(vkCreateSampler);
    X(vkCreateSemaphore);
    X(vkCreateShaderModule);
    X(vkCreateSwapchainKHR);
    X(vkDestroyBuffer);
    X(vkDestroyBufferView);
    X(vkDestroyCommandPool);
    X(vkDestroyDescriptorPool);
    X(vkDestroyDescriptorSetLayout);
    X(vkDestroyDescriptorUpdateTemplate);
    X(vkDestroyEvent);
    X(vkDestroyFence);
    X(vkDestroyFramebuffer);
    X(vkDestroyImage);
    X(vkDestroyImageView);
    X(vkDestroyPipeline);
    X(vkDestroyPipelineCache);
    X(vkDestroyPipelineLayout);
    X(vkDestroyQueryPool);
    X(vkDestroyRenderPass);
    X(vkDestroySampler);
    X(vkDestroySemaphore);
    X(vkDestroyShaderModule);
    X(vkDestroySwapchainKHR);
    X(vkDeviceWaitIdle);
    X(vkEndCommandBuffer);
    X(vkFreeCommandBuffers);
    X(vkFreeDescriptorSets);
    X(vkFreeMemory);
    X(vkGetBufferMemoryRequirements2);
    X(vkGetBufferMemoryRequirements);
    X(vkFlushMappedMemoryRanges);
    X(vkInvalidateMappedMemoryRanges);
    X(vkCmdWriteTimestamp);
    X(vkGetDeviceQueue);
    X(vkGetEventStatus);
    X(vkGetFenceStatus);
    X(vkGetImageMemoryRequirements);
    X(vkGetPipelineCacheData);
    X(vkGetMemoryFdKHR);
#ifdef _WIN32
    X(vkGetMemoryWin32HandleKHR);
#endif
    X(vkGetQueryPoolResults);
    X(vkGetPipelineExecutablePropertiesKHR);
    X(vkGetPipelineExecutableStatisticsKHR);
    X(vkGetSemaphoreCounterValue);
    X(vkMapMemory);
    X(vkQueueSubmit);
    X(vkQueueSubmit2);
    X(vkResetFences);
    X(vkResetQueryPool);
    X(vkSetDebugUtilsObjectNameEXT);
    X(vkSetDebugUtilsObjectTagEXT);
    X(vkUnmapMemory);
    X(vkUpdateDescriptorSetWithTemplate);
    X(vkUpdateDescriptorSets);
    X(vkGetBufferDeviceAddress);
    X(vkGetDescriptorSetLayoutSizeEXT);
    X(vkGetDescriptorSetLayoutBindingOffsetEXT);
    X(vkGetDescriptorEXT);
    X(vkCmdBindDescriptorBuffersEXT);
    X(vkCmdSetDescriptorBufferOffsetsEXT);
    X(vkWaitForFences);
    X(vkWaitSemaphores);

    // Support for timeline semaphores is mandatory in Vulkan 1.2
    if (!dld.vkGetSemaphoreCounterValue) {
        Proc(dld.vkGetSemaphoreCounterValue, dld, "vkGetSemaphoreCounterValueKHR", device);
        Proc(dld.vkWaitSemaphores, dld, "vkWaitSemaphoresKHR", device);
    }

    // Support for host query reset is mandatory in Vulkan 1.2
    if (!dld.vkResetQueryPool) {
        Proc(dld.vkResetQueryPool, dld, "vkResetQueryPoolEXT", device);
    }

    // Support for draw indirect with count is optional in Vulkan 1.2
    if (!dld.vkCmdDrawIndirectCount) {
        Proc(dld.vkCmdDrawIndirectCount, dld, "vkCmdDrawIndirectCountKHR", device);
        Proc(dld.vkCmdDrawIndexedIndirectCount, dld, "vkCmdDrawIndexedIndirectCountKHR", device);
    }

    // Synchronization2 is core in Vulkan 1.3, otherwise requires VK_KHR_synchronization2
    if (!dld.vkCmdPipelineBarrier2) {
        Proc(dld.vkCmdPipelineBarrier2, dld, "vkCmdPipelineBarrier2KHR", device);
    }
    if (!dld.vkQueueSubmit2) {
        Proc(dld.vkQueueSubmit2, dld, "vkQueueSubmit2KHR", device);
    }

    if (!dld.vkCreateRenderPass2) {
        Proc(dld.vkCreateRenderPass2, dld, "vkCreateRenderPass2KHR", device);
    }
#undef X
}

template <typename T>
void SetObjectName(const DeviceDispatch* dld, VkDevice device, T handle, VkObjectType type,
                   const char* name) {
    const VkDebugUtilsObjectNameInfoEXT name_info{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        .pNext = nullptr,
        .objectType = type,
        .objectHandle = reinterpret_cast<u64>(handle),
        .pObjectName = name,
    };
    if (dld->vkSetDebugUtilsObjectNameEXT) {
        Check(dld->vkSetDebugUtilsObjectNameEXT(device, &name_info));
    }
}

} // Anonymous namespace

bool Load(InstanceDispatch& dld) noexcept {
#define X(name) Proc(dld.name, dld, #name)
    return X(vkCreateInstance) && X(vkEnumerateInstanceExtensionProperties) &&
           X(vkEnumerateInstanceLayerProperties);
#undef X
}

bool Load(VkInstance instance, InstanceDispatch& dld) noexcept {
#define X(name) Proc(dld.name, dld, #name, instance)
    // These functions may fail to load depending on the enabled extensions.
    // Don't return a failure on these.
    X(vkCreateDebugUtilsMessengerEXT);
    X(vkCreateDebugReportCallbackEXT);
    X(vkDestroyDebugUtilsMessengerEXT);
    X(vkDestroyDebugReportCallbackEXT);
    X(vkDestroySurfaceKHR);
    X(vkGetPhysicalDeviceFeatures2);
    X(vkGetPhysicalDeviceFormatProperties2);
    X(vkGetPhysicalDeviceProperties2);
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    X(vkGetPhysicalDeviceSurfaceFormatsKHR);
    X(vkGetPhysicalDeviceSurfacePresentModesKHR);
    X(vkGetPhysicalDeviceSurfaceSupportKHR);
    X(vkGetPhysicalDeviceToolProperties);
    X(vkGetSwapchainImagesKHR);
    X(vkQueuePresentKHR);

    return X(vkCreateDevice) && X(vkDestroyDevice) && X(vkDestroyDevice) &&
           X(vkEnumerateDeviceExtensionProperties) && X(vkEnumeratePhysicalDevices) &&
           X(vkGetDeviceProcAddr) && X(vkGetPhysicalDeviceFormatProperties) &&
           X(vkGetPhysicalDeviceMemoryProperties) && X(vkGetPhysicalDeviceMemoryProperties2) &&
           X(vkGetPhysicalDeviceProperties) && X(vkGetPhysicalDeviceQueueFamilyProperties);
#undef X
}

const char* Exception::what() const noexcept {
    return string_VkResult(result);
}

void Destroy(VkInstance instance, const InstanceDispatch& dld) noexcept {
    dld.vkDestroyInstance(instance, nullptr);
}

void Destroy(VkDevice device, const InstanceDispatch& dld) noexcept {
    dld.vkDestroyDevice(device, DeviceHostAllocator());
}

void Destroy(VkDevice device, VkBuffer handle, const DeviceDispatch& dld) noexcept {
    dld.vkDestroyBuffer(device, handle, nullptr);
}

void Destroy(VkDevice device, VkBufferView handle, const DeviceDispatch& dld) noexcept {
    --g_live<VkBufferView>;
    dld.vkDestroyBufferView(device, handle, nullptr);
}

void Destroy(VkDevice device, VkCommandPool handle, const DeviceDispatch& dld) noexcept {
    --g_live<VkCommandPool>;
    dld.vkDestroyCommandPool(device, handle, nullptr);
}

void Destroy(VkDevice device, VkDescriptorPool handle, const DeviceDispatch& dld) noexcept {
    --g_live<VkDescriptorPool>;
    dld.vkDestroyDescriptorPool(device, handle, nullptr);
}

void Destroy(VkDevice device, VkDescriptorSetLayout handle, const DeviceDispatch& dld) noexcept {
    --g_live<VkDescriptorSetLayout>;
    dld.vkDestroyDescriptorSetLayout(device, handle, nullptr);
}

void Destroy(VkDevice device, VkDescriptorUpdateTemplate handle,
             const DeviceDispatch& dld) noexcept {
    --g_live<VkDescriptorUpdateTemplate>;
    dld.vkDestroyDescriptorUpdateTemplate(device, handle, nullptr);
}

void Destroy(VkDevice device, VkDeviceMemory handle, const DeviceDispatch& dld) noexcept {
    --g_live<VkDeviceMemory>;
    dld.vkFreeMemory(device, handle, nullptr);
}

void Destroy(VkDevice device, VkEvent handle, const DeviceDispatch& dld) noexcept {
    --g_live<VkEvent>;
    dld.vkDestroyEvent(device, handle, nullptr);
}

void Destroy(VkDevice device, VkFence handle, const DeviceDispatch& dld) noexcept {
    --g_live<VkFence>;
    dld.vkDestroyFence(device, handle, nullptr);
}

void Destroy(VkDevice device, VkFramebuffer handle, const DeviceDispatch& dld) noexcept {
    --g_live<VkFramebuffer>;
    dld.vkDestroyFramebuffer(device, handle, nullptr);
}

void Destroy(VkDevice device, VkImage handle, const DeviceDispatch& dld) noexcept {
    dld.vkDestroyImage(device, handle, nullptr);
}

void Destroy(VkDevice device, VkImageView handle, const DeviceDispatch& dld) noexcept {
    --g_live<VkImageView>;
    dld.vkDestroyImageView(device, handle, nullptr);
}

void Destroy(VkDevice device, VkPipeline handle, const DeviceDispatch& dld) noexcept {
    --g_live<VkPipeline>;
    dld.vkDestroyPipeline(device, handle, nullptr);
}

void Destroy(VkDevice device, VkPipelineCache handle, const DeviceDispatch& dld) noexcept {
    --g_live<VkPipelineCache>;
    dld.vkDestroyPipelineCache(device, handle, nullptr);
}

void Destroy(VkDevice device, VkPipelineLayout handle, const DeviceDispatch& dld) noexcept {
    --g_live<VkPipelineLayout>;
    dld.vkDestroyPipelineLayout(device, handle, nullptr);
}

void Destroy(VkDevice device, VkQueryPool handle, const DeviceDispatch& dld) noexcept {
    --g_live<VkQueryPool>;
    dld.vkDestroyQueryPool(device, handle, nullptr);
}

void Destroy(VkDevice device, VkRenderPass handle, const DeviceDispatch& dld) noexcept {
    --g_live<VkRenderPass>;
    dld.vkDestroyRenderPass(device, handle, nullptr);
}

void Destroy(VkDevice device, VkSampler handle, const DeviceDispatch& dld) noexcept {
    --g_live<VkSampler>;
    dld.vkDestroySampler(device, handle, nullptr);
}

void Destroy(VkDevice device, VkSwapchainKHR handle, const DeviceDispatch& dld) noexcept {
    --g_live<VkSwapchainKHR>;
    dld.vkDestroySwapchainKHR(device, handle, nullptr);
}

void Destroy(VkDevice device, VkSemaphore handle, const DeviceDispatch& dld) noexcept {
    --g_live<VkSemaphore>;
    dld.vkDestroySemaphore(device, handle, nullptr);
}

void Destroy(VkDevice device, VkShaderModule handle, const DeviceDispatch& dld) noexcept {
    --g_live<VkShaderModule>;
    dld.vkDestroyShaderModule(device, handle, nullptr);
}

void Destroy(VkInstance instance, VkDebugUtilsMessengerEXT handle,
             const InstanceDispatch& dld) noexcept {
    dld.vkDestroyDebugUtilsMessengerEXT(instance, handle, nullptr);
}

void Destroy(VkInstance instance, VkDebugReportCallbackEXT handle,
             const InstanceDispatch& dld) noexcept {
    dld.vkDestroyDebugReportCallbackEXT(instance, handle, nullptr);
}

void Destroy(VkInstance instance, VkSurfaceKHR handle, const InstanceDispatch& dld) noexcept {
    dld.vkDestroySurfaceKHR(instance, handle, nullptr);
}

VkResult Free(VkDevice device, VkDescriptorPool handle, Span<VkDescriptorSet> sets,
              const DeviceDispatch& dld) noexcept {
    return dld.vkFreeDescriptorSets(device, handle, sets.size(), sets.data());
}

VkResult Free(VkDevice device, VkCommandPool handle, Span<VkCommandBuffer> buffers,
              const DeviceDispatch& dld) noexcept {
    dld.vkFreeCommandBuffers(device, handle, buffers.size(), buffers.data());
    return VK_SUCCESS;
}

Instance Instance::Create(u32 version, Span<const char*> layers, Span<const char*> extensions,
                          InstanceDispatch& dispatch) {
#ifdef __APPLE__
    constexpr VkFlags ci_flags{VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR};
#else
    constexpr VkFlags ci_flags{};
#endif
    // DO NOT TOUCH, breaks RNDA3!!
    // Don't know why, but gloom + yellow line glitch appears
    const VkApplicationInfo application_info{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext = nullptr,
        .pApplicationName = "yuzu Emulator",
        .applicationVersion = VK_MAKE_VERSION(1, 3, 0),
        .pEngineName = "yuzu Emulator",
        .engineVersion = VK_MAKE_VERSION(1, 3, 0),
        .apiVersion = VK_API_VERSION_1_3,
    };
    const VkInstanceCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = ci_flags,
        .pApplicationInfo = &application_info,
        .enabledLayerCount = layers.size(),
        .ppEnabledLayerNames = layers.data(),
        .enabledExtensionCount = extensions.size(),
        .ppEnabledExtensionNames = extensions.data(),
    };
    VkInstance instance;
    Check(dispatch.vkCreateInstance(&ci, nullptr, &instance));
    if (!Proc(dispatch.vkDestroyInstance, dispatch, "vkDestroyInstance", instance)) {
        // We successfully created an instance but the destroy function couldn't be loaded.
        // This is a good moment to panic.
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    return Instance(instance, dispatch);
}

std::vector<VkPhysicalDevice> Instance::EnumeratePhysicalDevices() const {
    u32 num;
    Check(dld->vkEnumeratePhysicalDevices(handle, &num, nullptr));
    std::vector<VkPhysicalDevice> physical_devices(num);
    Check(dld->vkEnumeratePhysicalDevices(handle, &num, physical_devices.data()));
    SortPhysicalDevices(physical_devices, *dld);
    return physical_devices;
}

DebugUtilsMessenger Instance::CreateDebugUtilsMessenger(
    const VkDebugUtilsMessengerCreateInfoEXT& create_info) const {
    VkDebugUtilsMessengerEXT object;
    Check(dld->vkCreateDebugUtilsMessengerEXT(handle, &create_info, nullptr, &object));
    return DebugUtilsMessenger(object, handle, *dld);
}

DebugReportCallback Instance::CreateDebugReportCallback(
    const VkDebugReportCallbackCreateInfoEXT& create_info) const {
    VkDebugReportCallbackEXT object;
    Check(dld->vkCreateDebugReportCallbackEXT(handle, &create_info, nullptr, &object));
    return DebugReportCallback(object, handle, *dld);
}

void Image::SetObjectNameEXT(const char* name) const {
    SetObjectName(dld, owner, handle, VK_OBJECT_TYPE_IMAGE, name);
}

void Image::Release() const noexcept {
    if (handle) {
        vmaDestroyImage(allocator, handle, allocation);
    }
}

void Buffer::Flush() const {
    if (!is_coherent) {
        vmaFlushAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
    }
}

void Buffer::Invalidate() const {
    if (!is_coherent) {
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
    }
}

void Buffer::SetObjectNameEXT(const char* name) const {
    SetObjectName(dld, owner, handle, VK_OBJECT_TYPE_BUFFER, name);
}

void Buffer::Release() const noexcept {
    if (handle) {
        vmaDestroyBuffer(allocator, handle, allocation);
    }
}

void BufferView::SetObjectNameEXT(const char* name) const {
    SetObjectName(dld, owner, handle, VK_OBJECT_TYPE_BUFFER_VIEW, name);
}

void ImageView::SetObjectNameEXT(const char* name) const {
    SetObjectName(dld, owner, handle, VK_OBJECT_TYPE_IMAGE_VIEW, name);
}

int DeviceMemory::GetMemoryFdKHR() const {
    const VkMemoryGetFdInfoKHR get_fd_info{
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .pNext = nullptr,
        .memory = handle,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR,
    };
    int fd;
    Check(dld->vkGetMemoryFdKHR(owner, &get_fd_info, &fd));
    return fd;
}

#ifdef _WIN32
HANDLE DeviceMemory::GetMemoryWin32HandleKHR() const {
    const VkMemoryGetWin32HandleInfoKHR get_win32_handle_info{
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
        .pNext = nullptr,
        .memory = handle,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT_KHR,
    };
    HANDLE win32_handle;
    Check(dld->vkGetMemoryWin32HandleKHR(owner, &get_win32_handle_info, &win32_handle));
    return win32_handle;
}
#endif

void DeviceMemory::SetObjectNameEXT(const char* name) const {
    SetObjectName(dld, owner, handle, VK_OBJECT_TYPE_DEVICE_MEMORY, name);
}

void Fence::SetObjectNameEXT(const char* name) const {
    SetObjectName(dld, owner, handle, VK_OBJECT_TYPE_FENCE, name);
}

void Framebuffer::SetObjectNameEXT(const char* name) const {
    SetObjectName(dld, owner, handle, VK_OBJECT_TYPE_FRAMEBUFFER, name);
}

DescriptorSets DescriptorPool::Allocate(const VkDescriptorSetAllocateInfo& ai) const {
    const std::size_t num = ai.descriptorSetCount;
    std::unique_ptr sets = std::make_unique<VkDescriptorSet[]>(num);
    switch (const VkResult result = dld->vkAllocateDescriptorSets(owner, &ai, sets.get())) {
    case VK_SUCCESS:
        return DescriptorSets(std::move(sets), num, owner, handle, *dld);
    case VK_ERROR_OUT_OF_POOL_MEMORY:
    case VK_ERROR_FRAGMENTED_POOL:
        return {};
    default:
        throw Exception(result);
    }
}

void DescriptorPool::SetObjectNameEXT(const char* name) const {
    SetObjectName(dld, owner, handle, VK_OBJECT_TYPE_DESCRIPTOR_POOL, name);
}

CommandBuffers CommandPool::Allocate(std::size_t num_buffers, VkCommandBufferLevel level) const {
    const VkCommandBufferAllocateInfo ai{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = handle,
        .level = level,
        .commandBufferCount = static_cast<u32>(num_buffers),
    };

    std::unique_ptr buffers = std::make_unique<VkCommandBuffer[]>(num_buffers);
    switch (const VkResult result = dld->vkAllocateCommandBuffers(owner, &ai, buffers.get())) {
    case VK_SUCCESS:
        return CommandBuffers(std::move(buffers), num_buffers, owner, handle, *dld);
    case VK_ERROR_OUT_OF_POOL_MEMORY:
    case VK_ERROR_FRAGMENTED_POOL:
        return {};
    default:
        throw Exception(result);
    }
}

void CommandPool::SetObjectNameEXT(const char* name) const {
    SetObjectName(dld, owner, handle, VK_OBJECT_TYPE_COMMAND_POOL, name);
}

std::vector<VkImage> SwapchainKHR::GetImages() const {
    u32 num;
    Check(dld->vkGetSwapchainImagesKHR(owner, handle, &num, nullptr));
    std::vector<VkImage> images(num);
    Check(dld->vkGetSwapchainImagesKHR(owner, handle, &num, images.data()));
    return images;
}

void Event::SetObjectNameEXT(const char* name) const {
    SetObjectName(dld, owner, handle, VK_OBJECT_TYPE_EVENT, name);
}

void ShaderModule::SetObjectNameEXT(const char* name) const {
    SetObjectName(dld, owner, handle, VK_OBJECT_TYPE_SHADER_MODULE, name);
}

void PipelineCache::SetObjectNameEXT(const char* name) const {
    SetObjectName(dld, owner, handle, VK_OBJECT_TYPE_PIPELINE_CACHE, name);
}

void Semaphore::SetObjectNameEXT(const char* name) const {
    SetObjectName(dld, owner, handle, VK_OBJECT_TYPE_SEMAPHORE, name);
}

Device Device::Create(VkPhysicalDevice physical_device, Span<VkDeviceQueueCreateInfo> queues_ci,
                      Span<const char*> enabled_extensions, const void* next,
                      DeviceDispatch& dispatch) {
    const VkDeviceCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = next,
        .flags = 0,
        .queueCreateInfoCount = queues_ci.size(),
        .pQueueCreateInfos = queues_ci.data(),
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = enabled_extensions.size(),
        .ppEnabledExtensionNames = enabled_extensions.data(),
        .pEnabledFeatures = nullptr,
    };
    VkDevice device;
    Check(dispatch.vkCreateDevice(physical_device, &ci, DeviceHostAllocator(), &device));
    Load(device, dispatch);
    return Device(device, dispatch);
}

Queue Device::GetQueue(u32 family_index) const noexcept {
    VkQueue queue;
    dld->vkGetDeviceQueue(handle, family_index, 0, &queue);
    return Queue(queue, *dld);
}

BufferView Device::CreateBufferView(const VkBufferViewCreateInfo& ci) const {
    VkBufferView object;
    Check(dld->vkCreateBufferView(handle, &ci, nullptr, &object));
    ++g_live<VkBufferView>;
    return BufferView(object, handle, *dld);
}

ImageView Device::CreateImageView(const VkImageViewCreateInfo& ci) const {
    VkImageView object;
    Check(dld->vkCreateImageView(handle, &ci, nullptr, &object));
    ++g_live<VkImageView>;
    return ImageView(object, handle, *dld);
}

Semaphore Device::CreateSemaphore() const {
    static constexpr VkSemaphoreCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
    };
    return CreateSemaphore(ci);
}

Semaphore Device::CreateSemaphore(const VkSemaphoreCreateInfo& ci) const {
    VkSemaphore object;
    Check(dld->vkCreateSemaphore(handle, &ci, nullptr, &object));
    ++g_live<VkSemaphore>;
    return Semaphore(object, handle, *dld);
}

Fence Device::CreateFence(const VkFenceCreateInfo& ci) const {
    VkFence object;
    Check(dld->vkCreateFence(handle, &ci, nullptr, &object));
    ++g_live<VkFence>;
    return Fence(object, handle, *dld);
}

DescriptorPool Device::CreateDescriptorPool(const VkDescriptorPoolCreateInfo& ci) const {
    VkDescriptorPool object;
    Check(dld->vkCreateDescriptorPool(handle, &ci, nullptr, &object));
    ++g_live<VkDescriptorPool>;
    return DescriptorPool(object, handle, *dld);
}

RenderPass Device::CreateRenderPass(const VkRenderPassCreateInfo& ci) const {
    VkRenderPass object;
    Check(dld->vkCreateRenderPass(handle, &ci, nullptr, &object));
    ++g_live<VkRenderPass>;
    return RenderPass(object, handle, *dld);
}

RenderPass Device::CreateRenderPass2(const VkRenderPassCreateInfo2& ci) const {
    VkRenderPass object;
    Check(dld->vkCreateRenderPass2(handle, &ci, nullptr, &object));
    ++g_live<VkRenderPass>;
    return RenderPass(object, handle, *dld);
}

DescriptorSetLayout Device::CreateDescriptorSetLayout(
    const VkDescriptorSetLayoutCreateInfo& ci) const {
    VkDescriptorSetLayout object;
    Check(dld->vkCreateDescriptorSetLayout(handle, &ci, nullptr, &object));
    ++g_live<VkDescriptorSetLayout>;
    return DescriptorSetLayout(object, handle, *dld);
}

PipelineCache Device::CreatePipelineCache(const VkPipelineCacheCreateInfo& ci) const {
    VkPipelineCache cache;
    Check(dld->vkCreatePipelineCache(handle, &ci, nullptr, &cache));
    ++g_live<VkPipelineCache>;
    return PipelineCache(cache, handle, *dld);
}

PipelineLayout Device::CreatePipelineLayout(const VkPipelineLayoutCreateInfo& ci) const {
    VkPipelineLayout object;
    Check(dld->vkCreatePipelineLayout(handle, &ci, nullptr, &object));
    ++g_live<VkPipelineLayout>;
    return PipelineLayout(object, handle, *dld);
}

Pipeline Device::CreateGraphicsPipeline(const VkGraphicsPipelineCreateInfo& ci, VkPipelineCache cache) const {
    VkPipeline object = VK_NULL_HANDLE;
    auto const result = dld->vkCreateGraphicsPipelines(handle, cache, 1, &ci, nullptr, &object);
    // Own any returned handle before checking the result, including partial failures.
    if (object != VK_NULL_HANDLE) {
        ++g_live<VkPipeline>;
    }
    Pipeline pipeline(object, handle, *dld);
    // Adreno 5xx drivers do not properly return when a graphics pipeline fails to be created
    // Some (unkown) Mali drivers also do not properly return
    // This result code is out of spec, but should be handled as "kinda working"
    if (result != VK_INCOMPLETE) {
        Check(result);
    }
    // Neither success nor the legacy incomplete result makes a null handle usable.
    if (!pipeline) {
        Check(VK_ERROR_INITIALIZATION_FAILED);
    }
    return pipeline;
}

Pipeline Device::CreateComputePipeline(const VkComputePipelineCreateInfo& ci,
                                       VkPipelineCache cache) const {
    VkPipeline object;
    Check(dld->vkCreateComputePipelines(handle, cache, 1, &ci, nullptr, &object));
    ++g_live<VkPipeline>;
    return Pipeline(object, handle, *dld);
}

Sampler Device::CreateSampler(const VkSamplerCreateInfo& ci) const {
    VkSampler object;
    Check(dld->vkCreateSampler(handle, &ci, nullptr, &object));
    ++g_live<VkSampler>;
    return Sampler(object, handle, *dld);
}

Framebuffer Device::CreateFramebuffer(const VkFramebufferCreateInfo& ci) const {
    VkFramebuffer object;
    Check(dld->vkCreateFramebuffer(handle, &ci, nullptr, &object));
    ++g_live<VkFramebuffer>;
    return Framebuffer(object, handle, *dld);
}

CommandPool Device::CreateCommandPool(const VkCommandPoolCreateInfo& ci) const {
    VkCommandPool object;
    Check(dld->vkCreateCommandPool(handle, &ci, nullptr, &object));
    ++g_live<VkCommandPool>;
    return CommandPool(object, handle, *dld);
}

DescriptorUpdateTemplate Device::CreateDescriptorUpdateTemplate(
    const VkDescriptorUpdateTemplateCreateInfo& ci) const {
    VkDescriptorUpdateTemplate object;
    Check(dld->vkCreateDescriptorUpdateTemplate(handle, &ci, nullptr, &object));
    ++g_live<VkDescriptorUpdateTemplate>;
    return DescriptorUpdateTemplate(object, handle, *dld);
}

QueryPool Device::CreateQueryPool(const VkQueryPoolCreateInfo& ci) const {
    VkQueryPool object;
    Check(dld->vkCreateQueryPool(handle, &ci, nullptr, &object));
    ++g_live<VkQueryPool>;
    return QueryPool(object, handle, *dld);
}

ShaderModule Device::CreateShaderModule(const VkShaderModuleCreateInfo& ci) const {
    VkShaderModule object;
    Check(dld->vkCreateShaderModule(handle, &ci, nullptr, &object));
    ++g_live<VkShaderModule>;
    return ShaderModule(object, handle, *dld);
}

Event Device::CreateEvent() const {
    static constexpr VkEventCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
    };

    VkEvent object;
    Check(dld->vkCreateEvent(handle, &ci, nullptr, &object));
    ++g_live<VkEvent>;
    return Event(object, handle, *dld);
}

SwapchainKHR Device::CreateSwapchainKHR(const VkSwapchainCreateInfoKHR& ci) const {
    VkSwapchainKHR object;
    Check(dld->vkCreateSwapchainKHR(handle, &ci, nullptr, &object));
    ++g_live<VkSwapchainKHR>;
    return SwapchainKHR(object, handle, *dld);
}

DeviceMemory Device::TryAllocateMemory(const VkMemoryAllocateInfo& ai) const noexcept {
    VkDeviceMemory memory;
    if (dld->vkAllocateMemory(handle, &ai, nullptr, &memory) != VK_SUCCESS) {
        return {};
    }
    ++g_live<VkDeviceMemory>;
    return DeviceMemory(memory, handle, *dld);
}

DeviceMemory Device::AllocateMemory(const VkMemoryAllocateInfo& ai) const {
    VkDeviceMemory memory;
    Check(dld->vkAllocateMemory(handle, &ai, nullptr, &memory));
    ++g_live<VkDeviceMemory>;
    return DeviceMemory(memory, handle, *dld);
}

VkMemoryRequirements Device::GetBufferMemoryRequirements(VkBuffer buffer,
                                                         void* pnext) const noexcept {
    const VkBufferMemoryRequirementsInfo2 info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
        .pNext = nullptr,
        .buffer = buffer,
    };
    VkMemoryRequirements2 requirements{
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = pnext,
        .memoryRequirements{},
    };
    dld->vkGetBufferMemoryRequirements2(handle, &info, &requirements);
    return requirements.memoryRequirements;
}

VkMemoryRequirements Device::GetImageMemoryRequirements(VkImage image) const noexcept {
    VkMemoryRequirements requirements;
    dld->vkGetImageMemoryRequirements(handle, image, &requirements);
    return requirements;
}

std::vector<VkPipelineExecutablePropertiesKHR> Device::GetPipelineExecutablePropertiesKHR(
    VkPipeline pipeline) const {
    const VkPipelineInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR,
        .pNext = nullptr,
        .pipeline = pipeline,
    };
    u32 num{};
    dld->vkGetPipelineExecutablePropertiesKHR(handle, &info, &num, nullptr);
    std::vector<VkPipelineExecutablePropertiesKHR> properties(num);
    for (auto& property : properties) {
        property.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR;
    }
    Check(dld->vkGetPipelineExecutablePropertiesKHR(handle, &info, &num, properties.data()));
    return properties;
}

std::vector<VkPipelineExecutableStatisticKHR> Device::GetPipelineExecutableStatisticsKHR(
    VkPipeline pipeline, u32 executable_index) const {
    const VkPipelineExecutableInfoKHR executable_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR,
        .pNext = nullptr,
        .pipeline = pipeline,
        .executableIndex = executable_index,
    };
    u32 num{};
    dld->vkGetPipelineExecutableStatisticsKHR(handle, &executable_info, &num, nullptr);
    std::vector<VkPipelineExecutableStatisticKHR> statistics(num);
    for (auto& statistic : statistics) {
        statistic.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR;
    }
    Check(dld->vkGetPipelineExecutableStatisticsKHR(handle, &executable_info, &num,
                                                    statistics.data()));
    return statistics;
}

void Device::UpdateDescriptorSets(Span<VkWriteDescriptorSet> writes,
                                  Span<VkCopyDescriptorSet> copies) const noexcept {
    dld->vkUpdateDescriptorSets(handle, writes.size(), writes.data(), copies.size(), copies.data());
}

VkPhysicalDeviceProperties PhysicalDevice::GetProperties() const noexcept {
    VkPhysicalDeviceProperties properties;
    dld->vkGetPhysicalDeviceProperties(physical_device, &properties);
    return properties;
}

void PhysicalDevice::GetProperties2(VkPhysicalDeviceProperties2& properties) const noexcept {
    dld->vkGetPhysicalDeviceProperties2(physical_device, &properties);
}

VkPhysicalDeviceFeatures PhysicalDevice::GetFeatures() const noexcept {
    VkPhysicalDeviceFeatures2 features2;
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = nullptr;
    dld->vkGetPhysicalDeviceFeatures2(physical_device, &features2);
    return features2.features;
}

void PhysicalDevice::GetFeatures2(VkPhysicalDeviceFeatures2& features) const noexcept {
    dld->vkGetPhysicalDeviceFeatures2(physical_device, &features);
}

VkFormatProperties PhysicalDevice::GetFormatProperties(VkFormat format) const noexcept {
    VkFormatProperties properties;
    dld->vkGetPhysicalDeviceFormatProperties(physical_device, format, &properties);
    return properties;
}

VkFormatProperties3 PhysicalDevice::GetFormatProperties3(VkFormat format) const noexcept {
    VkFormatProperties3 properties3{
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3,
        .pNext = nullptr,
        .linearTilingFeatures = 0,
        .optimalTilingFeatures = 0,
        .bufferFeatures = 0,
    };
    VkFormatProperties2 properties2{
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &properties3,
        .formatProperties = {},
    };
    dld->vkGetPhysicalDeviceFormatProperties2(physical_device, format, &properties2);
    return properties3;
}

std::vector<VkExtensionProperties> PhysicalDevice::EnumerateDeviceExtensionProperties() const {
    u32 num;
    dld->vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &num, nullptr);
    std::vector<VkExtensionProperties> properties(num);
    dld->vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &num, properties.data());
    return properties;
}

std::vector<VkQueueFamilyProperties> PhysicalDevice::GetQueueFamilyProperties() const {
    u32 num;
    dld->vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &num, nullptr);
    std::vector<VkQueueFamilyProperties> properties(num);
    dld->vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &num, properties.data());
    return properties;
}

std::vector<VkPhysicalDeviceToolProperties> PhysicalDevice::GetPhysicalDeviceToolProperties()
    const {
    u32 num = 0;
    if (!dld->vkGetPhysicalDeviceToolProperties) {
        return {};
    }
    dld->vkGetPhysicalDeviceToolProperties(physical_device, &num, nullptr);
    std::vector<VkPhysicalDeviceToolProperties> properties(num);
    dld->vkGetPhysicalDeviceToolProperties(physical_device, &num, properties.data());
    return properties;
}

bool PhysicalDevice::GetSurfaceSupportKHR(u32 queue_family_index, VkSurfaceKHR surface) const {
    VkBool32 supported;
    Check(dld->vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, queue_family_index, surface,
                                                    &supported));
    return supported == VK_TRUE;
}

VkSurfaceCapabilitiesKHR PhysicalDevice::GetSurfaceCapabilitiesKHR(VkSurfaceKHR surface) const {
    VkSurfaceCapabilitiesKHR capabilities;
    Check(dld->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface, &capabilities));
    return capabilities;
}

std::vector<VkSurfaceFormatKHR> PhysicalDevice::GetSurfaceFormatsKHR(VkSurfaceKHR surface) const {
    u32 num;
    Check(dld->vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &num, nullptr));
    std::vector<VkSurfaceFormatKHR> formats(num);
    Check(
        dld->vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &num, formats.data()));
    return formats;
}

std::vector<VkPresentModeKHR> PhysicalDevice::GetSurfacePresentModesKHR(
    VkSurfaceKHR surface) const {
    u32 num;
    Check(dld->vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &num, nullptr));
    std::vector<VkPresentModeKHR> modes(num);
    Check(dld->vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &num,
                                                         modes.data()));
    return modes;
}

VkPhysicalDeviceMemoryProperties2 PhysicalDevice::GetMemoryProperties(
    void* next_structures) const noexcept {
    VkPhysicalDeviceMemoryProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    properties.pNext = next_structures;
    dld->vkGetPhysicalDeviceMemoryProperties2(physical_device, &properties);
    return properties;
}

u32 AvailableVersion(const InstanceDispatch& dld) noexcept {
    PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion;
    if (!Proc(vkEnumerateInstanceVersion, dld, "vkEnumerateInstanceVersion")) {
        // If the procedure is not found, Vulkan 1.0 is assumed
        return VK_API_VERSION_1_0;
    }
    u32 version;
    if (const VkResult result = vkEnumerateInstanceVersion(&version); result != VK_SUCCESS) {
        LOG_ERROR(Render_Vulkan, "vkEnumerateInstanceVersion returned {}, assuming Vulkan 1.1",
                  string_VkResult(result));
        return VK_API_VERSION_1_1;
    }
    return version;
}

std::optional<std::vector<VkExtensionProperties>> EnumerateInstanceExtensionProperties(
    const InstanceDispatch& dld) {
    u32 num;
    if (dld.vkEnumerateInstanceExtensionProperties(nullptr, &num, nullptr) != VK_SUCCESS) {
        return std::nullopt;
    }
    std::vector<VkExtensionProperties> properties(num);
    if (dld.vkEnumerateInstanceExtensionProperties(nullptr, &num, properties.data()) !=
        VK_SUCCESS) {
        return std::nullopt;
    }
    return properties;
}

std::optional<std::vector<VkLayerProperties>> EnumerateInstanceLayerProperties(
    const InstanceDispatch& dld) {
    u32 num;
    if (dld.vkEnumerateInstanceLayerProperties(&num, nullptr) != VK_SUCCESS) {
        return std::nullopt;
    }
    std::vector<VkLayerProperties> properties(num);
    if (dld.vkEnumerateInstanceLayerProperties(&num, properties.data()) != VK_SUCCESS) {
        return std::nullopt;
    }
    return properties;
}

std::string GetDriverName(VkPhysicalDeviceDriverProperties driver) {
    switch (driver.driverID) {
    case VK_DRIVER_ID_AMD_PROPRIETARY:
        return "AMD";
    case VK_DRIVER_ID_AMD_OPEN_SOURCE:
        return "AMDVLK";
    case VK_DRIVER_ID_MESA_RADV:
        return "RADV";
    case VK_DRIVER_ID_NVIDIA_PROPRIETARY:
        return "Nvidia";
    case VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS:
        return "Intel";
    case VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA:
        return "ANV";
    case VK_DRIVER_ID_IMAGINATION_PROPRIETARY:
        return "PowerVR";
    case VK_DRIVER_ID_QUALCOMM_PROPRIETARY:
        return "Qualcomm";
    case VK_DRIVER_ID_ARM_PROPRIETARY:
        return "Mali";
    case VK_DRIVER_ID_SAMSUNG_PROPRIETARY:
        return "Xclipse";
    case VK_DRIVER_ID_GOOGLE_SWIFTSHADER:
        return "SwiftShader";
    case VK_DRIVER_ID_BROADCOM_PROPRIETARY:
        return "Broadcom";
    case VK_DRIVER_ID_MESA_LLVMPIPE:
        return "llvmpipe";
    case VK_DRIVER_ID_MOLTENVK:
        return "MoltenVK";
    case VK_DRIVER_ID_VERISILICON_PROPRIETARY:
        return "Vivante";
    case VK_DRIVER_ID_MESA_TURNIP:
        return "Turnip";
    case VK_DRIVER_ID_MESA_V3DV:
        return "V3DV";
    case VK_DRIVER_ID_MESA_PANVK:
        return "PanVK";
    case VK_DRIVER_ID_MESA_VENUS:
        return "Venus";
    case VK_DRIVER_ID_MESA_DOZEN:
        return "Dozen";
    case VK_DRIVER_ID_MESA_NVK:
        return "NVK";
    case VK_DRIVER_ID_IMAGINATION_OPEN_SOURCE_MESA:
        return "PVR";
    case VK_DRIVER_ID_MESA_HONEYKRISP:
        return "HoneyKrisp";
    case VK_DRIVER_ID_MESA_KOSMICKRISP:
        return "KosmicKrisp";
    case VK_DRIVER_ID_GGP_PROPRIETARY:
    case VK_DRIVER_ID_COREAVI_PROPRIETARY:
    case VK_DRIVER_ID_JUICE_PROPRIETARY:
    case VK_DRIVER_ID_VULKAN_SC_EMULATION_ON_VULKAN:
    case VK_DRIVER_ID_MAX_ENUM:
    default:
        return driver.driverName;
    }
}

std::string LiveObjectSummary() {
    const auto get = [](const std::atomic<long long>& v) { return v.load(std::memory_order_relaxed); };
    return fmt::format("mem={} views={} bufviews={} fb={} samplers={} pipelines={} descpools={} "
                       "querypools={} cmdpools={} sem={} fences={} events={} renderpasses={} "
                       "shadermods={}",
                       get(g_live<VkDeviceMemory>), get(g_live<VkImageView>),
                       get(g_live<VkBufferView>), get(g_live<VkFramebuffer>),
                       get(g_live<VkSampler>), get(g_live<VkPipeline>),
                       get(g_live<VkDescriptorPool>), get(g_live<VkQueryPool>),
                       get(g_live<VkCommandPool>), get(g_live<VkSemaphore>), get(g_live<VkFence>),
                       get(g_live<VkEvent>), get(g_live<VkRenderPass>),
                       get(g_live<VkShaderModule>)) +
           HostAllocSummary();
}

} // namespace Vulkan::vk
