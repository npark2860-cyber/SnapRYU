#include "Identity.h"
#include "CemuPad/control/VerifiedTls.h"
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace CemuPad::Store {
namespace {
void Need(bool value) { if(!value) throw std::runtime_error("Invalid CemuPad identity"); }
struct Erase { std::vector<std::uint8_t>& bytes; bool active=true; ~Erase(){if(active)OPENSSL_cleanse(bytes.data(),bytes.size());} };
void Valid(X509* cert,bool currentTime=true) {
    Need(cert&&Crypto::IsP256(X509_get0_pubkey(cert))&&X509_verify(cert,X509_get0_pubkey(cert))==1);
    Need(ASN1_TIME_check(X509_get0_notBefore(cert))==1&&ASN1_TIME_check(X509_get0_notAfter(cert))==1&&
         ASN1_TIME_compare(X509_get0_notBefore(cert),X509_get0_notAfter(cert))<0);
    if(currentTime)Need(X509_cmp_current_time(X509_get0_notBefore(cert))<0&&X509_cmp_current_time(X509_get0_notAfter(cert))>0);
}
void U32(std::vector<std::uint8_t>& b,std::size_t size) {
    Need(size<=32768); for(int s=24;s>=0;s-=8)b.push_back(static_cast<std::uint8_t>(size>>s));
}
}
std::string Base64(std::span<const std::uint8_t> b) {
    Need(b.size()<=32768);std::string s((b.size()+2)/3*4,'\0');
    Need(EVP_EncodeBlock(reinterpret_cast<unsigned char*>(s.data()),b.data(),static_cast<int>(b.size()))==static_cast<int>(s.size()));return s;
}
void ValidateCertificate(std::span<const std::uint8_t> bytes,const Crypto::Digest& pin,bool currentTime) {
    Need(!bytes.empty()&&bytes.size()<=12288);const auto* p=bytes.data();
    std::unique_ptr<X509,decltype(&X509_free)> cert(d2i_X509(nullptr,&p,static_cast<long>(bytes.size())),X509_free);
    Need(p==bytes.data()+bytes.size());Valid(cert.get(),currentTime);Need(Crypto::SpkiFingerprint(X509_get0_pubkey(cert.get()))==pin);
}
Identity Identity::Generate() {
    Identity out;out.key={EVP_PKEY_Q_keygen(nullptr,nullptr,"EC","P-256"),EVP_PKEY_free};Need(bool(out.key));
    out.certificate={X509_new(),X509_free};Need(bool(out.certificate));auto* c=out.certificate.get();
    std::array<std::uint8_t,16> random{};Need(RAND_bytes(random.data(),static_cast<int>(random.size()))==1);
    out.storeId=Control::Hex(random);random[0]&=0x7f;random[0]|=1;
    std::unique_ptr<BIGNUM,decltype(&BN_free)> serial(BN_bin2bn(random.data(),static_cast<int>(random.size()),nullptr),BN_free);
    Need(bool(serial));Need(BN_to_ASN1_INTEGER(serial.get(),X509_get_serialNumber(c))!=nullptr);
    Need(X509_set_version(c,2)==1&&X509_set_pubkey(c,out.key.get())==1);
    Need(X509_gmtime_adj(X509_getm_notBefore(c),-300)!=nullptr&&X509_gmtime_adj(X509_getm_notAfter(c),315360000L)!=nullptr);
    auto* name=X509_get_subject_name(c);Need(name!=nullptr);
    constexpr char cn[]="CemuPad persistent identity";
    Need(X509_NAME_add_entry_by_txt(name,"CN",MBSTRING_ASC,reinterpret_cast<const unsigned char*>(cn),-1,-1,0)==1);
    Need(X509_set_issuer_name(c,name)==1&&X509_sign(c,out.key.get(),EVP_sha256())>0);Valid(c);return out;
}
std::vector<std::uint8_t> Identity::CertificateDer() const {
    int size=i2d_X509(certificate.get(),nullptr);Need(size>0&&size<=16384);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));auto* p=bytes.data();Need(i2d_X509(certificate.get(),&p)==size);return bytes;
}
Crypto::Digest Identity::Pin() const { Need(bool(key));return Crypto::SpkiFingerprint(key.get()); }
std::vector<std::uint8_t> Identity::Encode() const {
    Need(key&&certificate&&storeId.size()==32);int size=i2d_PrivateKey(key.get(),nullptr);Need(size>0&&size<=8192);
    std::vector<std::uint8_t> secret(static_cast<std::size_t>(size));Erase erased{secret};auto* p=secret.data();Need(i2d_PrivateKey(key.get(),&p)==size);
    auto cert=CertificateDer();auto pin=Pin();std::vector<std::uint8_t> out;out.reserve(4+16+4+secret.size()+4+cert.size()+32);Erase outputErase{out};
    out.insert(out.end(),{'C','P','I','1'});
    auto id=Control::Session(storeId);out.insert(out.end(),id.begin(),id.end());
    U32(out,secret.size());out.insert(out.end(),secret.begin(),secret.end());U32(out,cert.size());out.insert(out.end(),cert.begin(),cert.end());
    out.insert(out.end(),pin.begin(),pin.end());outputErase.active=false;return out;
}
Identity Identity::Decode(std::span<const std::uint8_t> b) {
    Need(b.size()>=60&&b.size()<=32768&&std::equal(b.begin(),b.begin()+4,"CPI1"));std::size_t at=20;
    auto next=[&](){Need(at+4<=b.size());std::size_t n=0;for(unsigned i=0;i<4;++i)n=(n<<8)|b[at++];Need(n>0&&n<=16384&&n<=b.size()-at);auto part=b.subspan(at,n);at+=n;return part;};
    auto rawKey=next(),rawCert=next();Need(at+32==b.size());Crypto::Digest pin{};std::copy_n(b.begin()+at,32,pin.begin());
    Identity out;const auto* k=rawKey.data();out.key={d2i_AutoPrivateKey(nullptr,&k,static_cast<long>(rawKey.size())),EVP_PKEY_free};
    Need(out.key&&k==rawKey.data()+rawKey.size()&&Crypto::IsP256(out.key.get())&&Crypto::SpkiFingerprint(out.key.get())==pin);
    std::unique_ptr<EVP_PKEY_CTX,decltype(&EVP_PKEY_CTX_free)> keyContext(EVP_PKEY_CTX_new_from_pkey(nullptr,out.key.get(),nullptr),EVP_PKEY_CTX_free);
    Need(keyContext&&EVP_PKEY_pairwise_check(keyContext.get())==1);
    const auto* c=rawCert.data();out.certificate={d2i_X509(nullptr,&c,static_cast<long>(rawCert.size())),X509_free};
    Need(c==rawCert.data()+rawCert.size());Valid(out.certificate.get(),false);Need(X509_check_private_key(out.certificate.get(),out.key.get())==1);
    out.storeId=Control::Hex(b.subspan(4,16));Need(std::any_of(b.begin()+4,b.begin()+20,[](auto v){return v!=0;}));return out;
}
}
