#include "HapticLease.h"

namespace CemuPad
{
void HapticLease::Bind(SessionId id, std::uint32_t generation)
{
    m_session = id; m_generation = generation; m_sequence = 0; m_received = 0; Cancel();
}
bool HapticLease::Accept(const RumblePacket& p, LocalTimeUs now)
{
    if (IsZero(m_session) || p.header.session != m_session || p.header.type != MessageType::Rumble ||
        p.outputGeneration != m_generation || p.header.sequence <= m_sequence || now < m_received ||
        (p.on ? (p.leaseMs == 0 || p.leaseMs > 100) : p.leaseMs != 0)) return false;
    m_sequence = p.header.sequence; m_received = now; m_lease = p.leaseMs * 1000u; m_on = p.on;
    return true;
}
bool HapticLease::IsOn(LocalTimeUs now) const { return m_on && IsFresh(now, m_received, m_lease); }
void HapticLease::Cancel() { m_on = false; m_lease = 0; }
}
