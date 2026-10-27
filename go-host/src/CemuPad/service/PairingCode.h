#pragma once
#include "CemuPad/transport/WindowsHost.h"

namespace CemuPad::Service {
// A move-only, memory-only copy for the explicit pairing dialog. Never log it.
struct PairingCode {
    Transport::Address endpoint;
    std::string storeId,hostPin;
    std::uint64_t generation=0;
    LocalTimeUs deadline=0;
    std::vector<std::uint8_t> text;
    PairingCode()=default;
    ~PairingCode();
    PairingCode(const PairingCode&)=delete;
    PairingCode& operator=(const PairingCode&)=delete;
    PairingCode(PairingCode&&) noexcept;
    PairingCode& operator=(PairingCode&&) noexcept;
    static PairingCode Create(const Transport::Address&,const Store::Snapshot&,const Control::PairingOffer&);
};
}
