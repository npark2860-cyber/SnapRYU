#include "WireCodec.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace CemuPad
{
bool IsZero(const SessionId& id) { return std::all_of(id.begin(), id.end(), [](auto b) { return b == 0; }); }
bool IsFresh(LocalTimeUs now, LocalTimeUs since, LocalTimeUs duration) { return now >= since && now - since < duration; }
namespace
{
std::uint64_t Read(std::span<const std::uint8_t> b, std::size_t p, std::size_t n)
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < n; ++i) value = (value << 8) | b[p + i];
    return value;
}
void Write(std::span<std::uint8_t> b, std::size_t p, std::uint64_t v, std::size_t n)
{
    for (std::size_t i = 0; i < n; ++i) { b[p + n - 1 - i] = static_cast<std::uint8_t>(v); v >>= 8; }
}
DecodeError ReadHeader(std::span<const std::uint8_t> b, MessageType type, std::size_t size, Header& h)
{
    if (b.size() != size || Read(b, 4, 2) != size - HeaderSize) return DecodeError::Length;
    if (Read(b, 0, 2) != ProtocolVersion) return DecodeError::Version;
    if (Read(b, 2, 2) != static_cast<std::uint16_t>(type)) return DecodeError::Type;
    if (Read(b, 6, 2)) return DecodeError::Flags;
    std::copy_n(b.begin() + 8, 16, h.session.begin());
    if (IsZero(h.session)) return DecodeError::Session;
    h.type = type; h.sequence = Read(b, 24, 8); h.senderTimeUs = Read(b, 32, 8);
    return h.sequence ? DecodeError::None : DecodeError::Sequence;
}
void WriteHeader(std::span<std::uint8_t> b, const Header& h, MessageType type)
{
    Write(b, 0, ProtocolVersion, 2); Write(b, 2, static_cast<std::uint16_t>(type), 2);
    Write(b, 4, b.size() - HeaderSize, 2); Write(b, 6, 0, 2);
    std::copy(h.session.begin(), h.session.end(), b.begin() + 8);
    Write(b, 24, h.sequence, 8); Write(b, 32, h.senderTimeUs, 8);
}
bool ValidSensor(const std::array<float, 3>& v, std::uint64_t time, bool valid, float limit)
{
    if (!std::isfinite(limit) || limit <= 0) return false;
    for (float x : v)
        if (!std::isfinite(x) || (valid ? std::abs(x) > limit : x != 0)) return false;
    return valid ? time != 0 : time == 0;
}
}
Decoded<InputPacket> DecodeInput(std::span<const std::uint8_t> b, SensorLimits limits)
{
    InputPacket p;
    if (auto e = ReadHeader(b, MessageType::Input, InputPacketSize, p.header); e != DecodeError::None) return {{}, e};
    auto payload = b.subspan(HeaderSize);
    auto& s = p.state;
    s.buttons = static_cast<std::uint32_t>(Read(payload, 0, 4));
    if (s.buttons & ~ButtonMask) return {{}, DecodeError::Buttons};
    for (std::size_t i = 0; i < 4; ++i)
    {
        s.sticks[i] = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(Read(payload, 4 + i * 2, 2)));
        if (s.sticks[i] == std::numeric_limits<std::int16_t>::min()) return {{}, DecodeError::Stick};
    }
    // Each stick is already normalized on Android; allow quantization at the unit-circle edge.
    for (std::size_t i = 0; i < 4; i += 2)
    {
        double x = s.sticks[i] / 32767.0, y = s.sticks[i + 1] / 32767.0;
        if (x * x + y * y > 1.0 + 4.0 / 32767.0) return {{}, DecodeError::Stick};
    }
    for (std::size_t i = 0; i < 2; ++i) s.triggers[i] = static_cast<std::uint16_t>(Read(payload, 12 + i * 2, 2));
    if (payload[16] > 1) return {{}, DecodeError::Boolean};
    s.touchActive = payload[16] != 0; s.touchId = payload[17];
    s.touchX = static_cast<std::uint16_t>(Read(payload, 18, 2)); s.touchY = static_cast<std::uint16_t>(Read(payload, 20, 2));
    s.validity = static_cast<std::uint16_t>(Read(payload, 22, 2));
    if ((s.validity & ~std::uint16_t(15)) || (!(s.validity & TouchValid) && s.touchActive)) return {{}, DecodeError::Validity};
    if (!(s.validity & PadValid) && (s.buttons || std::any_of(s.sticks.begin(), s.sticks.end(), [](auto v) { return v != 0; }) || s.triggers[0] || s.triggers[1])) return {{}, DecodeError::Validity};
    for (std::size_t i = 0; i < 3; ++i)
    {
        s.gyro[i] = std::bit_cast<float>(static_cast<std::uint32_t>(Read(payload, 24 + i * 4, 4)));
        s.accel[i] = std::bit_cast<float>(static_cast<std::uint32_t>(Read(payload, 36 + i * 4, 4)));
    }
    s.gyroTimeUs = Read(payload, 48, 8); s.accelTimeUs = Read(payload, 56, 8);
    s.deviceGeneration = static_cast<std::uint32_t>(Read(payload, 64, 4));
    if (!ValidSensor(s.gyro, s.gyroTimeUs, (s.validity & GyroValid) != 0, limits.gyroAbsMax) ||
        !ValidSensor(s.accel, s.accelTimeUs, (s.validity & AccelValid) != 0, limits.accelAbsMax)) return {{}, DecodeError::Sensor};
    return {p, DecodeError::None};
}
Decoded<RumblePacket> DecodeRumble(std::span<const std::uint8_t> b)
{
    RumblePacket p;
    if (auto e = ReadHeader(b, MessageType::Rumble, RumblePacketSize, p.header); e != DecodeError::None) return {{}, e};
    auto payload = b.subspan(HeaderSize);
    p.outputGeneration = static_cast<std::uint32_t>(Read(payload, 0, 4));
    if (payload[4] > 1) return {{}, DecodeError::Boolean};
    if (payload[5]) return {{}, DecodeError::Reserved};
    p.on = payload[4] != 0; p.leaseMs = static_cast<std::uint16_t>(Read(payload, 6, 2));
    if (p.on ? (p.leaseMs == 0 || p.leaseMs > 100) : p.leaseMs != 0) return {{}, DecodeError::RumbleLease};
    return {p, DecodeError::None};
}
std::array<std::uint8_t, InputPacketSize> EncodeInput(const InputPacket& p)
{
    std::array<std::uint8_t, InputPacketSize> b{}; WriteHeader(b, p.header, MessageType::Input);
    std::span<std::uint8_t> s(b.data() + HeaderSize, InputPayloadSize);
    const auto& v = p.state;
    Write(s, 0, v.buttons, 4);
    for (std::size_t i = 0; i < 4; ++i) Write(s, 4 + i * 2, std::bit_cast<std::uint16_t>(v.sticks[i]), 2);
    for (std::size_t i = 0; i < 2; ++i) Write(s, 12 + i * 2, v.triggers[i], 2);
    s[16] = v.touchActive ? 1 : 0; s[17] = v.touchId;
    Write(s, 18, v.touchX, 2); Write(s, 20, v.touchY, 2); Write(s, 22, v.validity, 2);
    for (std::size_t i = 0; i < 3; ++i)
    {
        Write(s, 24 + i * 4, std::bit_cast<std::uint32_t>(v.gyro[i]), 4);
        Write(s, 36 + i * 4, std::bit_cast<std::uint32_t>(v.accel[i]), 4);
    }
    Write(s, 48, v.gyroTimeUs, 8); Write(s, 56, v.accelTimeUs, 8); Write(s, 64, v.deviceGeneration, 4);
    return b;
}
std::array<std::uint8_t, RumblePacketSize> EncodeRumble(const RumblePacket& p)
{
    std::array<std::uint8_t, RumblePacketSize> b{}; WriteHeader(b, p.header, MessageType::Rumble);
    Write(b, 40, p.outputGeneration, 4); b[44] = p.on ? 1 : 0; Write(b, 46, p.leaseMs, 2); return b;
}
}
