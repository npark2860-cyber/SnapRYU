#pragma once
#include "TlsSocket.h"
#include <span>

namespace CemuPad::Transport {
// One socket/SSL/SRTP owner, on the same executor as its control connection.
class DtlsChannel {
public:
    DtlsChannel(const Store::Identity&,Crypto::Digest expectedPeer,Control::Role,
        Control::Channel,std::string bindAddress,const Cancellation&);
    ~DtlsChannel();
    DtlsChannel(const DtlsChannel&)=delete;DtlsChannel& operator=(const DtlsChannel&)=delete;
    Control::Endpoint Endpoint() const;
    Control::VerifiedChannel Handshake(SessionId,const Control::Endpoint& hostEndpoint,Address controlPeer,Deadline,const std::function<void()>& progress={});
    void Pump();
    void SetSsrc(std::uint32_t);
    bool SendRtp(std::span<const std::uint8_t>);
    std::optional<std::vector<std::uint8_t>> ReceiveRtp();
    bool SendData(std::span<const std::uint8_t>);
    std::optional<std::vector<std::uint8_t>> ReceiveData();
private:
    struct Impl;std::unique_ptr<Impl> m;
};
}
