#pragma once
#include "TlsSocket.h"
#include "CemuPad/control/ControlSession.h"
#include "CemuPad/core/SessionCore.h"
#include "CemuPad/core/HapticLease.h"
#include "CemuPad/core/RumbleState.h"
#include "CemuPad/core/MotionControl.h"
#include <thread>

namespace CemuPad::Transport {
// Effects execute on the transport owner. Caller keeps one core across reconnects.
struct Lifecycle {
    SessionCore& core;HapticLease& haptic;
    std::function<void()> cancelOutput;
    Control::PairingAuthority* pairing=nullptr;
    bool outputCancelFailed=false,coreStopFailed=false;
    RumbleState* rumble=nullptr;
    MotionControlMailbox* motionControls=nullptr;
    void Stop(StopReason reason,LocalTimeUs now) noexcept;
    void CancelOutput() noexcept;
    // Service owner drives this clock even without a connection or actual input reads.
    void Tick(LocalTimeUs now);
};
struct Received {Control::Message message;Control::Decision decision;};
// Construct, drive and destroy on one owner thread. Only Cancellation::Request is cross-thread.
// Caller supplies absolute handshake/read/write deadlines and drives this Tick while connected.
// Lifecycle::Tick is the independent service clock after disconnect; old connection callbacks are inert.
class ControlTransport {
public:
    ControlTransport(std::unique_ptr<TlsSocket>,SessionId hostId,Lifecycle&,const Cancellation&);
    ~ControlTransport();
    ControlTransport(const ControlTransport&)=delete;ControlTransport& operator=(const ControlTransport&)=delete;
    Received Receive(Deadline,LocalTimeUs now);
    std::optional<Received> PollReceive(Deadline replyDeadline,LocalTimeUs now);
    bool HasPartialFrame() const;
    Control::Decision Send(const Control::Message&,Deadline,LocalTimeUs now);
    Control::Decision Tick(LocalTimeUs now);
    void Close(StopReason reason,LocalTimeUs now);
    Control::ControlSnapshot Snapshot() const;
    Crypto::Digest AuthBinding(const Crypto::Digest& hostNonce,const Crypto::Digest& clientNonce) const;
    Address HostAddress() const;
    Address LocalAddress() const {return local;}
    Address PeerAddress() const;
    Control::ConnectionToken Connection() const {return token;}
    bool WithAuthorizedInput(SessionId,const std::function<void()>&);
    std::optional<Control::WorkToken> BeginChannel(Control::Channel);
    Control::Decision CompleteChannel(const Control::WorkToken&,const Control::VerifiedChannel&);
    Control::Decision CompleteApplication(const Control::WorkToken&);
    bool WithAuthorizedStream(SessionId,Control::Channel,std::uint32_t generation,std::uint32_t ssrc,const std::function<void()>&);
    bool WithAuthorizedHapticOn(SessionId,std::uint32_t generation,const std::function<void()>&);
    Control::Decision CancelWork(const Control::WorkToken&,LocalTimeUs now);
private:
    void Owner() const;
    void Apply(const Control::Decision&,const Control::Message*,LocalTimeUs,bool closeSocket=true);
    void Stop(StopReason,LocalTimeUs) noexcept;
    void Failed(LocalTimeUs) noexcept;
    Received Dispatch(Control::Message,Deadline,LocalTimeUs);
    std::thread::id owner=std::this_thread::get_id();
    std::unique_ptr<TlsSocket> socket;
    Control::VerifiedTls tls;
    Control::ControlSession session;
    Control::ConnectionToken token=0;
    Lifecycle& effects;const Cancellation& cancellation;
    Address local,peer;
    bool closed=false,disabledApplied=false;
};
Control::Message MakeMessage(Control::Type type,std::uint64_t id,std::string_view payload);
Control::Message Hello(Control::Role role,std::uint64_t id,SessionId hostId);
}
