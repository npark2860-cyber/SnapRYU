/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "../helpers/pointers.hpp"

#include <bitset>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>
#include <vulkan/vk_layer.h>

#ifdef CreateSemaphore
#undef CreateSemaphore
#endif
namespace vk {

    /// Module-local diagnostics. The host serializes every operation on a Vulkan wrapper.
    struct VulkanStats {
        uint64_t modules{};
        uint64_t pipelines{};
        uint64_t dispatches{};
        uint64_t submissions{};
    };

    /// vulkan instance function pointers
    struct VulkanInstanceFuncs {
        PFN_vkDestroyInstance DestroyInstance;
        PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
        PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
        PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2;
        PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
        PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2;
        PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
        PFN_vkCreateDevice CreateDevice;
        PFN_vkGetDeviceProcAddr GetDeviceProcAddr;

        // extension functions
        PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR;
    };

    /// initialize vulkan instance function pointers
    /// @param instance vulkan instance handle
    /// @param mpa function to get instance proc addresses
    /// @param graphical whether the device is graphical (rather than compute)
    /// @return initialized function pointers
    VulkanInstanceFuncs initVulkanInstanceFuncs(VkInstance instance, PFN_vkGetInstanceProcAddr mpa,
        bool graphical);

    using PhysicalDeviceSelector = const std::function<
        VkPhysicalDevice(
            const VulkanInstanceFuncs&,
            const std::vector<VkPhysicalDevice>&
        )
    >&;

    /// vulkan device function pointers
    struct VulkanDeviceFuncs {
        PFN_vkGetDeviceQueue GetDeviceQueue;
        PFN_vkDeviceWaitIdle DeviceWaitIdle;
        PFN_vkCreateCommandPool CreateCommandPool;
        PFN_vkDestroyCommandPool DestroyCommandPool;
        PFN_vkCreateDescriptorPool CreateDescriptorPool;
        PFN_vkDestroyDescriptorPool DestroyDescriptorPool;
        PFN_vkCreateBuffer CreateBuffer;
        PFN_vkDestroyBuffer DestroyBuffer;
        PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
        PFN_vkAllocateMemory AllocateMemory;
        PFN_vkFreeMemory FreeMemory;
        PFN_vkBindBufferMemory BindBufferMemory;
        PFN_vkMapMemory MapMemory;
        PFN_vkUnmapMemory UnmapMemory;
        PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
        PFN_vkFreeCommandBuffers FreeCommandBuffers;
        PFN_vkBeginCommandBuffer BeginCommandBuffer;
        PFN_vkEndCommandBuffer EndCommandBuffer;
        PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
        PFN_vkCmdBlitImage CmdBlitImage;
        PFN_vkCmdClearColorImage CmdClearColorImage;
        PFN_vkCmdBindPipeline CmdBindPipeline;
        PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets;
        PFN_vkCmdDispatch CmdDispatch;
        PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage;
        PFN_vkQueueSubmit QueueSubmit;
        PFN_vkAllocateDescriptorSets AllocateDescriptorSets;
        PFN_vkFreeDescriptorSets FreeDescriptorSets;
        PFN_vkUpdateDescriptorSets UpdateDescriptorSets;
        PFN_vkCreateFence CreateFence;
        PFN_vkDestroyFence DestroyFence;
        PFN_vkResetFences ResetFences;
        PFN_vkWaitForFences WaitForFences;
        PFN_vkCreateImage CreateImage;
        PFN_vkDestroyImage DestroyImage;
        PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
        PFN_vkBindImageMemory BindImageMemory;
        PFN_vkCreateImageView CreateImageView;
        PFN_vkDestroyImageView DestroyImageView;
        PFN_vkCreateSampler CreateSampler;
        PFN_vkDestroySampler DestroySampler;
        PFN_vkCreateSemaphore CreateSemaphore;
        PFN_vkDestroySemaphore DestroySemaphore;
        PFN_vkCreateShaderModule CreateShaderModule;
        PFN_vkDestroyShaderModule DestroyShaderModule;
        PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout;
        PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout;
        PFN_vkCreatePipelineLayout CreatePipelineLayout;
        PFN_vkDestroyPipelineLayout DestroyPipelineLayout;
        PFN_vkCreatePipelineCache CreatePipelineCache;
        PFN_vkDestroyPipelineCache DestroyPipelineCache;
        PFN_vkGetPipelineCacheData GetPipelineCacheData;
        PFN_vkCreateComputePipelines CreateComputePipelines;
        PFN_vkDestroyPipeline DestroyPipeline;

        // timeline semaphore functions (core in Vulkan 1.2, extension fallback)
        PFN_vkSignalSemaphore SignalSemaphore;
        PFN_vkWaitSemaphores WaitSemaphores;
        PFN_vkSignalSemaphoreKHR SignalSemaphoreKHR;
        PFN_vkWaitSemaphoresKHR WaitSemaphoresKHR;

