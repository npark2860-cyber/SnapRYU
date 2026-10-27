// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <utility>

#include "video_core/vulkan_common/vulkan_output_policy.h"

namespace Vulkan {
namespace {

// FNV-1a over explicit, length-prefixed fields. Never hash native struct storage.
class PolicyDigest {
public:
    void Integer(u64 value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            Byte(static_cast<u8>(value >> shift));
        }
    }

    void String(std::string_view value) {
        Integer(value.size());
        for (const unsigned char byte : value) {
            Byte(byte);
        }
    }

    void Fingerprint(const DeviceFingerprint& value) {
        Integer(value.vendor_id);
        Integer(value.device_id);
        Integer(value.driver_id);
        Integer(value.driver_version);
        Integer(value.api_version);
        Integer(static_cast<u32>(value.platform));
        for (const auto& uuid : {value.device_uuid, value.driver_uuid, value.pipeline_cache_uuid}) {
            for (const u8 byte : uuid) {
                Byte(byte);
            }
        }
    }

    void Snapshot(const DeviceCapabilitySnapshot& value) {
        Integer(value.values.size());
        for (const auto& entry : value.values) {
            String(entry.name);
            Integer(entry.value);
        }
        Integer(value.extensions.size());
        for (const auto& extension : value.extensions) {
            String(extension);
        }
    }

    u64 Get() const {
        return hash;
    }

private:
    void Byte(u8 value) {
        hash = (hash ^ value) * 1099511628211ULL;
    }
    u64 hash{14695981039346656037ULL};
};

void Canonicalize(DeviceCapabilitySnapshot& snapshot) {
    std::ranges::sort(snapshot.values, {}, &CapabilityValue::name);
    std::ranges::sort(snapshot.extensions);
}

std::string_view FindDiagnostic(const std::vector<DiagnosticValue>& diagnostics,
                                std::string_view name) {
    const auto it = std::ranges::find(diagnostics, name, &DiagnosticValue::name);
    return it == diagnostics.end() ? std::string_view{} : std::string_view{it->value};
}

} // namespace

u64 DeviceFingerprint::GetDigest() const {
    PolicyDigest digest;
    digest.String("EdenVulkanDeviceFingerprintV1");
    digest.Fingerprint(*this);
    return digest.Get();
}

u64 DeviceCapabilitySnapshot::GetValue(std::string_view name, u64 default_value) const {
    const auto it = std::ranges::find(values, name, &CapabilityValue::name);
    return it == values.end() ? default_value : it->value;
}

VulkanOutputPolicy::VulkanOutputPolicy(Settings::VulkanBackendMode requested,
                                     DeviceFingerprint fingerprint_, EnabledCaps enabled,
                                     DeviceCapabilitySnapshot output,
                                     std::vector<DiagnosticValue> diagnostics_)
    : requested_mode{requested}, fingerprint{std::move(fingerprint_)},
      enabled_caps{std::move(enabled)}, output_caps{std::move(output)},
      diagnostics{std::move(diagnostics_)} {
    Canonicalize(enabled_caps);
    Canonicalize(output_caps);
    std::ranges::sort(diagnostics, {}, &DiagnosticValue::name);

    const bool qualcomm_vendor = fingerprint.vendor_id == 0x5143;
    const bool qualcomm_driver = fingerprint.driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY;
    const bool turnip_driver = fingerprint.driver_id == VK_DRIVER_ID_MESA_TURNIP;
    const bool supported_platform = fingerprint.platform == DevicePlatform::Windows ||
                                    fingerprint.platform == DevicePlatform::Android ||
                                    fingerprint.platform == DevicePlatform::Linux;

    if (requested == Settings::VulkanBackendMode::Adreno) {
        if (!qualcomm_vendor) {
            fallback_reason = "Physical device vendor is not Qualcomm";
        } else if (!qualcomm_driver && !turnip_driver) {
            fallback_reason = "No existing Adreno policy for the reported Vulkan driver ID";
        } else if (!supported_platform) {
            fallback_reason = "No existing Adreno policy for this platform";
        } else {
            effective_mode = Settings::VulkanBackendMode::Adreno;
            profile_name = turnip_driver ? "Turnip" : "Qualcomm";
        }
    } else if (requested != Settings::VulkanBackendMode::Standard) {
        fallback_reason = "Unknown Vulkan backend selection";
    }

    const auto no_subgroup = FindDiagnostic(diagnostics, "EDEN_GPU_NO_SUBGROUP");
    disable_subgroups = !no_subgroup.empty() && no_subgroup.front() != '0';
    const auto extract_fix = FindDiagnostic(diagnostics, "EDEN_QCOM_SUBGROUP_EXTRACT_FIX");
    subgroup_mask_extract_workaround = qualcomm_driver &&
                                      (extract_fix.empty() || extract_fix.front() != '0');

    PolicyDigest hash;
    hash.String("EdenVulkanOutputPolicy");
    hash.Integer(SerializationRevision);
    hash.Integer(ExistingRulesRevision);
    // Requested mode is not an output contract: fallback must share Standard semantics.
    hash.Integer(static_cast<u32>(effective_mode));
    hash.Fingerprint(fingerprint);
    hash.String(profile_name);
    hash.Snapshot(enabled_caps);
    hash.Snapshot(output_caps);
    hash.Integer(disable_subgroups);
    hash.Integer(subgroup_mask_extract_workaround);
    hash.Integer(diagnostics.size());
    for (const auto& entry : diagnostics) {
        hash.String(entry.name);
        hash.String(entry.value);
    }
    digest = hash.Get();
}

} // namespace Vulkan
