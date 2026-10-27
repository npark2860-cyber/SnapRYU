#pragma once
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace CemuPad::Service {
// Explicit local IPv4 only: canonical RFC1918 or the development loopback.
// These ranges do not describe the Windows firewall/network profile.
std::optional<std::array<unsigned char,4>> ParseBindAddress(std::string_view);
std::vector<std::string> LocalBindAddresses();
bool BindAddressAvailable(std::string_view);
}
