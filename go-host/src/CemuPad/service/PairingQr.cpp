#include "PairingQr.h"
#include "CemuPad/third_party/qrcodegen/qrcodegen.hpp"
#include <openssl/crypto.h>
#include <algorithm>
#include <stdexcept>
#include <string_view>
namespace CemuPad::Service {
PairingRaster::~PairingRaster(){if(!pixels.empty())OPENSSL_cleanse(pixels.data(),pixels.size());}
PairingQr::~PairingQr(){if(!modules.empty())OPENSSL_cleanse(modules.data(),modules.size());}
PairingQr PairingQr::Encode(std::span<const unsigned char> payload){
    constexpr std::string_view prefix="CemuPadPairV1:";
    if(payload.size()!=134||!std::equal(prefix.begin(),prefix.end(),payload.begin()))throw std::invalid_argument("Pairing QR payload");
    for(auto c:payload.subspan(prefix.size()))if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='+'||c=='/'))
        throw std::invalid_argument("Pairing QR alphabet");
    std::vector<unsigned char> bytes(payload.begin(),payload.end());
    struct Wipe{decltype(bytes)& v;~Wipe(){OPENSSL_cleanse(v.data(),v.size());}} wipe{bytes};
    auto qr=qrcodegen::QrCode::encodeSegments({qrcodegen::QrSegment::makeBytes(bytes)},qrcodegen::QrCode::Ecc::MEDIUM,1,10,-1,false);
    PairingQr result;result.size=qr.getSize();result.modules.resize(result.size*result.size);
    for(int y=0;y<result.size;++y)for(int x=0;x<result.size;++x)result.modules[y*result.size+x]=qr.getModule(x,y)?1:0;
    return result;
}
PairingRaster PairingQr::Raster(int scale) const{
    if(scale<2||scale>8||size<21||size>57||modules.size()!=static_cast<std::size_t>(size*size))throw std::invalid_argument("Pairing QR raster");
    PairingRaster result;result.width=(size+8)*scale;result.pixels.assign(result.width*result.width,255);
    for(int y=0;y<size;++y)for(int x=0;x<size;++x)if(modules[y*size+x])
        for(int dy=0;dy<scale;++dy)for(int dx=0;dx<scale;++dx)result.pixels[((y+4)*scale+dy)*result.width+(x+4)*scale+dx]=0;
    return result;
}
}
