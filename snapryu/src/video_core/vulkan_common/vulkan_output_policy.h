// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan_core.h>

#include "common/common_types.h"
#include "common/settings_enums.h"

namespace Vulkan {

enum class DevicePlatform : u32 { Other, Windows, Android, Linux, MacOS };

struct DeviceFingerprint {
    u32 vendor_id{};
    u32 device_id{};
    u32 driver_id{};
    u32 driver_version{};
    u32 api_version{};
    DevicePlatform platform{};
    std::array<u8, VK_UUID_SIZE> device_uuid{};
    std::array<u8, VK_UUID_SIZE> driver_uuid{};
    std::array<u8, VK_UUID_SIZE> pipeline_cache_uuid{};

    // Explicit little-endian serialization, independent of struct padding and std::hash.
    [[nodiscard]] u64 GetDigest() const;
};

struct CapabilityValue {
    std::string name;
    u64 value{};
};

// Only scalar values are copied. No Vulkan pNext pointer escapes into these snapshots.
// Floating-point limits are stored by their IEEE bit representation.
struct DeviceCapabilitySnapshot {
    std::vector<CapabilityValue> values;
    std::vector<std::string> extensions;

    [[nodiscard]] u64 GetValue(std::string_view name, u64 default_value = 0) const;
};

// Raw covers the feature/property structures actually probed by this renderer, not every
// Vulkan extension in existence. Enabled is the exact feature chain submitted to vkCreateDevice.
using RawDeviceCaps = DeviceCapabilitySnapshot;
using EnabledCaps = DeviceCapabilitySnapshot;

struct DiagnosticValue {
    std::string name;
    std::string value;
};

// Baseline identity only: revision 1 preserves the existing Vulkan driver workarounds.
// It does not certify an optimization or fix on all devices using a recognized driver.
class VulkanOutputPolicy {
public:
    static constexpr u32 SerializationRevision = 1;
    static constexpr u32 ExistingRulesRevision = 1;

    VulkanOutputPolicy(Settings::VulkanBackendMode requested, DeviceFingerprint fingerprint,
                       EnabledCaps enabled, DeviceCapabilitySnapshot output,
                       std::vector<DiagnosticValue> diagnostics = {});

    [[nodiscard]] Settings::VulkanBackendMode GetRequestedMode() const {
        return requested_mode;
    }
    [[nodiscard]] Settings::VulkanBackendMode GetEffectiveMode() const {
        return effective_mode;
    }
    [[nodiscard]] std::string_view GetProfileName() const {
        return profile_name;
    }
    [[nodiscard]] std::string_view GetFallbackReason() const {
        return fallback_reason;
    }
    [[nodiscard]] u64 GetDigest() const {
        return digest;
    }
    [[nodiscard]] bool CanPersistCaches() const {
        return diagnostics.empty();
    }
    [[nodiscard]] bool DisableSubgroups() const {
        return disable_subgroups;
    }
    [[nodiscard]] bool NeedsSubgroupMaskExtractWorkaround() const {
        return subgroup_mask_extract_workaround;
    }
    [[nodiscard]] const EnabledCaps& GetEnabledCaps() const {
        return enabled_caps;
    }
    [[nodiscard]] const DeviceCapabilitySnapshot& GetOutputCaps() const {
        return output_caps;
    }

private:
    Settings::VulkanBackendMode requested_mode;
    Settings::VulkanBackendMode effective_mode{Settings::VulkanBackendMode::Standard};
    DeviceFingerprint fingerprint;
    EnabledCaps enabled_caps;
    DeviceCapabilitySnapshot output_caps;
    std::vector<DiagnosticValue> diagnostics;
    std::string profile_name{"Standard"};
    std::string fallback_reason;
    bool disable_subgroups{};
    bool subgroup_mask_extract_workaround{};
    u64 digest{};
};

} // namespace Vulkan
