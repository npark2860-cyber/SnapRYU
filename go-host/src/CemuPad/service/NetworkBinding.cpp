#include <winsock2.h>
#include <iphlpapi.h>
#include "NetworkBinding.h"
#include <algorithm>
#include <charconv>
#include <stdexcept>
namespace CemuPad::Service {
std::optional<std::array<unsigned char,4>> ParseBindAddress(std::string_view text){
    std::array<unsigned char,4> ip{};std::size_t at=0;
    for(unsigned i=0;i<4;++i){
        const auto end=i==3?text.size():text.find('.',at);if(end==std::string_view::npos||end<=at)return {};
        const auto part=text.substr(at,end-at);unsigned number=256;auto parsed=std::from_chars(part.data(),part.data()+part.size(),number);
        if(parsed.ec!=std::errc{}||parsed.ptr!=part.data()+part.size()||number>255||std::to_string(number)!=part)return {};
        ip[i]=static_cast<unsigned char>(number);at=end+1;
    }
    const bool local=text=="127.0.0.1"||ip[0]==10||(ip[0]==172&&ip[1]>=16&&ip[1]<=31)||(ip[0]==192&&ip[1]==168);
    return local?std::optional{ip}:std::nullopt;
}
std::vector<std::string> LocalBindAddresses(){
    std::vector<std::string> result{"127.0.0.1"};ULONG bytes=15000;std::vector<unsigned char> buffer;
    ULONG code=ERROR_BUFFER_OVERFLOW;
    for(unsigned attempt=0;attempt<3&&code==ERROR_BUFFER_OVERFLOW;++attempt){
        if(bytes>1024*1024)throw std::runtime_error("Adapter address buffer exceeds bound");
        buffer.resize(bytes);code=GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,
            nullptr,reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),&bytes);
    }
    if(code==ERROR_NO_DATA)return result;
    if(code!=NO_ERROR)throw std::runtime_error("Local adapter addresses unavailable");
    for(auto* adapter=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());adapter;adapter=adapter->Next){
        if(adapter->OperStatus!=IfOperStatusUp)continue;
        for(auto* unicast=adapter->FirstUnicastAddress;unicast;unicast=unicast->Next){
            if(unicast->DadState!=IpDadStatePreferred||!unicast->Address.lpSockaddr||unicast->Address.lpSockaddr->sa_family!=AF_INET)continue;
            const auto* a=reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
            const auto* ip=reinterpret_cast<const unsigned char*>(&a->sin_addr);
            auto text=std::to_string(ip[0])+'.'+std::to_string(ip[1])+'.'+std::to_string(ip[2])+'.'+std::to_string(ip[3]);
            if(text!="127.0.0.1"&&ParseBindAddress(text))result.push_back(std::move(text));
        }
    }
    std::sort(result.begin()+1,result.end());result.erase(std::unique(result.begin(),result.end()),result.end());return result;
}
bool BindAddressAvailable(std::string_view ip){
    if(!ParseBindAddress(ip))return false;if(ip=="127.0.0.1")return true;
    const auto addresses=LocalBindAddresses();return std::find(addresses.begin(),addresses.end(),ip)!=addresses.end();
}
}
