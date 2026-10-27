#include "RtpH264.h"
#include <algorithm>
#include <stdexcept>

namespace CemuPad::Media {
namespace {
void Need(bool v){if(!v)throw std::invalid_argument("Invalid H264 RTP");}
std::uint32_t U32(std::span<const std::uint8_t> p,unsigned at){return(std::uint32_t(p[at])<<24)|(std::uint32_t(p[at+1])<<16)|(std::uint32_t(p[at+2])<<8)|p[at+3];}
void Put(Bytes& b,unsigned at,std::uint32_t value){for(unsigned i=0;i<4;++i)b[at+i]=static_cast<std::uint8_t>(value>>(24-i*8));}
}
void H264Packetizer::Send(const std::vector<Bytes>& nals,std::uint32_t time,const std::function<void(Bytes)>& output){
    Need(ssrc!=0&&!nals.empty()&&nals.size()<=256);
    auto packet=[&](std::span<const std::uint8_t> body,bool marker){
        Need(body.size()<=1172);Bytes result(12+body.size());result[0]=0x80;result[1]=96|(marker?128:0);
        result[2]=sequence>>8;result[3]=static_cast<std::uint8_t>(sequence++);Put(result,4,time);Put(result,8,ssrc);
        std::copy(body.begin(),body.end(),result.begin()+12);output(std::move(result));
    };
    for(std::size_t i=0;i<nals.size();++i){
        const auto& nal=nals[i];Need(!nal.empty()&&!(nal[0]&128)&&(nal[0]&31)>0&&(nal[0]&31)<24);
        if(nal.size()<=1172){packet(nal,i+1==nals.size());continue;}
        for(std::size_t at=1;at<nal.size();){
            auto count=std::min<std::size_t>(1170,nal.size()-at);bool last=at+count==nal.size();
            Bytes body{static_cast<std::uint8_t>((nal[0]&0xe0)|28),static_cast<std::uint8_t>((at==1?128:0)|(last?64:0)|(nal[0]&31))};
            body.insert(body.end(),nal.begin()+at,nal.begin()+at+count);packet(body,last&&i+1==nals.size());at+=count;
        }
    }
}
void H264Depacketizer::Reset(){active=false;damaged=false;fragment=false;idr=false;bytes.clear();}
std::optional<AccessUnit> H264Depacketizer::Push(std::span<const std::uint8_t> rtp){
    if(rtp.size()<13||rtp.size()>1184||rtp[0]!=0x80||(rtp[1]&127)!=96||U32(rtp,8)!=ssrc){++dropped;Reset();return {};}
    auto seq=static_cast<std::uint16_t>((rtp[2]<<8)|rtp[3]);auto time=U32(rtp,4);bool marker=(rtp[1]&128)!=0;
    if(!active||time!=timestamp){if(active&&!bytes.empty())++dropped;Reset();active=true;timestamp=time;}
    if(!haveSequence){next=seq;haveSequence=true;}
    if(seq!=next){damaged=true;++dropped;}next=static_cast<std::uint16_t>(seq+1);
    auto body=rtp.subspan(12);auto type=body[0]&31;
    auto append=[&](std::span<const std::uint8_t> nal){
        if(nal.empty()||(nal[0]&128)||(nal[0]&31)==0||(nal[0]&31)>=24||bytes.size()+nal.size()+4>2*1024*1024){damaged=true;return;}
        bytes.insert(bytes.end(),{0,0,0,1});bytes.insert(bytes.end(),nal.begin(),nal.end());idr|=(nal[0]&31)==5;
    };
    if(body[0]&128)damaged=true;
    else if(type>0&&type<24){if(fragment)damaged=true;append(body);}
    else if(type==24){
        if(fragment)damaged=true;
        std::size_t at=1;
        while(at<body.size()){
            if(body.size()-at<2){damaged=true;break;}auto count=(body[at]<<8)|body[at+1];at+=2;
            if(count==0||static_cast<std::size_t>(count)>body.size()-at){damaged=true;break;}
            append(body.subspan(at,count));at+=count;
        }
    }else if(type==28&&body.size()>=3){
        bool start=body[1]&128,end=body[1]&64;auto nalType=body[1]&31;
        if(nalType==0||nalType>=24||(body[1]&32)||(start&&end))damaged=true;
        std::uint8_t header=(body[0]&0xe0)|nalType;
        if(start){if(fragment)damaged=true;fragment=true;fragmentHeader=header;append(std::span(&header,1));}
        else if(!fragment||fragmentHeader!=header)damaged=true;
        if(bytes.size()+body.size()-2>2*1024*1024)damaged=true;
        else bytes.insert(bytes.end(),body.begin()+2,body.end());
        if(end)fragment=false;
    }else damaged=true;
    if(!marker)return {};
    if(damaged||fragment||bytes.empty()){++dropped;Reset();return {};}
    AccessUnit result{std::move(bytes),timestamp,idr};Reset();return result;
}
}
