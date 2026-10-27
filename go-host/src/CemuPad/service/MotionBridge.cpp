#include <algorithm>
#include <cmath>
#include <cassert>
#include "MotionBridge.h"
#include "input/motion/MotionHandler.h"
#include "RecenterProjection.h"
#include <deque>

namespace CemuPad::Service {
struct MotionBridge::Impl {
    WiiUMotionHandler handler;MotionOutput output;
    RecenterProjection projection;MahonySensorFusion outputAngles;
    RecenterProjection::Q deliveredPose{1,0,0,0};std::array<float,3> angleOffset{};
    MotionOutput lastActive;std::uint64_t controlEpoch=0;
    struct PoseAt {std::uint64_t time;RecenterProjection::Q pose;};std::deque<PoseAt> history;
    bool primed=false;std::uint32_t source=0;std::uint64_t epoch=0,integrations=0;
};
MotionBridge::MotionBridge():m(std::make_unique<Impl>()){}
MotionBridge::~MotionBridge()=default;
std::uint64_t MotionBridge::Integrations() const{return m->integrations;}
MotionOutput MotionBridge::Apply(const std::optional<ConsumedInput>& input){
    return Apply(input,nullptr,0);
}
MotionOutput MotionBridge::Apply(const std::optional<ConsumedInput>& input,MotionControlMailbox* controls,LocalTimeUs now){
    auto& s=*m;
    if(controls){auto epoch=controls->Epoch();if(epoch!=s.controlEpoch){s.controlEpoch=epoch;s.projection.Clear();s.history.clear();s.primed=false;}
        if(auto request=controls->Pending(now);request&&request->action==2){s.projection.Clear();s.primed=false;s.history.clear();controls->Finish(request->id,MotionControlResult::Applied);}}
    auto clear=[&]{bool previous=s.output.active;s.output={};s.output.neutralize=previous;s.primed=false;s.history.clear();return s.output;};
    if(!input||input->handoverRelease)return clear();
    const auto& motion=input->motion;
    if(motion.gyro.action==MotionAction::Invalid||motion.accel.action==MotionAction::Invalid)return clear();
    bool reset=!s.primed||s.source!=input->state.deviceGeneration||s.epoch!=input->ownershipEpoch
        ||motion.gyro.action==MotionAction::Prime||motion.gyro.action==MotionAction::Reset||motion.accel.action==MotionAction::Reset;
    bool unchanged=motion.gyro.action==MotionAction::NoChange;
    if(unchanged&&reset)return clear();
    // Fixed upright pad frame: the screen normal is the controller face normal and
    // the screen's up edge points away from the player, (x,y,z)->(x,z,-y), applied
    // identically to both sensors. Real gravity is kept, so an upright phone reads
    // as an upright pad; a lying phone reads like S50's upright values.
    const auto& gv=motion.gyro.values;const auto& av=motion.accel.values;
    const float g[3]{gv[0],gv[2],-gv[1]},a[3]{av[0],av[2],-av[1]};
    // Use the same display/body axes as Cemu's existing SDL sensor path.
    float gx=g[0],gy=-g[1],gz=-g[2];
    float ax=-a[0]/9.81f,ay=a[1]/9.81f,az=a[2]/9.81f;
    if(reset){s.handler.resetFromGravity(ax,ay,az);s.source=input->state.deviceGeneration;s.epoch=input->ownershipEpoch;s.history.clear();}
    float dt=reset?0:static_cast<float>(motion.gyro.deltaUs)/1'000'000.0f;
    if(!unchanged){s.handler.processMotionSample(dt,gx,gy,gz,ax,ay,az);if(!reset)++s.integrations;}
    auto sample=s.handler.getMotionSample();float rawQ[4];sample.getQuaternion(rawQ);
    RecenterProjection::Q physical{rawQ[0],rawQ[1],rawQ[2],rawQ[3]};
    if(!unchanged){s.history.push_back({motion.gyro.timeUs,physical});if(s.history.size()>32)s.history.pop_front();}
    auto pose=s.projection.Pose(physical);
    float rawAccel[3],rawGyro[3];sample.getAccelerometer(rawAccel);sample.getGyrometer(rawGyro);
    RecenterProjection::V currentAccel{rawAccel[0],rawAccel[1],rawAccel[2]};
    if(reset&&s.projection.Active()){
        s.projection.Resume(physical,s.deliveredPose,currentAccel,{-s.lastActive.accel[0],-s.lastActive.accel[1],s.lastActive.accel[2]});pose=s.deliveredPose;
    }
    if(s.projection.Active()){
        auto acc=s.projection.Accel(physical,pose,currentAccel),gyro=s.projection.Gyro({rawGyro[0],rawGyro[1],rawGyro[2]});
        s.outputAngles.setOrientationForCemuPad(Quaternionf(pose[0],pose[1],pose[2],pose[3]));constexpr float tau=6.2831853071795864769f;
        float angles[3]{-s.outputAngles.getYawRadians()/tau-.5f+s.angleOffset[0],-s.outputAngles.getPitchRadians()/tau-.5f+s.angleOffset[1],s.outputAngles.getRollRadians()/tau+s.angleOffset[2]};
        float previous[3]{-s.lastActive.accel[0],-s.lastActive.accel[1],s.lastActive.accel[2]};
        sample=MotionSample(acc.data(),MotionSample::calculateAccAcceleration(previous,acc.data()),gyro.data(),angles,pose.data());
    }
    MotionOutput out;out.active=true;
    sample.getGyrometer(out.physicalGyro.data());sample.getAccelerometer(out.physicalAccel.data());
    sample.getQuaternion(out.physicalQuaternion.data());
    out.sensorTimeUs=motion.gyro.timeUs;out.deltaUs=reset?0:motion.gyro.deltaUs;
    sample.getVPADGyroChange(out.gyro.data());sample.getVPADOrientation(out.orientation.data());sample.getVPADAccelerometer(out.accel.data());
    out.accMagnitude=sample.getVPADAccMagnitude();out.accAcceleration=sample.getVPADAccAcceleration();
    if(out.accMagnitude>0.000001f)sample.getVPADAccXY(out.accXY.data());
    sample.getVPADAttitudeMatrix(out.attitude.data());
    if(unchanged){out=s.output;out.neutralize=false;}
    s.deliveredPose=pose;
    if(controls)if(auto request=controls->Pending(now)){
        auto finish=[&](MotionControlResult result){controls->Finish(request->id,result);};
        if(request->generation!=s.source||reset)finish(MotionControlResult::Stale);
        else if(!s.history.empty()&&request->gyroUs<s.history.front().time)finish(MotionControlResult::Stale);
        else {
            std::optional<RecenterProjection::Q> reference;
            for(std::size_t i=0;i<s.history.size();i++){
                const auto& h=s.history[i];
                if(h.time==request->gyroUs){reference=h.pose;break;}
                if(i&&s.history[i-1].time<request->gyroUs&&request->gyroUs<h.time&&h.time-s.history[i-1].time<InputLeaseUs){const auto& p=s.history[i-1];reference=RecenterProjection::Interpolate(p.pose,h.pose,static_cast<float>(request->gyroUs-p.time)/static_cast<float>(h.time-p.time));break;}
            }
            if(reference){
                // Capture the press pose, but anchor at the value already delivered
                // now: transport latency cannot itself create a camera jump.
                RecenterProjection::V pressAccel{-request->accel[0]/9.81f,request->accel[2]/9.81f,-request->accel[1]/9.81f};
                if(s.projection.Align(*reference,pressAccel,physical,pose,currentAccel,{-out.accel[0],-out.accel[1],out.accel[2]})){
                    s.outputAngles=MahonySensorFusion{};s.outputAngles.setOrientationForCemuPad(Quaternionf(pose[0],pose[1],pose[2],pose[3]));constexpr float tau=6.2831853071795864769f;
                    s.angleOffset={out.orientation[0]+s.outputAngles.getYawRadians()/tau+.5f,out.orientation[1]+s.outputAngles.getPitchRadians()/tau+.5f,out.orientation[2]-s.outputAngles.getRollRadians()/tau};
                    finish(MotionControlResult::Applied);
                }else finish(MotionControlResult::NotReady);
            }
        }
    }
    if(reset)s.primed=true;
    s.output=out;s.lastActive=out;return out;
}
}
