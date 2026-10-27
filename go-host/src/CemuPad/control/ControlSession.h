#pragma once
#include "VerifiedTls.h"
#include <array>
#include <memory>
#include <optional>
#include <functional>

namespace CemuPad::Control {
using ConnectionToken=std::uint64_t;
enum class WorkKind { Channel,Stream,Haptic };
struct WorkToken {
    ConnectionToken connection=0; SessionId session{}; std::uint64_t operation=0;
    WorkKind kind=WorkKind::Channel; unsigned slot=0;
    bool operator==(const WorkToken&) const=default;
};
enum class DecisionCode { Accepted,Duplicate,Rejected,StaleCallback,ReconnectRequired };
struct Decision {
    DecisionCode code=DecisionCode::Rejected;
    std::optional<Message> replay{};
    std::optional<WorkToken> work{};
    bool dataStopped=false,outputCancelled=false,controlClosed=false;
    explicit operator bool() const { return code==DecisionCode::Accepted||code==DecisionCode::Duplicate; }
};
struct Negotiated {
    unsigned inputHz=0; double gyroAbsMax=0,accelAbsMax=0;
    bool gyro=false,accel=false,haptics=false,video=false,audio=false;
    bool operator==(const Negotiated&) const=default;
};
struct ControlLimits { std::size_t transactionsPerInitiator=4096,cachedBytes=2*1024*1024; };
struct ControlSnapshot {
    ConnectionToken connection=0; bool open=false,authenticated=false,capabilities=false;
    SessionId session{}; std::array<bool,3> dataReady{};
    std::array<bool,2> streamReady{}; bool hapticReady=false;
    Negotiated negotiated{}; std::array<std::uint64_t,2> requestWatermarks{};
    std::size_t transactions=0,cachedBytes=0; std::uint64_t timeEstimatesApplied=0;
    bool operator==(const ControlSnapshot&) const=default;
};
class ControlSession {
public:
    explicit ControlSession(ControlLimits limits={});
    ~ControlSession();
    ControlSession(const ControlSession&)=delete; ControlSession& operator=(const ControlSession&)=delete;
    ConnectionToken Open(VerifiedTls tls,SessionId hostId,std::string hostAddress);
    Decision Send(ConnectionToken connection,const Message& message,LocalTimeUs now);
    Decision Receive(ConnectionToken connection,const Message& message,LocalTimeUs now);
    std::optional<WorkToken> BeginChannel(ConnectionToken connection,Channel channel);
    Decision CompleteChannel(const WorkToken& token,const VerifiedChannel& evidence);
    Decision CompleteApplication(const WorkToken& token);
    Decision CancelWork(const WorkToken& token);
    // Callback runs under the same lock as lifecycle/token checks; it must not
    // reenter this object or wait. A product adapter uses this to publish to core.
    bool WithAuthorizedInput(ConnectionToken connection,SessionId session,const std::function<void()>& action);
    bool WithAuthorizedStream(ConnectionToken connection,SessionId session,Channel stream,std::uint32_t generation,std::uint32_t ssrc,const std::function<void()>& action);
    bool WithAuthorizedHapticOn(ConnectionToken connection,SessionId session,std::uint32_t outputGeneration,const std::function<void()>& action);
    Decision Tick(ConnectionToken connection,LocalTimeUs now);
    Decision Close(ConnectionToken connection);
    ControlSnapshot Snapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
}
