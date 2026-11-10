#include "winfsp_readonly.hpp"
#include "native_windows_fixture.hpp"
#include "native_security_fixture.hpp"
#include <sddl.h>
#include <cstring>
#include <cstddef>
#include <iostream>
namespace {
using namespace flitzfs;
using namespace native_windows_fixture;
namespace sec=native_security_fixture;
unsigned checks=0;
void check(bool ok,const char* why){++checks;require(ok,why);}
Bytes sddl(PCWSTR text) {
  PSECURITY_DESCRIPTOR raw=nullptr;ULONG size=0;
  check(ConvertStringSecurityDescriptorToSecurityDescriptorW(text,SDDL_REVISION_1,&raw,&size),
      "Windows SDDL conversion");
  struct Free {PSECURITY_DESCRIPTOR value;~Free(){LocalFree(value);}} release{raw};
  check(size<=kNativeMaxSecurityBytes && IsValidSecurityDescriptor(raw),"Windows descriptor validity/size");
  const auto* begin=reinterpret_cast<const std::byte*>(raw);
  Bytes result(begin,begin+size);
  check(native_security_descriptor_framed(result),"actual Windows descriptor framing");return result;
}
void os_oracles() {
  static_assert(sizeof(SECURITY_DESCRIPTOR_RELATIVE)==20 &&
      offsetof(SECURITY_DESCRIPTOR_RELATIVE,Owner)==4 &&
      offsetof(SECURITY_DESCRIPTOR_RELATIVE,Group)==8 &&
      offsetof(SECURITY_DESCRIPTOR_RELATIVE,Sacl)==12 &&
      offsetof(SECURITY_DESCRIPTOR_RELATIVE,Dacl)==16);
  static_assert(sizeof(ACL)==8 && sizeof(ACE_HEADER)==4 &&
      offsetof(SID,SubAuthority)==8 && SID_MAX_SUB_AUTHORITIES==15 &&
      ACL_REVISION==2 && ACL_REVISION_DS==4 && SE_SELF_RELATIVE==0x8000);
  for(auto value:{sec::descriptor(),sec::null_dacl(),sec::null_dacl(false),sec::empty_dacl()}) {
    check(native_security_descriptor_framed(value) && IsValidSecurityDescriptor(value.data()),
        "manual descriptor differs from Windows validity");
    BOOL present=FALSE,defaulted=FALSE;PACL acl=nullptr;
    check(GetSecurityDescriptorDacl(value.data(),&present,&acl,&defaulted),"Windows DACL query");
    check(bool(present)==bool(sec::get(value,2,2)&SE_DACL_PRESENT),"absent versus present DACL lost");
    if(present)check((acl==nullptr)==(sec::get(value,16)==0),"NULL versus empty DACL lost");
  }
  // Current OS-produced simple, inherited, protected, audit and mandatory-label
  // descriptors. No privilege mutation or real-file ACL write is performed.
  for(const auto* text:{L"O:SYG:BAD:P(A;;GR;;;WD)",L"O:SYG:BAD:(A;OICI;GA;;;SY)",
      L"O:SYG:BAD:(D;;GW;;;WD)(A;;GR;;;WD)",L"O:SYG:BAD:S:(AU;SA;GR;;;WD)",
      L"O:SYG:BAD:S:(ML;;NW;;;LW)"}) {
    auto value=sddl(text);check(IsValidSecurityDescriptor(value.data()),"OS-produced security changed");
  }
  auto original=sddl(L"O:SYG:BAD:(A;;GA;;;SY)");
  auto modification=sddl(L"D:P(D;;GW;;;WD)(A;;GR;;;WD)");
  PSECURITY_DESCRIPTOR merged=nullptr;
  check(FspSetSecurityDescriptor(original.data(),DACL_SECURITY_INFORMATION,modification.data(),&merged)==STATUS_SUCCESS,
      "selected SDK security merge");
  struct FreeFsp {
    PSECURITY_DESCRIPTOR value;
    ~FreeFsp(){if(value)FspDeleteSecurityDescriptor(value,(NTSTATUS (*)())FspSetSecurityDescriptor);}
  } release{merged};
  const auto length=GetSecurityDescriptorLength(merged);
  check(length<=kNativeMaxSecurityBytes && IsValidSecurityDescriptor(merged) &&
      native_security_descriptor_framed({reinterpret_cast<const std::byte*>(merged),length}),
      "SDK merge output cannot be persisted");
  PSID owner_before=nullptr,owner_after=nullptr;BOOL ignored=FALSE;
  check(GetSecurityDescriptorOwner(original.data(),&owner_before,&ignored) &&
      GetSecurityDescriptorOwner(merged,&owner_after,&ignored) && EqualSid(owner_before,owner_after),
      "DACL-only SDK update changed owner");
}
void callbacks() {
  Fixture fixture;auto root=sddl(L"O:SYG:BAD:P(A;;GA;;;SY)");
  auto file=sddl(L"O:SYG:BAD:P(D;;GW;;;WD)(A;;GR;;;WD)");
  {
    auto writer=NativeFilesystemWriter::initialize(ObjectStoreWriter::create_file(
        fixture.image(),identity(),{4096,true},{StoreCompressionMode::automatic,6,false}));
    const auto operation=create("f");
    const std::array<NativeBasicMutation,2> basic{{{1,0,root_basic()},{0,1,sample_basic()}}};
    const std::array<NativeSecurityMutation,2> security{{{1,0,root},{0,1,file}}};
    writer.apply_with_metadata({&operation,1},basic,security);
    check(writer.snapshot().security_descriptor(1)==root &&
        writer.snapshot().security_descriptor(2)==file,"ordinary file security publication");
  }
  const auto before=read_image(fixture.image());
  {
    auto projection=ReadOnlyProjection::open_native_file(fixture.image(),identity());
    check(projection.security_descriptor(L"\\")==root && projection.security_descriptor(L"\\f")==file,
        "independent native projection descriptor mismatch");
    WinFspReadOnly adapter(std::move(projection));FSP_FILE_SYSTEM fs{};adapter.bind_callbacks(fs);
    const auto& cb=WinFspReadOnly::callbacks();
    for(const auto* path:{L"\\",L"\\f"}) {
      const auto& expected=std::wstring_view(path)==L"\\"?root:file;
      SIZE_T length=0;UINT32 attributes=0;
      check(cb.GetSecurityByName(&fs,const_cast<PWSTR>(path),&attributes,nullptr,&length)==STATUS_BUFFER_OVERFLOW &&
          length==expected.size(),"per-inode security sizing");
      Bytes output(length,std::byte{0x5a});
      SIZE_T short_size=length-1;
      check(cb.GetSecurityByName(&fs,const_cast<PWSTR>(path),nullptr,output.data(),&short_size)==STATUS_BUFFER_OVERFLOW &&
          short_size==length && output==Bytes(length,std::byte{0x5a}),"short security buffer modified");
      check(cb.GetSecurityByName(&fs,const_cast<PWSTR>(path),nullptr,output.data(),&length)==STATUS_SUCCESS &&
          output==expected && IsValidSecurityDescriptor(output.data()),"persisted security-by-name callback");
      PVOID context=nullptr;FSP_FSCTL_FILE_INFO info{};
      check(cb.Open(&fs,const_cast<PWSTR>(path),0,FILE_GENERIC_READ,&context,&info)==STATUS_SUCCESS,"descriptor read-only context");
      length=output.size();std::fill(output.begin(),output.end(),std::byte{0});
      check(cb.GetSecurity(&fs,context,output.data(),&length)==STATUS_SUCCESS && output==expected,
          "persisted descriptor context callback");
      check(cb.SetSecurity(&fs,context,DACL_SECURITY_INFORMATION,output.data())==STATUS_MEDIA_WRITE_PROTECTED,
          "security persistence enabled mounted writes");
      cb.Close(&fs,context);
      check(cb.GetSecurity(&fs,context,output.data(),&length)==STATUS_INVALID_HANDLE,"stale security context accepted");
    }
  }
  check(read_image(fixture.image())==before,"read-only security callbacks altered backing image");
}
}
int main(){try {
  hosted();os_oracles();callbacks();
  std::cout<<"Windows security evidence: {\"schema\":1,\"checks\":"<<checks
      <<",\"complete\":true,\"mount\":false,\"scope\":\"OS/SDK validation and native persisted read-only callbacks; not ACL authorization or mounted writes\"}\n";
  return 0;
}catch(const std::exception& e){std::cerr<<"Windows security failure: "<<e.what()<<"\n";return 1;}}
