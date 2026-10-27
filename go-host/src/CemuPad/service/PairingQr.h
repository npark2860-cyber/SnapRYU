#pragma once
#include <span>
#include <vector>
namespace CemuPad::Service {
// Matrix and pixels encode the secret, so keep them memory-only as well.
struct PairingRaster {
    int width=0;std::vector<unsigned char> pixels;
    PairingRaster()=default;~PairingRaster();
    PairingRaster(const PairingRaster&)=delete;PairingRaster& operator=(const PairingRaster&)=delete;
    PairingRaster(PairingRaster&&) noexcept=default;PairingRaster& operator=(PairingRaster&&)=delete;
};
struct PairingQr {
    int size=0;std::vector<unsigned char> modules;
    PairingQr()=default;~PairingQr();
    PairingQr(const PairingQr&)=delete;PairingQr& operator=(const PairingQr&)=delete;
    PairingQr(PairingQr&&) noexcept=default;PairingQr& operator=(PairingQr&&)=delete;
    static PairingQr Encode(std::span<const unsigned char> bootstrap);
    PairingRaster Raster(int scale=6) const; // fixed four-module quiet zone
};
}
