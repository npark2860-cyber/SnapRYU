#include "ControlMessage.h"
#include "ControlSchema.h"
#include "CemuPad/core/ControlFramer.h"
#include "CemuPad/crypto/AuthPrimitives.h"
#include "CemuPad/media/H264.h"
#include "CemuPad/third_party/nlohmann/json.hpp"
#include <openssl/x509.h>
#include <openssl/ec.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace CemuPad::Control {
namespace {
using Json=nlohmann::json;
struct Invalid { ParseError code; };
constexpr std::array<const char*,18> names{"HELLO","CAPABILITIES","PAIR","PAIR_RESULT","AUTH","AUTH_RESULT","SESSION_START","CHANNEL_READY",
    "STREAM_CONFIG","STREAM_READY","HAPTIC_CONFIG","HAPTIC_READY","HEARTBEAT","TIME_SYNC","KEYFRAME_REQUEST","SESSION_STOP","ERROR","VIDEO_PREFERENCES"};
bool U64(std::string_view s,std::uint64_t* out=nullptr) {
    if(s.empty()||s.size()>20||(s.size()>1&&s.front()=='0')) return false;
    std::uint64_t value=0; auto r=std::from_chars(s.data(),s.data()+s.size(),value);
    if(r.ec!=std::errc{}||r.ptr!=s.data()+s.size()) return false;
    if(out) *out=value; return true;
}
bool Hex(std::string_view s,std::size_t n) {
    return s.size()==n&&std::all_of(s.begin(),s.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
std::size_t Codepoints(std::string_view s) { return static_cast<std::size_t>(std::count_if(s.begin(),s.end(),[](unsigned char c){return (c&0xc0)!=0x80;})); }
bool Pattern(const std::string& pattern,const std::string& s) {
    if(pattern=="\\s") return s.find_first_of(" \t\r\n\f\v")!=std::string::npos;
    if(pattern=="^(0|[1-9][0-9]{0,19})$") {
        // Actual uint64 overflow is a separate semantic check.
        return !s.empty()&&s.size()<=20&&(s.size()==1||s.front()!='0')&&std::all_of(s.begin(),s.end(),[](char c){return c>='0'&&c<='9';});
    }
    if(pattern=="^[0-9a-f]{32}$") return Hex(s,32);
    if(pattern=="^[0-9a-f]{64}$") return Hex(s,64);
    if(pattern=="^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$") {
        if(s.empty()) return true;
        if(s.size()%4) return false;
        std::size_t pad=s.back()=='='?1:0; if(s.size()>1&&s[s.size()-2]=='=') ++pad;
        for(std::size_t i=0;i<s.size();++i) { char c=s[i];
            if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='+'||c=='/'||(c=='='&&i>=s.size()-pad))) return false;
        }
        return pad<=2;
    }
    throw std::logic_error("Unsupported frozen control schema pattern");
}
void CheckSchema(const Json& s) {
    static const std::set<std::string> supported{"$schema","title","type","properties","required","additionalProperties","const","enum",
        "minimum","maximum","minLength","maxLength","pattern","minItems","maxItems","items","uniqueItems","oneOf","anyOf","allOf","not","contains","minContains","maxContains"};
    if(!s.is_object()) throw std::logic_error("Unsupported frozen control schema node");
    for(auto i=s.begin();i!=s.end();++i) {
        if(!supported.contains(i.key())) throw std::logic_error("Unsupported frozen control schema keyword");
        if(i.key()=="properties") for(const auto& child:i.value()) CheckSchema(child);
        else if(i.key()=="oneOf"||i.key()=="anyOf"||i.key()=="allOf") for(const auto& child:i.value()) CheckSchema(child);
        else if(i.key()=="items"||i.key()=="not"||i.key()=="contains") CheckSchema(i.value());
        else if(i.key()=="pattern") (void)Pattern(i.value().get<std::string>(),"");
        else if(i.key()=="additionalProperties"&&i.value()!=false) throw std::logic_error("Unsupported additional-properties schema");
    }
}
bool Matches(const Json& v,const Json& s) {
    for(const char* keyword:{"oneOf","anyOf","allOf"}) if(s.contains(keyword)) {
        std::size_t hits=0; for(const auto& child:s.at(keyword)) if(Matches(v,child)) ++hits;
        if((std::string_view(keyword)=="oneOf"&&hits!=1)||(std::string_view(keyword)=="anyOf"&&hits==0)||(std::string_view(keyword)=="allOf"&&hits!=s.at(keyword).size())) return false;
    }
    if(s.contains("not")&&Matches(v,s.at("not"))) return false;
    if(s.contains("const")&&v!=s.at("const")) return false;
    if(s.contains("enum")&&std::find(s.at("enum").begin(),s.at("enum").end(),v)==s.at("enum").end()) return false;
    if(s.contains("type")) {
        auto type=s.at("type").get<std::string>();
        bool integer=v.is_number()&&std::isfinite(v.get<double>())&&std::floor(v.get<double>())==v.get<double>();
        if(!((type=="object"&&v.is_object())||(type=="array"&&v.is_array())||(type=="string"&&v.is_string())||
            (type=="boolean"&&v.is_boolean())||(type=="number"&&v.is_number())||(type=="integer"&&integer))) return false;
    }
    if(v.is_number()) {
        if(!std::isfinite(v.get<double>())) return false;
        if(s.contains("minimum")&&v.get<double>()<s.at("minimum").get<double>()) return false;
        if(s.contains("maximum")&&v.get<double>()>s.at("maximum").get<double>()) return false;
    }
    if(v.is_string()) {
        const auto& text=v.get_ref<const std::string&>(); auto n=Codepoints(text);
        if((s.contains("minLength")&&n<s.at("minLength").get<std::size_t>())||(s.contains("maxLength")&&n>s.at("maxLength").get<std::size_t>())) return false;
        if(s.contains("pattern")&&!Pattern(s.at("pattern").get<std::string>(),text)) return false;
    }
    if(v.is_object()) {
        if(s.contains("required")) for(const auto& name:s.at("required")) if(!v.contains(name.get<std::string>())) return false;
        for(auto i=v.begin();i!=v.end();++i) {
            bool known=s.contains("properties")&&s.at("properties").contains(i.key());
            if(known&&!Matches(i.value(),s.at("properties").at(i.key()))) return false;
            if(!known&&s.contains("additionalProperties")&&s.at("additionalProperties")==false) return false;
        }
    }
    if(v.is_array()) {
        if((s.contains("minItems")&&v.size()<s.at("minItems").get<std::size_t>())||(s.contains("maxItems")&&v.size()>s.at("maxItems").get<std::size_t>())) return false;
        if(s.value("uniqueItems",false)) for(std::size_t i=0;i<v.size();++i) for(std::size_t j=0;j<i;++j) if(v[i]==v[j]) return false;
        if(s.contains("items")) for(const auto& item:v) if(!Matches(item,s.at("items"))) return false;
        if(s.contains("contains")) {
            std::size_t count=0; for(const auto& item:v) if(Matches(item,s.at("contains"))) ++count;
            if(count<s.value("minContains",std::size_t{1})||count>s.value("maxContains",std::numeric_limits<std::size_t>::max())) return false;
        }
    }
    return true;
}
const Json& Schema() { static const Json s=[](){ auto j=Json::parse(Detail::SchemaText); CheckSchema(j); return j; }(); return s; }
void Semantics(const Json& v) {
    static const std::set<std::string> u64{"request_id","game_epoch","monotonic_us","t1","t2","t3","t4","host_time_us","sample_base"};
    static const std::set<std::string> base64{"client_certificate_der","proof_der","sps","pps"};
    if(v.is_object()) for(auto i=v.begin();i!=v.end();++i) {
        if(u64.contains(i.key())&&(!i.value().is_string()||!U64(i.value().get_ref<const std::string&>()))) throw Invalid{ParseError::Semantic};
        if(base64.contains(i.key())&&!Crypto::DecodeBase64Strict(i.value().get_ref<const std::string&>())) throw Invalid{ParseError::Semantic};
        Semantics(i.value());
    } else if(v.is_array()) for(const auto& item:v) Semantics(item);
}
void MessageSemantics(const Json& doc) {
    Semantics(doc); const auto& p=doc.at("payload"); const auto type=doc.at("type").get<std::string>();
    if(type=="HAPTIC_CONFIG"&&p.at("route")=="none"&&p.at("available").get<bool>()) throw Invalid{ParseError::Semantic};
    if(type=="TIME_SYNC") {
        std::uint64_t t1=0,t2=0,t3=0,t4=0;
        if(p.contains("t2")) { U64(p.at("t2").get_ref<const std::string&>(),&t2); U64(p.at("t3").get_ref<const std::string&>(),&t3); if(t3<t2) throw Invalid{ParseError::Semantic}; }
        if(p.contains("t4")) { U64(p.at("t1").get_ref<const std::string&>(),&t1); U64(p.at("t4").get_ref<const std::string&>(),&t4); if(t4<t1) throw Invalid{ParseError::Semantic}; }
    }
    if(type=="STREAM_CONFIG"&&p.at("stream")=="video") {
        auto crop=p.at("crop");
        if(crop.at("left").get<double>()+crop.at("width").get<double>()>p.at("coded_width").get<double>()||
           crop.at("top").get<double>()+crop.at("height").get<double>()>p.at("coded_height").get<double>()) throw Invalid{ParseError::Semantic};
        auto sps=Crypto::DecodeBase64Strict(p.at("sps").get_ref<const std::string&>()),pps=Crypto::DecodeBase64Strict(p.at("pps").get_ref<const std::string&>());
        if(!sps||!pps||sps->empty()||pps->empty()||((*sps)[0]&0x9f)!=7||((*pps)[0]&0x9f)!=8) throw Invalid{ParseError::Semantic};
    }
    if(type=="PAIR") {
        auto cert=Crypto::DecodeBase64Strict(p.at("client_certificate_der").get_ref<const std::string&>());
        auto signature=Crypto::DecodeBase64Strict(p.at("proof_der").get_ref<const std::string&>());
        const unsigned char* cursor=cert->data();
        std::unique_ptr<X509,decltype(&X509_free)> x(d2i_X509(nullptr,&cursor,static_cast<long>(cert->size())),X509_free);
        if(!x||cursor!=cert->data()+cert->size()||!Crypto::IsP256(X509_get0_pubkey(x.get()))) throw Invalid{ParseError::Semantic};
        cursor=signature->data(); std::unique_ptr<ECDSA_SIG,decltype(&ECDSA_SIG_free)> sig(d2i_ECDSA_SIG(nullptr,&cursor,static_cast<long>(signature->size())),ECDSA_SIG_free);
        if(!sig||cursor!=signature->data()+signature->size()) throw Invalid{ParseError::Semantic};
        int size=i2d_ECDSA_SIG(sig.get(),nullptr); if(size!=static_cast<int>(signature->size())) throw Invalid{ParseError::Semantic};
        std::vector<unsigned char> canonical(signature->size()); auto* out=canonical.data();
        if(i2d_ECDSA_SIG(sig.get(),&out)!=size||canonical!=*signature) throw Invalid{ParseError::Semantic};
    }
}
Channel ChannelName(const std::string& s) { return s=="input_haptics"?Channel::InputHaptics:s=="video"?Channel::Video:Channel::Audio; }
}
struct Message::Data { Json document; std::string wire; Type type; std::uint64_t id; };
DecodeResult Decode(std::string_view text) {
    if(text.empty()||text.size()>ControlFramer::MaxJsonBytes) return {{},ParseError::Length};
    if(!ValidUtf8(text)) return {{},ParseError::Utf8};
    if(text.find('\0')!=std::string_view::npos) return {{},ParseError::Json};
    try {
        std::vector<std::set<std::string>> objects; unsigned depth=0;
        auto callback=[&](int,Json::parse_event_t event,Json& value) {
            if(event==Json::parse_event_t::object_start||event==Json::parse_event_t::array_start) { if(++depth>16) throw Invalid{ParseError::Depth}; }
            if(event==Json::parse_event_t::object_start) objects.emplace_back();
            if(event==Json::parse_event_t::key&&!objects.back().insert(value.get<std::string>()).second) throw Invalid{ParseError::DuplicateKey};
            if(event==Json::parse_event_t::object_end) objects.pop_back();
            if(event==Json::parse_event_t::object_end||event==Json::parse_event_t::array_end) --depth;
            return true;
        };
        auto doc=Json::parse(text.begin(),text.end(),callback,true,false);
        if(!doc.is_object()||!Matches(doc,Schema())) return {{},ParseError::Schema};
        MessageSemantics(doc); auto name=doc.at("type").get<std::string>(); auto it=std::find(names.begin(),names.end(),name);
        if(it==names.end()) return {{},ParseError::Schema};
        std::uint64_t id=0; if(!U64(doc.at("request_id").get_ref<const std::string&>(),&id)) return {{},ParseError::Semantic};
        auto data=std::make_shared<Message::Data>(); data->document=std::move(doc); data->wire=data->document.dump(); data->id=id; data->type=static_cast<Type>(it-names.begin());
        return {Message(std::move(data)),ParseError::None};
    } catch(const Invalid& e) { return {{},e.code}; }
      catch(const Json::exception&) { return {{},ParseError::Json}; }
}
Type Message::Kind() const { return m_data->type; }
std::uint64_t Message::Id() const { return m_data->id; }
std::string_view Message::Text(std::string_view key) const { return m_data->document.at("payload").at(std::string(key)).get_ref<const std::string&>(); }
std::uint64_t Message::Integer(std::string_view key) const {
    const auto& v=m_data->document.at("payload").at(std::string(key)); std::uint64_t n=0;
    if(v.is_string()) { if(!U64(v.get_ref<const std::string&>(),&n)) throw std::logic_error("validated uint64 field"); return n; }
    if(!v.is_number()||v.get<double>()<0||v.get<double>()>4294967295.0||std::floor(v.get<double>())!=v.get<double>()) throw std::logic_error("validated integer field");
    return static_cast<std::uint64_t>(v.get<double>());
}
double Message::Number(std::string_view key) const { return m_data->document.at("payload").at(std::string(key)).get<double>(); }
bool Message::Flag(std::string_view key) const { return m_data->document.at("payload").at(std::string(key)).get<bool>(); }
bool Message::Has(std::string_view key) const { return m_data->document.at("payload").contains(std::string(key)); }
std::vector<Endpoint> Message::Endpoints() const {
    std::vector<Endpoint> out; for(const auto& v:m_data->document.at("payload").at("channels")) out.push_back({ChannelName(v.at("channel").get<std::string>()),v.at("address").get<std::string>(),static_cast<std::uint16_t>(v.at("port").get<double>()),v.at("host_spki_sha256").get<std::string>()}); return out;
}
std::vector<std::string> Message::Strings(std::string_view key) const { return m_data->document.at("payload").at(std::string(key)).get<std::vector<std::string>>(); }
const std::string& Message::JsonForWire() const { return m_data->wire; }
bool Message::operator==(const Message& other) const { return m_data->document==other.m_data->document; }
std::vector<std::uint8_t> Frame(const Message& m) {
    auto n=static_cast<std::uint32_t>(m.JsonForWire().size()); std::vector<std::uint8_t> out;
    for(unsigned i=0;i<4;++i) out.push_back(static_cast<std::uint8_t>(n>>(24-i*8)));
    out.insert(out.end(),m.JsonForWire().begin(),m.JsonForWire().end()); return out;
}
const char* Name(Type type) { return names.at(static_cast<std::size_t>(type)); }
const char* Name(Channel channel) { constexpr std::array<const char*,3> s{"input_haptics","video","audio"}; return s.at(static_cast<std::size_t>(channel)); }
Role Other(Role role) { return role==Role::Host?Role::Client:Role::Host; }
bool ValidH264Configuration(const Message& message){
    try{
        if(message.Kind()!=Type::StreamConfig||message.Text("stream")!="video")return false;
        auto p=Json::parse(message.JsonForWire()).at("payload");
        auto sps=Crypto::DecodeBase64Strict(p.at("sps").get<std::string>()),pps=Crypto::DecodeBase64Strict(p.at("pps").get<std::string>());
        if(!sps||!pps)return false;auto f=Media::ValidateH264(*sps,*pps);auto crop=p.at("crop");
        return f.codedWidth==p.at("coded_width")&&f.codedHeight==p.at("coded_height")&&f.level==p.at("level")
            &&f.left==crop.at("left")&&f.top==crop.at("top")&&f.width==crop.at("width")&&f.height==crop.at("height");
    }catch(...){return false;}
}

}
