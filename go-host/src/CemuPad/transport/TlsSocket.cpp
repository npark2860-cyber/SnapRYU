#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#endif
#include "TlsSocket.h"
#include "DtlsChannel.h"
#include "CemuPad/core/ControlFramer.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/srtp.h>
#include <srtp2/srtp.h>
#include <algorithm>
#include <deque>
#include <utility>
#include <mutex>

namespace CemuPad::Transport {
namespace {
void Require(bool ok,Failure code=Failure::Io) {if(!ok)throw Error(code);}
#ifdef _WIN32
using Handle=SOCKET;constexpr Handle Invalid=INVALID_SOCKET;
void CloseHandle(Handle h){closesocket(h);}
bool Again(){auto e=WSAGetLastError();return e==WSAEWOULDBLOCK||e==WSAEINPROGRESS;}
struct Network {Network(){WSADATA d{};Require(WSAStartup(MAKEWORD(2,2),&d)==0);}~Network(){WSACleanup();}};
#else
using Handle=int;constexpr Handle Invalid=-1;
void CloseHandle(Handle h){close(h);}
bool Again(){return errno==EWOULDBLOCK||errno==EAGAIN||errno==EINPROGRESS||errno==EINTR;}
struct Network {};
#endif
struct Socket {
    Network network;Handle value=Invalid;
    explicit Socket(int type){value=socket(AF_INET,type,0);Require(value!=Invalid);Nonblock();}
    explicit Socket(Handle handle,bool):value(handle){Require(value!=Invalid);Nonblock();}
    ~Socket(){Close();}
    Socket(const Socket&)=delete;Socket& operator=(const Socket&)=delete;
    void Close() noexcept {if(value!=Invalid){CloseHandle(value);value=Invalid;}}
    void Nonblock(){
#ifdef _WIN32
        u_long one=1;int result=ioctlsocket(value,FIONBIO,&one);
#else
        int flags=fcntl(value,F_GETFL,0);int result=flags<0?-1:fcntl(value,F_SETFL,flags|O_NONBLOCK);
#endif
        if(result!=0){Close();throw Error(Failure::Io);}
    }
};
sockaddr_in Numeric(Address address){
    sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(address.port);
    Require(inet_pton(AF_INET,address.ip.c_str(),&a.sin_addr)==1);return a;
}
Address Inspect(const Socket& socket,bool peer){
    sockaddr_in a{};
#ifdef _WIN32
    int size=sizeof(a);
#else
    socklen_t size=sizeof(a);
#endif
    Require((peer?getpeername(socket.value,reinterpret_cast<sockaddr*>(&a),&size):getsockname(socket.value,reinterpret_cast<sockaddr*>(&a),&size))==0);
    char ip[INET_ADDRSTRLEN]{};Require(a.sin_family==AF_INET&&inet_ntop(AF_INET,&a.sin_addr,ip,sizeof(ip))!=nullptr);
    return {ip,ntohs(a.sin_port)};
}
void Check(const Cancellation& cancel,Deadline end){
    if(cancel.Reason()!=StopReason::None)throw Error(Failure::Cancelled);
    cancel.Pulse();
    if(cancel.Reason()!=StopReason::None)throw Error(Failure::Cancelled);
    if(Clock::now()>=end)throw Error(Failure::Timeout);
}
void Wait(const Socket& socket,bool write,const Cancellation& cancel,Deadline end,const std::function<bool()>& permit={}){
    for(;;){
        Check(cancel,end);if(permit)Require(permit(),Failure::PeerRejected);
        auto micros=std::chrono::duration_cast<std::chrono::microseconds>(end-Clock::now()).count();
#ifdef _WIN32
        fd_set f;FD_ZERO(&f);FD_SET(socket.value,&f);
        timeval timeout{0,static_cast<long>(std::clamp<std::int64_t>(micros,1,20'000))};
        int result=select(static_cast<int>(socket.value+1),write?nullptr:&f,write?&f:nullptr,nullptr,&timeout);
        if(result<0&&WSAGetLastError()==WSAEINTR)continue;
#else
        pollfd descriptor{socket.value,static_cast<short>(write?POLLOUT:POLLIN),0};
        int timeout=static_cast<int>(std::clamp<std::int64_t>((micros+999)/1000,1,20));
        int result=poll(&descriptor,1,timeout);
        if(result<0&&errno==EINTR)continue;
        Require(!(descriptor.revents&POLLNVAL));
#endif
        Require(result>=0);Check(cancel,end);if(permit)Require(permit(),Failure::PeerRejected);if(result>0)return;
    }
}
#ifndef _WIN32
// OpenSSL's ordinary socket BIO uses write(). Keep SIGPIPE handling local to this BIO.
int BioFd(BIO* bio){return static_cast<int>(reinterpret_cast<std::intptr_t>(BIO_get_data(bio))-1);}
int BioWrite(BIO* bio,const char* data,std::size_t size,std::size_t* written){
    *written=0;BIO_clear_retry_flags(bio);auto n=send(BioFd(bio),data,size,MSG_NOSIGNAL);
    if(n>0){*written=static_cast<std::size_t>(n);return 1;}if(n<0&&Again())BIO_set_retry_write(bio);return 0;
}
int BioRead(BIO* bio,char* data,std::size_t size,std::size_t* read){
    *read=0;BIO_clear_retry_flags(bio);auto n=recv(BioFd(bio),data,size,0);
    if(n>0){*read=static_cast<std::size_t>(n);return 1;}if(n<0&&Again())BIO_set_retry_read(bio);return 0;
}
long BioControl(BIO* bio,int command,long,void* data){
    if(command==BIO_CTRL_FLUSH||command==BIO_CTRL_DUP)return 1;
    if(command==BIO_C_GET_FD){int fd=BioFd(bio);if(data)*static_cast<int*>(data)=fd;return fd;}return 0;
}
BIO* SafeSocketBio(int fd){
    static const auto method=[](){
        std::unique_ptr<BIO_METHOD,decltype(&BIO_meth_free)> result(BIO_meth_new(BIO_TYPE_SOURCE_SINK|BIO_get_new_index(),"CemuPad socket"),BIO_meth_free);
        Require(result&&BIO_meth_set_write_ex(result.get(),BioWrite)==1&&BIO_meth_set_read_ex(result.get(),BioRead)==1
            &&BIO_meth_set_ctrl(result.get(),BioControl)==1&&BIO_meth_set_create(result.get(),[](BIO* b){BIO_set_init(b,1);return 1;})==1
            &&BIO_meth_set_destroy(result.get(),[](BIO* b){BIO_set_data(b,nullptr);BIO_set_init(b,0);return 1;})==1);
        return result;
    }();
    BIO* bio=BIO_new(method.get());Require(bio!=nullptr);BIO_set_data(bio,reinterpret_cast<void*>(static_cast<std::intptr_t>(fd)+1));return bio;
}
#endif
void Bind(Socket& socket,Address address){auto a=Numeric(std::move(address));Require(bind(socket.value,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0);}
}
LocalTimeUs Now(){return static_cast<LocalTimeUs>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());}
void Cancellation::Request(StopReason value){
    if(value!=StopReason::Off&&value!=StopReason::Shutdown&&value!=StopReason::TransportLost&&value!=StopReason::Error)
        throw std::invalid_argument("Invalid transport cancellation reason");
    auto priority=[](StopReason r){return r==StopReason::Shutdown?3:r==StopReason::Off?2:r==StopReason::None?0:1;};
    auto previous=reason.load();while(priority(value)>priority(previous)&&!reason.compare_exchange_weak(previous,value)){}
}
StopReason Cancellation::Reason() const{return reason.load();}
void Cancellation::SetOwnerPulse(std::function<void()> value){
    if(pulse&&pulseOwner!=std::this_thread::get_id())throw std::logic_error("I/O pulse owner mismatch");
    pulseOwner=std::this_thread::get_id();pulse=std::move(value);
}
void Cancellation::Pulse() const{
    if(!pulse)return;
    if(pulseOwner!=std::this_thread::get_id())throw std::logic_error("I/O pulse outside owner");
    pulse();
}
struct Listener::Impl {Socket socket{SOCK_STREAM};};
Listener::Listener(std::string address,std::uint16_t port):m(std::make_unique<Impl>()){
    Bind(m->socket,{std::move(address),port});Require(listen(m->socket.value,4)==0);
}
Listener::~Listener()=default;
Address Listener::LocalAddress() const{return Inspect(m->socket,false);}
struct DatagramReservation::Impl {Socket socket{SOCK_DGRAM};};
DatagramReservation::DatagramReservation(std::string address):m(std::make_unique<Impl>()){Bind(m->socket,{std::move(address),0});}
DatagramReservation::~DatagramReservation()=default;
Control::Endpoint DatagramReservation::Endpoint(Control::Channel channel,const Crypto::Digest& pin) const{
    auto address=Inspect(m->socket,false);return {channel,address.ip,address.port,Control::Hex(pin)};
}
struct TlsSocket::Impl {
    std::unique_ptr<Socket> socket;
    Store::Identity identity;
    std::optional<Crypto::Digest> expected;
    const Cancellation& cancel;
    bool host=false,rejected=false,closed=false,pairCommitted=false;
    std::function<bool()> permit;
    std::unique_ptr<SSL_CTX,decltype(&SSL_CTX_free)> context{nullptr,SSL_CTX_free};
    std::unique_ptr<SSL,decltype(&SSL_free)> ssl{nullptr,SSL_free};
    ControlFramer framer;std::deque<Control::Message> received;
    std::array<std::uint8_t,16384> readBuffer{};
    void Feed(std::size_t size){
        Require(size>0&&size<=readBuffer.size());
        auto error=framer.Feed(std::span(readBuffer).first(size),[&](std::string_view body){
            auto decoded=Control::Decode(body);if(!decoded||received.size()>=16)return false;
            received.push_back(*decoded.message);return true;
        });Require(error==FramingError::None,Failure::Protocol);
    }
    Impl(std::unique_ptr<Socket> s,const Store::Identity& id,std::optional<Crypto::Digest> pin,const Cancellation& stop,bool server)
        :socket(std::move(s)),identity(id),expected(pin),cancel(stop),host(server){}
    static int Verify(int preverified,X509_STORE_CTX* store){
        auto* ssl=static_cast<SSL*>(X509_STORE_CTX_get_ex_data(store,SSL_get_ex_data_X509_STORE_CTX_idx()));
        auto* self=static_cast<Impl*>(SSL_get_app_data(ssl));bool valid=false;
        try{
            auto* cert=X509_STORE_CTX_get_current_cert(store);int error=X509_STORE_CTX_get_error(store);
            valid=self&&cert&&X509_STORE_CTX_get_error_depth(store)==0&&(preverified||error==X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT)
                &&X509_NAME_cmp(X509_get_subject_name(cert),X509_get_issuer_name(cert))==0
                &&Crypto::IsP256(X509_get0_pubkey(cert))&&X509_verify(cert,X509_get0_pubkey(cert))==1
                &&X509_cmp_current_time(X509_get0_notBefore(cert))<0&&X509_cmp_current_time(X509_get0_notAfter(cert))>0
                &&(self->expected?Crypto::SpkiFingerprint(X509_get0_pubkey(cert))==*self->expected:
                    self->host&&self->permit&&self->permit());
        }catch(...){valid=false;}
        if(self&&!valid)self->rejected=true;return valid?1:0;
    }
    void Retry(int result,Deadline end,Failure fallback){
        int error=SSL_get_error(ssl.get(),result);
        if(error==SSL_ERROR_WANT_READ||error==SSL_ERROR_WANT_WRITE){Wait(*socket,error==SSL_ERROR_WANT_WRITE,cancel,end,host&&!expected&&!pairCommitted?permit:std::function<bool()>{});return;}
        throw Error(rejected?Failure::PeerRejected:error==SSL_ERROR_ZERO_RETURN?Failure::Eof:fallback);
    }
    void Handshake(Deadline end){
        Check(cancel,end);context.reset(SSL_CTX_new(TLS_method()));Require(bool(context),Failure::Handshake);
        Require(SSL_CTX_set_min_proto_version(context.get(),TLS1_3_VERSION)==1&&SSL_CTX_set_max_proto_version(context.get(),TLS1_3_VERSION)==1
            &&SSL_CTX_set_ciphersuites(context.get(),"TLS_AES_128_GCM_SHA256")==1
            &&SSL_CTX_use_certificate(context.get(),identity.Certificate())==1&&SSL_CTX_use_PrivateKey(context.get(),identity.Key())==1
            &&SSL_CTX_check_private_key(context.get())==1,Failure::Handshake);
        SSL_CTX_set_verify(context.get(),SSL_VERIFY_PEER|SSL_VERIFY_FAIL_IF_NO_PEER_CERT,Verify);
        SSL_CTX_set_verify_depth(context.get(),0);SSL_CTX_set_session_cache_mode(context.get(),SSL_SESS_CACHE_OFF);
        SSL_CTX_set_options(context.get(),SSL_OP_NO_TICKET|SSL_OP_NO_RENEGOTIATION);
        Require(SSL_CTX_set_num_tickets(context.get(),0)==1,Failure::Handshake);
        ssl.reset(SSL_new(context.get()));Require(bool(ssl),Failure::Handshake);SSL_set_app_data(ssl.get(),this);
#ifdef _WIN32
        Require(SSL_set_fd(ssl.get(),static_cast<int>(socket->value))==1,Failure::Handshake);
#else
        BIO* bio=SafeSocketBio(socket->value);SSL_set_bio(ssl.get(),bio,bio);
#endif
        SSL_set_mode(ssl.get(),SSL_MODE_ENABLE_PARTIAL_WRITE|SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        if(host)SSL_set_accept_state(ssl.get());else SSL_set_connect_state(ssl.get());
        while(!SSL_is_init_finished(ssl.get())){
            Check(cancel,end);if(host&&!expected)Require(permit&&permit(),Failure::PeerRejected);
            ERR_clear_error();int result=SSL_do_handshake(ssl.get());if(result!=1)Retry(result,end,Failure::Handshake);
        }
        // TLS 1.3 server may finish before learning that a client rejected its certificate.
        if(expected)Require(Control::VerifiedTls::FromSsl(ssl.get(),host?Control::Role::Host:Control::Role::Client,*expected).has_value(),Failure::PeerRejected);
    }
    void Open(Deadline end) const{Require(!closed&&socket&&socket->value!=Invalid,Failure::Io);Check(cancel,end);
        if(host&&!expected&&!pairCommitted)Require(permit&&permit(),Failure::PeerRejected);}
};
TlsSocket::TlsSocket(std::unique_ptr<Impl> impl):m(std::move(impl)){}
TlsSocket::~TlsSocket(){Close();}
std::unique_ptr<TlsSocket> TlsSocket::Connect(const Store::Identity& identity,Address address,const Crypto::Digest& pin,const Cancellation& stop,Deadline end){
    auto socket=std::make_unique<Socket>(SOCK_STREAM);auto a=Numeric(std::move(address));Check(stop,end);
    if(connect(socket->value,reinterpret_cast<sockaddr*>(&a),sizeof(a))!=0){
        Require(Again());for(;;){Wait(*socket,true,stop,end);int error=0;
#ifdef _WIN32
            int size=sizeof(error);
#else
            socklen_t size=sizeof(error);
#endif
            Require(getsockopt(socket->value,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&size)==0);
            Require(error==0);break;
        }
    }
    auto impl=std::make_unique<Impl>(std::move(socket),identity,pin,stop,false);impl->Handshake(end);
    return std::unique_ptr<TlsSocket>(new TlsSocket(std::move(impl)));
}
std::unique_ptr<TlsSocket> TlsSocket::AcceptPinned(Listener& listener,const Store::Identity& identity,const Crypto::Digest& pin,const Cancellation& stop,Deadline end){
    std::unique_ptr<Socket> socket;
    for(;;){Check(stop,end);auto h=accept(listener.m->socket.value,nullptr,nullptr);if(h!=Invalid){socket=std::make_unique<Socket>(h,true);break;}Require(Again());Wait(listener.m->socket,false,stop,end);}
    auto impl=std::make_unique<Impl>(std::move(socket),identity,pin,stop,true);impl->Handshake(end);
    return std::unique_ptr<TlsSocket>(new TlsSocket(std::move(impl)));
}
std::unique_ptr<TlsSocket> TlsSocket::AcceptProvisional(Listener& listener,const Store::Identity& identity,const Cancellation& stop,Deadline end,std::function<bool()> permit){
    Require(permit&&permit(),Failure::PeerRejected);
    std::unique_ptr<Socket> socket;
    for(;;){Check(stop,end);Require(permit(),Failure::PeerRejected);auto h=accept(listener.m->socket.value,nullptr,nullptr);if(h!=Invalid){socket=std::make_unique<Socket>(h,true);break;}Require(Again());Wait(listener.m->socket,false,stop,end,permit);}
    auto impl=std::make_unique<Impl>(std::move(socket),identity,std::nullopt,stop,true);impl->permit=std::move(permit);
    Require(impl->permit(),Failure::PeerRejected);impl->Handshake(end);
    return std::unique_ptr<TlsSocket>(new TlsSocket(std::move(impl)));
}
Address TlsSocket::LocalAddress() const{return Inspect(*m->socket,false);}
Address TlsSocket::PeerAddress() const{return Inspect(*m->socket,true);}
Crypto::Digest TlsSocket::PeerPin() const{
    Require(!m->closed,Failure::Io);std::unique_ptr<X509,decltype(&X509_free)> cert(SSL_get1_peer_certificate(m->ssl.get()),X509_free);
    Require(bool(cert),Failure::PeerRejected);return Crypto::SpkiFingerprint(X509_get0_pubkey(cert.get()));
}
std::vector<std::uint8_t> TlsSocket::PeerCertificateDer() const{
    Require(!m->closed&&SSL_is_init_finished(m->ssl.get()),Failure::Io);
    std::unique_ptr<X509,decltype(&X509_free)> cert(SSL_get1_peer_certificate(m->ssl.get()),X509_free);
    Require(bool(cert),Failure::PeerRejected);int size=i2d_X509(cert.get(),nullptr);Require(size>0&&size<=12288,Failure::PeerRejected);
    std::vector<std::uint8_t> result(static_cast<std::size_t>(size));auto* cursor=result.data();
    Require(i2d_X509(cert.get(),&cursor)==size,Failure::PeerRejected);return result;
}
std::optional<Control::VerifiedTls> TlsSocket::Verified() const{
    if(m->closed||!m->expected)return {};
    return Control::VerifiedTls::FromSsl(m->ssl.get(),m->host?Control::Role::Host:Control::Role::Client,*m->expected);
}
void TlsSocket::Write(std::span<const std::uint8_t> bytes,Deadline end){
    try{
        m->Open(end);while(!bytes.empty()){
            m->Open(end);std::size_t size=0;ERR_clear_error();int result=SSL_write_ex(m->ssl.get(),bytes.data(),bytes.size(),&size);
            if(result==1){Require(size>0&&size<=bytes.size());bytes=bytes.subspan(size);}else m->Retry(result,end,Failure::Io);
        }
    }catch(...){Close();throw;}
}
void TlsSocket::Send(const Control::Message& message,Deadline end){auto frame=Control::Frame(message);Write(frame,end);}
Control::Message TlsSocket::Receive(Deadline end){
    try{
        m->Open(end);while(m->received.empty()){
            m->Open(end);std::size_t size=0;ERR_clear_error();
            int result=SSL_read_ex(m->ssl.get(),m->readBuffer.data(),m->readBuffer.size(),&size);
            if(result!=1){m->Retry(result,end,Failure::Io);continue;}
            m->Feed(size);
        }
        auto result=m->received.front();m->received.pop_front();return result;
    }catch(...){Close();throw;}
}
std::optional<Control::Message> TlsSocket::PollReceive(){
    try{
        m->Open(Deadline::max());
        if(m->received.empty()){
            std::size_t size=0;ERR_clear_error();int result=SSL_read_ex(m->ssl.get(),m->readBuffer.data(),m->readBuffer.size(),&size);
            if(result!=1){
                int error=SSL_get_error(m->ssl.get(),result);
                if(error==SSL_ERROR_WANT_READ||error==SSL_ERROR_WANT_WRITE)return {};
                throw Error(m->rejected?Failure::PeerRejected:error==SSL_ERROR_ZERO_RETURN?Failure::Eof:Failure::Io);
            }
            m->Feed(size);
        }
        if(m->received.empty())return {};
        auto result=m->received.front();m->received.pop_front();return result;
    }catch(...){Close();throw;}
}
bool TlsSocket::HasPartialFrame() const{return !m->closed&&m->framer.HasPartialFrame();}
Control::PairOutcome TlsSocket::SubmitPair(Control::PairingAuthority& authority,std::uint64_t generation,const Control::Message& ch,const Control::Message& hh,const Control::Message& pair,const Control::PairingAuthority::Commit& commit){
    Require(!m->closed&&m->host&&!m->expected,Failure::Protocol);
    m->Open(Clock::now()+std::chrono::seconds(1));
    auto result=authority.Submit(generation,m->ssl.get(),ch,hh,pair,commit);
    m->pairCommitted=result==Control::PairOutcome::Paired;return result;
}
Crypto::Digest TlsSocket::PairExporter(const Crypto::Digest& hn,const Crypto::Digest& cn) const{
    Require(!m->closed,Failure::Io);std::array<std::uint8_t,64> context{};
    std::copy(hn.begin(),hn.end(),context.begin());std::copy(cn.begin(),cn.end(),context.begin()+32);
    Crypto::Digest value{};constexpr std::string_view label="EXPORTER-CemuPad-Pair-v1";
    Require(SSL_export_keying_material(m->ssl.get(),value.data(),value.size(),label.data(),label.size(),context.data(),context.size(),1)==1,Failure::Handshake);return value;
}
void TlsSocket::Close() noexcept{
    if(!m||m->closed)return;m->closed=true;m->received.clear();m->framer.Reset();
    m->ssl.reset();m->context.reset();m->socket->Close();
}
struct DtlsChannel::Impl {
    Socket socket{SOCK_DGRAM};Store::Identity identity;Crypto::Digest expected;
    Control::Role role;Control::Channel kind;const Cancellation& cancel;
    std::thread::id owner=std::this_thread::get_id();
    std::unique_ptr<SSL_CTX,decltype(&SSL_CTX_free)> context{nullptr,SSL_CTX_free};
    std::unique_ptr<SSL,decltype(&SSL_free)> ssl{nullptr,SSL_free};
    std::unique_ptr<BIO,decltype(&BIO_free_all)> wire{nullptr,BIO_free_all};
    srtp_t outgoing=nullptr,incoming=nullptr;bool rejected=false,verified=false;
    std::deque<std::vector<std::uint8_t>> application;
    Impl(const Store::Identity& id,Crypto::Digest pin,Control::Role r,Control::Channel k,std::string address,const Cancellation& stop)
        :identity(id),expected(pin),role(r),kind(k),cancel(stop){
        Bind(socket,{std::move(address),0});
        int bufferBytes=1024*1024;
        Require(setsockopt(socket.value,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<const char*>(&bufferBytes),sizeof(bufferBytes))==0
            &&setsockopt(socket.value,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<const char*>(&bufferBytes),sizeof(bufferBytes))==0,Failure::Io);
        context.reset(SSL_CTX_new(DTLS_method()));Require(bool(context),Failure::Handshake);
        Require(SSL_CTX_set_min_proto_version(context.get(),DTLS1_2_VERSION)==1
            &&SSL_CTX_set_max_proto_version(context.get(),DTLS1_2_VERSION)==1
            &&SSL_CTX_set_cipher_list(context.get(),"ECDHE-ECDSA-AES128-GCM-SHA256")==1
            &&SSL_CTX_use_certificate(context.get(),identity.Certificate())==1
            &&SSL_CTX_use_PrivateKey(context.get(),identity.Key())==1
            &&SSL_CTX_check_private_key(context.get())==1,Failure::Handshake);
        SSL_CTX_set_verify(context.get(),SSL_VERIFY_PEER|SSL_VERIFY_FAIL_IF_NO_PEER_CERT,Verify);
        SSL_CTX_set_verify_depth(context.get(),0);SSL_CTX_set_session_cache_mode(context.get(),SSL_SESS_CACHE_OFF);
        SSL_CTX_set_options(context.get(),SSL_OP_NO_TICKET|SSL_OP_NO_RENEGOTIATION);
        if(kind!=Control::Channel::InputHaptics)Require(SSL_CTX_set_tlsext_use_srtp(context.get(),"SRTP_AEAD_AES_128_GCM")==0,Failure::Handshake);
        ssl.reset(SSL_new(context.get()));Require(bool(ssl),Failure::Handshake);SSL_set_app_data(ssl.get(),this);
        BIO* inside=nullptr;BIO* outside=nullptr;
        Require(BIO_new_bio_dgram_pair(&inside,65536,&outside,65536)==1,Failure::Handshake);
        wire.reset(outside);SSL_set_bio(ssl.get(),inside,inside);
        SSL_set_options(ssl.get(),SSL_OP_NO_QUERY_MTU);SSL_set_mtu(ssl.get(),1200);
        if(role==Control::Role::Host)SSL_set_accept_state(ssl.get());else SSL_set_connect_state(ssl.get());
    }
    ~Impl(){if(outgoing)srtp_dealloc(outgoing);if(incoming)srtp_dealloc(incoming);}
    void Owner() const{Require(std::this_thread::get_id()==owner,Failure::Protocol);}
    static int Verify(int preverified,X509_STORE_CTX* store){
        auto* ssl=static_cast<SSL*>(X509_STORE_CTX_get_ex_data(store,SSL_get_ex_data_X509_STORE_CTX_idx()));
        auto* self=static_cast<Impl*>(SSL_get_app_data(ssl));bool good=false;
        try{
            auto* cert=X509_STORE_CTX_get_current_cert(store);int error=X509_STORE_CTX_get_error(store);
            good=self&&cert&&X509_STORE_CTX_get_error_depth(store)==0&&(preverified||error==X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT)
                &&X509_NAME_cmp(X509_get_subject_name(cert),X509_get_issuer_name(cert))==0
                &&Crypto::IsP256(X509_get0_pubkey(cert))&&X509_verify(cert,X509_get0_pubkey(cert))==1
                &&X509_cmp_current_time(X509_get0_notBefore(cert))<0&&X509_cmp_current_time(X509_get0_notAfter(cert))>0
                &&Crypto::SpkiFingerprint(X509_get0_pubkey(cert))==self->expected;
        }catch(...){good=false;}
        if(self&&!good)self->rejected=true;return good?1:0;
    }
    void Result(int result){if(result==1)return;int error=SSL_get_error(ssl.get(),result);
        if(error!=SSL_ERROR_WANT_READ&&error!=SSL_ERROR_WANT_WRITE)throw Error(rejected?Failure::PeerRejected:Failure::Handshake);}
    bool Send(std::span<const std::uint8_t> bytes){
        Require(!bytes.empty()&&bytes.size()<=1200,Failure::Protocol);
        auto n=send(socket.value,reinterpret_cast<const char*>(bytes.data()),static_cast<int>(bytes.size()),0);
        if(n<0){Require(Again());return false;}Require(static_cast<std::size_t>(n)==bytes.size());return true;
    }
    void Flush(){
        while(BIO_ctrl_pending(wire.get())>0){std::array<std::uint8_t,1500> bytes{};
            int size=BIO_read(wire.get(),bytes.data(),static_cast<int>(bytes.size()));Require(size>0&&size<=1200,Failure::Handshake);
            // Handshake timers retransmit a flight if a nonblocking UDP send drops it.
            Send(std::span(bytes).first(static_cast<std::size_t>(size)));
        }
    }
    std::optional<std::vector<std::uint8_t>> Read(){
        std::array<std::uint8_t,65536> bytes{};auto size=recv(socket.value,reinterpret_cast<char*>(bytes.data()),static_cast<int>(bytes.size()),0);
        if(size<0){Require(Again());return {};}
        if(size<=0||size>1200)return std::vector<std::uint8_t>{};
        return std::vector<std::uint8_t>(bytes.begin(),bytes.begin()+size);
    }
    void Feed(std::span<const std::uint8_t> bytes){Require(!bytes.empty()&&BIO_write(wire.get(),bytes.data(),static_cast<int>(bytes.size()))==static_cast<int>(bytes.size()),Failure::Handshake);}
    void Control(std::span<const std::uint8_t> bytes){
        Feed(bytes);std::array<std::uint8_t,1200> output{};std::size_t size=0;ERR_clear_error();int result=SSL_read_ex(ssl.get(),output.data(),output.size(),&size);
        if(result==1){
            Require(kind==Control::Channel::InputHaptics&&size>0&&size<=1200&&application.size()<64,Failure::Protocol);
            application.emplace_back(output.begin(),output.begin()+size);
        }else Result(result);
        Require(SSL_handle_events(ssl.get())==1,Failure::Handshake);Flush();ERR_clear_error();
    }
};
DtlsChannel::DtlsChannel(const Store::Identity& id,Crypto::Digest pin,Control::Role role,Control::Channel kind,std::string address,const Cancellation& stop)
    :m(std::make_unique<Impl>(id,pin,role,kind,std::move(address),stop)){}
DtlsChannel::~DtlsChannel()=default;
Control::Endpoint DtlsChannel::Endpoint() const{
    m->Owner();auto address=Inspect(m->socket,false);return {m->kind,address.ip,address.port,Control::Hex(m->identity.Pin())};
}
Control::VerifiedChannel DtlsChannel::Handshake(SessionId session,const Control::Endpoint& endpoint,Address peer,Deadline end,const std::function<void()>& progress){
    m->Owner();Require(!m->verified&&endpoint.channel==m->kind,Failure::Protocol);
    if(m->role==Control::Role::Client){
        Require(endpoint.address==peer.ip,Failure::PeerRejected);auto target=Numeric({endpoint.address,endpoint.port});
        Require(connect(m->socket.value,reinterpret_cast<sockaddr*>(&target),sizeof(target))==0||Again());
    }else{
        Require(Endpoint().port==endpoint.port&&endpoint.address==Inspect(m->socket,false).ip,Failure::Protocol);
        for(;;){
            Check(m->cancel,end);if(progress)progress();sockaddr_in source{};
#ifdef _WIN32
            int size=sizeof(source);
#else
            socklen_t size=sizeof(source);
#endif
            std::array<std::uint8_t,65536> bytes{};
            auto n=recvfrom(m->socket.value,reinterpret_cast<char*>(bytes.data()),static_cast<int>(bytes.size()),MSG_PEEK,reinterpret_cast<sockaddr*>(&source),&size);
            if(n<0){Require(Again());Wait(m->socket,false,m->cancel,end);continue;}
            auto expected=Numeric({peer.ip,0});
            if(n<=0||n>1200||source.sin_addr.s_addr!=expected.sin_addr.s_addr){
                recvfrom(m->socket.value,reinterpret_cast<char*>(bytes.data()),static_cast<int>(bytes.size()),0,nullptr,nullptr);continue;
            }
            Require(connect(m->socket.value,reinterpret_cast<sockaddr*>(&source),sizeof(source))==0||Again());break;
        }
    }
    for(;;){
        Check(m->cancel,end);if(progress)progress();ERR_clear_error();int result=SSL_do_handshake(m->ssl.get());m->Flush();
        if(result==1)break;m->Result(result);
        auto packet=m->Read();if(packet&&!packet->empty())m->Feed(*packet);
        if(!packet)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        Require(SSL_handle_events(m->ssl.get())==1,Failure::Handshake);m->Flush();
    }
    auto evidence=Control::VerifiedChannel::FromSsl(m->ssl.get(),m->role,session,m->kind,m->expected,endpoint);
    Require(evidence.has_value(),Failure::PeerRejected);m->verified=true;return *evidence;
}
void DtlsChannel::SetSsrc(std::uint32_t ssrc){
    m->Owner();Require(m->verified&&m->kind!=Control::Channel::InputHaptics&&ssrc!=0&&!m->outgoing&&!m->incoming,Failure::Protocol);
    static std::once_flag once;static bool initialized=false;std::call_once(once,[]{initialized=srtp_init()==srtp_err_status_ok;});Require(initialized,Failure::Handshake);
    std::array<std::uint8_t,56> material{};
    struct Wipe{std::array<std::uint8_t,56>& value;~Wipe(){OPENSSL_cleanse(value.data(),value.size());}} wipe{material};
    constexpr char label[]="EXTRACTOR-dtls_srtp";
    Require(SSL_export_keying_material(m->ssl.get(),material.data(),material.size(),label,sizeof(label)-1,nullptr,0,0)==1,Failure::Handshake);
    auto create=[&](srtp_t* context,bool client){
        std::array<std::uint8_t,28> key{};
        std::copy_n(material.begin()+(client?0:16),16,key.begin());std::copy_n(material.begin()+(client?32:44),12,key.begin()+16);
        srtp_policy_t policy{};srtp_crypto_policy_set_aes_gcm_128_16_auth(&policy.rtp);srtp_crypto_policy_set_aes_gcm_128_16_auth(&policy.rtcp);
        policy.key=key.data();policy.ssrc.type=ssrc_specific;policy.ssrc.value=ssrc;policy.window_size=128;
        auto result=srtp_create(context,&policy);OPENSSL_cleanse(key.data(),key.size());Require(result==srtp_err_status_ok,Failure::Handshake);
    };
    bool client=m->role==Control::Role::Client;create(&m->outgoing,client);create(&m->incoming,!client);
}
void DtlsChannel::Pump(){
    m->Owner();Require(m->verified,Failure::Protocol);Check(m->cancel,Clock::now()+std::chrono::seconds(1));
    for(unsigned i=0;i<8;++i){auto packet=m->Read();if(!packet)break;if(!packet->empty()&&packet->front()<128)m->Control(*packet);}
    Require(SSL_handle_events(m->ssl.get())==1,Failure::Handshake);m->Flush();
}
bool DtlsChannel::SendRtp(std::span<const std::uint8_t> plain){
    m->Owner();Require(m->verified&&m->outgoing&&plain.size()>=12&&plain.size()<=1184,Failure::Protocol);
    std::vector<std::uint8_t> packet(plain.begin(),plain.end());packet.resize(packet.size()+SRTP_MAX_TRAILER_LEN);
    int size=static_cast<int>(plain.size());Require(srtp_protect(m->outgoing,packet.data(),&size)==srtp_err_status_ok,Failure::Protocol);
    return m->Send(std::span(packet).first(static_cast<std::size_t>(size)));
}
std::optional<std::vector<std::uint8_t>> DtlsChannel::ReceiveRtp(){
    m->Owner();Require(m->verified&&m->incoming,Failure::Protocol);
    for(unsigned i=0;i<32;++i){
        auto packet=m->Read();if(!packet)return {};if(packet->empty())continue;
        if(packet->front()<128){m->Control(*packet);continue;}
        if(packet->size()<28||packet->front()!=0x80||((*packet)[1]>=192&&(*packet)[1]<=223))continue;
        int size=static_cast<int>(packet->size());if(srtp_unprotect(m->incoming,packet->data(),&size)!=srtp_err_status_ok)continue;
        packet->resize(static_cast<std::size_t>(size));return packet;
    }return {};
}

bool DtlsChannel::SendData(std::span<const std::uint8_t> plain){
    m->Owner();Require(m->verified&&m->kind==Control::Channel::InputHaptics&&!plain.empty()&&plain.size()<=1080,Failure::Protocol);
    std::size_t size=0;ERR_clear_error();int result=SSL_write_ex(m->ssl.get(),plain.data(),plain.size(),&size);
    Require(result==1&&size==plain.size(),Failure::Io);m->Flush();return true;
}
std::optional<std::vector<std::uint8_t>> DtlsChannel::ReceiveData(){
    m->Owner();Require(m->verified&&m->kind==Control::Channel::InputHaptics,Failure::Protocol);
    Pump();if(m->application.empty())return {};auto bytes=std::move(m->application.front());m->application.pop_front();return bytes;
}

}
