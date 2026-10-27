#pragma once
#include <cstdint>
#include <memory>
#include <string>
namespace CemuPad::Service {
inline constexpr std::uint16_t DefaultControlPort=26762;
// Only a routing hint. TLS pin + host ID + existing trust remain authoritative.
class DiscoveryPublisher {
public:
    DiscoveryPublisher(std::string address,std::uint16_t port,std::string hostId);
    ~DiscoveryPublisher();
    DiscoveryPublisher(const DiscoveryPublisher&)=delete;
    DiscoveryPublisher& operator=(const DiscoveryPublisher&)=delete;
private:
    struct State;
    std::shared_ptr<State> state;
};
}
