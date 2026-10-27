#include "ControlSession.h"
#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>

namespace CemuPad::Control {
namespace {
unsigned Index(Role r) { return r==Role::Host?0u:1u; }
std::atomic<ConnectionToken> nextConnection{0};
ConnectionToken NewConnection() {
    auto previous=nextConnection.load();
    for(;;) {
        if(previous==std::numeric_limits<ConnectionToken>::max()) throw std::overflow_error("Control connection exhausted");
        if(nextConnection.compare_exchange_weak(previous,previous+1)) return previous+1;
    }
}
unsigned Index(Channel c) { return static_cast<unsigned>(c); }
Channel ChannelOf(std::string_view s) { return s=="input_haptics"?Channel::InputHaptics:s=="video"?Channel::Video:Channel::Audio; }
bool Notification(Type t) { return t==Type::SessionStart||t==Type::ChannelReady||t==Type::Heartbeat||t==Type::KeyframeRequest||t==Type::SessionStop||t==Type::VideoPreferences; }
bool Request(Role sender,const Message& v) {
    if(v.Kind()==Type::StreamConfig) return sender==Role::Host;
    if(v.Kind()==Type::TimeSync) return sender==Role::Client&&v.Text("phase")=="request";
    return sender==Role::Client&&(v.Kind()==Type::Hello||v.Kind()==Type::Capabilities||v.Kind()==Type::Pair||v.Kind()==Type::Auth||v.Kind()==Type::HapticConfig);
}
Type Response(Type t) {
    switch(t) {
    case Type::Hello:return Type::Hello; case Type::Capabilities:return Type::Capabilities; case Type::Pair:return Type::PairResult;
    case Type::Auth:return Type::AuthResult; case Type::HapticConfig:return Type::HapticReady; case Type::StreamConfig:return Type::StreamReady;
    case Type::TimeSync:return Type::TimeSync; default:throw std::logic_error("Not a control request");
    }
}
bool SameSession(const Message& v,SessionId session) { return !v.Has("session_id")||Session(v.Text("session_id"))==session; }
struct Key { Role initiator; std::uint64_t id; bool operator==(const Key&) const=default;
    bool operator<(const Key& r) const { return std::pair(Index(initiator),id)<std::pair(Index(r.initiator),r.id); } };
struct Transaction {
    Message request; std::optional<Message> response,complete; LocalTimeUs started=0;
    bool expired=false,cancelled=false; std::optional<WorkToken> work{};
};
struct ChannelState { std::optional<Endpoint> endpoint; std::optional<WorkToken> work; std::optional<Crypto::Digest> evidence,peerReady;
    bool localReady=false,peerMatched=false; };
struct AppliedState { std::optional<WorkToken> work; std::optional<Message> configuration; bool applied=false,ready=false; };
Decision Accepted() { return {DecisionCode::Accepted}; }
}
struct ControlSession::Impl {
    mutable std::mutex mutex; ControlLimits limits; ControlSnapshot state;
    std::optional<VerifiedTls> tls; Role local=Role::Host; SessionId hostId{}; std::string address;
    std::uint64_t operation=0; LocalTimeUs lastNow=0;
    std::map<Key,Transaction> transactions; std::array<std::size_t,2> transactionCount{};
    std::optional<Message> clientHello,hostHello,clientCaps,hostCaps;
    bool authRequested=false; std::optional<Key> sync;
    std::array<ChannelState,3> channels; std::array<AppliedState,2> streams; AppliedState haptic;
    std::array<std::optional<std::uint32_t>,2> lastStreamGeneration;
    std::optional<std::uint32_t> lastOutputGeneration;
    std::vector<SessionId> sessions;
    std::vector<std::uint32_t> ssrcs; std::vector<LocalTimeUs> keyframeTimes;
    explicit Impl(ControlLimits l):limits(l) { if(!l.transactionsPerInitiator||!l.cachedBytes) throw std::invalid_argument("Empty control budget"); }
    WorkToken Work(WorkKind kind,unsigned slot) {
        if(operation==std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Control operation exhausted");
        return {state.connection,state.session,++operation,kind,slot};
    }
    void ClearData() {
        state.session={}; state.dataReady={}; state.streamReady={}; state.hapticReady=false; channels={}; streams={}; haptic={}; lastStreamGeneration={}; lastOutputGeneration={}; keyframeTimes.clear();
        for(auto& [k,t]:transactions) { (void)k; if(t.request.Has("session_id")) t.cancelled=true; }
    }
    void Close() {
        ClearData(); state.open=false; state.authenticated=false; state.capabilities=false; state.negotiated={}; state.cachedBytes=0; state.transactions=0; state.requestWatermarks={}; state.timeEstimatesApplied=0;
        transactions.clear(); transactionCount={}; sync.reset(); clientHello.reset(); hostHello.reset(); clientCaps.reset(); hostCaps.reset(); authRequested=false; tls.reset(); sessions.clear(); ssrcs.clear(); lastNow=0;
    }
    void Refresh(Channel c) { const auto& v=channels[Index(c)]; state.dataReady[Index(c)]=v.localReady&&v.peerMatched&&v.evidence.has_value(); }
    bool Token(const WorkToken& t) const { return state.open&&t.connection==state.connection&&!IsZero(state.session)&&t.session==state.session; }
    bool Fits(std::size_t bytes) const { return bytes<=limits.cachedBytes-state.cachedBytes; }
    Decision Process(ConnectionToken connection,Role sender,const Message& msg,LocalTimeUs now) {
        if(!state.open||connection!=state.connection) return {DecisionCode::StaleCallback};
        if(now<lastNow) return {};
        if(msg.Has("session_id")&&(IsZero(state.session)||!SameSession(msg,state.session))&&msg.Kind()!=Type::SessionStart) {
            // A peer can have queued a heartbeat/ready before it receives STOP.
            // Retired sessions on this authenticated TLS connection grant no
            // effects, but must not tear down the next game's connection.
            auto retired=Session(msg.Text("session_id"));
            if(sender!=local&&state.authenticated&&std::find(sessions.begin(),sessions.end(),retired)!=sessions.end())return {DecisionCode::StaleCallback};
            return {};
        }
        bool request=Request(sender,msg),notice=Notification(msg.Kind());
        bool complete=msg.Kind()==Type::TimeSync&&msg.Text("phase")=="complete";
        bool error=msg.Kind()==Type::Error;
        if(msg.Kind()==Type::TimeSync&&((msg.Text("phase")=="response")!=(sender==Role::Host))) return {};
        std::optional<Key> key;
        Transaction* existing=nullptr;
        if(!notice&&!(error&&msg.Id()==0)) {
            Role initiator=(request||complete)?sender:Other(sender); key=Key{initiator,msg.Id()};
            auto found=transactions.find(*key); if(found!=transactions.end()) existing=&found->second;
            if(existing) {
                if(existing->expired||existing->cancelled||(existing->request.Kind()==Type::TimeSync&&now-existing->started>=2'000'000)) {
                    // Once the initial clock exchange succeeded, periodic samples
                    // are optional. A DTLS game-start handshake can delay their
                    // replies; discard that sample without retiring the game/TLS.
                    // The existing bounded transaction ledger still authenticates
                    // the exact request and response fields; initial/forged replies
                    // retain the strict failure path.
                    if(!existing->cancelled&&state.timeEstimatesApplied>0&&sender!=local&&!request&&
                       existing->request.Kind()==Type::TimeSync&&msg.Kind()==Type::TimeSync&&
                       msg.Integer("t1")==existing->request.Integer("t1")) {
                        bool matches=complete
                            ?existing->response&&existing->response->Kind()==Type::TimeSync&&
                                msg.Integer("t2")==existing->response->Integer("t2")&&
                                msg.Integer("t3")==existing->response->Integer("t3")&&
                                (!existing->complete||*existing->complete==msg)
                            :(!existing->response||*existing->response==msg);
                        if(matches)return {DecisionCode::StaleCallback};
                    }
                    return {};
                }
                const Message* previous=request?&existing->request:complete?(existing->complete?&*existing->complete:nullptr):(existing->response?&*existing->response:nullptr);
                if(previous) {
                    if(!(*previous==msg)) return {};
                    Decision d{DecisionCode::Duplicate};
                    if(sender!=local&&request&&existing->response) d.replay=existing->response;
                    if(sender!=local&&!request&&!complete&&existing->complete) d.replay=existing->complete;
                    return d;
                }
                if(request) return {};
                if(error) { if(existing->response||complete) return {}; }
                else if(msg.Kind()!=Response(existing->request.Kind())) return {};
                if(complete&&(!existing->response||existing->response->Kind()!=Type::TimeSync||existing->request.Kind()!=Type::TimeSync)) return {};
            } else if(!request||msg.Id()<=state.requestWatermarks[Index(initiator)]) return {};
        }
        // All checks below precede mutations. Effects are committed only after this switch.
        std::optional<Negotiated> negotiated;
        std::vector<Endpoint> endpoints;
        unsigned slot=0; std::optional<Crypto::Digest> readyDigest;
        switch(msg.Kind()) {
        case Type::Hello:
            if(state.authenticated||msg.Text("role")!=(sender==Role::Host?"host":"client")||Session(msg.Text("host_id"))!=hostId) return {};
            if(request) { if(clientHello||hostHello) return {}; }
            else if(sender!=Role::Host||!clientHello||hostHello) return {};
            break;
        case Type::Pair: case Type::PairResult:
            // Provisional PAIR requires PairingAuthority + durable store, never this pinned-reconnect path.
            return {};
        case Type::Auth:
            if(sender!=Role::Client||state.authenticated||authRequested||!clientHello||!hostHello||
               Digest(msg.Text("peer_spki_sha256"))!=tls->ClientPin()||Digest(msg.Text("control_binding"))!=tls->AuthBinding(Digest(hostHello->Text("nonce")),Digest(clientHello->Text("nonce")))) return {};
            break;
        case Type::AuthResult:
            if(sender!=Role::Host||!authRequested||state.authenticated||Digest(msg.Text("client_spki_sha256"))!=tls->ClientPin()||Digest(msg.Text("host_spki_sha256"))!=tls->HostPin()) return {};
            break;
        case Type::Capabilities:
            if(!state.authenticated||state.capabilities) return {};
            if(request) { if(clientCaps||hostCaps) return {}; }
            else {
                if(sender!=Role::Host||!clientCaps||hostCaps) return {};
                const auto& c=*clientCaps;
                Negotiated n; n.inputHz=static_cast<unsigned>(std::min(c.Integer("input_hz"),msg.Integer("input_hz")));
                n.gyroAbsMax=std::min(c.Number("gyro_abs_max"),msg.Number("gyro_abs_max")); n.accelAbsMax=std::min(c.Number("accel_abs_max"),msg.Number("accel_abs_max"));
                n.gyro=c.Flag("gyro")&&msg.Flag("gyro"); n.accel=c.Flag("accel")&&msg.Flag("accel"); n.haptics=c.Flag("haptics")&&msg.Flag("haptics");
                n.video=!c.Strings("video_codecs").empty()&&!msg.Strings("video_codecs").empty(); n.audio=!c.Strings("audio_codecs").empty()&&!msg.Strings("audio_codecs").empty(); negotiated=n;
            }
            break;
        case Type::SessionStart:
            if(sender!=Role::Host||!state.capabilities||!IsZero(state.session)) return {};
            if(std::find(sessions.begin(),sessions.end(),Session(msg.Text("session_id")))!=sessions.end()) return {};
            endpoints=msg.Endpoints();
            for(const auto& e:endpoints) if(e.address!=address||Digest(e.hostPin)!=tls->HostPin()||(e.channel==Channel::Video&&!state.negotiated.video)||(e.channel==Channel::Audio&&!state.negotiated.audio)) return {};
            if(sessions.size()>=4096) { Close(); return {DecisionCode::ReconnectRequired,{},{},true,true,true}; }
            break;
        case Type::ChannelReady: {
            auto c=ChannelOf(msg.Text("channel")); slot=Index(c); auto& channel=channels[slot];
            if(!channel.endpoint||!channel.work) return {};
            readyDigest=Digest(msg.Text("dtls_exporter_sha256"));
            if(sender==local) { if(!channel.evidence||*channel.evidence!=*readyDigest) return {}; }
            else if((channel.peerReady&&*channel.peerReady!=*readyDigest)||(channel.evidence&&*channel.evidence!=*readyDigest)) return {};
            break;
        }
        case Type::StreamConfig: {
            slot=msg.Text("stream")=="video"?0u:1u;
            if(sender!=Role::Host||!state.dataReady[slot+1]||streams[slot].work||
               (lastStreamGeneration[slot]&&msg.Integer("generation")<=*lastStreamGeneration[slot])||
               std::find(ssrcs.begin(),ssrcs.end(),static_cast<std::uint32_t>(msg.Integer("ssrc")))!=ssrcs.end()) return {};
            if(slot==0&&!ValidH264Configuration(msg)) return {};
            break;
        }
        case Type::StreamReady:
            slot=msg.Text("stream")=="video"?0u:1u;
            if(sender!=Role::Client||!existing||!streams[slot].configuration||!streams[slot].work||
               existing->work!=streams[slot].work||msg.Integer("generation")!=streams[slot].configuration->Integer("generation")||msg.Integer("ssrc")!=streams[slot].configuration->Integer("ssrc")||
               (sender==local&&!streams[slot].applied)) return {};
            break;
        case Type::HapticConfig:
            if(sender!=Role::Client||!state.dataReady[0]||!state.negotiated.haptics||haptic.work||
               (lastOutputGeneration&&msg.Integer("output_generation")<=*lastOutputGeneration)) return {};
            break;
        case Type::HapticReady:
            if(sender!=Role::Host||!existing||!haptic.configuration||!haptic.work||existing->work!=haptic.work||msg.Integer("output_generation")!=haptic.configuration->Integer("output_generation")) return {};
            break;
        case Type::VideoPreferences:
            if(sender!=Role::Client||!state.capabilities||!state.negotiated.video||!IsZero(state.session))return {};
            break;
        case Type::Heartbeat:
            if(!state.capabilities) return {};
            break;
        case Type::TimeSync:
            if(!state.authenticated) return {};
            if(request) {
                if(sync) { auto& previous=transactions.at(*sync); if(!previous.complete&&!previous.expired&&!(previous.response&&previous.response->Kind()==Type::Error)&&now-previous.started<2'000'000) return {}; }
            } else {
                if(!existing||existing->request.Kind()!=Type::TimeSync||now-existing->started>=2'000'000||
                   msg.Integer("t1")!=existing->request.Integer("t1")||
                   (!complete&&sender!=Role::Host)||(complete&&sender!=Role::Client)) return {};
                if(complete&&(msg.Integer("t2")!=existing->response->Integer("t2")||msg.Integer("t3")!=existing->response->Integer("t3"))) return {};
            }
            break;
        case Type::KeyframeRequest:
            if(sender!=Role::Client||!streams[0].ready||!streams[0].configuration||msg.Integer("generation")!=streams[0].configuration->Integer("generation")||msg.Integer("ssrc")!=streams[0].configuration->Integer("ssrc")) return {};
            if(std::count_if(keyframeTimes.begin(),keyframeTimes.end(),[&](auto t){return now-t<1'000'000;})>=2) return {};
            break;
        case Type::SessionStop:
            if(!state.authenticated) return {};
            break;
        case Type::Error:
            if(msg.Id()!=0&&!existing) return {};
            break;
        }
        if((request&&transactionCount[Index(sender)]>=limits.transactionsPerInitiator)||
           (!notice&&!Fits(msg.JsonForWire().size()))) { Close(); return {DecisionCode::ReconnectRequired,{},{},true,true,true}; }
        // Commit transaction ledger only after full validation.
        if(key) {
            if(request) {
                auto [i,inserted]=transactions.emplace(*key,Transaction{msg,{},{},now});
                if(!inserted) throw std::logic_error("Control duplicate commit"); existing=&i->second;
                ++transactionCount[Index(key->initiator)]; state.requestWatermarks[Index(key->initiator)]=msg.Id(); state.transactions=transactions.size();
            } else if(complete) existing->complete=msg;
            else existing->response=msg;
            state.cachedBytes+=msg.JsonForWire().size();
        }
        lastNow=now; Decision decision=Accepted();
        switch(msg.Kind()) {
        case Type::Hello: if(sender==Role::Client) clientHello=msg; else hostHello=msg; break;
        case Type::Auth: authRequested=true; break;
        case Type::AuthResult: state.authenticated=true; break;
        case Type::Capabilities: if(sender==Role::Client) clientCaps=msg; else { hostCaps=msg; state.capabilities=true; state.negotiated=*negotiated; } break;
        case Type::SessionStart:
            state.session=Session(msg.Text("session_id")); sessions.push_back(state.session);
            for(auto& e:endpoints) channels[Index(e.channel)].endpoint=std::move(e);
            break;
        case Type::ChannelReady: {
            auto& v=channels[slot]; if(sender==local) v.localReady=true; else { v.peerReady=readyDigest; v.peerMatched=v.evidence&&*v.evidence==*readyDigest; }
            Refresh(static_cast<Channel>(slot)); break;
        }
        case Type::StreamConfig:
            streams[slot].configuration=msg; streams[slot].work=Work(WorkKind::Stream,slot); streams[slot].ready=false; streams[slot].applied=false;
            state.streamReady[slot]=false;
            lastStreamGeneration[slot]=static_cast<std::uint32_t>(msg.Integer("generation")); ssrcs.push_back(static_cast<std::uint32_t>(msg.Integer("ssrc"))); decision.work=streams[slot].work; existing->work=decision.work; break;
        case Type::StreamReady: streams[slot].ready=true; state.streamReady[slot]=true; streams[slot].work.reset(); break;
        case Type::HapticConfig:
            haptic.configuration=msg; haptic.work=Work(WorkKind::Haptic,0); haptic.applied=false; haptic.ready=false;
            state.hapticReady=false;
            lastOutputGeneration=static_cast<std::uint32_t>(msg.Integer("output_generation")); decision.work=haptic.work; decision.outputCancelled=true; existing->work=decision.work; break;
        case Type::HapticReady:
            haptic.ready=true; state.hapticReady=local==Role::Host||haptic.applied;
            if(state.hapticReady) haptic.work.reset(); break;
        case Type::TimeSync:
            if(request) { if(sync) { auto& previous=transactions.at(*sync); if(!previous.complete) previous.expired=true; } sync=key; }
            if(complete) ++state.timeEstimatesApplied;
            break;
        case Type::KeyframeRequest:
            std::erase_if(keyframeTimes,[&](auto t){return now-t>=1'000'000;}); keyframeTimes.push_back(now); break;
        case Type::SessionStop: {
            auto reason=msg.Text("reason"); decision.dataStopped=true; decision.outputCancelled=true;
            if(reason=="game_end"||reason=="replaced") ClearData(); else { Close(); decision.controlClosed=true; } break;
        }
        case Type::Error:
            if(sync&&key&&*sync==*key) sync.reset();
            if(msg.Flag("fatal")) { Close(); decision.dataStopped=true; decision.outputCancelled=true; decision.controlClosed=true; }
            else if(existing&&existing->work) {
                const auto& work=*existing->work;
                if(work.kind==WorkKind::Stream&&streams[work.slot].work==work) { streams[work.slot]={}; state.streamReady[work.slot]=false; }
                else if(work.kind==WorkKind::Haptic&&haptic.work==work) { haptic={}; state.hapticReady=false; decision.outputCancelled=true; }
            }
            break;
        default:break;
        }
        return decision;
    }
};
ControlSession::ControlSession(ControlLimits limits):m(std::make_unique<Impl>(limits)){}
ControlSession::~ControlSession()=default;
ConnectionToken ControlSession::Open(VerifiedTls tls,SessionId hostId,std::string hostAddress) {
    std::lock_guard lock(m->mutex);
    if(IsZero(hostId)||hostAddress.empty()) throw std::invalid_argument("Control open identity");
    auto connection=NewConnection(); m->Close(); m->tls=std::move(tls); m->local=m->tls->LocalRole(); m->hostId=hostId; m->address=std::move(hostAddress); m->state.connection=connection; m->state.open=true; return m->state.connection;
}
Decision ControlSession::Send(ConnectionToken token,const Message& v,LocalTimeUs now) { std::lock_guard lock(m->mutex); return m->Process(token,m->local,v,now); }
Decision ControlSession::Receive(ConnectionToken token,const Message& v,LocalTimeUs now) { std::lock_guard lock(m->mutex); return m->Process(token,Other(m->local),v,now); }
std::optional<WorkToken> ControlSession::BeginChannel(ConnectionToken connection,Channel channel) {
    std::lock_guard lock(m->mutex); unsigned slot=Index(channel);
    if(!m->state.open||m->state.connection!=connection||IsZero(m->state.session)||slot>2||!m->channels[slot].endpoint||m->channels[slot].work) return {};
    m->channels[slot].work=m->Work(WorkKind::Channel,slot); return m->channels[slot].work;
}
Decision ControlSession::CompleteChannel(const WorkToken& token,const VerifiedChannel& evidence) {
    std::lock_guard lock(m->mutex);
    if(!m->Token(token)||token.kind!=WorkKind::Channel||token.slot>2||m->channels[token.slot].work!=token) return {DecisionCode::StaleCallback};
    auto& channel=m->channels[token.slot];
    auto localPin=m->local==Role::Host?m->tls->HostPin():m->tls->ClientPin();
    if(evidence.Session()!=token.session||Index(evidence.Kind())!=token.slot||evidence.PeerPin()!=m->tls->PeerPin()||evidence.LocalPin()!=localPin||evidence.LocalRole()!=m->local||!channel.endpoint||evidence.HostEndpoint()!=*channel.endpoint) return {};
    if(channel.evidence) return {*channel.evidence==evidence.Binding()?DecisionCode::Duplicate:DecisionCode::Rejected};
    if(channel.peerReady&&*channel.peerReady!=evidence.Binding()) return {};
    channel.evidence=evidence.Binding(); channel.peerMatched=channel.peerReady.has_value(); m->Refresh(evidence.Kind()); return Accepted();
}
Decision ControlSession::CompleteApplication(const WorkToken& token) {
    std::lock_guard lock(m->mutex); if(!m->Token(token)) return {DecisionCode::StaleCallback};
    AppliedState* work=token.kind==WorkKind::Stream&&token.slot<2?&m->streams[token.slot]:token.kind==WorkKind::Haptic&&token.slot==0?&m->haptic:nullptr;
    if(!work||work->work!=token) return {DecisionCode::StaleCallback};
    if(work->applied) return {DecisionCode::Duplicate}; work->applied=true;
    if(token.kind==WorkKind::Haptic&&work->ready) { m->state.hapticReady=true; work->work.reset(); }
    return Accepted();
}
Decision ControlSession::CancelWork(const WorkToken& token) {
    std::lock_guard lock(m->mutex); if(!m->Token(token)) return {DecisionCode::StaleCallback};
    if(token.kind==WorkKind::Channel&&token.slot<3) {
        auto& channel=m->channels[token.slot]; if(channel.work!=token) return {DecisionCode::StaleCallback};
        if(token.slot==0) { m->Close(); return {DecisionCode::Accepted,{},{},true,true,true}; }
        auto endpoint=channel.endpoint; channel={}; channel.endpoint=std::move(endpoint); m->state.dataReady[token.slot]=false;
        m->streams[token.slot-1]={}; m->state.streamReady[token.slot-1]=false;
        for(auto& [k,t]:m->transactions) { (void)k; if(t.work&&t.work->kind==WorkKind::Stream&&t.work->slot==token.slot-1) t.cancelled=true; }
    } else {
        AppliedState* work=token.kind==WorkKind::Stream&&token.slot<2?&m->streams[token.slot]:token.kind==WorkKind::Haptic&&token.slot==0?&m->haptic:nullptr;
        if(!work||work->work!=token) return {DecisionCode::StaleCallback}; *work={};
        if(token.kind==WorkKind::Stream) m->state.streamReady[token.slot]=false; else m->state.hapticReady=false;
    }
    for(auto& [k,t]:m->transactions) { (void)k; if(t.work==token) t.cancelled=true; }
    return Accepted();
}
bool ControlSession::WithAuthorizedInput(ConnectionToken token,SessionId session,const std::function<void()>& action) {
    std::lock_guard lock(m->mutex);
    if(!m->state.open||m->state.connection!=token||m->state.session!=session||IsZero(session)||!m->state.dataReady[0]||!action) return false;
    action(); return true;
}
bool ControlSession::WithAuthorizedStream(ConnectionToken token,SessionId session,Channel stream,std::uint32_t generation,std::uint32_t ssrc,const std::function<void()>& action) {
    std::lock_guard lock(m->mutex); unsigned channel=Index(stream);
    if(!m->state.open||m->state.connection!=token||m->state.session!=session||IsZero(session)||channel<1||channel>2||!m->state.dataReady[channel]||!m->state.streamReady[channel-1]||!action) return false;
    const auto& config=m->streams[channel-1].configuration;
    if(!config||config->Integer("generation")!=generation||config->Integer("ssrc")!=ssrc) return false;
    action(); return true;
}
bool ControlSession::WithAuthorizedHapticOn(ConnectionToken token,SessionId session,std::uint32_t generation,const std::function<void()>& action) {
    std::lock_guard lock(m->mutex);
    if(!m->state.open||m->state.connection!=token||m->state.session!=session||IsZero(session)||!m->state.dataReady[0]||!m->state.hapticReady||!m->haptic.configuration||!m->haptic.configuration->Flag("available")||m->haptic.configuration->Integer("output_generation")!=generation||!action) return false;
    action(); return true;
}
Decision ControlSession::Tick(ConnectionToken token,LocalTimeUs now) {
    std::lock_guard lock(m->mutex); if(!m->state.open||token!=m->state.connection) return {DecisionCode::StaleCallback};
    if(now<m->lastNow) return {}; m->lastNow=now;
    if(m->sync) { auto& t=m->transactions.at(*m->sync); if(!t.complete&&now-t.started>=2'000'000) { t.expired=true; m->sync.reset(); } }
    return Accepted();
}
Decision ControlSession::Close(ConnectionToken token) {
    std::lock_guard lock(m->mutex); if(!m->state.open||token!=m->state.connection) return {DecisionCode::StaleCallback}; m->Close(); return {DecisionCode::Accepted,{},{},true,true,true};
}
ControlSnapshot ControlSession::Snapshot() const { std::lock_guard lock(m->mutex); return m->state; }
}
