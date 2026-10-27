#include "PairingAuthority.h"
#include "VerifiedTls.h"
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <algorithm>
#include <chrono>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace CemuPad::Control {
struct PairingAuthority::Impl {
    std::mutex mutex; enum class State { Closed,Open,Committing,Used } state=State::Closed;
    std::uint64_t generation=0; RawPairToken token; LocalTimeUs opened=0,deadline=0,lastNow=0;
    Clock clock;
    void Erase() { OPENSSL_cleanse(token.bytes.data(),token.bytes.size()); }
    ~Impl(){Erase();}
};
PairingAuthority::PairingAuthority(Clock clock):m(std::make_unique<Impl>()) {
    m->clock=clock?std::move(clock):Clock([](){return static_cast<LocalTimeUs>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());});
}
PairingAuthority::~PairingAuthority()=default;
PairingOffer PairingAuthority::Open() {
    std::lock_guard lock(m->mutex);
    auto now=m->clock();
    if(m->generation==std::numeric_limits<std::uint64_t>::max()||now>std::numeric_limits<LocalTimeUs>::max()-120'000'000) throw std::overflow_error("Pairing clock/generation exhausted");
    RawPairToken token;
    struct WipeLocal {RawPairToken& token;~WipeLocal(){OPENSSL_cleanse(token.bytes.data(),token.bytes.size());}} wipe{token};
    if(RAND_bytes(token.bytes.data(),static_cast<int>(token.bytes.size()))!=1) throw std::runtime_error("Pairing entropy failed");
    m->Erase(); m->token=token; m->state=Impl::State::Open; ++m->generation; m->opened=now; m->lastNow=now; m->deadline=now+120'000'000;
    return {m->generation,token,m->deadline};
}
void PairingAuthority::Close() { std::lock_guard lock(m->mutex); m->state=Impl::State::Closed; m->Erase(); }
bool PairingAuthority::CanAccept(std::uint64_t generation) {
    std::lock_guard lock(m->mutex);
    if(generation!=m->generation||m->state!=Impl::State::Open)return false;
    auto now=m->clock();
    if(now<m->opened||now<m->lastNow)return false;
    if(now>=m->deadline){m->state=Impl::State::Closed;m->Erase();return false;}
    return true;
}
PairOutcome PairingAuthority::Submit(std::uint64_t generation,SSL* ssl,const Message& ch,const Message& hh,const Message& pair,const Commit& commit) {
    std::lock_guard lock(m->mutex);
    auto now=m->clock();
    if(generation!=m->generation) return PairOutcome::StaleWindow;
    if(m->state==Impl::State::Used) return PairOutcome::Used;
    if(m->state!=Impl::State::Open) return PairOutcome::Closed;
    if(now<m->opened||now<m->lastNow) return PairOutcome::Invalid;
    if(now>=m->deadline) { m->state=Impl::State::Closed; m->Erase(); return PairOutcome::Expired; }
    if(!ssl||!SSL_is_init_finished(ssl)||!SSL_is_server(ssl)||SSL_get_version(ssl)!=std::string_view("TLSv1.3")||
       SSL_get_cipher_name(ssl)!=std::string_view("TLS_AES_128_GCM_SHA256")||ch.Kind()!=Type::Hello||hh.Kind()!=Type::Hello||pair.Kind()!=Type::Pair||
       ch.Text("role")!="client"||hh.Text("role")!="host"||ch.Id()!=hh.Id()||ch.Text("host_id")!=hh.Text("host_id")||pair.Text("client_nonce")!=ch.Text("nonce")) return PairOutcome::Invalid;
    try {
        auto rawToken=Digest(pair.Text("token"));
        bool tokenEqual=CRYPTO_memcmp(rawToken.data(),m->token.bytes.data(),rawToken.size())==0;
        OPENSSL_cleanse(rawToken.data(),rawToken.size()); if(!tokenEqual) return PairOutcome::Invalid;
        std::unique_ptr<X509,decltype(&X509_free)> peer(SSL_get1_peer_certificate(ssl),X509_free);
        auto* host=SSL_get_certificate(ssl);
        auto valid=[](X509* cert) { return cert&&Crypto::IsP256(X509_get0_pubkey(cert))&&X509_cmp_current_time(X509_get0_notBefore(cert))<0&&X509_cmp_current_time(X509_get0_notAfter(cert))>0&&X509_verify(cert,X509_get0_pubkey(cert))==1; };
        if(!valid(peer.get())||!valid(host)) return PairOutcome::Invalid;
        auto certBytes=Crypto::DecodeBase64Strict(pair.Text("client_certificate_der")),signature=Crypto::DecodeBase64Strict(pair.Text("proof_der"));
        if(!certBytes||!signature) return PairOutcome::Invalid;
        const unsigned char* cursor=certBytes->data(); std::unique_ptr<X509,decltype(&X509_free)> cert(d2i_X509(nullptr,&cursor,static_cast<long>(certBytes->size())),X509_free);
        if(!valid(cert.get())||cursor!=certBytes->data()+certBytes->size()) return PairOutcome::Invalid;
        auto clientPin=Crypto::SpkiFingerprint(X509_get0_pubkey(peer.get())),hostPin=Crypto::SpkiFingerprint(X509_get0_pubkey(host));
        if(Crypto::SpkiFingerprint(X509_get0_pubkey(cert.get()))!=clientPin) return PairOutcome::Invalid;
        auto hn=Digest(hh.Text("nonce")),cn=Digest(ch.Text("nonce")); std::array<std::uint8_t,64> context{};
        std::copy(hn.begin(),hn.end(),context.begin()); std::copy(cn.begin(),cn.end(),context.begin()+32);
        Crypto::Digest exporter{}; constexpr std::string_view label="EXPORTER-CemuPad-Pair-v1";
        if(SSL_export_keying_material(ssl,exporter.data(),exporter.size(),label.data(),label.size(),context.data(),context.size(),1)!=1) return PairOutcome::Invalid;
        auto transcript=Crypto::PairTranscript(hostPin,clientPin,hn,cn,m->token.bytes,exporter);
        if(!Crypto::VerifyPairProof(X509_get0_pubkey(peer.get()),transcript,*signature)) return PairOutcome::Invalid;
        if(!commit) return PairOutcome::StoreFailed;
        TrustRecord record; record.clientPin=clientPin; record.hostPin=hostPin; record.clientCertificate=std::move(*certBytes);
        if(RAND_bytes(record.transaction.data(),static_cast<int>(record.transaction.size()))!=1) return PairOutcome::StoreFailed;
        auto reserved=m->clock();
        if(reserved<now||reserved<m->lastNow) return PairOutcome::Invalid;
        if(reserved>=m->deadline) { m->state=Impl::State::Closed; m->Erase(); return PairOutcome::Expired; }
        m->state=Impl::State::Committing; m->lastNow=reserved;
        CommitOutcome result=CommitOutcome::Indeterminate;
        try { result=commit(record); } catch(...) { result=CommitOutcome::Indeterminate; }
        if(result==CommitOutcome::NotCommitted) { m->state=Impl::State::Open; return PairOutcome::StoreFailed; }
        m->state=Impl::State::Used; m->Erase();
        return result==CommitOutcome::Committed?PairOutcome::Paired:PairOutcome::Indeterminate;
    } catch(const std::exception&) { return PairOutcome::Invalid; }
}
}
