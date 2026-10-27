#pragma once
#include "CemuPad/transport/WindowsHost.h"
#include "PairingCode.h"
#include <mutex>
#include <condition_variable>

namespace CemuPad::Service {
enum class Phase { Disabled,Starting,Listening,Negotiating,Connected,Suspended,Faulted,Shutdown,Pairing };
struct HostStatus {
    Phase phase=Phase::Disabled;
    bool enabled=false,suspended=false,shutdown=false,worker=false;
    Transport::Address endpoint{};
    std::uint64_t generation=0,ticks=0,controlTicks=0,accepted=0,authenticated=0,failures=0,partialTimeouts=0,maxTickGapUs=0;
    std::uint64_t peerRejected=0,protocolRejected=0;
    Control::ControlSnapshot control{};
    SessionState core=SessionState::Disabled;
    std::string storeId,pin,peerPin,revision,hostNonce,clientNonce,authBinding;
    bool outputOn=false,outputCancelFailed=false,coreStopFailed=false;
    bool gameRunning=false;
    bool videoStreaming=false;
    std::uint64_t videoFrames=0;
    std::uint64_t gameChanges=0;
    bool pairing=false;
    LocalTimeUs pairingDeadline=0;
    std::uint64_t pairingGeneration=0,pairAttempts=0,pairCommits=0,pairRejected=0;
};
// Dedicated owner of listener/TLS/control and lifecycle clocks. Core is shared via
// its own mutex; all haptic/output access belongs to this owner while enabled.
// Output callbacks must not wait on or reenter service lifecycle methods.
// The caller keeps store/effects alive until this object's destructor has joined.
class HostService {
public:
    HostService(Store::WindowsStore&,Transport::Lifecycle&,std::string numericAddress,std::uint16_t controlPort=0,bool discoverable=false);
    ~HostService();
    HostService(const HostService&)=delete;HostService& operator=(const HostService&)=delete;
    void SetEnabled(bool);
    // Explicit first pairing only. No existing peer replacement or initialization.
    // Returned bootstrap must remain in memory. Cancelling joins the provisional
    // connection; a commit already durable remains eligible for pinned recovery.
    std::optional<PairingCode> OpenPairing();
    void CancelPairing();
    void Suspend();
    void Resume();
    // Synchronous owner barrier. Never call while holding an InputManager lock
    // or from an output callback. Game end retains the authenticated TLS owner.
    void SetGameRunning(bool);
    void Shutdown();
    HostStatus Snapshot() const;
    bool Connected() const;
private:
    void StartLocked(std::optional<Store::Snapshot> expected={});
    void StopLocked(Transport::StopReason);
    void Run(std::shared_ptr<Transport::Cancellation>,std::optional<Store::Snapshot>) noexcept;
    void Connection(Transport::ControlTransport&,const Store::Snapshot&,Transport::Cancellation&);
    bool Pair(Transport::Listener&,Store::Snapshot&,std::uint64_t,const Transport::Cancellation&);
    Store::WindowsStore& store;Transport::Lifecycle& effects;const std::string address;
    const std::uint16_t controlPort;const bool discoverable;
    mutable std::mutex stateMutex;std::mutex operationMutex;
    HostStatus state;
    std::shared_ptr<Transport::Cancellation> cancellation;
    std::thread worker;
    std::condition_variable changed;
    bool requestedGame=false;
    std::uint64_t requestedGameChange=0,appliedGameChange=0;
    Control::PairingAuthority pairing;
    std::uint64_t pairGeneration=0;
};
const char* Name(Phase);
}
