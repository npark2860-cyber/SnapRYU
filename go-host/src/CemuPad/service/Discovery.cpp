#include "Discovery.h"
#include "NetworkBinding.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windns.h>
#include <iphlpapi.h>
#include <mutex>
#include <vector>

namespace CemuPad::Service {
struct DiscoveryPublisher::State:std::enable_shared_from_this<State> {
    struct Operation {
        DNS_SERVICE_REGISTER_REQUEST request{};
        DNS_SERVICE_CANCEL cancel{};
        std::shared_ptr<State> owner;
        bool remove=false;
    };
    PDNS_SERVICE_INSTANCE instance=nullptr;
    std::mutex mutex;
    std::shared_ptr<Operation> pending;
    bool stopping=false,registered=false;
    ~State(){if(instance)DnsServiceFreeInstance(instance);}
    static void WINAPI Complete(DWORD status,PVOID context,PDNS_SERVICE_INSTANCE result){
        auto* raw=static_cast<Operation*>(context);auto self=raw->owner;
        std::shared_ptr<Operation> operation;bool remove=false;
        {
            std::lock_guard lock(self->mutex);
            operation=std::move(self->pending);
            self->registered=!raw->remove&&status==ERROR_SUCCESS;
            remove=self->registered&&self->stopping;
        }
        if(result&&result!=self->instance)DnsServiceFreeInstance(result);
        if(remove)self->Begin(true);
    }
    void Begin(bool remove){
        auto operation=std::make_shared<Operation>();operation->owner=shared_from_this();operation->remove=remove;
        operation->request.Version=DNS_QUERY_REQUEST_VERSION1;
        operation->request.InterfaceIndex=instance->dwInterfaceIndex;
        operation->request.pServiceInstance=instance;
        operation->request.pRegisterCompletionCallback=Complete;
        operation->request.pQueryContext=operation.get();operation->request.unicastEnabled=FALSE;
        {
            std::lock_guard lock(mutex);
            if(pending||(remove?!registered:stopping))return;
            pending=operation;
        }
        const auto status=remove?DnsServiceDeRegister(&operation->request,nullptr):DnsServiceRegister(&operation->request,&operation->cancel);
        if(status!=DNS_REQUEST_PENDING)Complete(status,operation.get(),nullptr);
    }
    void Stop(){
        std::shared_ptr<Operation> operation;bool remove=false;
        {std::lock_guard lock(mutex);stopping=true;operation=pending;remove=!operation&&registered;}
        // Keep all request/cancel memory alive until its completion callback.
        if(operation&&!operation->remove)DnsServiceRegisterCancel(&operation->cancel);
        if(remove)Begin(true);
    }
};

DiscoveryPublisher::DiscoveryPublisher(std::string address,std::uint16_t port,std::string hostId){
    auto ip=ParseBindAddress(address);
    if(!ip||(*ip)[0]==127||!port||hostId.size()!=32||hostId.find_first_not_of("0123456789abcdef")!=std::string::npos)return;
    IP4_ADDRESS ipv4{};if(inet_pton(AF_INET,address.c_str(),&ipv4)!=1)return;
    ULONG size=0,index=0;GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,nullptr,nullptr,&size);
    std::vector<unsigned char> buffer(size);
    if(size&&GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,nullptr,reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()),&size)==NO_ERROR){
        for(auto* adapter=reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());adapter;adapter=adapter->Next)
            for(auto* unicast=adapter->FirstUnicastAddress;unicast;unicast=unicast->Next)
                if(unicast->Address.lpSockaddr->sa_family==AF_INET&&reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr)->sin_addr.s_addr==ipv4)index=adapter->IfIndex;
    }
    if(!index)return; // Never publish on interfaces other than the selected LAN.
    wchar_t computer[256]{};DWORD length=256;if(!GetComputerNameExW(ComputerNameDnsHostname,computer,&length))return;
    std::wstring id(hostId.begin(),hostId.end()),name=L"SnapRYUGo-"+id+L"._snapryugo._tcp.local",host=std::wstring(computer)+L".local";
    const wchar_t* keys[]={L"v",L"id"};const wchar_t* values[]={L"1",id.c_str()};
    auto next=std::make_shared<State>();
    next->instance=DnsServiceConstructInstance(name.c_str(),host.c_str(),&ipv4,nullptr,port,0,0,2,keys,values);
    if(!next->instance)return;
    next->instance->dwInterfaceIndex=index;state=std::move(next);state->Begin(false);
}
DiscoveryPublisher::~DiscoveryPublisher(){if(state)state->Stop();}
}
