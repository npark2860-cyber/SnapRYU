// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <bit>
#include <string>

#include "video_core/vulkan_common/vulkan_capability_snapshot.h"

namespace Vulkan {

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceFeatures& value) {
    out.values.push_back({"VkPhysicalDeviceFeatures.robustBufferAccess", static_cast<u64>(value.robustBufferAccess)});
    out.values.push_back({"VkPhysicalDeviceFeatures.fullDrawIndexUint32", static_cast<u64>(value.fullDrawIndexUint32)});
    out.values.push_back({"VkPhysicalDeviceFeatures.imageCubeArray", static_cast<u64>(value.imageCubeArray)});
    out.values.push_back({"VkPhysicalDeviceFeatures.independentBlend", static_cast<u64>(value.independentBlend)});
    out.values.push_back({"VkPhysicalDeviceFeatures.geometryShader", static_cast<u64>(value.geometryShader)});
    out.values.push_back({"VkPhysicalDeviceFeatures.tessellationShader", static_cast<u64>(value.tessellationShader)});
    out.values.push_back({"VkPhysicalDeviceFeatures.sampleRateShading", static_cast<u64>(value.sampleRateShading)});
    out.values.push_back({"VkPhysicalDeviceFeatures.dualSrcBlend", static_cast<u64>(value.dualSrcBlend)});
    out.values.push_back({"VkPhysicalDeviceFeatures.logicOp", static_cast<u64>(value.logicOp)});
    out.values.push_back({"VkPhysicalDeviceFeatures.multiDrawIndirect", static_cast<u64>(value.multiDrawIndirect)});
    out.values.push_back({"VkPhysicalDeviceFeatures.drawIndirectFirstInstance", static_cast<u64>(value.drawIndirectFirstInstance)});
    out.values.push_back({"VkPhysicalDeviceFeatures.depthClamp", static_cast<u64>(value.depthClamp)});
    out.values.push_back({"VkPhysicalDeviceFeatures.depthBiasClamp", static_cast<u64>(value.depthBiasClamp)});
    out.values.push_back({"VkPhysicalDeviceFeatures.fillModeNonSolid", static_cast<u64>(value.fillModeNonSolid)});
    out.values.push_back({"VkPhysicalDeviceFeatures.depthBounds", static_cast<u64>(value.depthBounds)});
    out.values.push_back({"VkPhysicalDeviceFeatures.wideLines", static_cast<u64>(value.wideLines)});
    out.values.push_back({"VkPhysicalDeviceFeatures.largePoints", static_cast<u64>(value.largePoints)});
    out.values.push_back({"VkPhysicalDeviceFeatures.alphaToOne", static_cast<u64>(value.alphaToOne)});
    out.values.push_back({"VkPhysicalDeviceFeatures.multiViewport", static_cast<u64>(value.multiViewport)});
    out.values.push_back({"VkPhysicalDeviceFeatures.samplerAnisotropy", static_cast<u64>(value.samplerAnisotropy)});
    out.values.push_back({"VkPhysicalDeviceFeatures.textureCompressionETC2", static_cast<u64>(value.textureCompressionETC2)});
    out.values.push_back({"VkPhysicalDeviceFeatures.textureCompressionASTC_LDR", static_cast<u64>(value.textureCompressionASTC_LDR)});
    out.values.push_back({"VkPhysicalDeviceFeatures.textureCompressionBC", static_cast<u64>(value.textureCompressionBC)});
    out.values.push_back({"VkPhysicalDeviceFeatures.occlusionQueryPrecise", static_cast<u64>(value.occlusionQueryPrecise)});
    out.values.push_back({"VkPhysicalDeviceFeatures.pipelineStatisticsQuery", static_cast<u64>(value.pipelineStatisticsQuery)});
    out.values.push_back({"VkPhysicalDeviceFeatures.vertexPipelineStoresAndAtomics", static_cast<u64>(value.vertexPipelineStoresAndAtomics)});
    out.values.push_back({"VkPhysicalDeviceFeatures.fragmentStoresAndAtomics", static_cast<u64>(value.fragmentStoresAndAtomics)});
    out.values.push_back({"VkPhysicalDeviceFeatures.shaderTessellationAndGeometryPointSize", static_cast<u64>(value.shaderTessellationAndGeometryPointSize)});
    out.values.push_back({"VkPhysicalDeviceFeatures.shaderImageGatherExtended", static_cast<u64>(value.shaderImageGatherExtended)});
    out.values.push_back({"VkPhysicalDeviceFeatures.shaderStorageImageExtendedFormats", static_cast<u64>(value.shaderStorageImageExtendedFormats)});
    out.values.push_back({"VkPhysicalDeviceFeatures.shaderStorageImageMultisample", static_cast<u64>(value.shaderStorageImageMultisample)});
    out.values.push_back({"VkPhysicalDeviceFeatures.shaderStorageImageReadWithoutFormat", static_cast<u64>(value.shaderStorageImageReadWithoutFormat)});
    out.values.push_back({"VkPhysicalDeviceFeatures.shaderStorageImageWriteWithoutFormat", static_cast<u64>(value.shaderStorageImageWriteWithoutFormat)});
    out.values.push_back({"VkPhysicalDeviceFeatures.shaderUniformBufferArrayDynamicIndexing", static_cast<u64>(value.shaderUniformBufferArrayDynamicIndexing)});
    out.values.push_back({"VkPhysicalDeviceFeatures.shaderSampledImageArrayDynamicIndexing", static_cast<u64>(value.shaderSampledImageArrayDynamicIndexing)});
    out.values.push_back({"VkPhysicalDeviceFeatures.shaderStorageBufferArrayDynamicIndexing", static_cast<u64>(value.shaderStorageBufferArrayDynamicIndexing)});
    out.values.push_back({"VkPhysicalDeviceFeatures.shaderStorageImageArrayDynamicIndexing", static_cast<u64>(value.shaderStorageImageArrayDynamicIndexing)});
    out.values.push_back({"VkPhysicalDeviceFeatures.shaderClipDistance", static_cast<u64>(value.shaderClipDistance)});
    out.values.push_back({"VkPhysicalDeviceFeatures.shaderCullDistance", static_cast<u64>(value.shaderCullDistance)});
    out.values.push_back({"VkPhysicalDeviceFeatures.shaderFloat64", static_cast<u64>(value.shaderFloat64)});
    out.values.push_back({"VkPhysicalDeviceFeatures.shaderInt64", static_cast<u64>(value.shaderInt64)});
    out.values.push_back({"VkPhysicalDeviceFeatures.shaderInt16", static_cast<u64>(value.shaderInt16)});
    out.values.push_back({"VkPhysicalDeviceFeatures.shaderResourceResidency", static_cast<u64>(value.shaderResourceResidency)});
    out.values.push_back({"VkPhysicalDeviceFeatures.shaderResourceMinLod", static_cast<u64>(value.shaderResourceMinLod)});
    out.values.push_back({"VkPhysicalDeviceFeatures.sparseBinding", static_cast<u64>(value.sparseBinding)});
    out.values.push_back({"VkPhysicalDeviceFeatures.sparseResidencyBuffer", static_cast<u64>(value.sparseResidencyBuffer)});
    out.values.push_back({"VkPhysicalDeviceFeatures.sparseResidencyImage2D", static_cast<u64>(value.sparseResidencyImage2D)});
    out.values.push_back({"VkPhysicalDeviceFeatures.sparseResidencyImage3D", static_cast<u64>(value.sparseResidencyImage3D)});
    out.values.push_back({"VkPhysicalDeviceFeatures.sparseResidency2Samples", static_cast<u64>(value.sparseResidency2Samples)});
    out.values.push_back({"VkPhysicalDeviceFeatures.sparseResidency4Samples", static_cast<u64>(value.sparseResidency4Samples)});
    out.values.push_back({"VkPhysicalDeviceFeatures.sparseResidency8Samples", static_cast<u64>(value.sparseResidency8Samples)});
    out.values.push_back({"VkPhysicalDeviceFeatures.sparseResidency16Samples", static_cast<u64>(value.sparseResidency16Samples)});
    out.values.push_back({"VkPhysicalDeviceFeatures.sparseResidencyAliased", static_cast<u64>(value.sparseResidencyAliased)});
    out.values.push_back({"VkPhysicalDeviceFeatures.variableMultisampleRate", static_cast<u64>(value.variableMultisampleRate)});
    out.values.push_back({"VkPhysicalDeviceFeatures.inheritedQueries", static_cast<u64>(value.inheritedQueries)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceLimits& value) {
    out.values.push_back({"VkPhysicalDeviceLimits.maxImageDimension1D", static_cast<u64>(value.maxImageDimension1D)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxImageDimension2D", static_cast<u64>(value.maxImageDimension2D)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxImageDimension3D", static_cast<u64>(value.maxImageDimension3D)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxImageDimensionCube", static_cast<u64>(value.maxImageDimensionCube)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxImageArrayLayers", static_cast<u64>(value.maxImageArrayLayers)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxTexelBufferElements", static_cast<u64>(value.maxTexelBufferElements)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxUniformBufferRange", static_cast<u64>(value.maxUniformBufferRange)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxStorageBufferRange", static_cast<u64>(value.maxStorageBufferRange)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxPushConstantsSize", static_cast<u64>(value.maxPushConstantsSize)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxMemoryAllocationCount", static_cast<u64>(value.maxMemoryAllocationCount)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxSamplerAllocationCount", static_cast<u64>(value.maxSamplerAllocationCount)});
    out.values.push_back({"VkPhysicalDeviceLimits.bufferImageGranularity", static_cast<u64>(value.bufferImageGranularity)});
    out.values.push_back({"VkPhysicalDeviceLimits.sparseAddressSpaceSize", static_cast<u64>(value.sparseAddressSpaceSize)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxBoundDescriptorSets", static_cast<u64>(value.maxBoundDescriptorSets)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxPerStageDescriptorSamplers", static_cast<u64>(value.maxPerStageDescriptorSamplers)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxPerStageDescriptorUniformBuffers", static_cast<u64>(value.maxPerStageDescriptorUniformBuffers)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxPerStageDescriptorStorageBuffers", static_cast<u64>(value.maxPerStageDescriptorStorageBuffers)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxPerStageDescriptorSampledImages", static_cast<u64>(value.maxPerStageDescriptorSampledImages)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxPerStageDescriptorStorageImages", static_cast<u64>(value.maxPerStageDescriptorStorageImages)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxPerStageDescriptorInputAttachments", static_cast<u64>(value.maxPerStageDescriptorInputAttachments)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxPerStageResources", static_cast<u64>(value.maxPerStageResources)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxDescriptorSetSamplers", static_cast<u64>(value.maxDescriptorSetSamplers)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxDescriptorSetUniformBuffers", static_cast<u64>(value.maxDescriptorSetUniformBuffers)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxDescriptorSetUniformBuffersDynamic", static_cast<u64>(value.maxDescriptorSetUniformBuffersDynamic)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxDescriptorSetStorageBuffers", static_cast<u64>(value.maxDescriptorSetStorageBuffers)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxDescriptorSetStorageBuffersDynamic", static_cast<u64>(value.maxDescriptorSetStorageBuffersDynamic)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxDescriptorSetSampledImages", static_cast<u64>(value.maxDescriptorSetSampledImages)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxDescriptorSetStorageImages", static_cast<u64>(value.maxDescriptorSetStorageImages)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxDescriptorSetInputAttachments", static_cast<u64>(value.maxDescriptorSetInputAttachments)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxVertexInputAttributes", static_cast<u64>(value.maxVertexInputAttributes)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxVertexInputBindings", static_cast<u64>(value.maxVertexInputBindings)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxVertexInputAttributeOffset", static_cast<u64>(value.maxVertexInputAttributeOffset)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxVertexInputBindingStride", static_cast<u64>(value.maxVertexInputBindingStride)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxVertexOutputComponents", static_cast<u64>(value.maxVertexOutputComponents)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxTessellationGenerationLevel", static_cast<u64>(value.maxTessellationGenerationLevel)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxTessellationPatchSize", static_cast<u64>(value.maxTessellationPatchSize)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxTessellationControlPerVertexInputComponents", static_cast<u64>(value.maxTessellationControlPerVertexInputComponents)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxTessellationControlPerVertexOutputComponents", static_cast<u64>(value.maxTessellationControlPerVertexOutputComponents)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxTessellationControlPerPatchOutputComponents", static_cast<u64>(value.maxTessellationControlPerPatchOutputComponents)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxTessellationControlTotalOutputComponents", static_cast<u64>(value.maxTessellationControlTotalOutputComponents)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxTessellationEvaluationInputComponents", static_cast<u64>(value.maxTessellationEvaluationInputComponents)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxTessellationEvaluationOutputComponents", static_cast<u64>(value.maxTessellationEvaluationOutputComponents)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxGeometryShaderInvocations", static_cast<u64>(value.maxGeometryShaderInvocations)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxGeometryInputComponents", static_cast<u64>(value.maxGeometryInputComponents)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxGeometryOutputComponents", static_cast<u64>(value.maxGeometryOutputComponents)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxGeometryOutputVertices", static_cast<u64>(value.maxGeometryOutputVertices)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxGeometryTotalOutputComponents", static_cast<u64>(value.maxGeometryTotalOutputComponents)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxFragmentInputComponents", static_cast<u64>(value.maxFragmentInputComponents)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxFragmentOutputAttachments", static_cast<u64>(value.maxFragmentOutputAttachments)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxFragmentDualSrcAttachments", static_cast<u64>(value.maxFragmentDualSrcAttachments)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxFragmentCombinedOutputResources", static_cast<u64>(value.maxFragmentCombinedOutputResources)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxComputeSharedMemorySize", static_cast<u64>(value.maxComputeSharedMemorySize)});
    for (std::size_t i = 0; i < 3; ++i) {
        out.values.push_back({"VkPhysicalDeviceLimits.maxComputeWorkGroupCount[" + std::to_string(i) + "]", static_cast<u64>(value.maxComputeWorkGroupCount[i])});
    }
    out.values.push_back({"VkPhysicalDeviceLimits.maxComputeWorkGroupInvocations", static_cast<u64>(value.maxComputeWorkGroupInvocations)});
    for (std::size_t i = 0; i < 3; ++i) {
        out.values.push_back({"VkPhysicalDeviceLimits.maxComputeWorkGroupSize[" + std::to_string(i) + "]", static_cast<u64>(value.maxComputeWorkGroupSize[i])});
    }
    out.values.push_back({"VkPhysicalDeviceLimits.subPixelPrecisionBits", static_cast<u64>(value.subPixelPrecisionBits)});
    out.values.push_back({"VkPhysicalDeviceLimits.subTexelPrecisionBits", static_cast<u64>(value.subTexelPrecisionBits)});
    out.values.push_back({"VkPhysicalDeviceLimits.mipmapPrecisionBits", static_cast<u64>(value.mipmapPrecisionBits)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxDrawIndexedIndexValue", static_cast<u64>(value.maxDrawIndexedIndexValue)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxDrawIndirectCount", static_cast<u64>(value.maxDrawIndirectCount)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxSamplerLodBias", std::bit_cast<u32>(value.maxSamplerLodBias)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxSamplerAnisotropy", std::bit_cast<u32>(value.maxSamplerAnisotropy)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxViewports", static_cast<u64>(value.maxViewports)});
    for (std::size_t i = 0; i < 2; ++i) {
        out.values.push_back({"VkPhysicalDeviceLimits.maxViewportDimensions[" + std::to_string(i) + "]", static_cast<u64>(value.maxViewportDimensions[i])});
    }
    for (std::size_t i = 0; i < 2; ++i) {
        out.values.push_back({"VkPhysicalDeviceLimits.viewportBoundsRange[" + std::to_string(i) + "]", std::bit_cast<u32>(value.viewportBoundsRange[i])});
    }
    out.values.push_back({"VkPhysicalDeviceLimits.viewportSubPixelBits", static_cast<u64>(value.viewportSubPixelBits)});
    out.values.push_back({"VkPhysicalDeviceLimits.minMemoryMapAlignment", static_cast<u64>(value.minMemoryMapAlignment)});
    out.values.push_back({"VkPhysicalDeviceLimits.minTexelBufferOffsetAlignment", static_cast<u64>(value.minTexelBufferOffsetAlignment)});
    out.values.push_back({"VkPhysicalDeviceLimits.minUniformBufferOffsetAlignment", static_cast<u64>(value.minUniformBufferOffsetAlignment)});
    out.values.push_back({"VkPhysicalDeviceLimits.minStorageBufferOffsetAlignment", static_cast<u64>(value.minStorageBufferOffsetAlignment)});
    out.values.push_back({"VkPhysicalDeviceLimits.minTexelOffset", static_cast<u64>(value.minTexelOffset)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxTexelOffset", static_cast<u64>(value.maxTexelOffset)});
    out.values.push_back({"VkPhysicalDeviceLimits.minTexelGatherOffset", static_cast<u64>(value.minTexelGatherOffset)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxTexelGatherOffset", static_cast<u64>(value.maxTexelGatherOffset)});
    out.values.push_back({"VkPhysicalDeviceLimits.minInterpolationOffset", std::bit_cast<u32>(value.minInterpolationOffset)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxInterpolationOffset", std::bit_cast<u32>(value.maxInterpolationOffset)});
    out.values.push_back({"VkPhysicalDeviceLimits.subPixelInterpolationOffsetBits", static_cast<u64>(value.subPixelInterpolationOffsetBits)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxFramebufferWidth", static_cast<u64>(value.maxFramebufferWidth)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxFramebufferHeight", static_cast<u64>(value.maxFramebufferHeight)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxFramebufferLayers", static_cast<u64>(value.maxFramebufferLayers)});
    out.values.push_back({"VkPhysicalDeviceLimits.framebufferColorSampleCounts", static_cast<u64>(value.framebufferColorSampleCounts)});
    out.values.push_back({"VkPhysicalDeviceLimits.framebufferDepthSampleCounts", static_cast<u64>(value.framebufferDepthSampleCounts)});
    out.values.push_back({"VkPhysicalDeviceLimits.framebufferStencilSampleCounts", static_cast<u64>(value.framebufferStencilSampleCounts)});
    out.values.push_back({"VkPhysicalDeviceLimits.framebufferNoAttachmentsSampleCounts", static_cast<u64>(value.framebufferNoAttachmentsSampleCounts)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxColorAttachments", static_cast<u64>(value.maxColorAttachments)});
    out.values.push_back({"VkPhysicalDeviceLimits.sampledImageColorSampleCounts", static_cast<u64>(value.sampledImageColorSampleCounts)});
    out.values.push_back({"VkPhysicalDeviceLimits.sampledImageIntegerSampleCounts", static_cast<u64>(value.sampledImageIntegerSampleCounts)});
    out.values.push_back({"VkPhysicalDeviceLimits.sampledImageDepthSampleCounts", static_cast<u64>(value.sampledImageDepthSampleCounts)});
    out.values.push_back({"VkPhysicalDeviceLimits.sampledImageStencilSampleCounts", static_cast<u64>(value.sampledImageStencilSampleCounts)});
    out.values.push_back({"VkPhysicalDeviceLimits.storageImageSampleCounts", static_cast<u64>(value.storageImageSampleCounts)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxSampleMaskWords", static_cast<u64>(value.maxSampleMaskWords)});
    out.values.push_back({"VkPhysicalDeviceLimits.timestampComputeAndGraphics", static_cast<u64>(value.timestampComputeAndGraphics)});
    out.values.push_back({"VkPhysicalDeviceLimits.timestampPeriod", std::bit_cast<u32>(value.timestampPeriod)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxClipDistances", static_cast<u64>(value.maxClipDistances)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxCullDistances", static_cast<u64>(value.maxCullDistances)});
    out.values.push_back({"VkPhysicalDeviceLimits.maxCombinedClipAndCullDistances", static_cast<u64>(value.maxCombinedClipAndCullDistances)});
    out.values.push_back({"VkPhysicalDeviceLimits.discreteQueuePriorities", static_cast<u64>(value.discreteQueuePriorities)});
    for (std::size_t i = 0; i < 2; ++i) {
        out.values.push_back({"VkPhysicalDeviceLimits.pointSizeRange[" + std::to_string(i) + "]", std::bit_cast<u32>(value.pointSizeRange[i])});
    }
    for (std::size_t i = 0; i < 2; ++i) {
        out.values.push_back({"VkPhysicalDeviceLimits.lineWidthRange[" + std::to_string(i) + "]", std::bit_cast<u32>(value.lineWidthRange[i])});
    }
    out.values.push_back({"VkPhysicalDeviceLimits.pointSizeGranularity", std::bit_cast<u32>(value.pointSizeGranularity)});
    out.values.push_back({"VkPhysicalDeviceLimits.lineWidthGranularity", std::bit_cast<u32>(value.lineWidthGranularity)});
    out.values.push_back({"VkPhysicalDeviceLimits.strictLines", static_cast<u64>(value.strictLines)});
    out.values.push_back({"VkPhysicalDeviceLimits.standardSampleLocations", static_cast<u64>(value.standardSampleLocations)});
    out.values.push_back({"VkPhysicalDeviceLimits.optimalBufferCopyOffsetAlignment", static_cast<u64>(value.optimalBufferCopyOffsetAlignment)});
    out.values.push_back({"VkPhysicalDeviceLimits.optimalBufferCopyRowPitchAlignment", static_cast<u64>(value.optimalBufferCopyRowPitchAlignment)});
    out.values.push_back({"VkPhysicalDeviceLimits.nonCoherentAtomSize", static_cast<u64>(value.nonCoherentAtomSize)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceSparseProperties& value) {
    out.values.push_back({"VkPhysicalDeviceSparseProperties.residencyStandard2DBlockShape", static_cast<u64>(value.residencyStandard2DBlockShape)});
    out.values.push_back({"VkPhysicalDeviceSparseProperties.residencyStandard2DMultisampleBlockShape", static_cast<u64>(value.residencyStandard2DMultisampleBlockShape)});
    out.values.push_back({"VkPhysicalDeviceSparseProperties.residencyStandard3DBlockShape", static_cast<u64>(value.residencyStandard3DBlockShape)});
    out.values.push_back({"VkPhysicalDeviceSparseProperties.residencyAlignedMipSize", static_cast<u64>(value.residencyAlignedMipSize)});
    out.values.push_back({"VkPhysicalDeviceSparseProperties.residencyNonResidentStrict", static_cast<u64>(value.residencyNonResidentStrict)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDevice16BitStorageFeatures& value) {
    out.values.push_back({"VkPhysicalDevice16BitStorageFeatures.storageBuffer16BitAccess", static_cast<u64>(value.storageBuffer16BitAccess)});
    out.values.push_back({"VkPhysicalDevice16BitStorageFeatures.uniformAndStorageBuffer16BitAccess", static_cast<u64>(value.uniformAndStorageBuffer16BitAccess)});
    out.values.push_back({"VkPhysicalDevice16BitStorageFeatures.storagePushConstant16", static_cast<u64>(value.storagePushConstant16)});
    out.values.push_back({"VkPhysicalDevice16BitStorageFeatures.storageInputOutput16", static_cast<u64>(value.storageInputOutput16)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceShaderAtomicInt64Features& value) {
    out.values.push_back({"VkPhysicalDeviceShaderAtomicInt64Features.shaderBufferInt64Atomics", static_cast<u64>(value.shaderBufferInt64Atomics)});
    out.values.push_back({"VkPhysicalDeviceShaderAtomicInt64Features.shaderSharedInt64Atomics", static_cast<u64>(value.shaderSharedInt64Atomics)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceShaderDrawParametersFeatures& value) {
    out.values.push_back({"VkPhysicalDeviceShaderDrawParametersFeatures.shaderDrawParameters", static_cast<u64>(value.shaderDrawParameters)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceShaderFloat16Int8Features& value) {
    out.values.push_back({"VkPhysicalDeviceShaderFloat16Int8Features.shaderFloat16", static_cast<u64>(value.shaderFloat16)});
    out.values.push_back({"VkPhysicalDeviceShaderFloat16Int8Features.shaderInt8", static_cast<u64>(value.shaderInt8)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceUniformBufferStandardLayoutFeatures& value) {
    out.values.push_back({"VkPhysicalDeviceUniformBufferStandardLayoutFeatures.uniformBufferStandardLayout", static_cast<u64>(value.uniformBufferStandardLayout)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceVariablePointerFeatures& value) {
    out.values.push_back({"VkPhysicalDeviceVariablePointerFeatures.variablePointersStorageBuffer", static_cast<u64>(value.variablePointersStorageBuffer)});
    out.values.push_back({"VkPhysicalDeviceVariablePointerFeatures.variablePointers", static_cast<u64>(value.variablePointers)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceDescriptorIndexingFeatures& value) {
    out.values.push_back({"VkPhysicalDeviceDescriptorIndexingFeatures.shaderInputAttachmentArrayDynamicIndexing", static_cast<u64>(value.shaderInputAttachmentArrayDynamicIndexing)});
    out.values.push_back({"VkPhysicalDeviceDescriptorIndexingFeatures.shaderUniformTexelBufferArrayDynamicIndexing", static_cast<u64>(value.shaderUniformTexelBufferArrayDynamicIndexing)});
    out.values.push_back({"VkPhysicalDeviceDescriptorIndexingFeatures.shaderStorageTexelBufferArrayDynamicIndexing", static_cast<u64>(value.shaderStorageTexelBufferArrayDynamicIndexing)});
    out.values.push_back({"VkPhysicalDeviceDescriptorIndexingFeatures.shaderUniformBufferArrayNonUniformIndexing", static_cast<u64>(value.shaderUniformBufferArrayNonUniformIndexing)});
    out.values.push_back({"VkPhysicalDeviceDescriptorIndexingFeatures.shaderSampledImageArrayNonUniformIndexing", static_cast<u64>(value.shaderSampledImageArrayNonUniformIndexing)});
    out.values.push_back({"VkPhysicalDeviceDescriptorIndexingFeatures.shaderStorageBufferArrayNonUniformIndexing", static_cast<u64>(value.shaderStorageBufferArrayNonUniformIndexing)});
    out.values.push_back({"VkPhysicalDeviceDescriptorIndexingFeatures.shaderStorageImageArrayNonUniformIndexing", static_cast<u64>(value.shaderStorageImageArrayNonUniformIndexing)});
    out.values.push_back({"VkPhysicalDeviceDescriptorIndexingFeatures.shaderInputAttachmentArrayNonUniformIndexing", static_cast<u64>(value.shaderInputAttachmentArrayNonUniformIndexing)});
    out.values.push_back({"VkPhysicalDeviceDescriptorIndexingFeatures.shaderUniformTexelBufferArrayNonUniformIndexing", static_cast<u64>(value.shaderUniformTexelBufferArrayNonUniformIndexing)});
    out.values.push_back({"VkPhysicalDeviceDescriptorIndexingFeatures.shaderStorageTexelBufferArrayNonUniformIndexing", static_cast<u64>(value.shaderStorageTexelBufferArrayNonUniformIndexing)});
    out.values.push_back({"VkPhysicalDeviceDescriptorIndexingFeatures.descriptorBindingUniformBufferUpdateAfterBind", static_cast<u64>(value.descriptorBindingUniformBufferUpdateAfterBind)});
    out.values.push_back({"VkPhysicalDeviceDescriptorIndexingFeatures.descriptorBindingSampledImageUpdateAfterBind", static_cast<u64>(value.descriptorBindingSampledImageUpdateAfterBind)});
    out.values.push_back({"VkPhysicalDeviceDescriptorIndexingFeatures.descriptorBindingStorageImageUpdateAfterBind", static_cast<u64>(value.descriptorBindingStorageImageUpdateAfterBind)});
    out.values.push_back({"VkPhysicalDeviceDescriptorIndexingFeatures.descriptorBindingStorageBufferUpdateAfterBind", static_cast<u64>(value.descriptorBindingStorageBufferUpdateAfterBind)});
    out.values.push_back({"VkPhysicalDeviceDescriptorIndexingFeatures.descriptorBindingUniformTexelBufferUpdateAfterBind", static_cast<u64>(value.descriptorBindingUniformTexelBufferUpdateAfterBind)});
    out.values.push_back({"VkPhysicalDeviceDescriptorIndexingFeatures.descriptorBindingStorageTexelBufferUpdateAfterBind", static_cast<u64>(value.descriptorBindingStorageTexelBufferUpdateAfterBind)});
    out.values.push_back({"VkPhysicalDeviceDescriptorIndexingFeatures.descriptorBindingUpdateUnusedWhilePending", static_cast<u64>(value.descriptorBindingUpdateUnusedWhilePending)});
    out.values.push_back({"VkPhysicalDeviceDescriptorIndexingFeatures.descriptorBindingPartiallyBound", static_cast<u64>(value.descriptorBindingPartiallyBound)});
    out.values.push_back({"VkPhysicalDeviceDescriptorIndexingFeatures.descriptorBindingVariableDescriptorCount", static_cast<u64>(value.descriptorBindingVariableDescriptorCount)});
    out.values.push_back({"VkPhysicalDeviceDescriptorIndexingFeatures.runtimeDescriptorArray", static_cast<u64>(value.runtimeDescriptorArray)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceHostQueryResetFeatures& value) {
    out.values.push_back({"VkPhysicalDeviceHostQueryResetFeatures.hostQueryReset", static_cast<u64>(value.hostQueryReset)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDevice8BitStorageFeatures& value) {
    out.values.push_back({"VkPhysicalDevice8BitStorageFeatures.storageBuffer8BitAccess", static_cast<u64>(value.storageBuffer8BitAccess)});
    out.values.push_back({"VkPhysicalDevice8BitStorageFeatures.uniformAndStorageBuffer8BitAccess", static_cast<u64>(value.uniformAndStorageBuffer8BitAccess)});
    out.values.push_back({"VkPhysicalDevice8BitStorageFeatures.storagePushConstant8", static_cast<u64>(value.storagePushConstant8)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceBufferDeviceAddressFeatures& value) {
    out.values.push_back({"VkPhysicalDeviceBufferDeviceAddressFeatures.bufferDeviceAddress", static_cast<u64>(value.bufferDeviceAddress)});
    out.values.push_back({"VkPhysicalDeviceBufferDeviceAddressFeatures.bufferDeviceAddressCaptureReplay", static_cast<u64>(value.bufferDeviceAddressCaptureReplay)});
    out.values.push_back({"VkPhysicalDeviceBufferDeviceAddressFeatures.bufferDeviceAddressMultiDevice", static_cast<u64>(value.bufferDeviceAddressMultiDevice)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceTimelineSemaphoreFeatures& value) {
    out.values.push_back({"VkPhysicalDeviceTimelineSemaphoreFeatures.timelineSemaphore", static_cast<u64>(value.timelineSemaphore)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceVulkanMemoryModelFeatures& value) {
    out.values.push_back({"VkPhysicalDeviceVulkanMemoryModelFeatures.vulkanMemoryModel", static_cast<u64>(value.vulkanMemoryModel)});
    out.values.push_back({"VkPhysicalDeviceVulkanMemoryModelFeatures.vulkanMemoryModelDeviceScope", static_cast<u64>(value.vulkanMemoryModelDeviceScope)});
    out.values.push_back({"VkPhysicalDeviceVulkanMemoryModelFeatures.vulkanMemoryModelAvailabilityVisibilityChains", static_cast<u64>(value.vulkanMemoryModelAvailabilityVisibilityChains)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceImageRobustnessFeatures& value) {
    out.values.push_back({"VkPhysicalDeviceImageRobustnessFeatures.robustImageAccess", static_cast<u64>(value.robustImageAccess)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures& value) {
    out.values.push_back({"VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures.shaderDemoteToHelperInvocation", static_cast<u64>(value.shaderDemoteToHelperInvocation)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceSubgroupSizeControlFeatures& value) {
    out.values.push_back({"VkPhysicalDeviceSubgroupSizeControlFeatures.subgroupSizeControl", static_cast<u64>(value.subgroupSizeControl)});
    out.values.push_back({"VkPhysicalDeviceSubgroupSizeControlFeatures.computeFullSubgroups", static_cast<u64>(value.computeFullSubgroups)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceMaintenance4Features& value) {
    out.values.push_back({"VkPhysicalDeviceMaintenance4Features.maintenance4", static_cast<u64>(value.maintenance4)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceSynchronization2Features& value) {
    out.values.push_back({"VkPhysicalDeviceSynchronization2Features.synchronization2", static_cast<u64>(value.synchronization2)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceBorderColorSwizzleFeaturesEXT& value) {
    out.values.push_back({"VkPhysicalDeviceBorderColorSwizzleFeaturesEXT.borderColorSwizzle", static_cast<u64>(value.borderColorSwizzle)});
    out.values.push_back({"VkPhysicalDeviceBorderColorSwizzleFeaturesEXT.borderColorSwizzleFromImage", static_cast<u64>(value.borderColorSwizzleFromImage)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceColorWriteEnableFeaturesEXT& value) {
    out.values.push_back({"VkPhysicalDeviceColorWriteEnableFeaturesEXT.colorWriteEnable", static_cast<u64>(value.colorWriteEnable)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceCustomBorderColorFeaturesEXT& value) {
    out.values.push_back({"VkPhysicalDeviceCustomBorderColorFeaturesEXT.customBorderColors", static_cast<u64>(value.customBorderColors)});
    out.values.push_back({"VkPhysicalDeviceCustomBorderColorFeaturesEXT.customBorderColorWithoutFormat", static_cast<u64>(value.customBorderColorWithoutFormat)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceDepthBiasControlFeaturesEXT& value) {
    out.values.push_back({"VkPhysicalDeviceDepthBiasControlFeaturesEXT.depthBiasControl", static_cast<u64>(value.depthBiasControl)});
    out.values.push_back({"VkPhysicalDeviceDepthBiasControlFeaturesEXT.leastRepresentableValueForceUnormRepresentation", static_cast<u64>(value.leastRepresentableValueForceUnormRepresentation)});
    out.values.push_back({"VkPhysicalDeviceDepthBiasControlFeaturesEXT.floatRepresentation", static_cast<u64>(value.floatRepresentation)});
    out.values.push_back({"VkPhysicalDeviceDepthBiasControlFeaturesEXT.depthBiasExact", static_cast<u64>(value.depthBiasExact)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceDepthClipControlFeaturesEXT& value) {
    out.values.push_back({"VkPhysicalDeviceDepthClipControlFeaturesEXT.depthClipControl", static_cast<u64>(value.depthClipControl)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceDescriptorBufferFeaturesEXT& value) {
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferFeaturesEXT.descriptorBuffer", static_cast<u64>(value.descriptorBuffer)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferFeaturesEXT.descriptorBufferCaptureReplay", static_cast<u64>(value.descriptorBufferCaptureReplay)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferFeaturesEXT.descriptorBufferImageLayoutIgnored", static_cast<u64>(value.descriptorBufferImageLayoutIgnored)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferFeaturesEXT.descriptorBufferPushDescriptors", static_cast<u64>(value.descriptorBufferPushDescriptors)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceExtendedDynamicStateFeaturesEXT& value) {
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicStateFeaturesEXT.extendedDynamicState", static_cast<u64>(value.extendedDynamicState)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceExtendedDynamicState2FeaturesEXT& value) {
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState2FeaturesEXT.extendedDynamicState2", static_cast<u64>(value.extendedDynamicState2)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState2FeaturesEXT.extendedDynamicState2LogicOp", static_cast<u64>(value.extendedDynamicState2LogicOp)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState2FeaturesEXT.extendedDynamicState2PatchControlPoints", static_cast<u64>(value.extendedDynamicState2PatchControlPoints)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceExtendedDynamicState3FeaturesEXT& value) {
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3TessellationDomainOrigin", static_cast<u64>(value.extendedDynamicState3TessellationDomainOrigin)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3DepthClampEnable", static_cast<u64>(value.extendedDynamicState3DepthClampEnable)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3PolygonMode", static_cast<u64>(value.extendedDynamicState3PolygonMode)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3RasterizationSamples", static_cast<u64>(value.extendedDynamicState3RasterizationSamples)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3SampleMask", static_cast<u64>(value.extendedDynamicState3SampleMask)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3AlphaToCoverageEnable", static_cast<u64>(value.extendedDynamicState3AlphaToCoverageEnable)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3AlphaToOneEnable", static_cast<u64>(value.extendedDynamicState3AlphaToOneEnable)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3LogicOpEnable", static_cast<u64>(value.extendedDynamicState3LogicOpEnable)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3ColorBlendEnable", static_cast<u64>(value.extendedDynamicState3ColorBlendEnable)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3ColorBlendEquation", static_cast<u64>(value.extendedDynamicState3ColorBlendEquation)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3ColorWriteMask", static_cast<u64>(value.extendedDynamicState3ColorWriteMask)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3RasterizationStream", static_cast<u64>(value.extendedDynamicState3RasterizationStream)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3ConservativeRasterizationMode", static_cast<u64>(value.extendedDynamicState3ConservativeRasterizationMode)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3ExtraPrimitiveOverestimationSize", static_cast<u64>(value.extendedDynamicState3ExtraPrimitiveOverestimationSize)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3DepthClipEnable", static_cast<u64>(value.extendedDynamicState3DepthClipEnable)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3SampleLocationsEnable", static_cast<u64>(value.extendedDynamicState3SampleLocationsEnable)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3ColorBlendAdvanced", static_cast<u64>(value.extendedDynamicState3ColorBlendAdvanced)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3ProvokingVertexMode", static_cast<u64>(value.extendedDynamicState3ProvokingVertexMode)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3LineRasterizationMode", static_cast<u64>(value.extendedDynamicState3LineRasterizationMode)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3LineStippleEnable", static_cast<u64>(value.extendedDynamicState3LineStippleEnable)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3DepthClipNegativeOneToOne", static_cast<u64>(value.extendedDynamicState3DepthClipNegativeOneToOne)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3ViewportWScalingEnable", static_cast<u64>(value.extendedDynamicState3ViewportWScalingEnable)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3ViewportSwizzle", static_cast<u64>(value.extendedDynamicState3ViewportSwizzle)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3CoverageToColorEnable", static_cast<u64>(value.extendedDynamicState3CoverageToColorEnable)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3CoverageToColorLocation", static_cast<u64>(value.extendedDynamicState3CoverageToColorLocation)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3CoverageModulationMode", static_cast<u64>(value.extendedDynamicState3CoverageModulationMode)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3CoverageModulationTableEnable", static_cast<u64>(value.extendedDynamicState3CoverageModulationTableEnable)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3CoverageModulationTable", static_cast<u64>(value.extendedDynamicState3CoverageModulationTable)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3CoverageReductionMode", static_cast<u64>(value.extendedDynamicState3CoverageReductionMode)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3RepresentativeFragmentTestEnable", static_cast<u64>(value.extendedDynamicState3RepresentativeFragmentTestEnable)});
    out.values.push_back({"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3ShadingRateImageEnable", static_cast<u64>(value.extendedDynamicState3ShadingRateImageEnable)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDevice4444FormatsFeaturesEXT& value) {
    out.values.push_back({"VkPhysicalDevice4444FormatsFeaturesEXT.formatA4R4G4B4", static_cast<u64>(value.formatA4R4G4B4)});
    out.values.push_back({"VkPhysicalDevice4444FormatsFeaturesEXT.formatA4B4G4R4", static_cast<u64>(value.formatA4B4G4R4)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceIndexTypeUint8FeaturesEXT& value) {
    out.values.push_back({"VkPhysicalDeviceIndexTypeUint8FeaturesEXT.indexTypeUint8", static_cast<u64>(value.indexTypeUint8)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceLineRasterizationFeaturesEXT& value) {
    out.values.push_back({"VkPhysicalDeviceLineRasterizationFeaturesEXT.rectangularLines", static_cast<u64>(value.rectangularLines)});
    out.values.push_back({"VkPhysicalDeviceLineRasterizationFeaturesEXT.bresenhamLines", static_cast<u64>(value.bresenhamLines)});
    out.values.push_back({"VkPhysicalDeviceLineRasterizationFeaturesEXT.smoothLines", static_cast<u64>(value.smoothLines)});
    out.values.push_back({"VkPhysicalDeviceLineRasterizationFeaturesEXT.stippledRectangularLines", static_cast<u64>(value.stippledRectangularLines)});
    out.values.push_back({"VkPhysicalDeviceLineRasterizationFeaturesEXT.stippledBresenhamLines", static_cast<u64>(value.stippledBresenhamLines)});
    out.values.push_back({"VkPhysicalDeviceLineRasterizationFeaturesEXT.stippledSmoothLines", static_cast<u64>(value.stippledSmoothLines)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT& value) {
    out.values.push_back({"VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT.primitiveTopologyListRestart", static_cast<u64>(value.primitiveTopologyListRestart)});
    out.values.push_back({"VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT.primitiveTopologyPatchListRestart", static_cast<u64>(value.primitiveTopologyPatchListRestart)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceProvokingVertexFeaturesEXT& value) {
    out.values.push_back({"VkPhysicalDeviceProvokingVertexFeaturesEXT.provokingVertexLast", static_cast<u64>(value.provokingVertexLast)});
    out.values.push_back({"VkPhysicalDeviceProvokingVertexFeaturesEXT.transformFeedbackPreservesProvokingVertex", static_cast<u64>(value.transformFeedbackPreservesProvokingVertex)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceRobustness2FeaturesEXT& value) {
    out.values.push_back({"VkPhysicalDeviceRobustness2FeaturesEXT.robustBufferAccess2", static_cast<u64>(value.robustBufferAccess2)});
    out.values.push_back({"VkPhysicalDeviceRobustness2FeaturesEXT.robustImageAccess2", static_cast<u64>(value.robustImageAccess2)});
    out.values.push_back({"VkPhysicalDeviceRobustness2FeaturesEXT.nullDescriptor", static_cast<u64>(value.nullDescriptor)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceTransformFeedbackFeaturesEXT& value) {
    out.values.push_back({"VkPhysicalDeviceTransformFeedbackFeaturesEXT.transformFeedback", static_cast<u64>(value.transformFeedback)});
    out.values.push_back({"VkPhysicalDeviceTransformFeedbackFeaturesEXT.geometryStreams", static_cast<u64>(value.geometryStreams)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT& value) {
    out.values.push_back({"VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT.vertexInputDynamicState", static_cast<u64>(value.vertexInputDynamicState)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceMaintenance5FeaturesKHR& value) {
    out.values.push_back({"VkPhysicalDeviceMaintenance5FeaturesKHR.maintenance5", static_cast<u64>(value.maintenance5)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceMaintenance6FeaturesKHR& value) {
    out.values.push_back({"VkPhysicalDeviceMaintenance6FeaturesKHR.maintenance6", static_cast<u64>(value.maintenance6)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR& value) {
    out.values.push_back({"VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR.pipelineExecutableInfo", static_cast<u64>(value.pipelineExecutableInfo)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR& value) {
    out.values.push_back({"VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR.workgroupMemoryExplicitLayout", static_cast<u64>(value.workgroupMemoryExplicitLayout)});
    out.values.push_back({"VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR.workgroupMemoryExplicitLayoutScalarBlockLayout", static_cast<u64>(value.workgroupMemoryExplicitLayoutScalarBlockLayout)});
    out.values.push_back({"VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR.workgroupMemoryExplicitLayout8BitAccess", static_cast<u64>(value.workgroupMemoryExplicitLayout8BitAccess)});
    out.values.push_back({"VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR.workgroupMemoryExplicitLayout16BitAccess", static_cast<u64>(value.workgroupMemoryExplicitLayout16BitAccess)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceProperties& value) {
    out.values.push_back({"VkPhysicalDeviceProperties.apiVersion", static_cast<u64>(value.apiVersion)});
    out.values.push_back({"VkPhysicalDeviceProperties.driverVersion", static_cast<u64>(value.driverVersion)});
    out.values.push_back({"VkPhysicalDeviceProperties.vendorID", static_cast<u64>(value.vendorID)});
    out.values.push_back({"VkPhysicalDeviceProperties.deviceID", static_cast<u64>(value.deviceID)});
    out.values.push_back({"VkPhysicalDeviceProperties.deviceType", static_cast<u64>(value.deviceType)});
    AppendCapabilitySnapshot(out, value.limits);
    AppendCapabilitySnapshot(out, value.sparseProperties);
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceSubgroupProperties& value) {
    out.values.push_back({"VkPhysicalDeviceSubgroupProperties.subgroupSize", static_cast<u64>(value.subgroupSize)});
    out.values.push_back({"VkPhysicalDeviceSubgroupProperties.supportedStages", static_cast<u64>(value.supportedStages)});
    out.values.push_back({"VkPhysicalDeviceSubgroupProperties.supportedOperations", static_cast<u64>(value.supportedOperations)});
    out.values.push_back({"VkPhysicalDeviceSubgroupProperties.quadOperationsInAllStages", static_cast<u64>(value.quadOperationsInAllStages)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceFloatControlsProperties& value) {
    out.values.push_back({"VkPhysicalDeviceFloatControlsProperties.denormBehaviorIndependence", static_cast<u64>(value.denormBehaviorIndependence)});
    out.values.push_back({"VkPhysicalDeviceFloatControlsProperties.roundingModeIndependence", static_cast<u64>(value.roundingModeIndependence)});
    out.values.push_back({"VkPhysicalDeviceFloatControlsProperties.shaderSignedZeroInfNanPreserveFloat16", static_cast<u64>(value.shaderSignedZeroInfNanPreserveFloat16)});
    out.values.push_back({"VkPhysicalDeviceFloatControlsProperties.shaderSignedZeroInfNanPreserveFloat32", static_cast<u64>(value.shaderSignedZeroInfNanPreserveFloat32)});
    out.values.push_back({"VkPhysicalDeviceFloatControlsProperties.shaderSignedZeroInfNanPreserveFloat64", static_cast<u64>(value.shaderSignedZeroInfNanPreserveFloat64)});
    out.values.push_back({"VkPhysicalDeviceFloatControlsProperties.shaderDenormPreserveFloat16", static_cast<u64>(value.shaderDenormPreserveFloat16)});
    out.values.push_back({"VkPhysicalDeviceFloatControlsProperties.shaderDenormPreserveFloat32", static_cast<u64>(value.shaderDenormPreserveFloat32)});
    out.values.push_back({"VkPhysicalDeviceFloatControlsProperties.shaderDenormPreserveFloat64", static_cast<u64>(value.shaderDenormPreserveFloat64)});
    out.values.push_back({"VkPhysicalDeviceFloatControlsProperties.shaderDenormFlushToZeroFloat16", static_cast<u64>(value.shaderDenormFlushToZeroFloat16)});
    out.values.push_back({"VkPhysicalDeviceFloatControlsProperties.shaderDenormFlushToZeroFloat32", static_cast<u64>(value.shaderDenormFlushToZeroFloat32)});
    out.values.push_back({"VkPhysicalDeviceFloatControlsProperties.shaderDenormFlushToZeroFloat64", static_cast<u64>(value.shaderDenormFlushToZeroFloat64)});
    out.values.push_back({"VkPhysicalDeviceFloatControlsProperties.shaderRoundingModeRTEFloat16", static_cast<u64>(value.shaderRoundingModeRTEFloat16)});
    out.values.push_back({"VkPhysicalDeviceFloatControlsProperties.shaderRoundingModeRTEFloat32", static_cast<u64>(value.shaderRoundingModeRTEFloat32)});
    out.values.push_back({"VkPhysicalDeviceFloatControlsProperties.shaderRoundingModeRTEFloat64", static_cast<u64>(value.shaderRoundingModeRTEFloat64)});
    out.values.push_back({"VkPhysicalDeviceFloatControlsProperties.shaderRoundingModeRTZFloat16", static_cast<u64>(value.shaderRoundingModeRTZFloat16)});
    out.values.push_back({"VkPhysicalDeviceFloatControlsProperties.shaderRoundingModeRTZFloat32", static_cast<u64>(value.shaderRoundingModeRTZFloat32)});
    out.values.push_back({"VkPhysicalDeviceFloatControlsProperties.shaderRoundingModeRTZFloat64", static_cast<u64>(value.shaderRoundingModeRTZFloat64)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDevicePushDescriptorPropertiesKHR& value) {
    out.values.push_back({"VkPhysicalDevicePushDescriptorPropertiesKHR.maxPushDescriptors", static_cast<u64>(value.maxPushDescriptors)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceDescriptorBufferPropertiesEXT& value) {
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.combinedImageSamplerDescriptorSingleArray", static_cast<u64>(value.combinedImageSamplerDescriptorSingleArray)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.bufferlessPushDescriptors", static_cast<u64>(value.bufferlessPushDescriptors)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.allowSamplerImageViewPostSubmitCreation", static_cast<u64>(value.allowSamplerImageViewPostSubmitCreation)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.descriptorBufferOffsetAlignment", static_cast<u64>(value.descriptorBufferOffsetAlignment)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.maxDescriptorBufferBindings", static_cast<u64>(value.maxDescriptorBufferBindings)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.maxResourceDescriptorBufferBindings", static_cast<u64>(value.maxResourceDescriptorBufferBindings)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.maxSamplerDescriptorBufferBindings", static_cast<u64>(value.maxSamplerDescriptorBufferBindings)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.maxEmbeddedImmutableSamplerBindings", static_cast<u64>(value.maxEmbeddedImmutableSamplerBindings)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.maxEmbeddedImmutableSamplers", static_cast<u64>(value.maxEmbeddedImmutableSamplers)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.bufferCaptureReplayDescriptorDataSize", static_cast<u64>(value.bufferCaptureReplayDescriptorDataSize)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.imageCaptureReplayDescriptorDataSize", static_cast<u64>(value.imageCaptureReplayDescriptorDataSize)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.imageViewCaptureReplayDescriptorDataSize", static_cast<u64>(value.imageViewCaptureReplayDescriptorDataSize)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.samplerCaptureReplayDescriptorDataSize", static_cast<u64>(value.samplerCaptureReplayDescriptorDataSize)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.accelerationStructureCaptureReplayDescriptorDataSize", static_cast<u64>(value.accelerationStructureCaptureReplayDescriptorDataSize)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.samplerDescriptorSize", static_cast<u64>(value.samplerDescriptorSize)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.combinedImageSamplerDescriptorSize", static_cast<u64>(value.combinedImageSamplerDescriptorSize)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.sampledImageDescriptorSize", static_cast<u64>(value.sampledImageDescriptorSize)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.storageImageDescriptorSize", static_cast<u64>(value.storageImageDescriptorSize)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.uniformTexelBufferDescriptorSize", static_cast<u64>(value.uniformTexelBufferDescriptorSize)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.robustUniformTexelBufferDescriptorSize", static_cast<u64>(value.robustUniformTexelBufferDescriptorSize)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.storageTexelBufferDescriptorSize", static_cast<u64>(value.storageTexelBufferDescriptorSize)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.robustStorageTexelBufferDescriptorSize", static_cast<u64>(value.robustStorageTexelBufferDescriptorSize)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.uniformBufferDescriptorSize", static_cast<u64>(value.uniformBufferDescriptorSize)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.robustUniformBufferDescriptorSize", static_cast<u64>(value.robustUniformBufferDescriptorSize)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.storageBufferDescriptorSize", static_cast<u64>(value.storageBufferDescriptorSize)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.robustStorageBufferDescriptorSize", static_cast<u64>(value.robustStorageBufferDescriptorSize)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.inputAttachmentDescriptorSize", static_cast<u64>(value.inputAttachmentDescriptorSize)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.accelerationStructureDescriptorSize", static_cast<u64>(value.accelerationStructureDescriptorSize)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.maxSamplerDescriptorBufferRange", static_cast<u64>(value.maxSamplerDescriptorBufferRange)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.maxResourceDescriptorBufferRange", static_cast<u64>(value.maxResourceDescriptorBufferRange)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.samplerDescriptorBufferAddressSpaceSize", static_cast<u64>(value.samplerDescriptorBufferAddressSpaceSize)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.resourceDescriptorBufferAddressSpaceSize", static_cast<u64>(value.resourceDescriptorBufferAddressSpaceSize)});
    out.values.push_back({"VkPhysicalDeviceDescriptorBufferPropertiesEXT.descriptorBufferAddressSpaceSize", static_cast<u64>(value.descriptorBufferAddressSpaceSize)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceSubgroupSizeControlProperties& value) {
    out.values.push_back({"VkPhysicalDeviceSubgroupSizeControlProperties.minSubgroupSize", static_cast<u64>(value.minSubgroupSize)});
    out.values.push_back({"VkPhysicalDeviceSubgroupSizeControlProperties.maxSubgroupSize", static_cast<u64>(value.maxSubgroupSize)});
    out.values.push_back({"VkPhysicalDeviceSubgroupSizeControlProperties.maxComputeWorkgroupSubgroups", static_cast<u64>(value.maxComputeWorkgroupSubgroups)});
    out.values.push_back({"VkPhysicalDeviceSubgroupSizeControlProperties.requiredSubgroupSizeStages", static_cast<u64>(value.requiredSubgroupSizeStages)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceTransformFeedbackPropertiesEXT& value) {
    out.values.push_back({"VkPhysicalDeviceTransformFeedbackPropertiesEXT.maxTransformFeedbackStreams", static_cast<u64>(value.maxTransformFeedbackStreams)});
    out.values.push_back({"VkPhysicalDeviceTransformFeedbackPropertiesEXT.maxTransformFeedbackBuffers", static_cast<u64>(value.maxTransformFeedbackBuffers)});
    out.values.push_back({"VkPhysicalDeviceTransformFeedbackPropertiesEXT.maxTransformFeedbackBufferSize", static_cast<u64>(value.maxTransformFeedbackBufferSize)});
    out.values.push_back({"VkPhysicalDeviceTransformFeedbackPropertiesEXT.maxTransformFeedbackStreamDataSize", static_cast<u64>(value.maxTransformFeedbackStreamDataSize)});
    out.values.push_back({"VkPhysicalDeviceTransformFeedbackPropertiesEXT.maxTransformFeedbackBufferDataSize", static_cast<u64>(value.maxTransformFeedbackBufferDataSize)});
    out.values.push_back({"VkPhysicalDeviceTransformFeedbackPropertiesEXT.maxTransformFeedbackBufferDataStride", static_cast<u64>(value.maxTransformFeedbackBufferDataStride)});
    out.values.push_back({"VkPhysicalDeviceTransformFeedbackPropertiesEXT.transformFeedbackQueries", static_cast<u64>(value.transformFeedbackQueries)});
    out.values.push_back({"VkPhysicalDeviceTransformFeedbackPropertiesEXT.transformFeedbackStreamsLinesTriangles", static_cast<u64>(value.transformFeedbackStreamsLinesTriangles)});
    out.values.push_back({"VkPhysicalDeviceTransformFeedbackPropertiesEXT.transformFeedbackRasterizationStreamSelect", static_cast<u64>(value.transformFeedbackRasterizationStreamSelect)});
    out.values.push_back({"VkPhysicalDeviceTransformFeedbackPropertiesEXT.transformFeedbackDraw", static_cast<u64>(value.transformFeedbackDraw)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceMaintenance5PropertiesKHR& value) {
    out.values.push_back({"VkPhysicalDeviceMaintenance5PropertiesKHR.earlyFragmentMultisampleCoverageAfterSampleCounting", static_cast<u64>(value.earlyFragmentMultisampleCoverageAfterSampleCounting)});
    out.values.push_back({"VkPhysicalDeviceMaintenance5PropertiesKHR.earlyFragmentSampleMaskTestBeforeSampleCounting", static_cast<u64>(value.earlyFragmentSampleMaskTestBeforeSampleCounting)});
    out.values.push_back({"VkPhysicalDeviceMaintenance5PropertiesKHR.depthStencilSwizzleOneSupport", static_cast<u64>(value.depthStencilSwizzleOneSupport)});
    out.values.push_back({"VkPhysicalDeviceMaintenance5PropertiesKHR.polygonModePointSize", static_cast<u64>(value.polygonModePointSize)});
    out.values.push_back({"VkPhysicalDeviceMaintenance5PropertiesKHR.nonStrictSinglePixelWideLinesUseParallelogram", static_cast<u64>(value.nonStrictSinglePixelWideLinesUseParallelogram)});
    out.values.push_back({"VkPhysicalDeviceMaintenance5PropertiesKHR.nonStrictWideLinesUseParallelogram", static_cast<u64>(value.nonStrictWideLinesUseParallelogram)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceDepthStencilResolveProperties& value) {
    out.values.push_back({"VkPhysicalDeviceDepthStencilResolveProperties.supportedDepthResolveModes", static_cast<u64>(value.supportedDepthResolveModes)});
    out.values.push_back({"VkPhysicalDeviceDepthStencilResolveProperties.supportedStencilResolveModes", static_cast<u64>(value.supportedStencilResolveModes)});
    out.values.push_back({"VkPhysicalDeviceDepthStencilResolveProperties.independentResolveNone", static_cast<u64>(value.independentResolveNone)});
    out.values.push_back({"VkPhysicalDeviceDepthStencilResolveProperties.independentResolve", static_cast<u64>(value.independentResolve)});
}

void AppendCapabilitySnapshot(DeviceCapabilitySnapshot& out, const VkPhysicalDeviceCustomBorderColorPropertiesEXT& value) {
    out.values.push_back({"VkPhysicalDeviceCustomBorderColorPropertiesEXT.maxCustomBorderColorSamplers", static_cast<u64>(value.maxCustomBorderColorSamplers)});
}

DeviceCapabilitySnapshot CaptureFeatureChain(const VkPhysicalDeviceFeatures2& features,
                                             const VkPhysicalDeviceFeatures* output_core) {
    DeviceCapabilitySnapshot out;
    AppendCapabilitySnapshot(out, output_core ? *output_core : features.features);
    const auto* entry = static_cast<const VkBaseInStructure*>(features.pNext);
    while (entry) {
        switch (entry->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDevice16BitStorageFeatures*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceShaderAtomicInt64Features*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceShaderDrawParametersFeatures*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceShaderFloat16Int8Features*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceUniformBufferStandardLayoutFeatures*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceVariablePointerFeatures*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceDescriptorIndexingFeatures*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceHostQueryResetFeatures*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDevice8BitStorageFeatures*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceBufferDeviceAddressFeatures*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreFeatures*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceVulkanMemoryModelFeatures*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceImageRobustnessFeatures*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceSubgroupSizeControlFeatures*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceMaintenance4Features*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceSynchronization2Features*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BORDER_COLOR_SWIZZLE_FEATURES_EXT:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceBorderColorSwizzleFeaturesEXT*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COLOR_WRITE_ENABLE_FEATURES_EXT:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceColorWriteEnableFeaturesEXT*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceCustomBorderColorFeaturesEXT*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceDepthBiasControlFeaturesEXT*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceDepthClipControlFeaturesEXT*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceDescriptorBufferFeaturesEXT*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceExtendedDynamicStateFeaturesEXT*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceExtendedDynamicState2FeaturesEXT*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceExtendedDynamicState3FeaturesEXT*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDevice4444FormatsFeaturesEXT*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceIndexTypeUint8FeaturesEXT*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceLineRasterizationFeaturesEXT*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceProvokingVertexFeaturesEXT*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceRobustness2FeaturesEXT*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceTransformFeedbackFeaturesEXT*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceMaintenance5FeaturesKHR*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceMaintenance6FeaturesKHR*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR*>(entry));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR:
            AppendCapabilitySnapshot(out, *reinterpret_cast<const VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR*>(entry));
            break;
        default:
            // This renderer currently enables only the registered structures above.
            // New feature structures must be added here before using their output in a cache.
            break;
        }
        entry = entry->pNext;
    }
    return out;
}

} // namespace Vulkan
