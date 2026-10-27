#pragma once
#include "CemuPad/crypto/AuthPrimitives.h"
#include <openssl/x509.h>
#include <memory>
#include <string>

namespace CemuPad::Store {
// Native TLS owns shared references. Serialized private bytes are short-lived and cleansed.
class Identity {
public:
    static Identity Generate();
    static Identity Decode(std::span<const std::uint8_t> encoded);
    std::vector<std::uint8_t> Encode() const;
    EVP_PKEY* Key() const { return key.get(); }
    X509* Certificate() const { return certificate.get(); }
    Crypto::Digest Pin() const;
    std::vector<std::uint8_t> CertificateDer() const;
    const std::string& StoreId() const { return storeId; }
private:
    std::shared_ptr<EVP_PKEY> key;
    std::shared_ptr<X509> certificate;
    std::string storeId;
};
std::string Base64(std::span<const std::uint8_t> bytes);
void ValidateCertificate(std::span<const std::uint8_t> bytes,const Crypto::Digest& pin,bool currentTime=true);
}
