#pragma once
#include "MotionGate.h"
#include "WireCodec.h"
#include <mutex>
#include <string_view>

namespace CemuPad
{
enum class SessionState { Disabled, Listening, Ready, Active, Stale, Reconnecting };
enum class InputConsumer { Game, Hle, AppMenu };
enum class AcceptResult { Accepted, Malformed, NoSession, WrongSession, Sequence, DeviceGeneration, Clock };
struct ConsumedInput
{
    InputState state;
    MotionRead motion;
    std::uint32_t pressed = 0, released = 0;
    std::uint64_t ownershipEpoch = 0;
    bool handoverRelease = false;
};

// Policy/core only. The transport must verify peer identity and AEAD before calling
// OnAuthenticatedSession / AcceptInput. This class does not implement cryptography.
class SessionCore
{
public:
    explicit SessionCore(SensorLimits limits = {}) : m_limits(limits) {}
    void SetEnabled(bool enabled, LocalTimeUs now);
    void StartGame(LocalTimeUs now);
    void EndGame(LocalTimeUs now);
    bool OnAuthenticatedSession(std::string_view peerFingerprint, SessionId id, LocalTimeUs now);
    AcceptResult AcceptInput(std::span<const std::uint8_t> packet, LocalTimeUs now);
    void OnTransportLost(LocalTimeUs now);
    void SetConsumer(InputConsumer consumer, LocalTimeUs now);
    void Tick(LocalTimeUs now);
    // Call only after the caller commits to returning an actual VPAD/HLE sample.
    std::optional<ConsumedInput> ConsumeForRead(InputConsumer consumer, LocalTimeUs now);
    SessionState State() const;
    bool HasRemoteOwnership() const;
private:
    LocalTimeUs ProcessTime(LocalTimeUs captured);
    void TickLocked(LocalTimeUs now);
    void NeutralizeLocked(bool preserveObservation = false);
    void StopLocked(bool disabled);
    void ClearGameLocked();
    static std::size_t ConsumerIndex(InputConsumer consumer);

    mutable std::mutex m_mutex;
    SensorLimits m_limits;
    SessionState m_state = SessionState::Disabled;
    InputConsumer m_consumer = InputConsumer::Game;
    bool m_enabled = false, m_game = false, m_bound = false, m_owned = false;
    std::string m_peer;
    SessionId m_session{}, m_previousSession{};
    std::uint64_t m_sequence = 0, m_epoch = 0;
    LocalTimeUs m_now = 0, m_received = 0, m_disconnected = 0;
    bool m_haveInput = false, m_haveGeneration = false;
    std::uint32_t m_generation = 0, m_observedHold = 0, m_physicalHold = 0, m_pendingPress = 0, m_suppressed = 0;
    std::array<LocalTimeUs, 17> m_pressTimes{};
    bool m_touchSuppressed = false, m_physicalTouch = false;
    InputState m_latest{};
    MotionGate m_motion;
    std::array<std::uint32_t, 2> m_deliveredHold{};
    std::array<bool, 2> m_deliveredTouch{}, m_releasePending{}, m_padReleasePending{};
    std::array<std::uint16_t, 2> m_touchX{}, m_touchY{};
};
}