        // extension functions
        PFN_vkGetMemoryFdKHR GetMemoryFdKHR;
        PFN_vkImportSemaphoreFdKHR ImportSemaphoreFdKHR;
        PFN_vkGetSemaphoreFdKHR GetSemaphoreFdKHR;
        PFN_vkCreateSwapchainKHR CreateSwapchainKHR;
        PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
        PFN_vkAcquireNextImageKHR AcquireNextImageKHR;
        PFN_vkQueuePresentKHR QueuePresentKHR;
        PFN_vkDestroySwapchainKHR DestroySwapchainKHR;
    };

    /// initialize vulkan device function pointers
    /// @param fi instance function pointers
    /// @param device logical device handle
    /// @param graphical whether the device is graphical (rather than compute)
    /// @return initialized function pointers
    VulkanDeviceFuncs initVulkanDeviceFuncs(const VulkanInstanceFuncs& fi, VkDevice device,
        bool graphical);

    /// vulkan version wrapper
    class version {
    public:
        /// construct from version numbers
        version(uint8_t major, uint8_t minor, uint8_t patch)
            : major(major), minor(minor), patch(patch) {}

        /// convert to Vulkan version
        [[nodiscard]] uint32_t into() const {
            return VK_MAKE_VERSION(major, minor, patch);
        }
    private:
        uint8_t major{};
        uint8_t minor{};
        uint8_t patch{};
    };

    /// vulkan instance
    class Vulkan {
    public:
        /// create based on an existing externally managed vulkan instance.
        /// @param instance vulkan instance handle
        /// @param device logical device handle
        /// @param physdev physical device handle
        /// @param instanceFuncs instance function pointers
        /// @param deviceFuncs device function pointers
        /// @param hostQueueFamily optional family actually created by the host
        /// @param isGraphical whether the device is graphical (rather than compute)
        /// @param setLoaderData optional function to set loader data
        /// @param cachefile optional path to pipeline cache file
        /// @param hostQueue exact host-owned queue; null retains the legacy queue-zero fallback
        /// @param hostFp16 host asserts shaderFloat16 is enabled on this device
        /// @throws ls::vulkan_error on failure
        Vulkan(VkInstance instance, VkDevice device,
            VkPhysicalDevice physdev,
            VulkanInstanceFuncs instanceFuncs,
            VulkanDeviceFuncs deviceFuncs,
            bool isGraphical = true,
            std::optional<PFN_vkSetDeviceLoaderData> setLoaderData = std::nullopt,
            const std::optional<std::filesystem::path>& cachefile = std::nullopt,
            std::optional<uint32_t> hostQueueFamily = std::nullopt,
            VkQueue hostQueue = VK_NULL_HANDLE,
            bool hostFp16 = false);

        /// find a memory type index
        /// @param validTypes bitset of valid memory types
        /// @param hostVisibility whether the memory should be host visible
        /// @return the memory type index
        [[nodiscard]] std::optional<uint32_t> findMemoryTypeIndex(
            std::bitset<32> validTypes, bool hostVisibility) const;

        /// persist the pipeline cache to file, silently failing on error
        void persistPipelineCache() const noexcept;

        /// get the vulkan instance
        /// @return the instance handle
        [[nodiscard]] const auto& inst() const { return this->instance.get(); }
        /// get the vulkan device
        /// @return the device handle
        [[nodiscard]] const auto& dev() const { return this->device.get(); }
        /// get the physical device
        /// @return the physical device handle
        [[nodiscard]] VkPhysicalDevice physdev() const { return this->phys_dev; }
        /// get the command pool
        /// @return the command pool handle
        [[nodiscard]] const auto& cmdpool() const { return this->cmdPool.get(); }
        /// get the pipeline cache
        /// @return the pipeline cache handle
        [[nodiscard]] const auto& cache() const { return this->pipelineCache.get(); }
        /// get the compute queue
        /// @return the compute queue handle
        [[nodiscard]] const auto& queue() const { return this->computeQueue; }
        /// Per-wrapper counters, never process-global. Requires host serialization.
        [[nodiscard]] VulkanStats& stats() const { return this->statistics; }

        /// check if fp16 is supported
        /// @return true if fp16 is supported
        [[nodiscard]] bool supportsFP16() const { return this->fp16; }

        /// get instance-level function pointers
        /// @return the instance function pointers
        [[nodiscard]] const auto& fi() const { return this->instance_funcs; }
        /// get device-level function pointers
        /// @return the device function pointers
        [[nodiscard]] const auto& df() const { return this->device_funcs; }
        /// get optional setLoaderData function
        /// @return the setLoaderData function
        [[nodiscard]] const auto& loaderdatafunc() const { return this->setLoaderData; }
    private:
        ls::owned_ptr<VkInstance> instance;
        VulkanInstanceFuncs instance_funcs;

        VkPhysicalDevice phys_dev;
        uint32_t queueFamilyIdx;
        bool fp16;

        ls::owned_ptr<VkDevice> device;
        std::optional<PFN_vkSetDeviceLoaderData> setLoaderData;
        VulkanDeviceFuncs device_funcs;

        VkQueue computeQueue;

        ls::owned_ptr<VkCommandPool> cmdPool;
        ls::owned_ptr<VkPipelineCache> pipelineCache;
        std::optional<std::filesystem::path> cachefile;
        mutable VulkanStats statistics{};
    };
}
