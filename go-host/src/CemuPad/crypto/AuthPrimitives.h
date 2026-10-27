#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>
#include <openssl/evp.h>

namespace CemuPad::Crypto
{
bool IsP256(EVP_PKEY* key);
using Digest = std::array<std::uint8_t,32>;
Digest Sha256(std::span<const std::uint8_t> bytes);
Digest SpkiFingerprint(EVP_PKEY* key);
std::optional<std::vector<std::uint8_t>> DecodeBase64Strict(std::string_view text);
std::vector<std::uint8_t> PairTranscript(const Digest& hostSpki, const Digest& clientSpki,
    const Digest& hostNonce, const Digest& clientNonce, const Digest& token, const Digest& tlsExporter);
bool VerifyPairProof(EVP_PKEY* key, std::span<const std::uint8_t> transcript, std::span<const std::uint8_t> derSignature);
}
