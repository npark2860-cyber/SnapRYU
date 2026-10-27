#include "SessionCore.h"
#include <algorithm>

namespace CemuPad
{
std::size_t SessionCore::ConsumerIndex(InputConsumer c) { return c == InputConsumer::Hle ? 1 : 0; }
LocalTimeUs SessionCore::ProcessTime(LocalTimeUs captured)
{
    // Threads can capture time before waiting for this mutex. Lifecycle commands
    // must still execute if another caller acquired the lock with a later time.
    m_now = std::max(captured, m_now); return m_now;
}
void SessionCore::NeutralizeLocked(bool preserveObservation)
{
    auto x = m_latest.touchX, y = m_latest.touchY;
    m_latest = {}; m_latest.touchX = x; m_latest.touchY = y;
    m_pendingPress = 0;
    if (preserveObservation) m_motion.Invalidate();
    else { m_observedHold = m_physicalHold = m_suppressed = 0; m_physicalTouch = m_touchSuppressed = false; m_motion.Reset(); }
}
void SessionCore::StopLocked(bool disabled)
{
    for (std::size_t i = 0; i < 2; ++i)
        m_releasePending[i] = m_releasePending[i] || m_deliveredHold[i] != 0 || m_deliveredTouch[i];
    NeutralizeLocked(); m_owned = m_bound = m_haveInput = m_haveGeneration = false;
    m_peer.clear(); m_session = {}; m_sequence = 0; ++m_epoch;
    m_state = disabled ? SessionState::Disabled : SessionState::Listening;
}
void SessionCore::ClearGameLocked()
{
    NeutralizeLocked(); m_owned = m_bound = m_haveInput = m_haveGeneration = false;
    m_session = {}; m_sequence = 0; m_deliveredHold = {}; m_deliveredTouch = {}; m_releasePending = {}; m_padReleasePending = {};
    m_consumer = InputConsumer::Game; ++m_epoch;
    if (m_state != SessionState::Reconnecting)
        m_state = !m_enabled ? SessionState::Disabled : m_peer.empty() ? SessionState::Listening : SessionState::Ready;
}
void SessionCore::SetEnabled(bool enabled, LocalTimeUs now)
{
    std::scoped_lock lock(m_mutex);
    now = ProcessTime(now);
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    if (!enabled) StopLocked(true); else m_state = SessionState::Listening;
}
void SessionCore::StartGame(LocalTimeUs now)
{
    std::scoped_lock lock(m_mutex);
    now = ProcessTime(now);
    if (m_game) return;
    m_game = true; ClearGameLocked(); // A new game always requires a fresh data session.
}
void SessionCore::EndGame(LocalTimeUs now)
{
    std::scoped_lock lock(m_mutex);
    now = ProcessTime(now);
    m_game = false; ClearGameLocked();
}
bool SessionCore::OnAuthenticatedSession(std::string_view peer, SessionId id, LocalTimeUs now)
{
    std::scoped_lock lock(m_mutex);
    now = ProcessTime(now);
    TickLocked(now);
    if (!m_enabled || peer.empty() || IsZero(id) || id == m_previousSession ||
        (!m_peer.empty() && m_peer != peer) || m_bound) return false;
    m_peer = peer; m_session = m_previousSession = id; m_sequence = 0; m_bound = true;
    m_haveInput = m_haveGeneration = false; NeutralizeLocked(); m_state = SessionState::Ready; ++m_epoch;
    return true;
}
AcceptResult SessionCore::AcceptInput(std::span<const std::uint8_t> bytes, LocalTimeUs now)
{
    auto decoded = DecodeInput(bytes, m_limits);
    if (!decoded) return AcceptResult::Malformed;
    std::scoped_lock lock(m_mutex);
    const auto received = now;
    now = ProcessTime(now);
    TickLocked(now);
    if (!IsFresh(now, received, InputLeaseUs)) return AcceptResult::Clock;
    const auto& p = *decoded.value;
    if (!m_enabled || !m_game || !m_bound) return AcceptResult::NoSession;
    if (p.header.session != m_session) return AcceptResult::WrongSession;
    if (p.header.sequence <= m_sequence) return AcceptResult::Sequence;
    if (m_haveGeneration && p.state.deviceGeneration < m_generation) return AcceptResult::DeviceGeneration;
    if (!m_haveGeneration || p.state.deviceGeneration != m_generation)
    {
        if (m_haveGeneration)
            for (std::size_t i = 0; i < 2; ++i)
                m_releasePending[i] = m_releasePending[i] || m_deliveredHold[i] != 0 || m_deliveredTouch[i];
        NeutralizeLocked(); m_generation = p.state.deviceGeneration; m_haveGeneration = true;
    }
    m_sequence = p.header.sequence; m_received = received; m_haveInput = true;
    auto rising = p.state.buttons & ~m_observedHold;
    if (p.state.validity & PadValid)
    {
        // Only a valid physical up can release a consumer handover suppression.
        m_physicalHold = p.state.buttons;
        m_suppressed &= p.state.buttons;
    }
    else
    {
        m_pendingPress = 0;
        for (std::size_t i = 0; i < 2; ++i)
            m_padReleasePending[i] = m_padReleasePending[i] || m_deliveredHold[i] != 0;
    }
    for (std::size_t i = 0; i < 17; ++i)
        if (!IsFresh(now, m_pressTimes[i], InputLeaseUs)) m_pendingPress &= ~(std::uint32_t(1) << i);
    if (m_consumer != InputConsumer::AppMenu)
    {
        rising &= ~m_suppressed;
        for (std::size_t i = 0; i < 17; ++i)
        {
            auto bit = std::uint32_t(1) << i;
            // A second tap may coalesce; it must not extend the first tap's lease.
            if ((rising & bit) && !(m_pendingPress & bit)) m_pressTimes[i] = received;
        }
        m_pendingPress |= rising;
    }
    // Latch edges still see the invalid neutral sample so a recovery tap is
    // retained. Physical observation survives invalidity for SetConsumer.
    m_observedHold = p.state.buttons;
    if (p.state.validity & TouchValid)
    {
        m_physicalTouch = p.state.touchActive;
        if (!p.state.touchActive) m_touchSuppressed = false;
    }
    m_latest = p.state; m_motion.Observe(p.state, received);
    if (!m_owned) { m_owned = true; ++m_epoch; }
    m_state = SessionState::Active;
    return AcceptResult::Accepted;
}
void SessionCore::TickLocked(LocalTimeUs now)
{
    if (m_state == SessionState::Reconnecting && !IsFresh(now, m_disconnected, ReconnectGraceUs))
        StopLocked(false);
    else if (m_state == SessionState::Active && m_haveInput && !IsFresh(now, m_received, InputLeaseUs))
    {
        // Stale must release a previously delivered hold/touch even if fresh
        // input arrives before the next actual sample. Raw observation survives.
        for (std::size_t i = 0; i < 2; ++i)
            m_releasePending[i] = m_releasePending[i] || m_deliveredHold[i] != 0 || m_deliveredTouch[i];
        NeutralizeLocked(true); m_state = SessionState::Stale;
    }
}
void SessionCore::Tick(LocalTimeUs now)
{
    std::scoped_lock lock(m_mutex); TickLocked(ProcessTime(now));
}
void SessionCore::OnTransportLost(LocalTimeUs now)
{
    std::scoped_lock lock(m_mutex);
    now = ProcessTime(now);
    if (!m_enabled || m_peer.empty() || m_state == SessionState::Reconnecting) return;
    for (std::size_t i = 0; i < 2; ++i)
        m_releasePending[i] = m_releasePending[i] || m_deliveredHold[i] != 0 || m_deliveredTouch[i];
    m_bound = false; m_session = {}; m_disconnected = now;
    NeutralizeLocked(); m_state = SessionState::Reconnecting;
}
void SessionCore::SetConsumer(InputConsumer consumer, LocalTimeUs now)
{
    std::scoped_lock lock(m_mutex);
    now = ProcessTime(now);
    if (consumer == m_consumer) return;
    if (m_consumer != InputConsumer::AppMenu)
    {
        auto i = ConsumerIndex(m_consumer);
        m_releasePending[i] = m_releasePending[i] || m_deliveredHold[i] || m_deliveredTouch[i];
    }
    m_pendingPress = 0; m_suppressed = m_physicalHold;
    m_touchSuppressed = m_physicalTouch; m_motion.Invalidate(); m_consumer = consumer; ++m_epoch;
}
std::optional<ConsumedInput> SessionCore::ConsumeForRead(InputConsumer consumer, LocalTimeUs now)
{
    std::scoped_lock lock(m_mutex);
    now = ProcessTime(now);
    if (consumer == InputConsumer::AppMenu) return {};
    TickLocked(now);
    auto i = ConsumerIndex(consumer);
    if (m_releasePending[i])
    {
        ConsumedInput out;
        out.released = m_deliveredHold[i]; out.ownershipEpoch = m_epoch; out.handoverRelease = true;
        out.state.touchX = m_touchX[i]; out.state.touchY = m_touchY[i];
        m_deliveredHold[i] = 0; m_deliveredTouch[i] = false; m_releasePending[i] = m_padReleasePending[i] = false;
        return out;
    }
    if (!m_game || !m_owned || consumer != m_consumer) return {};
    for (std::size_t bit = 0; bit < 17; ++bit)
        if (!IsFresh(now, m_pressTimes[bit], InputLeaseUs)) m_pendingPress &= ~(std::uint32_t(1) << bit);
    ConsumedInput out;
    out.state = m_latest;
    out.state.buttons = (m_latest.buttons | m_pendingPress) & ~m_suppressed;
    const bool padRelease = m_padReleasePending[i];
    if (padRelease)
    {
        // Phone touch/IMU can remain valid while the physical pad disappears.
        out.state.validity &= ~PadValid;
        out.state.buttons = 0; out.state.sticks = {}; out.state.triggers = {};
    }
    if (m_touchSuppressed) out.state.touchActive = false;
    out.motion = m_motion.Consume(now);
    if (out.motion.gyro.action == MotionAction::Invalid) { out.state.validity &= ~GyroValid; out.state.gyro = {}; out.state.gyroTimeUs = 0; }
    else { out.state.gyro = out.motion.gyro.values; out.state.gyroTimeUs = out.motion.gyro.timeUs; }
    if (out.motion.accel.action == MotionAction::Invalid) { out.state.validity &= ~AccelValid; out.state.accel = {}; out.state.accelTimeUs = 0; }
    else { out.state.accel = out.motion.accel.values; out.state.accelTimeUs = out.motion.accel.timeUs; }
    out.pressed = out.state.buttons & ~m_deliveredHold[i]; out.released = m_deliveredHold[i] & ~out.state.buttons;
    out.ownershipEpoch = m_epoch;
    m_deliveredHold[i] = out.state.buttons; m_deliveredTouch[i] = out.state.touchActive;
    m_touchX[i] = out.state.touchX; m_touchY[i] = out.state.touchY;
    // A recovery tap may have finished before this release sample. Keep that
    // latch for the following sample, subject to its original per-bit lease.
    if (!padRelease) m_pendingPress = 0;
    m_padReleasePending[i] = false;
    return out;
}
SessionState SessionCore::State() const { std::scoped_lock lock(m_mutex); return m_state; }
bool SessionCore::HasRemoteOwnership() const { std::scoped_lock lock(m_mutex); return m_owned; }
}
