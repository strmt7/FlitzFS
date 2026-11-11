#include "flitzfs/projection.hpp"
#include "native_windows_fixture.hpp"
#include <atomic>
#include <iostream>
#include <optional>
#include <thread>

namespace {
using namespace flitzfs;
using namespace native_windows_fixture;
template<class F> void rejects(F&& action,const char* why) {
  try{action();}catch(const std::exception&){return;}throw std::runtime_error(why);
}
}
namespace {
void os_basic_oracle(Fixture& fixture) {
  static_assert(FILE_ATTRIBUTE_READONLY==1 && FILE_ATTRIBUTE_HIDDEN==2 &&
      FILE_ATTRIBUTE_SYSTEM==4 && FILE_ATTRIBUTE_DIRECTORY==16 &&
      FILE_ATTRIBUTE_ARCHIVE==32 && FILE_ATTRIBUTE_TEMPORARY==256 &&
      FILE_ATTRIBUTE_NOT_CONTENT_INDEXED==8192);
  struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
  } handle;
  handle.value=CreateFileW((fixture.root/L"metadata-oracle.bin").c_str(),
      GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
  require(handle.value!=INVALID_HANDLE_VALUE,"new hosted Windows metadata oracle");
  const auto expected=sample_basic();FILE_BASIC_INFO request{};
  request.CreationTime.QuadPart=static_cast<LONGLONG>(expected.creation_time);
  request.LastAccessTime.QuadPart=static_cast<LONGLONG>(expected.last_access_time);
  request.LastWriteTime.QuadPart=static_cast<LONGLONG>(expected.last_write_time);
  request.ChangeTime.QuadPart=static_cast<LONGLONG>(expected.change_time);
  request.FileAttributes=FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_ARCHIVE|FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
  require(SetFileInformationByHandle(handle.value,FileBasicInfo,&request,sizeof(request)),
      "current Windows basic metadata oracle set");
  const auto check=[&] {
    FILE_BASIC_INFO readback{};
    require(GetFileInformationByHandleEx(handle.value,FileBasicInfo,&readback,sizeof(readback)),
        "current Windows basic metadata oracle query");
    require(readback.CreationTime.QuadPart==request.CreationTime.QuadPart &&
        readback.LastAccessTime.QuadPart==request.LastAccessTime.QuadPart &&
        readback.LastWriteTime.QuadPart==request.LastWriteTime.QuadPart &&
        readback.ChangeTime.QuadPart==request.ChangeTime.QuadPart &&
        readback.FileAttributes==request.FileAttributes,"Windows full basic tuple readback differs");
  };
  check();FILE_BASIC_INFO preserve{};
  require(SetFileInformationByHandle(handle.value,FileBasicInfo,&preserve,sizeof(preserve)),
      "current Windows zero basic request");
  check();
  std::cout<<"Windows basic oracle: explicit tuple and zero-preserve passed; hosted Server only\n";
}
}
int main(){try {
  Fixture fixture;os_basic_oracle(fixture);const auto expected=make_image(fixture.image(),false);
  const auto original=read_image(fixture.image());const auto bytes=data();
  const auto flushed=fixture.root/L"flushed.fltx";
  const auto synced=make_image(flushed,false,kNativeMaxFileBytes,true);
  require(synced.sequence==expected.sequence && synced.storage.generation==expected.storage.generation &&
      read_image(flushed)==original,"real ordinary-file flush altered native image bytes/generation");
  NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open_file(flushed)).verify_all();
  {
    auto view=ReadOnlyProjection::open_native_file(fixture.image(),identity());
    const auto info=view.native_info();
    require(info && info->identity==identity() && info->namespace_sequence==expected.sequence &&
        info->storage.physical_end==expected.storage.physical_end && info->image_bytes==original.size() &&
        info->storage.replicated_checkpoint && !info->storage.degraded_checkpoint,
        "native view lost persisted volume/checkpoint metadata");
    require(view.stat(L"\\").inode==1 && view.stat(L"\\DATA").inode==expected.directory_inode &&
        view.stat(L"\\data\\SAMPLE.BIN").inode==expected.sample_inode,
        "native projection regenerated persistent inode identities");
    require(view.basic_metadata(L"\\Data\\sample.bin")==sample_basic() &&
        view.basic_metadata(L"\\")==root_basic() && !view.basic_metadata(L"\\empty.bin"),
        "native projection fabricated or lost persisted basic tuple");
    require(view.list(L"\\").size()==3 && view.list(L"\\Data").size()==2,
        "native view namespace differs from committed rename/remove");
    rejects([&]{(void)view.stat(L"\\discard");},"removed inode escaped snapshot");
    rejects([&]{(void)view.stat(L"\\scratch.bin");},"old renamed path escaped snapshot");
    require(view.read(L"\\Data\\sample.bin",65536-13,64)==
        Bytes(bytes.begin()+65536-13,bytes.begin()+65536+51),"cross-native-extent read");
    require(view.read(L"\\Data\\sample.bin",bytes.size()-9,1024)==Bytes(bytes.end()-9,bytes.end()),
        "native view short EOF read");
    require(view.read(L"\\Data\\sample.bin",UINT64_MAX,1).empty() &&
        view.read(L"\\Data\\sample.bin",UINT64_MAX,0).empty(),"native view overflow EOF handling");
    require(view.stat(L"\\sparse.bin").logical_bytes==kNativeMaxFileBytes &&
        view.read(L"\\sparse.bin",kNativeMaxFileBytes-31,64)==[] {
          Bytes out(31);const auto suffix=tail();std::copy(suffix.begin(),suffix.end(),out.begin()+14);return out;
        }(),"bounded terabyte sparse-tail projection");
    view.verify_all(info->storage.logical_bytes);
    rejects([&]{view.verify_all(info->storage.logical_bytes-1);},"native verification budget ignored");
    rejects([&]{(void)view.read(L"\\sparse.bin",UINT64_MAX,kNativeMaxReadBytes+1);},
        "native oversized EOF read bypassed admission");
    for(const auto* invalid:{L"",L"C:\\bad",L"relative",L"\\\\unc",L"\\..\\escape",
        L"\\Data\\NUL.txt",L"\\Data\\sample.bin:ads",L"\\Data\\x?",L"\\Data\\x/"})
      rejects([&]{(void)view.stat(invalid);},"unsafe native projected path");
    rejects([&]{(void)view.read(L"\\Data",0,1);},"native projected directory read");
    // Deny ordinary write/delete opens for the complete view lifetime.
    for(const auto access:{DWORD(GENERIC_WRITE),DWORD(DELETE)}) {
      const auto handle=CreateFileW(fixture.image().c_str(),access,
          FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
      const auto error=GetLastError();if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);
      require(handle==INVALID_HANDLE_VALUE && error==ERROR_SHARING_VIOLATION,
          "native source allowed concurrent ordinary write/delete access");
    }
    std::atomic<bool> valid{true};
    {
      std::array<std::jthread,2> workers;
      for(std::size_t n=0;n<workers.size();++n)workers[n]=std::jthread([&,n] {
        try {
          for(unsigned i=0;i<16;++i) {
            const auto offset=(i*7919+n*33)%(bytes.size()-513);
            if(view.read(L"\\DATA\\sample.bin",offset,513)!=
                Bytes(bytes.begin()+static_cast<std::ptrdiff_t>(offset),
                      bytes.begin()+static_cast<std::ptrdiff_t>(offset+513)))valid=false;
          }
        }catch(...){valid=false;}
      });
    }
    require(valid,"native shared positional decoder concurrency mismatch");
    auto moved=std::move(view);
    require(moved.stat(L"\\Data\\sample.bin").inode==expected.sample_inode,"native projection move lost source");
    rejects([&]{(void)view.stat(L"\\");},"moved-from native projection was usable");
  }
  {
    auto reopened=ReadOnlyProjection::open_native_file(fixture.image(),identity());
    require(reopened.stat(L"\\Data\\sample.bin").inode==expected.sample_inode,
        "native inode changed on independent source reopen");
  }
  const auto legacy=fixture.root/L"legacy-basic.fltx";
  make_image(legacy,false,kNativeMaxFileBytes,false,false);
  {
    auto view=ReadOnlyProjection::open_native_file(legacy,identity());
    require(!view.basic_metadata(L"\\Data\\sample.bin") && !view.basic_metadata(L"\\"),
        "legacy native image fabricated persisted timestamps");
  }
  auto other=identity();other[1]=std::byte{1};
  rejects([&]{(void)ReadOnlyProjection::open_native_file(fixture.image(),other);},"wrong volume identity accepted");
  rejects([&]{(void)ReadOnlyProjection::open_native_file(fixture.image(),{});},"wildcard volume identity accepted");
  const auto damaged=fixture.root/L"damaged.fltx";
  require(std::filesystem::copy_file(fixture.image(),damaged),"new corruption fixture copy");
  {
    std::ofstream stream(damaged,std::ios::binary|std::ios::app);const Bytes tail(kStoreCommitUnit);
    stream.write(reinterpret_cast<const char*>(tail.data()),static_cast<std::streamsize>(tail.size()));
    require(bool(stream),"owned uncertain-tail fixture append");
  }
  {
    auto with_tail=ReadOnlyProjection::open_native_file(damaged,identity());const auto info=with_tail.native_info();
    require(info && info->image_bytes==original.size()+kStoreCommitUnit &&
        info->storage.physical_end==original.size() &&
        with_tail.read(L"\\Data\\sample.bin",0,1)==Bytes(bytes.begin(),bytes.begin()+1),
        "native image tail was discarded or confused with committed end");
  }
  {
    std::fstream stream(damaged,std::ios::binary|std::ios::in|std::ios::out);
    const Bytes zeros(kStoreCommitUnit);
    stream.write(reinterpret_cast<const char*>(zeros.data()),static_cast<std::streamsize>(zeros.size()));
    require(bool(stream),"owned single-root corruption");
  }
  {
    auto readable=ReadOnlyProjection::open_native_file(damaged,identity());
    const auto health=readable.native_info();
    require(health && health->storage.degraded_checkpoint &&
        health->namespace_sequence==expected.sequence &&
        readable.read(L"\\Data\\sample.bin",0,static_cast<std::uint32_t>(bytes.size()))==bytes,
        "read-only replicated recovery lost committed contents or hid degradation");
  }
  {
    std::fstream stream(damaged,std::ios::binary|std::ios::in|std::ios::out);
    require(bool(stream),"owned damaged image open");
    // Damage both root failure units, not just one recoverable metadata replica.
    const Bytes zeros(2*kStoreCommitUnit);
    stream.write(reinterpret_cast<const char*>(zeros.data()),static_cast<std::streamsize>(zeros.size()));
    require(bool(stream),"owned damaged image write");
  }
  rejects([&]{(void)ReadOnlyProjection::open_native_file(damaged,identity());},
      "corrupt native source exposed a projection");
  const auto generic=fixture.root/L"generic.fltx";
  {
    auto writer=ObjectStoreWriter::create_file(generic,identity(),{4096,true});
    const StoreMutation object{1,false,Bytes(32,std::byte{4})};writer.commit({&object,1});
  }
  rejects([&]{(void)ReadOnlyProjection::open_native_file(generic,identity());},
      "generic object store was treated as a native filesystem");
  for(const bool detach:{false,true}) {
    const auto lifetime=fixture.root/(detach?L"unlinked.fltx":L"pending.fltx");
    {
      auto writer=NativeFilesystemWriter::initialize(ObjectStoreWriter::create_file(
          lifetime,identity(),{4096,true}));
      const auto inode=writer.apply(std::array<NativeFileMutation,1>{create("pending")}).
          created_inodes[0];
      writer.set_delete_pending(inode,true);
      if(detach)writer.unlink_inode(inode);
    }
    const auto source=read_image(lifetime);
    require(NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open_file(lifetime)).
        needs_session_recovery(),"lifetime admission fixture is not a valid unresolved native image");
    rejects([&]{(void)ReadOnlyProjection::open_native_file(lifetime,identity());},
        "unresolved file contexts entered the read-only Windows projection");
    require(read_image(lifetime)==source,"lifetime admission rejection mutated source");
  }
  require(read_image(fixture.image())==original,"read-only native view changed source image");
  std::cout<<"PASS: native Windows image projection, persistent inodes, sparse tails, concurrent verified reads, source sharing and no backing changes.\n";
  return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
