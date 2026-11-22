#include "hip_uniform_lz4_compute.hpp"
#include "hip_decode_worker.hpp"
#include "hip_module_verification.hpp"
#include "gpu_budget.hpp"
#include "../native_fs_fixture.hpp"
#include "../native_lz4_fixture.hpp"
#include "flitzfs/native_session.hpp"
#include "flitzfs/paged_store.hpp"
#include <chrono>
#include <iostream>
#include <utility>

namespace {
using namespace flitzfs;
using namespace flitzfs::detail;
using native_fixture::Bytes;

void require(bool condition,const char* message) {
  if(!condition)throw std::runtime_error(message);
}

std::string ascii(std::wstring_view value) {
  require(std::ranges::all_of(value,[](wchar_t c){return c>0 && c<128;}),"non-ASCII probe argument");
  return {value.begin(),value.end()};
}

std::array<std::byte,8> parse_luid(std::wstring_view value) {
  const auto text=ascii(value);
  require(text.size()==16,"invalid LUID length");
  const auto digit=[](char c) {
    if(c>='0' && c<='9')return unsigned(c-'0');
    require(c>='a' && c<='f',"invalid LUID digit");
    return unsigned(c-'a'+10);
  };
  std::array<std::byte,8> luid{};
  for(std::size_t i=0;i<luid.size();++i)
    luid[i]=static_cast<std::byte>(16*digit(text[2*i])+digit(text[2*i+1]));
  return luid;
}

std::filesystem::path module_path(HMODULE module) {
  std::wstring path(32768,L'\0');
  const auto length=GetModuleFileNameW(module,path.data(),static_cast<DWORD>(path.size()));
  require(length && length<path.size(),"unavailable module path");
  path.resize(length);
  return path;
}

class LoadedModule final {
 public:
  explicit LoadedModule(const std::filesystem::path& path) {
    handle_=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    require(handle_!=nullptr,"verified module could not load");
    try {
      require(std::filesystem::equivalent(path,module_path(handle_)),"module resolved to a different file");
    }catch(...) {FreeLibrary(handle_);throw;}
  }
  ~LoadedModule(){if(handle_)FreeLibrary(handle_);}
  LoadedModule(const LoadedModule&)=delete;
  LoadedModule& operator=(const LoadedModule&)=delete;
  HMODULE get() const noexcept{return handle_;}
  void close(){if(auto h=std::exchange(handle_,nullptr))require(FreeLibrary(h)!=0,"module retirement failed");}
 private:
  HMODULE handle_=nullptr;
};

struct CapturedFrame {
  NativeLz4ReadKey key;
  Bytes encoded;
};
class ReadRoute final:public NativeLz4ReadRoute {
 public:
  std::array<CapturedFrame,2> captured;
  std::vector<NativeLz4DecodedFrame> ready;
  std::size_t count=0,hits=0,rejected=0;
  bool collecting=true;
  std::optional<Bytes> try_ready(const NativeLz4ReadKey& key,std::span<const std::byte> encoded) noexcept override {
    if(collecting && count<captured.size()) {
      try {
        captured[count]={key,{encoded.begin(),encoded.end()}};
        ++count;
      }catch(const std::exception&) {}
    }
    if(!collecting) {
      const auto match=std::ranges::find(ready,key,&NativeLz4DecodedFrame::key);
      if(match!=ready.end() && !match->bytes.empty()) {
        ++hits;
        return std::exchange(match->bytes,Bytes{});
      }
    }
    return {};
  }
  void reject(const NativeLz4ReadKey&) noexcept override{++rejected;}
};

void run(std::wstring_view runtime_name,std::wstring_view runtime_sha,std::wstring_view module_sha,
    std::wstring_view adapter,std::wstring_view architecture,std::wstring_view ordinal_text,
    const std::optional<std::filesystem::path>& corpus) {
  require_hip_worker_job(true);
  require(store_lz4_available(),"native LZ4 CPU reference unavailable");
  require(ordinal_text.size()==1 && ordinal_text[0]>=L'0' && ordinal_text[0]<=L'9',"invalid HIP ordinal");
  const int ordinal=ordinal_text[0]-L'0';
  const auto luid=parse_luid(adapter);
  const auto arch=ascii(architecture);
  const auto wave_mask=arch=="gfx1201"?1U:arch=="gfx90a"?2U:0U;
  require(wave_mask!=0,"architecture not in the compiled fixture");
  const hip_detail::Lz4KernelCoverage coverage{arch,wave_mask};

  auto backing=std::make_shared<native_fixture::Ram>();
  backing->failure_unit=4096;
  constexpr PagedStoreLimits limits{4U*1024U*1024U};
  constexpr StoreCompressionOptions fast{StoreCompressionMode::fast,1,false};
  auto writer=NativeFilesystemWriter::initialize_indexed(
      PagedObjectStoreWriter::create_mirrored(backing,native_fixture::identity(),limits,fast));
  const auto create=native_fixture::file_create("read-data");
  writer.apply({&create,1});
  auto session=NativeFileSession::create(std::move(writer));
  const auto reader=session.open("read-data",{true,true,true});
  Bytes first(kNativeExtentBytes),second(kNativeExtentBytes-37);
  if(corpus) {
    const auto content=ReadOnlyFile::open(*corpus);
    require(content.size()>=first.size()+second.size(),"public fixture is shorter than the requested prefix");
    content.read_exact_at(0,first.data(),first.size());
    content.read_exact_at(first.size(),second.data(),second.size());
    (void)session.write(reader,0,first);
  }else {
    first=native_lz4_fixture::source();
    const auto block=native_lz4_fixture::alternative_block(first);
    const NativeLz4ExtentCandidate hint{0,block};
    (void)session.write_storage_with_lz4_result(reader,0,first,{&hint,1});
    std::copy_n(first.begin(),second.size(),second.begin());
    second[31]^=std::byte{1};
  }
  (void)session.write(reader,kNativeExtentBytes,second);
  const auto first_size=static_cast<std::uint32_t>(first.size());
  const auto second_size=static_cast<std::uint32_t>(second.size());
  ReadRoute route;
  require(session.read(reader,0,first_size,&route)==first &&
      session.read(reader,kNativeExtentBytes,second_size,&route)==second &&
      route.count==2,"independent CPU native reads or frame capture differ");
  route.collecting=false;
  const std::array sources{
      NativeLz4DecodeSource{route.captured[0].key,route.captured[0].encoded},
      NativeLz4DecodeSource{route.captured[1].key,route.captured[1].encoded}};
  constexpr NativeLz4DecodeGrant grant{2,256U*1024U};
  const std::array batches{NativeLz4DecodeBatch::prepare({&sources[0],1},grant),
      NativeLz4DecodeBatch::prepare({&sources[1],1},grant)};
  const auto peak=(std::max)(batches[0].upload().size(),batches[1].upload().size());

  WindowsGpuBudgetSampler gpu(luid,1);
  const auto available=gpu.sample().snapshot;
  require(check_gpu_memory_budget(available,luid,1,GetTickCount64(),peak)==GpuGateCode::admitted,
      "current AMD GPU budget refused the probe");
  const hip_detail::VerifiedSystemModule system(runtime_name,ascii(runtime_sha));
  const auto native=module_path(nullptr).parent_path()/"flitzfs_rocm_native_lz4_decode.dll";
  const hip_detail::DirectoryPin pin(native.parent_path());
  const auto pinned=ReadOnlyFile::open(native);
  require(hip_detail::matches_sha256(pinned,ascii(module_sha)),"native decode module hash mismatch");
  const auto cpu_start=std::chrono::steady_clock::now();
  require(session.read(reader,0,first_size)==first &&
      session.read(reader,kNativeExtentBytes,second_size)==second,
      "matched CPU fixture reads failed");
  const auto cpu_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now()-cpu_start).count();
  const auto started=std::chrono::steady_clock::now();
  LoadedModule runtime(system.path()),kernel(native);
  const auto address=GetProcAddress(kernel.get(),"flitzfs_native_lz4_decode_api");
  require(address!=nullptr,"native decode function table unavailable");
  const auto entry=std::bit_cast<native_lz4_decode::KernelApiEntry>(address);
  const auto* table=entry();
  require(table && native_lz4_decode::compatible(*table),"native decode function table changed");
  route.ready=run_hip_native_lz4_decode_in_worker(runtime.get(),*table,ordinal,luid,1,batches,grant,{&coverage,1});
  kernel.close();
  runtime.close();
  const auto gpu_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now()-started).count();
  require(session.read(reader,0,first_size,&route)==first &&
      session.read(reader,kNativeExtentBytes,second_size,&route)==second &&
      route.hits==2 && !route.rejected,"verified AMD bytes did not reach both native file reads");
  require(session.read(reader,0,first_size,&route)==first && route.hits==2,
      "CPU fallback after consuming ready results failed");
  session.snapshot().verify_all();
  std::cout<<"native_lz4_hip {\"architecture\":\""<<arch
      <<"\",\"frames\":2,\"gpu_launched\":true,\"bytes_exact\":true,\"cpu_fallback_exact\":true"
      <<",\"first_decoded_bytes\":"<<sources[0].key.decoded_bytes
      <<",\"second_decoded_bytes\":"<<sources[1].key.decoded_bytes
      <<",\"kernel_launches\":"<<batches.size()<<",\"reused_device_slab_bytes\":"<<peak
      <<",\"module_and_compute_ns\":"<<gpu_ns
      <<",\"cpu_session_read_ns\":"<<cpu_ns<<",\"public_corpus\":"<<(corpus?"true":"false")<<"}\n";
}
}

int wmain(int argc,wchar_t** argv) {
  try {
    require(argc==7 || argc==8,"expected runtime name and hash, module hash, LUID, architecture and HIP ordinal");
    const auto corpus=argc==8?std::optional{std::filesystem::path(argv[7])}:std::nullopt;
    run(argv[1],argv[2],argv[3],argv[4],argv[5],argv[6],corpus);
  }catch(const flitzfs::HipWorkerFailure& failure) {
    std::cerr<<failure.what()<<" (stage "<<static_cast<unsigned>(failure.stage)<<")\n";
    return 2;
  }catch(const std::exception& failure) {
    std::cerr<<failure.what()<<'\n';
    return 1;
  }
}
