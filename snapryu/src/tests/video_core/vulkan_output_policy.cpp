// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <catch2/catch_test_macros.hpp>

#include "video_core/vulkan_common/vulkan_capability_snapshot.h"
#include "video_core/vulkan_common/vulkan_output_policy.h"

namespace {
using Mode = Settings::VulkanBackendMode;

Vulkan::DeviceFingerprint QualcommFingerprint() {
    return {.vendor_id = 0x5143,
            .driver_id = VK_DRIVER_ID_QUALCOMM_PROPRIETARY,
            .platform = Vulkan::DevicePlatform::Windows};
}
} // namespace

TEST_CASE("Vulkan backend resolves from actual vendor and driver", "[video_core][vulkan]") {
    const auto fingerprint = QualcommFingerprint();
    const Vulkan::VulkanOutputPolicy standard{Mode::Standard, fingerprint, {}, {}};
    const Vulkan::VulkanOutputPolicy adreno{Mode::Adreno, fingerprint, {}, {}};
    CHECK(standard.GetEffectiveMode() == Mode::Standard);
    CHECK(adreno.GetRequestedMode() == Mode::Adreno);
    CHECK(adreno.GetEffectiveMode() == Mode::Adreno);
    CHECK(adreno.GetProfileName() == "Qualcomm");
    CHECK(adreno.GetFallbackReason().empty());
    CHECK(standard.GetDigest() != adreno.GetDigest());

    auto turnip = fingerprint;
    turnip.driver_id = VK_DRIVER_ID_MESA_TURNIP;
    const Vulkan::VulkanOutputPolicy turnip_policy{Mode::Adreno, turnip, {}, {}};
    CHECK(turnip_policy.GetEffectiveMode() == Mode::Adreno);
    CHECK(turnip_policy.GetProfileName() == "Turnip");

    auto unknown = fingerprint;
    unknown.driver_id = VK_DRIVER_ID_ARM_PROPRIETARY;
    const Vulkan::VulkanOutputPolicy unknown_policy{Mode::Adreno, unknown, {}, {}};
    const Vulkan::VulkanOutputPolicy unknown_standard{Mode::Standard, unknown, {}, {}};
    CHECK(unknown_policy.GetEffectiveMode() == Mode::Standard);
    CHECK_FALSE(unknown_policy.GetFallbackReason().empty());
    CHECK(unknown_policy.GetDigest() == unknown_standard.GetDigest());

    auto vendor_mismatch = fingerprint;
    vendor_mismatch.vendor_id = 0x1002;
    CHECK(Vulkan::VulkanOutputPolicy{Mode::Adreno, vendor_mismatch, {}, {}}.GetEffectiveMode() ==
          Mode::Standard);
    auto platform_mismatch = fingerprint;
    platform_mismatch.platform = Vulkan::DevicePlatform::Other;
    CHECK(Vulkan::VulkanOutputPolicy{Mode::Adreno, platform_mismatch, {}, {}}.GetEffectiveMode() ==
          Mode::Standard);
}

