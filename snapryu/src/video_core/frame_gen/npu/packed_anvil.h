// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
namespace SnapRyuCemu {
struct AnvilTiming {
    double input_copy_ms{}, htp_ms{}, residual_copy_ms{}, service_ms{};
};
// Internal backend. ABI1 remains the legacy composed-RGBA interface.
class PackedAnvil {
public:
    PackedAnvil(const std::filesystem::path& runtime, const std::filesystem::path& model,
                uint32_t height);
    ~PackedAnvil();
    PackedAnvil(const PackedAnvil&) = delete;
    PackedAnvil& operator=(const PackedAnvil&) = delete;
    AnvilTiming Run(std::span<const uint8_t> packed, std::span<uint8_t> residual);
    std::span<const uint8_t> InputTensor() const;
    std::span<const uint8_t> ResidualTensor() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace SnapRyuCemu
