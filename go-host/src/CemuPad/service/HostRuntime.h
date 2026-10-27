#pragma once
#include "HostService.h"
#include "CemuPad/core/VpadBridge.h"
#include "MotionBridge.h"
#include <filesystem>
#include <memory>

namespace CemuPad::Service {
struct RuntimeStatus {
    bool configured=false,enabled=false,suspended=false,shutdown=false;
    bool ownerCreated=false,gameRunning=false,fault=false;
    bool bindingFault=false;
    std::string bindAddress="127.0.0.1";
    HostStatus host{};
};
// Process-level Cemu adapter. Construction/configuration/OFF never opens a store
// or creates the service owner. It never initializes or repairs a trust identity.
// Calls are serialized outside input/renderer locks. No callbacks into wxWidgets.
class HostRuntime {
public:
    HostRuntime();
    ~HostRuntime();
    HostRuntime(const HostRuntime&)=delete;HostRuntime& operator=(const HostRuntime&)=delete;
    void Configure(std::filesystem::path userDataDirectory);
    bool SetEnabled(bool) noexcept;
    bool SetBindAddress(std::string) noexcept; // explicit OFF selection; no store creation
    Store::StoreResult InitializeIdentity() noexcept;
    // Explicit user recovery. Stops/joins the service and clears only the peer,
    // preserving the existing identity and a durable copy of the old trust.
    Store::StoreResult ResetPairing() noexcept;
    std::optional<PairingCode> OpenPairing() noexcept;
    void CancelPairing() noexcept;
    bool SetGameRunning(bool) noexcept;
    void Suspend() noexcept;
    void Resume() noexcept;
    void Shutdown() noexcept;
    // Handles only applicable WM_POWERBROADCAST / confirmed WM_ENDSESSION.
    // Does not register notifications or manufacture an actual OS transition.
    bool HandleWindowsMessage(std::uint32_t message,std::uintptr_t wParam) noexcept;
    RuntimeStatus Snapshot() const;
    bool MediaActive() const;
    std::string StatusText() const;
    struct InputRead {VpadSample pad;std::optional<ConsumedInput> input;bool remotePad=false,takeover=false;MotionOutput motion;};
    InputRead ReadInput(VpadSample local,int delayMs,int pulseMs,bool menu);
    bool RemotePadOwned() const;
    // nullopt leaves the unchanged local motor path in charge.
    std::optional<bool> QueueRemoteRumble(std::span<const std::uint8_t> pattern,unsigned bits);
    void StopRemoteRumble();
    void SetRemoteRumbleLevel(bool);
private:
    struct Owner;
    mutable std::mutex mutex;
    RuntimeStatus state;
    std::filesystem::path root;
    std::unique_ptr<Owner> owner;
    bool invalidBindSelection=false;
};
HostRuntime& CemuHostRuntime();
}
