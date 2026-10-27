#include "H264.h"
#include <stdexcept>

namespace CemuPad::Media {
namespace {
void Need(bool v){if(!v)throw std::invalid_argument("Invalid H264 data");}
struct Bits {
    Bytes bytes;std::size_t bit=0;
    explicit Bits(std::span<const std::uint8_t> nal,unsigned type){
        Need(nal.size()>=2&&nal.size()<=65536&&!(nal[0]&128)&&(nal[0]&31)==type);
        unsigned zeros=0;
        for(std::size_t i=1;i<nal.size();++i){
            auto value=nal[i];
            if(zeros>=2&&value==3){Need(i+1<nal.size()&&nal[i+1]<=3);zeros=0;continue;}
            bytes.push_back(value);zeros=value==0?zeros+1:0;
        }
    }
    unsigned Read(unsigned count){Need(count<=32&&bit+count<=bytes.size()*8);unsigned value=0;
        while(count--){value=(value<<1)|((bytes[bit/8]>>(7-bit%8))&1);++bit;}return value;}
    unsigned Ue(){unsigned zeros=0;while(!Read(1)){Need(++zeros<24);}return ((1u<<zeros)-1)+Read(zeros);}
    int Se(){auto n=Ue();return n&1?static_cast<int>((n+1)/2):-static_cast<int>(n/2);}
};
}
H264Format ValidateH264(std::span<const std::uint8_t> sps,std::span<const std::uint8_t> pps){
    Bits bits(sps,7);H264Format f;f.profile=bits.Read(8);bits.Read(8);f.level=bits.Read(8);f.spsId=bits.Ue();
    Need(f.profile==66&&f.spsId<32&&f.level>=10&&f.level<=52);
    Need(bits.Ue()<=12);auto order=bits.Ue();Need(order<=2);
    if(order==0)Need(bits.Ue()<=12);
    if(order==1){bits.Read(1);bits.Se();bits.Se();auto count=bits.Ue();Need(count<=255);while(count--)bits.Se();}
    Need(bits.Ue()<=16);bits.Read(1);
    auto width=bits.Ue(),height=bits.Ue();Need(width<256&&height<135);
    bool progressive=bits.Read(1)!=0;Need(progressive);bits.Read(1);
    unsigned left=0,right=0,top=0,bottom=0;
    if(bits.Read(1)){left=bits.Ue();right=bits.Ue();top=bits.Ue();bottom=bits.Ue();}
    f.codedWidth=(width+1)*16;f.codedHeight=(height+1)*16;
    Need(left+right<f.codedWidth/2&&top+bottom<f.codedHeight/2);
    f.left=left*2;f.top=top*2;f.width=f.codedWidth-(left+right)*2;f.height=f.codedHeight-(top+bottom)*2;
    Need(f.width>=2&&f.height>=2);
    Bits picture(pps,8);Need(picture.Ue()<256&&picture.Ue()==f.spsId);
    Need(picture.Read(1)==0); // Baseline requires CAVLC, not CABAC.
    return f;
}
std::vector<Bytes> SplitH264(std::span<const std::uint8_t> data){
    Need(!data.empty()&&data.size()<=2*1024*1024);std::vector<Bytes> out;
    auto start=[&](std::size_t i)->std::size_t{
        if(i+3<=data.size()&&data[i]==0&&data[i+1]==0){
            if(data[i+2]==1)return 3;
            if(i+4<=data.size()&&data[i+2]==0&&data[i+3]==1)return 4;
        }return 0;
    };
    if(start(0)){
        std::size_t at=0;
        while(at<data.size()){
            auto prefix=start(at);Need(prefix!=0);auto begin=at+prefix;at=begin;
            while(at<data.size()&&!start(at))++at;
            auto end=at;while(end>begin&&data[end-1]==0)--end;
            Need(end>begin);out.emplace_back(data.begin()+begin,data.begin()+end);
        }
    }else{
        std::size_t at=0;
        while(at<data.size()){
            Need(data.size()-at>=4);std::uint32_t size=0;for(unsigned i=0;i<4;++i)size=(size<<8)|data[at++];
            Need(size>0&&size<=data.size()-at);out.emplace_back(data.begin()+at,data.begin()+at+size);at+=size;
        }
    }
    Need(!out.empty()&&out.size()<=256);
    for(const auto& nal:out)Need(!nal.empty()&&!(nal[0]&128)&&(nal[0]&31)>0&&(nal[0]&31)<24);
    return out;
}
bool HasIdr(const std::vector<Bytes>& nals){for(const auto& nal:nals)if(!nal.empty()&&(nal[0]&31)==5)return true;return false;}
Bytes AnnexB(const std::vector<Bytes>& nals){Bytes out;for(const auto& nal:nals){Need(out.size()+nal.size()+4<=2*1024*1024);out.insert(out.end(),{0,0,0,1});out.insert(out.end(),nal.begin(),nal.end());}return out;}
}
