#include "ControlFramer.h"
#include <algorithm>

namespace CemuPad
{
bool ValidUtf8(std::string_view text)
{
    for (std::size_t i = 0; i < text.size();)
    {
        auto first = static_cast<std::uint8_t>(text[i++]);
        if (first < 0x80) continue;
        unsigned tail = 0; std::uint32_t value = 0, minimum = 0;
        if (first >= 0xC2 && first <= 0xDF) { tail = 1; value = first & 31; minimum = 0x80; }
        else if (first >= 0xE0 && first <= 0xEF) { tail = 2; value = first & 15; minimum = 0x800; }
        else if (first >= 0xF0 && first <= 0xF4) { tail = 3; value = first & 7; minimum = 0x10000; }
        else return false;
        if (text.size() - i < tail) return false;
        for (unsigned j = 0; j < tail; ++j)
        {
            auto byte = static_cast<std::uint8_t>(text[i++]);
            if ((byte & 0xC0) != 0x80) return false;
            value = (value << 6) | (byte & 63);
        }
        if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) return false;
    }
    return true;
}
FramingError ControlFramer::Feed(std::span<const std::uint8_t> bytes, const std::function<bool(std::string_view)>& onMessage)
{
    if (m_error != FramingError::None) return m_error;
    while (!bytes.empty())
    {
        while (m_prefixBytes < 4 && !bytes.empty())
        {
            m_length = (m_length << 8) | bytes.front(); ++m_prefixBytes; bytes = bytes.subspan(1);
        }
        if (m_prefixBytes < 4) break;
        if (m_length == 0 || m_length > MaxJsonBytes) return m_error = FramingError::Length;
        auto count = std::min<std::size_t>(bytes.size(), m_length - m_body.size());
        m_body.append(reinterpret_cast<const char*>(bytes.data()), count); bytes = bytes.subspan(count);
        if (m_body.size() == m_length)
        {
            if (!ValidUtf8(m_body)) return m_error = FramingError::Utf8;
            // The callback must validate JSON and the message schema before changing session state.
            if (!onMessage(m_body)) return m_error = FramingError::Rejected;
            m_body.clear(); m_length = m_prefixBytes = 0;
        }
    }
    return FramingError::None;
}
void ControlFramer::Reset() { m_body.clear(); m_length = m_prefixBytes = 0; m_error = FramingError::None; }
}