TEST_CASE("Vulkan policy digest serializes values without padding", "[video_core][vulkan]") {
    const auto fingerprint = QualcommFingerprint();
    CHECK(fingerprint.GetDigest() == 0xa333964fd910911dULL);
    Vulkan::DeviceCapabilitySnapshot output;
    output.values = {{"descriptor.buffer", 1}, {"dynamic.mask", 3}, {"subgroup.size", 64}};
    output.extensions = {"VK_EXT_descriptor_buffer", "VK_EXT_subgroup_size_control"};
    const Vulkan::VulkanOutputPolicy original{Mode::Adreno, fingerprint, {}, output};
    auto reordered = output;
    std::ranges::reverse(reordered.values);
    std::ranges::reverse(reordered.extensions);
    CHECK(Vulkan::VulkanOutputPolicy{Mode::Adreno, fingerprint, {}, reordered}.GetDigest() ==
          original.GetDigest());
    for (std::size_t i = 0; i < output.values.size(); ++i) {
        auto changed = output;
        ++changed.values[i].value;
        CHECK(Vulkan::VulkanOutputPolicy{Mode::Adreno, fingerprint, {}, changed}.GetDigest() !=
              original.GetDigest());
    }
    auto changed_fingerprint = fingerprint;
    changed_fingerprint.driver_version = 1;
    CHECK(changed_fingerprint.GetDigest() != fingerprint.GetDigest());
    changed_fingerprint = fingerprint;
    changed_fingerprint.driver_uuid.back() = 1;
    CHECK(changed_fingerprint.GetDigest() != fingerprint.GetDigest());
    changed_fingerprint = fingerprint;
    changed_fingerprint.pipeline_cache_uuid.back() = 1;
    CHECK(changed_fingerprint.GetDigest() != fingerprint.GetDigest());
}

TEST_CASE("Vulkan capability snapshots separate enabled and emitter int64", "[video_core][vulkan]") {
    VkPhysicalDeviceFeatures2 creation{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    creation.features.shaderInt64 = VK_TRUE;
    VkPhysicalDeviceShaderFloat16Int8Features float_features{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES,
        .shaderFloat16 = VK_TRUE,
        .shaderInt8 = VK_TRUE};
    creation.pNext = &float_features;
    auto emitter = creation.features;
    emitter.shaderInt64 = VK_FALSE;
    const auto enabled = Vulkan::CaptureFeatureChain(creation);
    const auto output = Vulkan::CaptureFeatureChain(creation, &emitter);
    CHECK(enabled.GetValue("VkPhysicalDeviceFeatures.shaderInt64") == VK_TRUE);
    CHECK(output.GetValue("VkPhysicalDeviceFeatures.shaderInt64") == VK_FALSE);
    CHECK(output.GetValue("VkPhysicalDeviceShaderFloat16Int8Features.shaderFloat16") == VK_TRUE);
    CHECK(creation.features.shaderInt64 == VK_TRUE);
    CHECK(creation.pNext == &float_features);
    const Vulkan::VulkanOutputPolicy policy{Mode::Adreno, QualcommFingerprint(), enabled, output};
    CHECK(policy.GetEnabledCaps().GetValue("VkPhysicalDeviceFeatures.shaderInt64") == VK_TRUE);
    CHECK(policy.GetOutputCaps().GetValue("VkPhysicalDeviceFeatures.shaderInt64") == VK_FALSE);
}

TEST_CASE("Vulkan diagnostic output cannot persist in normal caches", "[video_core][vulkan]") {
    const auto fingerprint = QualcommFingerprint();
    const Vulkan::VulkanOutputPolicy normal{Mode::Standard, fingerprint, {}, {}};
    CHECK(normal.CanPersistCaches());
    CHECK_FALSE(normal.DisableSubgroups());
    CHECK(normal.NeedsSubgroupMaskExtractWorkaround());
    const Vulkan::VulkanOutputPolicy diagnostic{
        Mode::Standard, fingerprint, {}, {},
        {{"EDEN_GPU_NO_SUBGROUP", "1"}, {"EDEN_QCOM_SUBGROUP_EXTRACT_FIX", "0"}}};
    CHECK_FALSE(diagnostic.CanPersistCaches());
    CHECK(diagnostic.DisableSubgroups());
    CHECK_FALSE(diagnostic.NeedsSubgroupMaskExtractWorkaround());
    CHECK(diagnostic.GetDigest() != normal.GetDigest());
    const Vulkan::VulkanOutputPolicy explicit_default{
        Mode::Standard, fingerprint, {}, {}, {{"EDEN_GPU_NO_SUBGROUP", "0"}}};
    CHECK_FALSE(explicit_default.CanPersistCaches());
    CHECK_FALSE(explicit_default.DisableSubgroups());
}
