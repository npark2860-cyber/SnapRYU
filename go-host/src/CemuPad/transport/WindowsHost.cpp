#include "WindowsHost.h"

namespace CemuPad::Transport {
std::unique_ptr<ControlTransport> AcceptStoredHost(Listener& listener,Store::WindowsStore& store,Lifecycle& lifecycle,const Cancellation& stop,Deadline end){
    auto loaded=store.Load();if(!loaded.snapshot||!loaded.snapshot->peer)throw Error(Failure::PeerRejected);
    auto& snapshot=*loaded.snapshot;
    auto socket=TlsSocket::AcceptPinned(listener,snapshot.identity,snapshot.peer->clientPin,stop,end);
    return std::make_unique<ControlTransport>(std::move(socket),Control::Session(snapshot.identity.StoreId()),lifecycle,stop);
}
PairResult PairOnce(Listener& listener,Store::WindowsStore& store,Control::PairingAuthority& authority,std::uint64_t generation,const Cancellation& stop,Deadline end,const Store::Snapshot* expected){
    auto loaded=store.Load();if(!loaded.snapshot||loaded.snapshot->peer)throw Error(Failure::PeerRejected);
    auto& snapshot=*loaded.snapshot;
    // A live owner binds every attempt to the snapshot that opened its window.
    // CAS in Commit then protects changes occurring after this load as well.
    if(expected&&(expected->peer||snapshot.revision!=expected->revision||snapshot.identity.StoreId()!=expected->identity.StoreId()
        ||snapshot.identity.Pin()!=expected->identity.Pin()))throw Error(Failure::PeerRejected);
    try{
        auto socket=TlsSocket::AcceptProvisional(listener,snapshot.identity,stop,end,[&](){return authority.CanAccept(generation);});
        auto clientHello=socket->Receive(end);
        if(clientHello.Kind()!=Control::Type::Hello||clientHello.Text("role")!="client"||clientHello.Text("host_id")!=snapshot.identity.StoreId())throw Error(Failure::Protocol);
        auto hostHello=Hello(Control::Role::Host,clientHello.Id(),Control::Session(snapshot.identity.StoreId()));socket->Send(hostHello,end);
        auto pair=socket->Receive(end);
        if(pair.Kind()!=Control::Type::Pair||pair.Id()<=clientHello.Id())throw Error(Failure::Protocol);
        auto outcome=socket->SubmitPair(authority,generation,clientHello,hostHello,pair,[&](const Control::TrustRecord& record){
            return store.Commit(record,snapshot.revision).outcome;
        });
        PairResult result{outcome,false};
        if(outcome==Control::PairOutcome::Paired){
            auto response=MakeMessage(Control::Type::PairResult,pair.Id(),"{\"status\":\"paired\",\"client_spki_sha256\":\""+Control::Hex(socket->PeerPin())+"\",\"host_spki_sha256\":\""+Control::Hex(snapshot.identity.Pin())+"\"}");
            try{socket->Send(response,end);result.responseWritten=true;}catch(const Error&){/* Durable commit stands; client recovers by new pinned TLS. */}
        }
        socket->Close();return result;
    }catch(const Error& error){if(error.code==Failure::Cancelled)authority.Close();throw;}
}
}
