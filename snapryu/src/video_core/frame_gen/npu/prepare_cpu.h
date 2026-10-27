#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace EdenAnvilCpu {
constexpr int Width=1280, Height=720, Block=16, Columns=80, Rows=45, Blocks=Columns*Rows;
struct PreparationResult { bool scene_cut{}; unsigned valid_pixels{}; double match_error{}; };
struct PreparationDebug {
    std::array<int,Blocks*4> refined{};
    std::array<unsigned,Blocks*2> scores{};
    std::array<uint8_t,Blocks> accepted{};
};
// Same refinement and confidence rules as the baseline. CPU writes integer
// warped pixels directly to the NPU input; no dense coordinate grids exist.
template<bool Packed>
inline PreparationResult PrepareWarp(const uint8_t* frame_a,const uint8_t* frame_b,const uint8_t* flow,const uint8_t* costs,
                                 uint8_t* warped_a,uint8_t* warped_b,uint8_t* valid,
                                 PreparationDebug* debug=nullptr) {
    PreparationResult result;
    std::array<bool,Blocks> accepted{};
    std::array<int,Blocks*4> refined{};
    std::array<unsigned,Blocks*2> scores{};
    constexpr std::array<int,4> points{1,6,9,14};
    for(int direction=0;direction<2;++direction) for(int by=0;by<Rows;++by) for(int bx=0;bx<Columns;++bx) {
        int j=direction*Blocks+by*Columns+bx;
        int fx=(int(flow[j*2])-4)*4,fy=(int(flow[j*2+1])-4)*4;
        auto score=[&](int dx,int dy) {
            unsigned sum=0;
            if(std::abs(dx)>16||std::abs(dy)>16) return UINT32_MAX;
            for(int oy:points) for(int ox:points) {
                int x=bx*16+ox,y=by*16+oy,tx=x+dx,ty=y+dy;
                if(tx<0||tx>=Width||ty<0||ty>=Height) return UINT32_MAX;
                const auto* a=(direction?frame_b:frame_a)+(y*Width+x)*3;
                const auto* b=(direction?frame_a:frame_b)+(ty*Width+tx)*3;
                for(int c=0;c<3;++c) sum+=std::abs(int(a[c])-b[c]);
            }
            return sum;
        };
        int bestx=fx,besty=fy; unsigned best=score(fx,fy);
        for(int dy:{-2,0,2}) for(int dx:{-2,0,2}) {
            if(!best) break;
            if(!dx&&!dy) continue;
            auto error=score(fx+dx,fy+dy);
            if(error<best) {best=error;bestx=fx+dx;besty=fy+dy;}
        }
        // Luminance matching can choose an outlier for sub-quarter-pixel motion.
        // A local robust seed preserves independent objects better than a global
        // camera-motion assumption. RGB scoring still decides whether to use it.
        if(best) {
            std::array<int,9> nx{},ny{};int count=0;
            for(int oy=-1;oy<=1;++oy) for(int ox=-1;ox<=1;++ox) {
                int index=direction*Blocks+std::clamp(by+oy,0,Rows-1)*Columns+std::clamp(bx+ox,0,Columns-1);
                nx[count]=(int(flow[index*2])-4)*4;ny[count]=(int(flow[index*2+1])-4)*4;++count;
            }
            std::sort(nx.begin(),nx.end());std::sort(ny.begin(),ny.end());
            if(nx[4]!=fx||ny[4]!=fy) for(int dy:{-2,0,2}) for(int dx:{-2,0,2}) {
                if(!best)break;
                auto error=score(nx[4]+dx,ny[4]+dy);
                if(error<best){best=error;bestx=nx[4]+dx;besty=ny[4]+dy;}
            }
        }
        refined[j*2]=bestx;refined[j*2+1]=besty;scores[j]=best;
    }
    uint64_t error=0; size_t samples=0;
    for(int by=0;by<Rows;++by) for(int bx=0;bx<Columns;++bx) {
        int j=by*Columns+bx;
        int fx=refined[j*2], fy=refined[j*2+1];
        int tx=bx*16+8+fx, ty=by*16+8+fy;
        bool inside=tx>=0&&tx<Width&&ty>=0&&ty<Height;
        int back=(std::clamp(ty,0,Height-1)/16)*Columns+std::clamp(tx,0,Width-1)/16;
        int rx=refined[(Blocks+back)*2], ry=refined[(Blocks+back)*2+1];
        accepted[j]=inside&&std::abs(fx)<=16&&std::abs(fy)<=16&&std::abs(rx)<=16&&std::abs(ry)<=16&&
                    std::abs(fx+rx)<=4&&std::abs(fy+ry)<=4&&scores[j]<=48*12&&scores[Blocks+back]<=48*12;
        // Sparse full-color correspondence catches cuts that luminance pooling hides.
        for(int oy:{4,12}) for(int ox:{4,12}) {
            int x=bx*16+ox,y=by*16+oy,sx=x+fx,sy=y+fy;
            if(sx>=0&&sx<Width&&sy>=0&&sy<Height) {
                const uint8_t* a=frame_a+(y*Width+x)*3;
                const uint8_t* b=frame_b+(sy*Width+sx)*3;
                for(int c=0;c<3;++c) error+=std::abs(int(a[c])-b[c]); samples+=3;
            }
        }
    }
    result.match_error=samples?double(error)/(255.0*samples):1.0;
    result.scene_cut=result.match_error>0.22;
    for(int y=0;y<Height;++y) for(int bx=0;bx<Columns;++bx) {
        const int j=(y/16)*Columns+bx;
        int ax=refined[j*2]/2,ay=refined[j*2+1]/2;
        int bdx=refined[(Blocks+j)*2]/2,bdy=refined[(Blocks+j)*2+1]/2;
        const auto* row_a=frame_a+std::clamp(y-ay,0,Height-1)*Width*3;
        const auto* row_b=frame_b+std::clamp(y-bdy,0,Height-1)*Width*3;
        if constexpr(!Packed) {
            // Most spans are contiguous; only horizontal edges need clamping.
            for(int image=0;image<2;++image) {
                int dx=image?bdx:ax, sx=bx*16-dx;
                const auto* row=image?row_b:row_a;
                auto* dest=(image?warped_b:warped_a)+(y*Width+bx*16)*3;
                if(sx>=0&&sx+16<=Width) std::memcpy(dest,row+sx*3,16*3);
                else for(int ox=0;ox<16;++ox) std::memcpy(dest+ox*3,row+std::clamp(sx+ox,0,Width-1)*3,3);
            }
        }
        for(int x=bx*16;x<(bx+1)*16;++x) {
            int p=y*Width+x; const auto* a=frame_a+p*3; const auto* b=frame_b+p*3;
            if constexpr(Packed) {
                auto* dest=warped_a+p*6;
                std::memcpy(dest,row_a+std::clamp(x-ax,0,Width-1)*3,3);
                std::memcpy(dest+3,row_b+std::clamp(x-bdx,0,Width-1)*3,3);
            }
            bool changed=(a[0]!=b[0]||a[1]!=b[1]||a[2]!=b[2]);
            bool bounded=x-ax>=0&&x-ax<Width&&y-ay>=0&&y-ay<Height&&
                         x-bdx>=0&&x-bdx<Width&&y-bdy>=0&&y-bdy<Height;
            valid[p]=!result.scene_cut&&accepted[j]&&changed&&bounded;
            result.valid_pixels+=valid[p];
        }
    }
    if (debug) {
        debug->refined=refined;debug->scores=scores;
        for (int j=0;j<Blocks;++j) debug->accepted[j]=accepted[j];
    }
    return result;
}
// Required host finalization restores uncertain/static pixels from current B
// directly in the shared output tensor before it is released for presentation.
inline void Finalize(const uint8_t* frame_b,const uint8_t* valid,uint8_t* rgb) {
    for(int p=0;p<Width*Height;++p) if(!valid[p]) {
        rgb[p*3]=frame_b[p*3];rgb[p*3+1]=frame_b[p*3+1];rgb[p*3+2]=frame_b[p*3+2];
    }
}
}

