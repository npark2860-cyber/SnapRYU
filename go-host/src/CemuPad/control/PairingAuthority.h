#pragma once
#include "ControlMessage.h"
#include "CemuPad/core/Types.h"
#include "CemuPad/crypto/AuthPrimitives.h"
#include <openssl/types.h>
#include <functional>
#include <memory>

namespace CemuPad::Control {
struct RawPairToken { std::array<std::uint8_t,32> bytes{}; };
struct PairingOffer { std::uint64_t generation=0; RawPairToken token; LocalTimeUs deadline=0; };
enum class CommitOutcome { Committed,NotCommitted,Indeterminate };
enum class PairOutcome { Paired,Closed,Expired,Used,StaleWindow,Invalid,StoreFailed,Indeterminate };
struct TrustRecord {
    std::array<std::uint8_t,16> transaction{};
    Crypto::Digest clientPin{},hostPin{};
    std::vector<std::uint8_t> clientCertificate;
};
// One authority per user-data writer. Commit adapter must hold cross-process store
// ownership and synchronously report publication outcome. Empty adapter fails closed.
class PairingAuthority {
public:
    using Commit=std::function<CommitOutcome(const TrustRecord&)>;
    using Clock=std::function<LocalTimeUs()>;
    explicit PairingAuthority(Clock clock={}); ~PairingAuthority();
    PairingAuthority(const PairingAuthority&)=delete; PairingAuthority& operator=(const PairingAuthority&)=delete;
    PairingOffer Open();
    void Close();
    // Used before accepting an unregistered TLS certificate; reveals no token.
    bool CanAccept(std::uint64_t generation);
    PairOutcome Submit(std::uint64_t generation,SSL* currentTls,const Message& clientHello,const Message& hostHello,
                       const Message& pair,const Commit& commit);
private:
    struct Impl; std::unique_ptr<Impl> m;
};
}
