// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "video_core/vulkan_common/vulkan_output_policy.h"

namespace Vulkan {

// Explicit scalar snapshots; these overloads do not retain Vulkan pointer chains.
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceFeatures& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceLimits& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceSparseProperties& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDevice16BitStorageFeatures& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceShaderAtomicInt64Features& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceShaderDrawParametersFeatures& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceShaderFloat16Int8Features& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceUniformBufferStandardLayoutFeatures& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceVariablePointerFeatures& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceDescriptorIndexingFeatures& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceHostQueryResetFeatures& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDevice8BitStorageFeatures& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceBufferDeviceAddressFeatures& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceTimelineSemaphoreFeatures& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceVulkanMemoryModelFeatures& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceImageRobustnessFeatures& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceSubgroupSizeControlFeatures& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceMaintenance4Features& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceSynchronization2Features& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceBorderColorSwizzleFeaturesEXT& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceColorWriteEnableFeaturesEXT& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceCustomBorderColorFeaturesEXT& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceDepthBiasControlFeaturesEXT& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceDepthClipControlFeaturesEXT& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceDescriptorBufferFeaturesEXT& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceExtendedDynamicStateFeaturesEXT& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceExtendedDynamicState2FeaturesEXT& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceExtendedDynamicState3FeaturesEXT& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDevice4444FormatsFeaturesEXT& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceIndexTypeUint8FeaturesEXT& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceLineRasterizationFeaturesEXT& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceProvokingVertexFeaturesEXT& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceRobustness2FeaturesEXT& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceTransformFeedbackFeaturesEXT& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceMaintenance5FeaturesKHR& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceMaintenance6FeaturesKHR& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceProperties& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceSubgroupProperties& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceFloatControlsProperties& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDevicePushDescriptorPropertiesKHR& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceDescriptorBufferPropertiesEXT& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceSubgroupSizeControlProperties& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceTransformFeedbackPropertiesEXT& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceMaintenance5PropertiesKHR& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceDepthStencilResolveProperties& value);
void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceCustomBorderColorPropertiesEXT& value);

DeviceCapabilitySnapshot CaptureFeatureChain(const VkPhysicalDeviceFeatures2& features,
                                             const VkPhysicalDeviceFeatures* output_core = nullptr);

} // namespace Vulkan
