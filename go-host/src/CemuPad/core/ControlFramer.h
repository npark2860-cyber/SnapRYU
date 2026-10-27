#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace CemuPad
{
enum class FramingError { None, Length, Utf8, Rejected };
class ControlFramer
{
public:
    static constexpr std::uint32_t MaxJsonBytes = 65'536;
    FramingError Feed(std::span<const std::uint8_t> bytes, const std::function<bool(std::string_view)>& onMessage);
    void Reset();
    bool HasPartialFrame() const { return m_prefixBytes != 0; }
private:
    std::uint32_t m_length = 0, m_prefixBytes = 0;
    std::string m_body;
    FramingError m_error = FramingError::None;
};
bool ValidUtf8(std::string_view text);
}
