#include "ControlTransport.h"
#include <openssl/rand.h>
#include <algorithm>

namespace CemuPad::Transport {
namespace {
Control::VerifiedTls Verified(TlsSocket* socket){
    if(!socket)throw Error(Failure::Protocol);auto verified=socket->Verified();
    if(!verified)throw Error(Failure::PeerRejected);return *verified;
}
StopReason Reason(std::string_view text){
    if(text=="off")return StopReason::Off;if(text=="shutdown")return StopReason::Shutdown;
    if(text=="game_end")return StopReason::GameEnd;if(text=="replaced")return StopReason::Replaced;
    return text=="error"?StopReason::Error:StopReason::TransportLost;
}
}
Control::Message MakeMessage(Control::Type type,std::uint64_t id,std::string_view payload){
    auto decoded=Control::Decode("{\"version\":1,\"type\":\""+std::string(Control::Name(type))+"\",\"request_id\":\""+std::to_string(id)+"\",\"payload\":"+std::string(payload)+"}");
    if(!decoded)throw Error(Failure::Protocol);return *decoded.message;
}
Control::Message Hello(Control::Role role,std::uint64_t id,SessionId hostId){
    Crypto::Digest nonce{};if(RAND_bytes(nonce.data(),static_cast<int>(nonce.size()))!=1)throw Error(Failure::Handshake);
    return MakeMessage(Control::Type::Hello,id,"{\"role\":\""+std::string(role==Control::Role::Host?"host":"client")+"\",\"protocol_major\":1,\"nonce\":\""+Control::Hex(nonce)+"\",\"host_id\":\""+Control::Hex(hostId)+"\"}");
}
void Lifecycle::CancelOutput() noexcept{
    haptic.Cancel();if(rumble)rumble->Cancel();if(cancelOutput)try{cancelOutput();}catch(...){outputCancelFailed=true;}
}
void Lifecycle::Tick(LocalTimeUs now){core.Tick(now);}
void Lifecycle::Stop(StopReason reason,LocalTimeUs now) noexcept{
    try{
        if(reason==StopReason::Off||reason==StopReason::Shutdown){if(pairing)pairing->Close();core.SetEnabled(false,now);}
        else if(reason==StopReason::GameEnd)core.EndGame(now);
        else core.OnTransportLost(now);
    }catch(...){coreStopFailed=true;}
    CancelOutput();
}
ControlTransport::ControlTransport(std::unique_ptr<TlsSocket> connection,SessionId hostId,Lifecycle& lifecycle,const Cancellation& stop)
    :socket(std::move(connection)),tls(Verified(socket.get())),effects(lifecycle),cancellation(stop){
    local=socket->LocalAddress();peer=socket->PeerAddress();
    token=session.Open(tls,hostId,tls.LocalRole()==Control::Role::Host?local.ip:peer.ip);
}
ControlTransport::~ControlTransport(){if(!closed){session.Close(token);Stop(StopReason::TransportLost,Now());socket->Close();}
    auto reason=cancellation.Reason();if(!disabledApplied&&(reason==StopReason::Off||reason==StopReason::Shutdown))Stop(reason,Now());}
void ControlTransport::Owner() const{if(std::this_thread::get_id()!=owner)throw std::logic_error("Transport called outside owner executor");}
void ControlTransport::Stop(StopReason reason,LocalTimeUs now) noexcept{
    if(reason==StopReason::Off||reason==StopReason::Shutdown)disabledApplied=true;
    effects.Stop(reason,now);
}
void ControlTransport::Apply(const Control::Decision& decision,const Control::Message* message,LocalTimeUs now,bool closeSocket){
    if(decision.dataStopped)Stop(message&&message->Kind()==Control::Type::SessionStop?Reason(message->Text("reason")):StopReason::TransportLost,now);
    else if(decision.outputCancelled)effects.CancelOutput();
    if(decision.controlClosed){closed=true;if(closeSocket)socket->Close();}
}
void ControlTransport::Failed(LocalTimeUs now) noexcept{
    auto reason=cancellation.Reason();
    if(closed){if(!disabledApplied&&(reason==StopReason::Off||reason==StopReason::Shutdown))Stop(reason,now);socket->Close();return;}
    session.Close(token);closed=true;Stop(reason==StopReason::None?StopReason::TransportLost:reason,now);socket->Close();
}
Received ControlTransport::Receive(Deadline deadline,LocalTimeUs now){
    Owner();try{
        if(closed)throw Error(Failure::Io);
        return Dispatch(socket->Receive(deadline),deadline,now);
    }catch(...){Failed(std::max(now,Now()));throw;}
}
Received ControlTransport::Dispatch(Control::Message message,Deadline deadline,LocalTimeUs now){
    now=std::max(now,Now());auto decision=session.Receive(token,message,now);
    if(decision.code==Control::DecisionCode::StaleCallback)return {std::move(message),decision};
    if(!decision)throw Error(Failure::Protocol);
    Apply(decision,&message,now);if(decision.replay)socket->Send(*decision.replay,deadline);
    return {std::move(message),decision};
}
std::optional<Received> ControlTransport::PollReceive(Deadline deadline,LocalTimeUs now){
    Owner();try{
        if(closed)throw Error(Failure::Io);
        auto message=socket->PollReceive();if(!message)return {};
        return Dispatch(std::move(*message),deadline,now);
    }catch(...){Failed(std::max(now,Now()));throw;}
}
bool ControlTransport::HasPartialFrame() const{Owner();return !closed&&socket->HasPartialFrame();}
Control::Decision ControlTransport::Send(const Control::Message& message,Deadline deadline,LocalTimeUs now){
    Owner();try{
        if(closed)throw Error(Failure::Io);
        auto decision=session.Send(token,message,now);if(!decision)throw Error(Failure::Protocol);
        // Local release/cancellation must precede a potentially stalled terminal write.
        Apply(decision,&message,now,false);socket->Send(message,deadline);if(closed)socket->Close();return decision;
    }catch(...){Failed(std::max(now,Now()));throw;}
}
Control::Decision ControlTransport::Tick(LocalTimeUs now){
    Owner();if(closed){auto reason=cancellation.Reason();if(!disabledApplied&&(reason==StopReason::Off||reason==StopReason::Shutdown)){Stop(reason,now);return {Control::DecisionCode::Accepted,{},{},true,true,true};}return {Control::DecisionCode::StaleCallback};}
    if(cancellation.Reason()!=StopReason::None){Close(cancellation.Reason(),now);return {Control::DecisionCode::Accepted,{},{},true,true,true};}
    auto decision=session.Tick(token,now);Apply(decision,nullptr,now);effects.Tick(now);return decision;
}
void ControlTransport::Close(StopReason reason,LocalTimeUs now){
    Owner();if(closed){if(!disabledApplied&&(reason==StopReason::Off||reason==StopReason::Shutdown))Stop(reason,now);return;}
    session.Close(token);closed=true;Stop(reason,now);socket->Close();
}
Control::ControlSnapshot ControlTransport::Snapshot() const{return session.Snapshot();}
Crypto::Digest ControlTransport::AuthBinding(const Crypto::Digest& hn,const Crypto::Digest& cn) const{Owner();if(closed)throw Error(Failure::Io);return tls.AuthBinding(hn,cn);}
Address ControlTransport::HostAddress() const{return tls.LocalRole()==Control::Role::Host?local:peer;}
Address ControlTransport::PeerAddress() const{return peer;}
bool ControlTransport::WithAuthorizedInput(SessionId id,const std::function<void()>& action){Owner();return session.WithAuthorizedInput(token,id,action);}
std::optional<Control::WorkToken> ControlTransport::BeginChannel(Control::Channel channel){Owner();return session.BeginChannel(token,channel);}
Control::Decision ControlTransport::CompleteChannel(const Control::WorkToken& work,const Control::VerifiedChannel& evidence){Owner();return session.CompleteChannel(work,evidence);}
Control::Decision ControlTransport::CompleteApplication(const Control::WorkToken& work){Owner();return session.CompleteApplication(work);}
bool ControlTransport::WithAuthorizedStream(SessionId id,Control::Channel stream,std::uint32_t generation,std::uint32_t ssrc,const std::function<void()>& action){Owner();return session.WithAuthorizedStream(token,id,stream,generation,ssrc,action);}
bool ControlTransport::WithAuthorizedHapticOn(SessionId id,std::uint32_t generation,const std::function<void()>& action){Owner();return session.WithAuthorizedHapticOn(token,id,generation,action);}
Control::Decision ControlTransport::CancelWork(const Control::WorkToken& work,LocalTimeUs now){Owner();auto decision=session.CancelWork(work);Apply(decision,nullptr,now);return decision;}
}
