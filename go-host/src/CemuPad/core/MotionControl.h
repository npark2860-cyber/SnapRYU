#pragma once
#include "Types.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <mutex>

namespace CemuPad {
// Optional authenticated input-channel extension. Legacy peers ignore types 3/4;
// the client probes first and never substitutes a full reset for unsupported recenter.
enum class MotionControlResult : unsigned { Pending, Applied, Unsupported, Stale, NotReady, Supported };
struct MotionControlRequest {
    SessionId session{};std::uint64_t id=0,pressedUs=0,gyroUs=0,accelUs=0;
    std::uint32_t generation=0;unsigned action=0; // 0 probe, 1 recenter, 2 full reset
    std::array<float,3> gyro{},accel{};
    bool operator==(const MotionControlRequest&) const=default;
};
struct MotionControlReply {SessionId session{};std::uint64_t id=0;MotionControlResult result=MotionControlResult::Pending;};
namespace MotionWire {
inline std::uint64_t Read(std::span<const std::uint8_t> b,unsigned p,unsigned n){std::uint64_t v=0;for(unsigned i=0;i<n;i++)v=(v<<8)|b[p+i];return v;}
inline void Write(std::span<std::uint8_t> b,unsigned p,std::uint64_t v,unsigned n){for(unsigned i=0;i<n;i++){b[p+n-i-1]=static_cast<std::uint8_t>(v);v>>=8;}}
inline void Header(std::span<std::uint8_t> b,unsigned type,const SessionId& session,std::uint64_t id,std::uint64_t time){Write(b,0,1,2);Write(b,2,type,2);Write(b,4,b.size()-40,2);std::copy(session.begin(),session.end(),b.begin()+8);Write(b,24,id,8);Write(b,32,time,8);}
inline bool HeaderValid(std::span<const std::uint8_t> b,unsigned type,unsigned size){return b.size()==size&&Read(b,0,2)==1&&Read(b,2,2)==type&&Read(b,4,2)==size-40&&Read(b,6,2)==0&&Read(b,24,8)!=0&&std::any_of(b.begin()+8,b.begin()+24,[](auto v){return v!=0;});}
inline std::array<std::uint8_t,88> Encode(const MotionControlRequest& q){
    std::array<std::uint8_t,88> b{};Header(b,3,q.session,q.id,q.pressedUs);b[40]=static_cast<std::uint8_t>(q.action);Write(b,44,q.generation,4);Write(b,48,q.gyroUs,8);Write(b,56,q.accelUs,8);
    for(unsigned i=0;i<3;i++){Write(b,64+i*4,std::bit_cast<std::uint32_t>(q.gyro[i]),4);Write(b,76+i*4,std::bit_cast<std::uint32_t>(q.accel[i]),4);}return b;
}
inline std::optional<MotionControlRequest> Request(std::span<const std::uint8_t> b){
    if(!HeaderValid(b,3,88)||b[40]>2||b[41]||b[42]||b[43])return {};
    MotionControlRequest q;std::copy_n(b.begin()+8,16,q.session.begin());q.id=Read(b,24,8);q.pressedUs=Read(b,32,8);q.action=b[40];q.generation=static_cast<std::uint32_t>(Read(b,44,4));q.gyroUs=Read(b,48,8);q.accelUs=Read(b,56,8);
    for(unsigned i=0;i<3;i++){q.gyro[i]=std::bit_cast<float>(static_cast<std::uint32_t>(Read(b,64+i*4,4)));q.accel[i]=std::bit_cast<float>(static_cast<std::uint32_t>(Read(b,76+i*4,4)));if(!std::isfinite(q.gyro[i])||std::abs(q.gyro[i])>100||!std::isfinite(q.accel[i])||std::abs(q.accel[i])>200)return {};}
    if(q.action==1){if(!q.generation||!q.gyroUs||!q.accelUs||q.pressedUs<q.gyroUs||q.pressedUs<q.accelUs||q.pressedUs-q.gyroUs>30'000||q.pressedUs-q.accelUs>30'000)return {};}
    else if((q.action==0&&q.generation)||q.gyroUs||q.accelUs||q.gyro!=std::array<float,3>{}||q.accel!=std::array<float,3>{})return {};
    return q;
}
inline std::array<std::uint8_t,48> Encode(const MotionControlReply& q){std::array<std::uint8_t,48>b{};Header(b,4,q.session,q.id,0);b[40]=static_cast<std::uint8_t>(q.result);return b;}
inline std::optional<MotionControlReply> Reply(std::span<const std::uint8_t>b){if(!HeaderValid(b,4,48)||b[40]<1||b[40]>5||std::any_of(b.begin()+41,b.end(),[](auto v){return v!=0;}))return {};MotionControlReply r;std::copy_n(b.begin()+8,16,r.session.begin());r.id=Read(b,24,8);r.result=static_cast<MotionControlResult>(b[40]);return r;}
}
// Network owner publishes requests; VPAD owner applies them. No game-thread waits.
class MotionControlMailbox {
    std::mutex mutex;SessionId session{};std::optional<MotionControlRequest> request;
    MotionControlResult result=MotionControlResult::Pending;LocalTimeUs received=0;std::uint64_t epoch=0;
public:
    void Bind(SessionId id){std::lock_guard lock(mutex);session=id;request.reset();result=MotionControlResult::Pending;++epoch;}
    void Unbind(SessionId id){std::lock_guard lock(mutex);if(session==id){session={};request.reset();++epoch;}}
    std::uint64_t Epoch(){std::lock_guard lock(mutex);return epoch;}
    bool Submit(const MotionControlRequest& q,LocalTimeUs now){std::lock_guard lock(mutex);if(IsZero(session)||q.session!=session)return false;if(request&&q.id<=request->id)return q==*request;request=q;received=now;result=q.action?MotionControlResult::Pending:MotionControlResult::Supported;return true;}
    std::optional<MotionControlRequest> Pending(LocalTimeUs now){std::lock_guard lock(mutex);if(request&&result==MotionControlResult::Pending&&!IsFresh(now,received,150'000))result=MotionControlResult::Stale;return request&&result==MotionControlResult::Pending?request:std::nullopt;}
    void Finish(std::uint64_t id,MotionControlResult value){std::lock_guard lock(mutex);if(request&&request->id==id&&result==MotionControlResult::Pending)result=value;}
    std::optional<MotionControlReply> Reply(LocalTimeUs now){std::lock_guard lock(mutex);if(request&&result==MotionControlResult::Pending&&!IsFresh(now,received,150'000))result=MotionControlResult::Stale;if(!request||result==MotionControlResult::Pending||!IsFresh(now,received,1'000'000))return {};return MotionControlReply{session,request->id,result};}
};
}
