#pragma once
#include <array>
#include <cmath>

namespace CemuPad::Service {
// A virtual input frame layered over the unchanged physical S56 fusion.
// B is captured once at the press; it never follows gravity while playing.
class RecenterProjection {
public:
    using Q=std::array<float,4>;using V=std::array<float,3>;
    static Q Inverse(Q q){return {q[0],-q[1],-q[2],-q[3]};}
    static Q Multiply(Q a,Q b){return {a[0]*b[0]-a[1]*b[1]-a[2]*b[2]-a[3]*b[3],a[0]*b[1]+a[1]*b[0]+a[2]*b[3]-a[3]*b[2],a[0]*b[2]-a[1]*b[3]+a[2]*b[0]+a[3]*b[1],a[0]*b[3]+a[1]*b[2]-a[2]*b[1]+a[3]*b[0]};}
    static Q Normalize(Q q){float n=0;for(float v:q)n+=v*v;n=std::sqrt(n);for(float& v:q)v/=n;return q;}
    static V Rotate(Q q,V v){auto r=Multiply(Multiply(q,{0,v[0],v[1],v[2]}),Inverse(q));return {r[1],r[2],r[3]};}
    static V Add(V a,V b){for(unsigned i=0;i<3;i++)a[i]+=b[i];return a;}
    static V Subtract(V a,V b){for(unsigned i=0;i<3;i++)a[i]-=b[i];return a;}
    static Q Interpolate(Q a,Q b,float t){float dot=0;for(unsigned i=0;i<4;i++)dot+=a[i]*b[i];if(dot<0)for(float& v:b)v=-v;for(unsigned i=0;i<4;i++)a[i]+=(b[i]-a[i])*t;return Normalize(a);}
    bool Active() const{return active;}
    void Clear(){active=false;}
    void Resume(Q current,Q delivered,V currentAccel,V deliveredAccel){reference=current;anchor=delivered;physicalGravity=Rotate(current,currentAccel);virtualGravity=Rotate(delivered,deliveredAccel);}
    bool Align(Q press,V pressAccel,Q current,Q delivered,V currentAccel,V deliveredAccel){
        float n=0;for(float v:pressAccel)n+=v*v;n=std::sqrt(n);
        if(!std::isfinite(n)||n<.2f||n>3.f)return false;
        V a=pressAccel;for(float& v:a)v/=n;
        // Proper rotation onto S56's upright gravity -Z. +Z is antipodal:
        // choose screen X deterministically, never flip axes on a later sample.
        Q nextBasis=a[2]>.999999f?Q{0,1,0,0}:Normalize({1-a[2],-a[1],a[0],0});
        reference=press;basis=nextBasis;
        auto delta=Relative(current);
        anchor=Normalize(Multiply(delivered,Inverse(delta)));
        physicalGravity=Rotate(press,pressAccel);
        auto linear=Rotate(basis,Subtract(currentAccel,Rotate(Inverse(current),physicalGravity)));
        virtualGravity=Rotate(delivered,Subtract(deliveredAccel,linear));
        active=true;return true;
    }
    Q Relative(Q current) const{return Normalize(Multiply(Multiply(basis,Multiply(Inverse(reference),current)),Inverse(basis)));}
    Q Pose(Q current) const{return active?Normalize(Multiply(anchor,Relative(current))):current;}
    V Gyro(V value) const{return active?Rotate(basis,value):value;}
    V Accel(Q physical,Q virtualPose,V value) const{
        if(!active)return value;
        auto linear=Rotate(basis,Subtract(value,Rotate(Inverse(physical),physicalGravity)));
        return Add(Rotate(Inverse(virtualPose),virtualGravity),linear);
    }
private:
    bool active=false;Q reference{1,0,0,0},anchor{1,0,0,0},basis{1,0,0,0};
    V physicalGravity{},virtualGravity{};
};
}
