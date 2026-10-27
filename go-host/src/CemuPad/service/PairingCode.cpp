#include "PairingCode.h"
#include "NetworkBinding.h"
#include <openssl/crypto.h>
#include <algorithm>

namespace CemuPad::Service {
PairingCode::~PairingCode(){if(!text.empty())OPENSSL_cleanse(text.data(),text.size());}
PairingCode::PairingCode(PairingCode&& other) noexcept
    :endpoint(std::move(other.endpoint)),storeId(std::move(other.storeId)),hostPin(std::move(other.hostPin)),
    generation(other.generation),deadline(other.deadline),text(std::move(other.text)){}
PairingCode& PairingCode::operator=(PairingCode&& other) noexcept{
    if(this!=&other){if(!text.empty())OPENSSL_cleanse(text.data(),text.size());endpoint=std::move(other.endpoint);
        storeId=std::move(other.storeId);hostPin=std::move(other.hostPin);generation=other.generation;
        deadline=other.deadline;text=std::move(other.text);}
    return *this;
}
PairingCode PairingCode::Create(const Transport::Address& address,const Store::Snapshot& identity,const Control::PairingOffer& offer){
    const auto ip=ParseBindAddress(address.ip);
    if(!ip||!address.port||identity.peer||!offer.generation)
        throw std::invalid_argument("Pairing bootstrap scope");
    std::array<std::uint8_t,90> raw{};
    struct Wipe{decltype(raw)& v;~Wipe(){OPENSSL_cleanse(v.data(),v.size());}} wipe{raw};
    const auto id=Control::Session(identity.identity.StoreId());const auto pin=identity.identity.Pin();
    std::copy_n("CPQ1",4,raw.begin());std::copy(id.begin(),id.end(),raw.begin()+4);
    std::copy(pin.begin(),pin.end(),raw.begin()+20);std::copy(ip->begin(),ip->end(),raw.begin()+52);
    raw[56]=static_cast<std::uint8_t>(address.port>>8);raw[57]=static_cast<std::uint8_t>(address.port);
    std::copy(offer.token.bytes.begin(),offer.token.bytes.end(),raw.begin()+58);
    auto encoded=Store::Base64(raw);
    struct WipeText{std::string& v;~WipeText(){OPENSSL_cleanse(v.data(),v.size());}} erase{encoded};
    PairingCode result;result.endpoint=address;result.storeId=identity.identity.StoreId();result.hostPin=Control::Hex(pin);
    result.generation=offer.generation;result.deadline=offer.deadline;
    constexpr std::string_view prefix="CemuPadPairV1:";
    result.text.assign(prefix.begin(),prefix.end());result.text.insert(result.text.end(),encoded.begin(),encoded.end());
    return result;
}
}
