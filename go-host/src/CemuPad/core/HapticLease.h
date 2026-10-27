#pragma once
#include "WireCodec.h"

namespace CemuPad
{
// Called only after DTLS authentication, never directly from an untrusted UDP socket.
class HapticLease
{
public:
    void Bind(SessionId session, std::uint32_t outputGeneration);
    bool Accept(const RumblePacket& packet, LocalTimeUs now);
    bool IsOn(LocalTimeUs now) const;
    void Cancel();
private:
    SessionId m_session{};
    std::uint32_t m_generation = 0;
    std::uint64_t m_sequence = 0;
    LocalTimeUs m_received = 0, m_lease = 0;
    bool m_on = false;
};
}
