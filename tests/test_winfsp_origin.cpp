#include "winfsp_native.hpp"
#include "winfsp_readonly.hpp"
#include "native_windows_fixture.hpp"
#include <array>
#include <algorithm>
#include <iostream>
#include <stdexcept>
namespace {
using namespace flitzfs;
namespace nw=native_windows_fixture;
enum class Origin {self,foreign,kernel,throws};
Origin selected=Origin::self;
unsigned queries=0;
UINT32 query() {
  ++queries;
  switch(selected) {
    case Origin::self:return GetCurrentProcessId();
    case Origin::foreign:return GetCurrentProcessId()==1?2:1;
    case Origin::kernel:return 0;
    case Origin::throws:throw std::runtime_error("synthetic origin failure");
  }
  throw std::runtime_error("foreign synthetic origin");
}
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
void cleared(PVOID context,const FSP_FSCTL_FILE_INFO& info) {
  require(!context&&!info.FileSize&&!info.AllocationSize&&!info.IndexNumber&&!info.FileAttributes,
      "denied open exposed a context or metadata");
}
void readonly() {
  nw::Fixture fixture;(void)nw::make_image(fixture.image(),false);
  auto oracle=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open_file(fixture.image()));
  const auto expected=oracle.read("Data/sample.bin",0,17);
  WinFspReadOnly adapter(ReadOnlyProjection::open_native_file(fixture.image(),nw::identity()));
  FSP_FILE_SYSTEM fs{};fs.Version=static_cast<UINT16>(sizeof fs);
  fs.UmFileContextIsUserContext2=TRUE;fs.Interface=&WinFspReadOnly::callbacks();
  adapter.bind_callbacks(fs,query);
  const auto path=const_cast<PWSTR>(L"\\Data\\sample.bin");
  PVOID context=reinterpret_cast<PVOID>(1);FSP_FSCTL_FILE_INFO info{};info.FileSize=99;
  selected=Origin::self;
  const auto own=fs.Interface->Open(&fs,path,0,FILE_GENERIC_READ,&context,&info);
  if(context)fs.Interface->Close(&fs,context);
  require(own==STATUS_ACCESS_DENIED&&adapter.self_origin_rejected(),"same-process readonly Open admission regression");
  cleared(context,info);
  adapter.bind_callbacks(fs,nullptr);context=reinterpret_cast<PVOID>(1);info.FileSize=99;
  require(fs.Interface->Open(&fs,path,0,FILE_GENERIC_READ,&context,&info)==STATUS_ACCESS_DENIED&&
      !adapter.self_origin_rejected(),"missing readonly origin provider was not closed");
  cleared(context,info);
  adapter.bind_callbacks(fs,query);selected=Origin::throws;context=reinterpret_cast<PVOID>(1);info.FileSize=99;
  require(fs.Interface->Open(&fs,path,0,FILE_GENERIC_READ,&context,&info)==STATUS_ACCESS_DENIED,
      "throwing readonly origin escaped admission");cleared(context,info);
  for(auto origin:{Origin::foreign,Origin::kernel}) {
    selected=origin;
    require(fs.Interface->Open(&fs,path,0,FILE_GENERIC_READ,&context,&info)==STATUS_SUCCESS&&context,
        "foreign/kernel readonly open changed");
    const auto before=queries;selected=Origin::throws;
    std::array<std::byte,17> bytes{};ULONG count=0;
    require(fs.Interface->Read(&fs,context,bytes.data(),0,static_cast<ULONG>(bytes.size()),&count)==STATUS_SUCCESS&&
        count==bytes.size()&&std::equal(bytes.begin(),bytes.end(),expected.begin()),"readonly bytes changed");
    require(fs.Interface->GetFileInfo(&fs,context,&info)==STATUS_SUCCESS,"readonly metadata changed");
    fs.Interface->Close(&fs,context);context=nullptr;
    require(queries==before,"origin queried outside Create/Open");
  }
}
void native() {
  nw::Fixture fixture;
  auto adapter=WinFspNativeWritable::create_new_hosted(fixture.image(),nw::identity());
  FSP_FILE_SYSTEM fs{};fs.Version=static_cast<UINT16>(sizeof fs);fs.UmFileContextIsUserContext2=TRUE;
  fs.Interface=&WinFspNativeWritable::callbacks();adapter->bind_callbacks(fs,query);
  SIZE_T size=0;UINT32 attributes=0;
  require(fs.Interface->GetSecurityByName(&fs,const_cast<PWSTR>(L"\\"),&attributes,nullptr,&size)==
      STATUS_BUFFER_OVERFLOW&&size&&size<=kNativeMaxSecurityBytes,"native root descriptor size");
  std::vector<std::byte> security(size);
  require(fs.Interface->GetSecurityByName(&fs,const_cast<PWSTR>(L"\\"),&attributes,security.data(),&size)==
      STATUS_SUCCESS,"native root descriptor");
  const auto before=adapter->snapshot();
  const auto path=const_cast<PWSTR>(L"\\created.bin");
  auto unchanged=[&] {
    const auto after=adapter->snapshot();
    require(before.storage_info().generation==after.storage_info().generation&&
        before.storage_info().physical_end==after.storage_info().physical_end&&after.list().empty()&&
        !adapter->stats().contexts&&!adapter->requires_offline(),"denied native origin changed storage/session");
  };
  selected=Origin::self;
  PVOID context=reinterpret_cast<PVOID>(1);FSP_FSCTL_FILE_INFO info{};info.FileSize=99;
  const auto own=fs.Interface->Create(&fs,path,FILE_NON_DIRECTORY_FILE,FILE_ALL_ACCESS,
      FILE_ATTRIBUTE_NORMAL,security.data(),0,&context,&info);
  if(context)fs.Interface->Close(&fs,context);
  require(own==STATUS_ACCESS_DENIED&&adapter->self_origin_rejected(),"same-process native Create admission regression");
  cleared(context,info);unchanged();
  context=reinterpret_cast<PVOID>(1);info.FileSize=99;
  require(fs.Interface->Open(&fs,const_cast<PWSTR>(L"\\"),FILE_DIRECTORY_FILE,FILE_GENERIC_READ,
      &context,&info)==STATUS_ACCESS_DENIED,"same-process native Open admitted");cleared(context,info);unchanged();
  adapter->bind_callbacks(fs,nullptr);context=reinterpret_cast<PVOID>(1);info.FileSize=99;
  require(fs.Interface->Create(&fs,path,FILE_NON_DIRECTORY_FILE,FILE_ALL_ACCESS,FILE_ATTRIBUTE_NORMAL,
      security.data(),0,&context,&info)==STATUS_ACCESS_DENIED&&!adapter->self_origin_rejected(),
      "missing native origin provider was not closed");cleared(context,info);unchanged();
  adapter->bind_callbacks(fs,query);selected=Origin::throws;context=reinterpret_cast<PVOID>(1);info.FileSize=99;
  require(fs.Interface->Open(&fs,const_cast<PWSTR>(L"\\"),FILE_DIRECTORY_FILE,FILE_GENERIC_READ,
      &context,&info)==STATUS_ACCESS_DENIED,"throwing native origin escaped admission");cleared(context,info);unchanged();
  selected=Origin::foreign;
  require(fs.Interface->Create(&fs,path,FILE_NON_DIRECTORY_FILE,FILE_ALL_ACCESS,FILE_ATTRIBUTE_NORMAL,
      security.data(),0,&context,&info)==STATUS_SUCCESS&&context,"foreign native Create changed");
  std::array<std::byte,17> payload{};for(std::size_t i=0;i<payload.size();++i)payload[i]=static_cast<std::byte>(i*7+3);
  ULONG written=0;const auto calls=queries;selected=Origin::throws;
  require(fs.Interface->Write(&fs,context,payload.data(),0,static_cast<ULONG>(payload.size()),FALSE,FALSE,
      &written,&info)==STATUS_SUCCESS&&written==payload.size(),"foreign native Write changed");
  require(fs.Interface->Flush(&fs,context,&info)==STATUS_SUCCESS,"foreign native Flush changed");
  fs.Interface->Cleanup(&fs,context,path,0);fs.Interface->Close(&fs,context);context=nullptr;
  require(queries==calls&&!adapter->requires_offline()&&!adapter->stats().contexts,
      "native non-open callbacks queried origin or lost cleanup");
  require(adapter->snapshot().read("created.bin",0,17)==std::vector<std::byte>(payload.begin(),payload.end()),
      "guard changed committed CPU bytes");
  selected=Origin::kernel;
  // WinFsp encodes FILE_OPEN in the high byte. Zero means FILE_SUPERSEDE
  // and prepares an overwrite; it is not an existing-file read request.
  constexpr UINT32 open_existing=1U<<24;
  const auto committed=adapter->snapshot().storage_info().generation;
  require(fs.Interface->Open(&fs,path,open_existing,FILE_GENERIC_READ,&context,&info)==STATUS_SUCCESS&&context&&
      info.FileSize==payload.size()&&adapter->snapshot().storage_info().generation==committed,
      "kernel existing-file Open changed storage or metadata");
  const auto read_calls=queries;selected=Origin::throws;
  std::array<std::byte,17> bytes{};ULONG read=0;
  require(fs.Interface->Read(&fs,context,bytes.data(),0,static_cast<ULONG>(bytes.size()),&read)==STATUS_SUCCESS&&
      read==bytes.size()&&bytes==payload,"kernel native byte parity");
  require(queries==read_calls,"kernel Read queried origin");
  // Read adopts the prepared handle. An adopted handle must receive Cleanup
  // before Close, matching the SDK lifecycle and the existing native fixtures.
  fs.Interface->Cleanup(&fs,context,path,0);
  require(queries==read_calls,"kernel Cleanup queried origin");
  require(adapter->stats().contexts==1&&adapter->stats().cleaned==1&&!adapter->requires_offline(),
      "kernel Cleanup did not clean its adopted handle");
  fs.Interface->Close(&fs,context);context=nullptr;
  require(queries==read_calls,"kernel Close queried origin");
  require(!adapter->stats().contexts,"kernel Close leaked its cleaned context");
  require(!adapter->requires_offline(),"kernel lifecycle fenced the adapter");
}
}
int main(){try {
  nw::hosted();readonly();native();
  std::cout<<"WinFsp Create/Open origin admission passed: self/missing/throwing denied, foreign/kernel bytes preserved, no out-of-scope queries; CPU synthetic origins only, no mount/dispatcher/GPU.\n";
  return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<"\n";return 1;}}
