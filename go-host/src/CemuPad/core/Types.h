#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace CemuPad
{
using LocalTimeUs = std::uint64_t;
using SessionId = std::array<std::uint8_t, 16>;
constexpr std::uint16_t ProtocolVersion = 1;
constexpr std::size_t HeaderSize = 40;
constexpr std::size_t InputPayloadSize = 68;
constexpr std::size_t InputPacketSize = HeaderSize + InputPayloadSize;
constexpr std::size_t RumblePacketSize = HeaderSize + 8;
constexpr LocalTimeUs InputLeaseUs = 100'000;
constexpr LocalTimeUs ReconnectGraceUs = 3'000'000;
constexpr std::uint32_t ButtonMask = (1u << 17) - 1;
enum Validity : std::uint16_t { PadValid = 1, TouchValid = 2, GyroValid = 4, AccelValid = 8 };
enum class MessageType : std::uint16_t { Input = 1, Rumble = 2 };

struct Header
{
    MessageType type = MessageType::Input;
    SessionId session{};
    std::uint64_t sequence = 0;
    std::uint64_t senderTimeUs = 0;
};
struct InputState
{
    std::uint32_t buttons = 0;
    std::array<std::int16_t, 4> sticks{};
    std::array<std::uint16_t, 2> triggers{};
    bool touchActive = false;
    std::uint8_t touchId = 0;
    std::uint16_t touchX = 0, touchY = 0;
    std::uint16_t validity = 0;
    std::array<float, 3> gyro{}, accel{};
    std::uint64_t gyroTimeUs = 0, accelTimeUs = 0;
    std::uint32_t deviceGeneration = 0;
};
struct InputPacket { Header header; InputState state; };
struct RumblePacket
{
    Header header{MessageType::Rumble};
    std::uint32_t outputGeneration = 0;
    bool on = false;
    std::uint16_t leaseMs = 0;
};
struct SensorLimits { float gyroAbsMax = 100.0f, accelAbsMax = 200.0f; };
enum class DecodeError
{
    None, Length, Version, Type, Flags, Session, Sequence, Buttons, Stick,
    Boolean, Validity, Sensor, RumbleLease, Reserved
};
template<class T> struct Decoded
{
    std::optional<T> value;
    DecodeError error = DecodeError::None;
    explicit operator bool() const { return value.has_value(); }
};
bool IsZero(const SessionId& id);
bool IsFresh(LocalTimeUs now, LocalTimeUs since, LocalTimeUs duration);
}
