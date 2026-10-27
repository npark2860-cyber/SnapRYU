#include "VerifiedTls.h"
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/srtp.h>
#include <algorithm>
#include <stdexcept>
#include <new>

namespace CemuPad::Control {
namespace {
std::array<std::uint8_t,64> HandshakeIdentity(SSL* ssl) {
    std::array<std::uint8_t,64> bytes{};
    if(!ssl||!SSL_is_init_finished(ssl)||SSL_get_client_random(ssl,bytes.data(),32)!=32||SSL_get_server_random(ssl,bytes.data()+32,32)!=32) throw std::runtime_error("TLS handshake identity unavailable");
    return bytes;
}
struct BoundDatagram { SessionId session; Channel channel; std::array<std::uint8_t,64> handshake; };
void FreeDatagram(void*,void* value,CRYPTO_EX_DATA*,int,long,void*) { delete static_cast<BoundDatagram*>(value); }
int DupDatagram(CRYPTO_EX_DATA*,const CRYPTO_EX_DATA*,void** value,int,long,void*) {
    if(!*value) return 1;
    auto* copy=new(std::nothrow) BoundDatagram(*static_cast<BoundDatagram*>(*value));
    if(!copy) return 0; *value=copy; return 1;
}
bool BindDatagram(SSL* ssl,SessionId session,Channel channel) {
    static const int index=SSL_get_ex_new_index(0,nullptr,nullptr,DupDatagram,FreeDatagram);
    if(index<0) return false; auto identity=HandshakeIdentity(ssl);
    auto* bound=static_cast<BoundDatagram*>(SSL_get_ex_data(ssl,index));
    if(bound) return bound->session==session&&bound->channel==channel&&bound->handshake==identity;
    auto value=std::make_unique<BoundDatagram>(BoundDatagram{session,channel,identity});
    if(SSL_set_ex_data(ssl,index,value.get())!=1) return false;
    value.release(); return true;
}
bool Pin(SSL* ssl,const Crypto::Digest& expected,Crypto::Digest& peer) {
    if(!ssl||!SSL_is_init_finished(ssl)) return false;
    std::unique_ptr<X509,decltype(&X509_free)> cert(SSL_get1_peer_certificate(ssl),X509_free);
    if(!cert||X509_cmp_current_time(X509_get0_notBefore(cert.get()))>=0||X509_cmp_current_time(X509_get0_notAfter(cert.get()))<=0||!Crypto::IsP256(X509_get0_pubkey(cert.get()))) return false;
    peer=Crypto::SpkiFingerprint(X509_get0_pubkey(cert.get()));
    return peer==expected;
}
Crypto::Digest Export(SSL* ssl,const char* label,std::span<const std::uint8_t> context) {
    Crypto::Digest value{};
    if(SSL_export_keying_material(ssl,value.data(),value.size(),label,std::char_traits<char>::length(label),context.data(),context.size(),1)!=1) throw std::runtime_error("Control exporter failed");
    return value;
}
template<std::size_t N> std::array<std::uint8_t,N> ParseHex(std::string_view text) {
    if(text.size()!=N*2) throw std::invalid_argument("Validated hex length");
    std::array<std::uint8_t,N> bytes{};
    auto digit=[](char c)->unsigned { if(c>='0'&&c<='9') return c-'0'; if(c>='a'&&c<='f') return c-'a'+10; throw std::invalid_argument("Validated hex digit"); };
    for(std::size_t i=0;i<N;++i) bytes[i]=static_cast<std::uint8_t>((digit(text[i*2])<<4)|digit(text[i*2+1]));
    return bytes;
}
}
std::string Hex(std::span<const std::uint8_t> bytes) {
    constexpr char h[]="0123456789abcdef"; std::string out; out.reserve(bytes.size()*2);
    for(auto b:bytes) { out+=h[b>>4];out+=h[b&15]; } return out;
}
Crypto::Digest Digest(std::string_view text) { return ParseHex<32>(text); }
SessionId Session(std::string_view text) { return ParseHex<16>(text); }
struct VerifiedTls::Data {
    std::shared_ptr<SSL> ssl; Role role; Crypto::Digest host{},client{},peer{};
    std::array<std::uint8_t,64> handshake{};
};
std::optional<VerifiedTls> VerifiedTls::FromSsl(SSL* ssl,Role role,const Crypto::Digest& expected) {
    try {
        Crypto::Digest peer{};
        if(!Pin(ssl,expected,peer)||SSL_is_server(ssl)!=(role==Role::Host?1:0)||SSL_get_version(ssl)!=std::string_view("TLSv1.3")||SSL_get_cipher_name(ssl)!=std::string_view("TLS_AES_128_GCM_SHA256")) return {};
        auto* cert=SSL_get_certificate(ssl);
        if(!cert||!Crypto::IsP256(X509_get0_pubkey(cert))||X509_cmp_current_time(X509_get0_notBefore(cert))>=0||X509_cmp_current_time(X509_get0_notAfter(cert))<=0) return {};
        auto local=Crypto::SpkiFingerprint(X509_get0_pubkey(cert));
        auto data=std::make_shared<Data>(); if(SSL_up_ref(ssl)!=1) return {};
        data->ssl=std::shared_ptr<SSL>(ssl,SSL_free); data->role=role; data->peer=peer;
        data->handshake=HandshakeIdentity(ssl);
        data->host=role==Role::Host?local:peer; data->client=role==Role::Client?local:peer; return VerifiedTls(std::move(data));
    } catch(const std::exception&) { return {}; }
}
const Crypto::Digest& VerifiedTls::HostPin() const { return m_data->host; }
const Crypto::Digest& VerifiedTls::ClientPin() const { return m_data->client; }
const Crypto::Digest& VerifiedTls::PeerPin() const { return m_data->peer; }
Role VerifiedTls::LocalRole() const { return m_data->role; }
Crypto::Digest VerifiedTls::AuthBinding(const Crypto::Digest& hn,const Crypto::Digest& cn) const {
    if(HandshakeIdentity(m_data->ssl.get())!=m_data->handshake) throw std::runtime_error("Control TLS object reused");
    std::array<std::uint8_t,64> context{}; std::copy(hn.begin(),hn.end(),context.begin()); std::copy(cn.begin(),cn.end(),context.begin()+32);
    return Export(m_data->ssl.get(),"EXPORTER-CemuPad-Auth-v1",context);
}
std::optional<VerifiedChannel> VerifiedChannel::FromSsl(SSL* ssl,Role localRole,SessionId session,Channel channel,const Crypto::Digest& expectedPeer,const Endpoint& endpoint) {
    try {
        VerifiedChannel value;
        if(IsZero(session)||static_cast<unsigned>(channel)>2||endpoint.channel!=channel||endpoint.port==0||!Pin(ssl,expectedPeer,value.m_peer)||SSL_is_server(ssl)!=(localRole==Role::Host?1:0)||
           SSL_get_version(ssl)!=std::string_view("DTLSv1.2")||SSL_get_cipher_name(ssl)!=std::string_view("ECDHE-ECDSA-AES128-GCM-SHA256")) return {};
        auto* profile=SSL_get_selected_srtp_profile(ssl);
        if(channel==Channel::InputHaptics?profile!=nullptr:(!profile||profile->id!=SRTP_AEAD_AES_128_GCM)) return {};
        auto* localCert=SSL_get_certificate(ssl); if(!localCert||!Crypto::IsP256(X509_get0_pubkey(localCert))) return {};
        value.m_local=Crypto::SpkiFingerprint(X509_get0_pubkey(localCert)); value.m_role=localRole;
        auto hostPin=localRole==Role::Host?value.m_local:value.m_peer;
        if(Digest(endpoint.hostPin)!=hostPin) return {};
        std::array<std::uint8_t,17> context{}; std::copy(session.begin(),session.end(),context.begin()); context[16]=static_cast<std::uint8_t>(channel);
        value.m_session=session; value.m_channel=channel; value.m_endpoint=endpoint;
        value.m_binding=Crypto::Sha256(Export(ssl,"EXPORTER-CemuPad-Channel-v1",context));
        if(!BindDatagram(ssl,session,channel)) return {}; return value;
    } catch(const std::exception&) { return {}; }
}
}
