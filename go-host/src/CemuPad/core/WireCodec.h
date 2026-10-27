#pragma once
#include "Types.h"

namespace CemuPad
{
Decoded<InputPacket> DecodeInput(std::span<const std::uint8_t> bytes, SensorLimits limits = {});
Decoded<RumblePacket> DecodeRumble(std::span<const std::uint8_t> bytes);
std::array<std::uint8_t, InputPacketSize> EncodeInput(const InputPacket& packet);
std::array<std::uint8_t, RumblePacketSize> EncodeRumble(const RumblePacket& packet);
}
