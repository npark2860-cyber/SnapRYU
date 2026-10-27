#include "WindowsStore.h"
#include "CemuPad/control/VerifiedTls.h"
#include "CemuPad/third_party/nlohmann/json.hpp"
#include <windows.h>
#include <wincrypt.h>
#include <sddl.h>
#include <aclapi.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <algorithm>
#include <charconv>
#include <limits>
#include <stdexcept>

namespace CemuPad::Store {
namespace {
using Json=nlohmann::json;
using Bytes=std::vector<std::uint8_t>;
using Outcome=Control::CommitOutcome;
struct Failure {StoreError error;};
void Need(bool ok,StoreError error=StoreError::Invalid) {if(!ok)throw Failure{error};}
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h){}
    ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
    Handle(Handle&& h) noexcept:value(h.value){h.value=INVALID_HANDLE_VALUE;}
    void Close(){auto h=value;value=INVALID_HANDLE_VALUE;Need(CloseHandle(h)!=0,StoreError::Io);}
};
struct Local {void* data=nullptr;~Local(){if(data)LocalFree(data);}};
struct Secret {Bytes bytes;~Secret(){OPENSSL_cleanse(bytes.data(),bytes.size());}};
std::wstring CurrentSid() {
    Handle token;Need(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value)!=0,StoreError::Io);
    DWORD count=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&count);Need(count>0,StoreError::Io);
    Bytes buf(count);Need(GetTokenInformation(token.value,TokenUser,buf.data(),count,&count)!=0,StoreError::Io);
    Local sid;Need(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid,reinterpret_cast<LPWSTR*>(&sid.data))!=0,StoreError::Io);
    return static_cast<wchar_t*>(sid.data);
}
struct Security {
    Local descriptor;SECURITY_ATTRIBUTES attributes{};
    Security(){auto s=L"O:"+CurrentSid()+L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;"+CurrentSid()+L")";
        Need(ConvertStringSecurityDescriptorToSecurityDescriptorW(s.c_str(),SDDL_REVISION_1,reinterpret_cast<PSECURITY_DESCRIPTOR*>(&descriptor.data),nullptr)!=0,StoreError::Io);
        attributes={sizeof(attributes),descriptor.data,FALSE};}
};
void ValidateRootAcl(HANDLE file) {
    Local descriptor;PSID owner=nullptr;PACL dacl=nullptr;
    Need(GetSecurityInfo(file,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&owner,nullptr,&dacl,nullptr,reinterpret_cast<PSECURITY_DESCRIPTOR*>(&descriptor.data))==ERROR_SUCCESS,StoreError::Io);
    SECURITY_DESCRIPTOR_CONTROL control{};DWORD revision=0;
    Need(GetSecurityDescriptorControl(descriptor.data,&control,&revision)!=0&&bool(control&SE_DACL_PROTECTED)&&owner&&dacl);
    Local ownerText;Need(ConvertSidToStringSidW(owner,reinterpret_cast<LPWSTR*>(&ownerText.data))!=0,StoreError::Io);
    auto user=CurrentSid();Need(user==static_cast<wchar_t*>(ownerText.data));bool userAllowed=false;
    for(DWORD i=0;i<dacl->AceCount;++i){void* raw=nullptr;Need(GetAce(dacl,i,&raw)!=0);auto* head=static_cast<ACE_HEADER*>(raw);
        // Dedicated root created by this adapter has only these two allow principals.
        Need(head->AceType==ACCESS_ALLOWED_ACE_TYPE);auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);
        Local text;Need(ConvertSidToStringSidW(&ace->SidStart,reinterpret_cast<LPWSTR*>(&text.data))!=0);
        std::wstring sid=static_cast<wchar_t*>(text.data);Need(sid==user||sid==L"S-1-5-18");
        if(sid==user&&(ace->Mask&FILE_ALL_ACCESS)==FILE_ALL_ACCESS)userAllowed=true;
    }
    Need(userAllowed);
}
void CheckFile(HANDLE h,bool directory=false) {
    BY_HANDLE_FILE_INFORMATION info{};Need(GetFileInformationByHandle(h,&info)!=0,StoreError::Io);
    Need(!(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&bool(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)==directory);
    if(!directory)Need(info.nNumberOfLinks==1);
}
bool Exists(const std::filesystem::path& p) {
    auto attr=GetFileAttributesW(p.c_str());if(attr!=INVALID_FILE_ATTRIBUTES){Need(!(attr&FILE_ATTRIBUTE_REPARSE_POINT));return true;}
    auto e=GetLastError();Need(e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND,StoreError::Io);return false;
}
struct Guard {
    std::vector<Handle> directories;Handle lock;OVERLAPPED overlap{};bool locked=false;
    explicit Guard(const std::filesystem::path& root,bool initialize) {
        Need(root.is_absolute()&&root.root_name().wstring().size()==2&&root.native().size()<195,StoreError::UnsupportedPath);
        for(const auto& part:root)Need(part!=L".."&&part!=L".",StoreError::UnsupportedPath);
        auto base=root.root_path();wchar_t fs[MAX_PATH]{};
        Need(GetDriveTypeW(base.c_str())==DRIVE_FIXED&&GetVolumeInformationW(base.c_str(),nullptr,0,nullptr,nullptr,nullptr,fs,MAX_PATH)!=0&&std::wstring(fs)==L"NTFS",StoreError::UnsupportedPath);
        auto keep=[&](const std::filesystem::path& p){Handle h(CreateFileW(p.c_str(),FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            Need(h.value!=INVALID_HANDLE_VALUE,StoreError::Io);CheckFile(h.value,true);directories.push_back(std::move(h));};
        keep(base);auto parts=root.relative_path();
        for(const auto& part:parts){base/=part;if(base==root&&!Exists(base)){
                Need(initialize,StoreError::Missing);Security security;
                if(!CreateDirectoryW(base.c_str(),&security.attributes))Need(GetLastError()==ERROR_ALREADY_EXISTS,StoreError::Io);
            }keep(base);}
        ValidateRootAcl(directories.back().value);
        auto lockPath=root/L"writer.lock";
        lock.value=CreateFileW(lockPath.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        Need(lock.value!=INVALID_HANDLE_VALUE,StoreError::Io);CheckFile(lock.value);
        if(!LockFileEx(lock.value,LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY,0,1,0,&overlap)){
            auto e=GetLastError();throw Failure{e==ERROR_LOCK_VIOLATION||e==ERROR_IO_PENDING?StoreError::Busy:StoreError::Io};}
        locked=true;
        auto identity=root/L"identity";
        if(!Exists(identity)){Need(initialize,StoreError::Missing);Need(CreateDirectoryW(identity.c_str(),nullptr)!=0,StoreError::Io);}
        keep(identity);
    }
    ~Guard(){if(locked)UnlockFileEx(lock.value,0,1,0,&overlap);}
};
Bytes Read(const std::filesystem::path& path,std::size_t max=65536) {
    Handle h(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(h.value==INVALID_HANDLE_VALUE){auto e=GetLastError();throw Failure{e==ERROR_FILE_NOT_FOUND?StoreError::Missing:StoreError::Io};}
    CheckFile(h.value);LARGE_INTEGER size{};Need(GetFileSizeEx(h.value,&size)!=0,StoreError::Io);Need(size.QuadPart>0&&static_cast<std::uint64_t>(size.QuadPart)<=max);
    Bytes bytes(static_cast<std::size_t>(size.QuadPart));DWORD n=0;Need(ReadFile(h.value,bytes.data(),static_cast<DWORD>(bytes.size()),&n,nullptr)!=0&&n==bytes.size(),StoreError::Io);h.Close();return bytes;
}
Bytes Protect(std::span<const std::uint8_t> plain) {
    DATA_BLOB input{static_cast<DWORD>(plain.size()),const_cast<BYTE*>(plain.data())},encrypted{};
    Need(CryptProtectData(&input,L"CemuPad identity v1",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&encrypted)!=0,StoreError::Io);
    Local holder;holder.data=encrypted.pbData;return {encrypted.pbData,encrypted.pbData+encrypted.cbData};
}
Identity LoadIdentity(const std::filesystem::path& root) {
    auto encrypted=Read(root/L"identity/host.dpapi",32768);DATA_BLOB input{static_cast<DWORD>(encrypted.size()),encrypted.data()},plain{};
    Need(CryptUnprotectData(&input,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&plain)!=0);
    Local holder;holder.data=plain.pbData;
    struct Wipe{DATA_BLOB& b;~Wipe(){OPENSSL_cleanse(b.pbData,b.cbData);}} wipe{plain};
    return Identity::Decode(std::span(plain.pbData,plain.cbData));
}
std::string EncodeTrust(const Snapshot& snapshot) {
    Json j={{"version",1},{"store_id",snapshot.identity.StoreId()},{"host_spki_sha256",Control::Hex(snapshot.identity.Pin())},{"revision",std::to_string(snapshot.revision)},{"peer",nullptr}};
    if(snapshot.peer){const auto& p=*snapshot.peer;j["peer"]={{"transaction",Control::Hex(p.transaction)},{"client_spki_sha256",Control::Hex(p.clientPin)},{"host_spki_sha256",Control::Hex(p.hostPin)},{"client_certificate_der",Base64(p.clientCertificate)}};}
    return j.dump()+"\n";
}
Snapshot LoadUnlocked(const std::filesystem::path& root) {
    Snapshot out;out.identity=LoadIdentity(root);auto bytes=Read(root/L"trust.json");std::string raw(bytes.begin(),bytes.end());
    Need(raw.find('\0')==std::string::npos);
    auto j=Json::parse(raw,[](int depth,Json::parse_event_t,Json&){Need(depth<=8);return true;});
    Need(j.is_object()&&j.size()==5&&j.at("version").is_number_integer()&&j.at("version")==1);
    Need(j.at("store_id")==out.identity.StoreId()&&j.at("host_spki_sha256")==Control::Hex(out.identity.Pin()));
    auto revision=j.at("revision").get<std::string>();Need(!revision.empty()&&revision.size()<=20&&revision[0]!='0');
    auto r=std::from_chars(revision.data(),revision.data()+revision.size(),out.revision);Need(r.ec==std::errc{}&&r.ptr==revision.data()+revision.size()&&out.revision>0);
    if(!j.at("peer").is_null()){
        const auto& p=j.at("peer");Need(p.is_object()&&p.size()==4);Control::TrustRecord peer;
        peer.transaction=Control::Session(p.at("transaction").get<std::string>());Need(std::any_of(peer.transaction.begin(),peer.transaction.end(),[](auto v){return v!=0;}));
        peer.clientPin=Control::Digest(p.at("client_spki_sha256").get<std::string>());peer.hostPin=Control::Digest(p.at("host_spki_sha256").get<std::string>());Need(peer.hostPin==out.identity.Pin());
        auto cert=Crypto::DecodeBase64Strict(p.at("client_certificate_der").get<std::string>());Need(cert.has_value());peer.clientCertificate=std::move(*cert);ValidateCertificate(peer.clientCertificate,peer.clientPin,false);out.peer=std::move(peer);
    }
    // Canonical comparison also rejects duplicate/unknown fields, alternate spellings and trailing data.
    Need(EncodeTrust(out)==raw);return out;
}
std::filesystem::path Unique(const std::filesystem::path& target,const wchar_t* suffix) {
    std::array<std::uint8_t,16> id{};Need(RAND_bytes(id.data(),static_cast<int>(id.size()))==1,StoreError::Io);auto hex=Control::Hex(id);
    return target.wstring()+L"."+std::wstring(hex.begin(),hex.end())+suffix;
}
void WriteNew(const std::filesystem::path& path,std::span<const std::uint8_t> bytes,const WindowsStore::FaultHook& hook={}) {
    Handle h(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    Need(h.value!=INVALID_HANDLE_VALUE,StoreError::Io);CheckFile(h.value);DWORD n=0;
    Need(bytes.size()<=65536&&WriteFile(h.value,bytes.data(),static_cast<DWORD>(bytes.size()),&n,nullptr)!=0&&n==bytes.size(),StoreError::Io);
    if(hook)hook(StoreStage::TempWritten);Need(FlushFileBuffers(h.value)!=0,StoreError::Io);if(hook)hook(StoreStage::TempFlushed);h.Close();
}
StoreResult Publish(const std::filesystem::path& target,std::span<const std::uint8_t> bytes,bool replace,const WindowsStore::FaultHook& hook={}) {
    bool attempted=false;std::filesystem::path temp;
    try {
        // Inspect target before publication; never overwrite a reparse point or hardlink.
        if(Exists(target)){auto old=Read(target);Need(replace,StoreError::AlreadyExists);}
        temp=Unique(target,L".tmp");WriteNew(temp,bytes,hook);if(hook)hook(StoreStage::BeforePublish);
        attempted=true;Need(MoveFileExW(temp.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH|(replace?MOVEFILE_REPLACE_EXISTING:0))!=0,StoreError::Io);
        if(hook)hook(StoreStage::Published);
        Handle h(CreateFileW(target.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_WRITE_THROUGH|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        Need(h.value!=INVALID_HANDLE_VALUE,StoreError::Io);CheckFile(h.value);Need(FlushFileBuffers(h.value)!=0,StoreError::Io);h.Close();
        if(hook)hook(StoreStage::TargetFlushed);if(hook)hook(StoreStage::BeforeReadback);
        auto read=Read(target);Need(read.size()==bytes.size()&&std::equal(read.begin(),read.end(),bytes.begin()));
        return {Outcome::Committed,StoreError::None};
    } catch(const Failure& error) {
        if(!temp.empty())DeleteFileW(temp.c_str());return {attempted?Outcome::Indeterminate:Outcome::NotCommitted,error.error};
    } catch(...) {
        if(!temp.empty())DeleteFileW(temp.c_str());return {attempted?Outcome::Indeterminate:Outcome::NotCommitted,StoreError::Io};
    }
}
std::span<const std::uint8_t> AsBytes(const std::string& s){return {reinterpret_cast<const std::uint8_t*>(s.data()),s.size()};}
bool Same(const Control::TrustRecord& a,const Control::TrustRecord& b) {
    return a.transaction==b.transaction&&a.hostPin==b.hostPin&&a.clientPin==b.clientPin&&a.clientCertificate==b.clientCertificate;
}
}
WindowsStore::WindowsStore(std::filesystem::path path,FaultHook hook):root(std::move(path)),fault(std::move(hook)){}
StoreResult WindowsStore::Initialize() {
    bool identityPublished=false;
    try {
        Guard guard(root,true);Need(!Exists(root/L"identity/host.dpapi")&&!Exists(root/L"trust.json"),StoreError::AlreadyExists);
        // Do not silently recover an interrupted first initialization from orphan files.
        for(const auto& p:std::filesystem::directory_iterator(root/L"identity")){(void)p;throw Failure{StoreError::AlreadyExists};}
        Snapshot s;s.identity=Identity::Generate();s.revision=1;Secret secret{s.identity.Encode()};auto protectedBytes=Protect(secret.bytes);
        auto result=Publish(root/L"identity/host.dpapi",protectedBytes,false,fault);
        if(!result.Committed())return result;identityPublished=true;
        auto trust=EncodeTrust(s);result=Publish(root/L"trust.json",AsBytes(trust),false,fault);
        if(!result.Committed())return {Outcome::Indeterminate,result.error};
        auto verified=LoadUnlocked(root);Need(verified.identity.Pin()==s.identity.Pin()&&verified.revision==1&&!verified.peer);
        return {Outcome::Committed,StoreError::None};
    } catch(const Failure& error){return {identityPublished?Outcome::Indeterminate:Outcome::NotCommitted,error.error};}
      catch(...){return {identityPublished?Outcome::Indeterminate:Outcome::NotCommitted,StoreError::Invalid};}
}
LoadResult WindowsStore::Load() const {
    try{Guard guard(root,false);return {StoreError::None,LoadUnlocked(root)};}
    catch(const Failure& error){return {error.error,std::nullopt};}catch(...){return {StoreError::Invalid,std::nullopt};}
}
StoreResult WindowsStore::Commit(const Control::TrustRecord& record,std::uint64_t expectedRevision) {
    try {
        Guard guard(root,false);auto current=LoadUnlocked(root);Need(record.hostPin==current.identity.Pin());ValidateCertificate(record.clientCertificate,record.clientPin);
        Need(std::any_of(record.transaction.begin(),record.transaction.end(),[](auto v){return v!=0;}));
        if(current.peer&&current.peer->transaction==record.transaction){Need(Same(*current.peer,record),StoreError::Conflict);return {Outcome::Committed,StoreError::None};}
        Need(current.revision==expectedRevision&&current.revision<std::numeric_limits<std::uint64_t>::max(),StoreError::Conflict);
        Need(!current.peer||current.peer->clientPin==record.clientPin,StoreError::DifferentPeer);
        ++current.revision;current.peer=record;auto raw=EncodeTrust(current);return Publish(root/L"trust.json",AsBytes(raw),true,fault);
    } catch(const Failure& error){return {Outcome::NotCommitted,error.error};}catch(...){return {Outcome::NotCommitted,StoreError::Invalid};}
}
StoreResult WindowsStore::ResetTrust(std::uint64_t expectedRevision) {
    try {
        Guard guard(root,false);auto current=LoadUnlocked(root);
        Need(current.revision==expectedRevision&&current.revision<std::numeric_limits<std::uint64_t>::max(),StoreError::Conflict);
        auto old=Read(root/L"trust.json");auto backup=Unique(root/L"trust.json",L".backup");WriteNew(backup,old);
        Need(Read(backup)==old,StoreError::Io);++current.revision;current.peer.reset();auto raw=EncodeTrust(current);
        return Publish(root/L"trust.json",AsBytes(raw),true,fault);
    } catch(const Failure& error){return {Outcome::NotCommitted,error.error};}catch(...){return {Outcome::NotCommitted,StoreError::Invalid};}
}
}