namespace EdenAnvilCpu {
// Decode the original Anvil residual and combine at the midpoint. A lookup
// table avoids per-pixel floating point. Nearest-even rounding, saturation.
class ComposeTable {
    std::array<uint8_t,511*256> table{};
public:
    const uint8_t* Data() const {return table.data();}
    ComposeTable() {
        for(int sum=0;sum<=510;++sum) for(int residual=0;residual<256;++residual) {
            float value=0.5f*sum+(residual-128)*0.00330072409f*255.0f;
            table[sum*256+residual]=static_cast<uint8_t>(std::clamp(std::nearbyint(value),0.0f,255.0f));
        }
    }
    void Run(const uint8_t* packed,const uint8_t* residual,const uint8_t* current,
             const uint8_t* valid,uint8_t* rgb) const {
        for(int p=0;p<Width*Height;++p) {
            if(!valid[p]) std::memcpy(rgb+p*3,current+p*3,3);
            else for(int c=0;c<3;++c) rgb[p*3+c]=table[(int(packed[p*6+c])+packed[p*6+c+3])*256+residual[p*3+c]];
        }
    }
    // Renderer output path: identical RGB lookup/rounding, one RGBA traversal.
    // Keep Run for standalone RGB gold callers.
    void RunRgba(const uint8_t* packed,const uint8_t* residual,const uint8_t* current,
                 const uint8_t* valid,uint8_t* rgba) const {
        for(int p=0;p<Width*Height;++p) {
            if(!valid[p]) std::memcpy(rgba+p*4,current+p*3,3);
            else for(int c=0;c<3;++c) rgba[p*4+c]=table[(int(packed[p*6+c])+packed[p*6+c+3])*256+residual[p*3+c]];
            rgba[p*4+3]=255;
        }
    }
};
}
