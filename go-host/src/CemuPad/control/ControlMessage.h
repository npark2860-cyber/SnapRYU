#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace CemuPad::Control {
enum class Type { Hello,Capabilities,Pair,PairResult,Auth,AuthResult,SessionStart,ChannelReady,
    StreamConfig,StreamReady,HapticConfig,HapticReady,Heartbeat,TimeSync,KeyframeRequest,SessionStop,Error,VideoPreferences };
enum class Role { Host,Client };
enum class Channel : unsigned { InputHaptics=0,Video=1,Audio=2 };
enum class ParseError { None,Length,Utf8,Json,DuplicateKey,Depth,Schema,Semantic };
struct Endpoint { Channel channel; std::string address; std::uint16_t port; std::string hostPin;
    bool operator==(const Endpoint&) const=default; };
struct MessageAccess;
struct DecodeResult;
// Immutable validated envelope. No JSON-library types cross the public boundary.
class Message {
public:
    Type Kind() const;
    std::uint64_t Id() const;
    std::string_view Text(std::string_view field) const;
    std::uint64_t Integer(std::string_view field) const;
    double Number(std::string_view field) const;
    bool Flag(std::string_view field) const;
    bool Has(std::string_view field) const;
    std::vector<Endpoint> Endpoints() const;
    std::vector<std::string> Strings(std::string_view field) const;
    // Wire contents may contain a PAIR credential. Never use this as an error/log string.
    const std::string& JsonForWire() const;
    bool operator==(const Message& other) const;
private:
    struct Data;
    std::shared_ptr<const Data> m_data;
    explicit Message(std::shared_ptr<const Data> data):m_data(std::move(data)){}
    friend DecodeResult Decode(std::string_view);
    friend struct MessageAccess;
};
struct DecodeResult { std::optional<Message> message; ParseError error=ParseError::None;
    explicit operator bool() const { return message.has_value(); } };
DecodeResult Decode(std::string_view json);
std::vector<std::uint8_t> Frame(const Message& message);
const char* Name(Type type);
const char* Name(Channel channel);
Role Other(Role role);
bool ValidH264Configuration(const Message& message);
}
