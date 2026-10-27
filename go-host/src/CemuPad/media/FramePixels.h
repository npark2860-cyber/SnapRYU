#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace CemuPad::Media {
enum class PixelOrder { Rgba, Bgra };

inline void CopyPixelRows(std::uint8_t* destination, const std::uint8_t* source,
                          unsigned width, unsigned height, unsigned stride) {
    const auto row = static_cast<std::size_t>(width) * 4;
    if (stride == row) {
        std::memcpy(destination, source, row * height);
    } else {
        for (unsigned y = 0; y < height; ++y)
            std::memcpy(destination + y * row, source + static_cast<std::size_t>(y) * stride, row);
    }
}

// Keep native capture order through the upload. Only unsupported driver formats
// need a CPU swizzle; scratch is reused and the source remains immutable.
inline const std::uint8_t* UploadPixels(const std::vector<std::uint8_t>& pixels,
                                      PixelOrder captured, PixelOrder upload,
                                      std::vector<std::uint8_t>& scratch) {
    if (captured == upload) return pixels.data();
    scratch.resize(pixels.size());
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        scratch[i] = pixels[i + 2]; scratch[i + 1] = pixels[i + 1];
        scratch[i + 2] = pixels[i]; scratch[i + 3] = pixels[i + 3];
    }
    return scratch.data();
}
}
