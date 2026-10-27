#pragma once
#include "ControlMessage.h"
#include "CemuPad/core/Types.h"
#include "CemuPad/crypto/AuthPrimitives.h"
#include <openssl/types.h>
#include <memory>
#include <optional>

namespace CemuPad::Control {
std::string Hex(std::span<const std::uint8_t> bytes);
Crypto::Digest Digest(std::string_view text);
SessionId Session(std::string_view text);
class VerifiedTls {
public:
    // The expected pin is supplied by the trust/QR adapter, never by a peer message.
    static std::optional<VerifiedTls> FromSsl(SSL* ssl,Role localRole,const Crypto::Digest& expectedPeer);
    const Crypto::Digest& HostPin() const;
    const Crypto::Digest& ClientPin() const;
    const Crypto::Digest& PeerPin() const;
    Role LocalRole() const;
    Crypto::Digest AuthBinding(const Crypto::Digest& hostNonce,const Crypto::Digest& clientNonce) const;
private:
    struct Data;
    std::shared_ptr<const Data> m_data;
    explicit VerifiedTls(std::shared_ptr<const Data> data):m_data(std::move(data)){}
};
class VerifiedChannel {
public:
    static std::optional<VerifiedChannel> FromSsl(SSL* ssl,Role localRole,SessionId session,Channel channel,const Crypto::Digest& expectedPeer,const Endpoint& hostEndpoint);
    SessionId Session() const { return m_session; }
    Channel Kind() const { return m_channel; }
    const Crypto::Digest& PeerPin() const { return m_peer; }
    const Crypto::Digest& Binding() const { return m_binding; }
    const Crypto::Digest& LocalPin() const { return m_local; }
    Role LocalRole() const { return m_role; }
    const Endpoint& HostEndpoint() const { return m_endpoint; }
private:
    SessionId m_session{}; Channel m_channel=Channel::InputHaptics;
    Crypto::Digest m_peer{},m_binding{},m_local{}; Endpoint m_endpoint{}; Role m_role=Role::Host;
};
}
