#pragma once
#include "CemuPad/control/VerifiedTls.h"
#include "CemuPad/control/PairingAuthority.h"
#include "CemuPad/store/Identity.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>

namespace CemuPad::Transport {
using Clock=std::chrono::steady_clock;
using Deadline=Clock::time_point;
enum class StopReason { None,TransportLost,Off,Shutdown,GameEnd,Replaced,Error };
class Cancellation {
public:
    void Request(StopReason reason=StopReason::Off);
    StopReason Reason() const;
    // Set/clear on the I/O owner only. Pure timer work; never reenter TLS.
    void SetOwnerPulse(std::function<void()> pulse);
    void Pulse() const;
private:
    std::atomic<StopReason> reason{StopReason::None};
    std::function<void()> pulse;
    std::thread::id pulseOwner;
};
enum class Failure { Timeout,Cancelled,PeerRejected,Handshake,Io,Eof,Protocol };
class Error:public std::runtime_error {
public:
    explicit Error(Failure code):std::runtime_error("CemuPad transport failed"),code(code){}
    Failure code;
};
struct Address {std::string ip;std::uint16_t port=0;bool operator==(const Address&) const=default;};
class Listener {
public:
    // Numeric IPv4 only in this adapter. Port zero requests an OS-assigned port.
    explicit Listener(std::string address,std::uint16_t port=0);
    ~Listener();
    Listener(const Listener&)=delete;Listener& operator=(const Listener&)=delete;
    Address LocalAddress() const;
private:
    struct Impl;std::unique_ptr<Impl> m;
    friend class TlsSocket;
};
// Reservation holds an actual bound UDP socket. It grants no DTLS/data authority.
class DatagramReservation {
public:
    explicit DatagramReservation(std::string address);
    ~DatagramReservation();
    Control::Endpoint Endpoint(Control::Channel channel,const Crypto::Digest& hostPin) const;
private:
    struct Impl;std::unique_ptr<Impl> m;
};
class TlsSocket {
public:
    static std::unique_ptr<TlsSocket> Connect(const Store::Identity&,Address,const Crypto::Digest& hostPin,
                                             const Cancellation&,Deadline);
    static std::unique_ptr<TlsSocket> AcceptPinned(Listener&,const Store::Identity&,const Crypto::Digest& clientPin,
                                                 const Cancellation&,Deadline);
    // Caller must enter only from an explicit local pairing window. No VerifiedTls is exposed for this mode.
    static std::unique_ptr<TlsSocket> AcceptProvisional(Listener&,const Store::Identity&,const Cancellation&,Deadline,
                                                     std::function<bool()> localWindowPermit);
    ~TlsSocket();
    TlsSocket(const TlsSocket&)=delete;TlsSocket& operator=(const TlsSocket&)=delete;
    Address LocalAddress() const;
    Address PeerAddress() const;
    Crypto::Digest PeerPin() const;
    // Copies only the certificate from the current verified TLS connection.
    std::vector<std::uint8_t> PeerCertificateDer() const;
    std::optional<Control::VerifiedTls> Verified() const;
    Control::Message Receive(Deadline);
    // No wait. Keeps partial frames and queued messages on an empty result.
    std::optional<Control::Message> PollReceive();
    bool HasPartialFrame() const;
    void Send(const Control::Message&,Deadline);
    // Byte-stream primitive also used by adversarial peer fixtures; never dispatches control state.
    void Write(std::span<const std::uint8_t>,Deadline);
    Control::PairOutcome SubmitPair(Control::PairingAuthority&,std::uint64_t generation,
        const Control::Message& clientHello,const Control::Message& hostHello,const Control::Message& pair,
        const Control::PairingAuthority::Commit& commit);
    Crypto::Digest PairExporter(const Crypto::Digest& hostNonce,const Crypto::Digest& clientNonce) const;
    void Close() noexcept;
private:
    struct Impl;std::unique_ptr<Impl> m;
    explicit TlsSocket(std::unique_ptr<Impl>);
};
LocalTimeUs Now();
}
