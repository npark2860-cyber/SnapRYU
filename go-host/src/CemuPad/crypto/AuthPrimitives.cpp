#include "AuthPrimitives.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/x509.h>

namespace CemuPad::Crypto
{
bool IsP256(EVP_PKEY* key)
{
    if (!key || EVP_PKEY_is_a(key,"EC") != 1) return false;
    char group[64]{};
    return EVP_PKEY_get_utf8_string_param(key,OSSL_PKEY_PARAM_GROUP_NAME,group,sizeof(group),nullptr)==1 &&
        (std::strcmp(group,"prime256v1")==0 || std::strcmp(group,"P-256")==0);
}
Digest Sha256(std::span<const std::uint8_t> bytes)
{
    Digest result{}; unsigned int size = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), result.data(), &size, EVP_sha256(), nullptr) != 1 || size != result.size())
        throw std::runtime_error("SHA256 failed");
    return result;
}
Digest SpkiFingerprint(EVP_PKEY* key)
{
    int size = i2d_PUBKEY(key,nullptr);
    if (size <= 0) throw std::runtime_error("SPKI serialization failed");
    std::vector<std::uint8_t> der(static_cast<std::size_t>(size)); auto* p = der.data();
    if (i2d_PUBKEY(key,&p) != size) throw std::runtime_error("SPKI serialization mismatch");
    return Sha256(der);
}
std::optional<std::vector<std::uint8_t>> DecodeBase64Strict(std::string_view text)
{
    if (text.empty() || text.size() > 16384 || text.size() % 4) return {};
    std::size_t padding = 0;
    if (text.back() == '=') ++padding;
    if (text.size() >= 2 && text[text.size()-2] == '=') ++padding;
    for (std::size_t i=0;i<text.size();++i)
    {
        char ch=text[i]; bool alphabet=(ch>='A'&&ch<='Z')||(ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')||ch=='+'||ch=='/';
        if (!alphabet && !(ch=='='&&i>=text.size()-padding)) return {};
    }
    std::vector<std::uint8_t> bytes(text.size()/4*3);
    int size=EVP_DecodeBlock(bytes.data(),reinterpret_cast<const unsigned char*>(text.data()),static_cast<int>(text.size()));
    if(size<0 || static_cast<std::size_t>(size)<padding) return {};
    bytes.resize(static_cast<std::size_t>(size)-padding);
    std::vector<unsigned char> canonical(text.size()+1);
    int encoded=EVP_EncodeBlock(canonical.data(),bytes.data(),static_cast<int>(bytes.size()));
    if(encoded!=static_cast<int>(text.size()) || std::memcmp(canonical.data(),text.data(),text.size())!=0) return {};
    return bytes;
}
std::vector<std::uint8_t> PairTranscript(const Digest& host, const Digest& client,
    const Digest& hn, const Digest& cn, const Digest& token, const Digest& exporter)
{
    constexpr std::string_view label="CemuPadPairV1"; static_assert(label.size()==13);
    std::vector<std::uint8_t> out{0,static_cast<std::uint8_t>(label.size())};
    out.insert(out.end(),label.begin(),label.end());
    auto tokenHash=Sha256(token);
    const std::array<const Digest*,6> fields{&host,&client,&hn,&cn,&tokenHash,&exporter};
    for(const auto* field:fields) out.insert(out.end(),field->begin(),field->end());
    return out;
}
bool VerifyPairProof(EVP_PKEY* key, std::span<const std::uint8_t> message, std::span<const std::uint8_t> signature)
{
    if(!IsP256(key) || signature.empty() || signature.size()>80) return false;
    const auto* cursor=signature.data();
    std::unique_ptr<ECDSA_SIG,decltype(&ECDSA_SIG_free)> decoded(d2i_ECDSA_SIG(nullptr,&cursor,static_cast<long>(signature.size())),ECDSA_SIG_free);
    if(!decoded || cursor!=signature.data()+signature.size()) return false;
    int canonicalSize=i2d_ECDSA_SIG(decoded.get(),nullptr);
    if(canonicalSize!=static_cast<int>(signature.size())) return false;
    std::vector<std::uint8_t> canonical(static_cast<std::size_t>(canonicalSize)); auto* output=canonical.data();
    if(i2d_ECDSA_SIG(decoded.get(),&output)!=canonicalSize || !std::equal(canonical.begin(),canonical.end(),signature.begin())) return false;
    std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),EVP_MD_CTX_free);
    if(!context || EVP_DigestVerifyInit(context.get(),nullptr,EVP_sha256(),nullptr,key)!=1) return false;
    return EVP_DigestVerify(context.get(),signature.data(),signature.size(),message.data(),message.size())==1;
}
}
