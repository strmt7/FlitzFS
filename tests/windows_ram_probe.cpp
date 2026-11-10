#include "locked_ram_fixture.hpp"
#include "bounded_process.hpp"
#include "flitzfs/volume_head.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string_view>
#ifndef FLITZFS_RAM_SOURCE_COMMIT
#error Bound RAM probe provenance is required
#endif
namespace {
using namespace flitzfs;using Bytes=std::vector<std::byte>;namespace ram=locked_ram_fixture;
struct Handle {
  HANDLE value=INVALID_HANDLE_VALUE;
  explicit Handle(HANDLE handle=INVALID_HANDLE_VALUE) noexcept:value(handle){}
  Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
  ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
std::filesystem::path executable() {
  std::vector<wchar_t> name(32768);const auto n=GetModuleFileNameW(nullptr,name.data(),static_cast<DWORD>(name.size()));
  require(n&&n<name.size(),"probe executable identity");return std::wstring(name.data(),n);
}
void local_path(const std::filesystem::path& path) {
  const auto text=path.native();
  require(text.size()>=4&&text.size()<=1024&&
      ((text[0]>=L'A'&&text[0]<=L'Z')||(text[0]>=L'a'&&text[0]<=L'z'))&&
      text[1]==L':'&&text[2]==L'\\'&&text.find(L':',2)==std::wstring::npos,"absolute local path required; no UNC/ADS");
  for(const auto& part:path)require(part!=L".."&&part!=L".","relative path component");
  const auto drive=GetDriveTypeW(path.root_path().c_str());
  require(drive==DRIVE_FIXED||drive==DRIVE_RAMDISK,"local fixed/RAM source drive required");
  const auto attributes=GetFileAttributesW(path.parent_path().c_str());
  require(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY)&&
      !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"ordinary report/source parent required");
}
Bytes input(const std::filesystem::path& path,std::size_t cap=256*1024) {
  local_path(path);
  Handle h{CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr)};
  require(h.value!=INVALID_HANDLE_VALUE,"source open failed");
  BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
  require(GetFileType(h.value)==FILE_TYPE_DISK&&GetFileInformationByHandle(h.value,&info)&&
      !(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&
      info.nNumberOfLinks==1&&GetFileSizeEx(h.value,&size)&&size.QuadPart>0&&static_cast<std::uint64_t>(size.QuadPart)<=cap,
      "source must be a bounded ordinary single-link file");
  Bytes bytes(static_cast<std::size_t>(size.QuadPart));DWORD n=0;
  require(ReadFile(h.value,bytes.data(),static_cast<DWORD>(bytes.size()),&n,nullptr)&&n==bytes.size(),
      "short source read");return bytes;
}
StoreIdentity id(unsigned n){StoreIdentity value{};value[0]=static_cast<std::byte>(n);value[1]=std::byte{73};return value;}
NativeFilesystemSnapshot view(std::shared_ptr<ram::Store> io) {
  return NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(std::move(io)));
}
struct Sample {std::uint64_t input=0,source=0,payload=0,compacted=0,write_ns=0,read_ns=0,compact_ns=0;};
std::uint64_t elapsed(std::chrono::steady_clock::time_point start) {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now()-start).count());
}
Sample exercise(const std::vector<Bytes>& inputs,StoreCompressionMode mode) {
  ram::headroom();require(!inputs.empty()&&inputs.size()<=6,"input count cap");
  auto source=std::make_shared<ram::Store>(8U*1024*1024);
  auto target=std::make_shared<ram::Store>(8U*1024*1024);
  auto head=std::make_shared<ram::Store>(kVolumeHeadBytes);
  Sample result;std::vector<NativeFileMutation> mutations;mutations.reserve(inputs.size()*2);
  for(std::size_t i=0;i<inputs.size();++i) {
    const auto name="file"+std::to_string(i);result.input+=inputs[i].size();
    NativeFileMutation create;create.path=name;mutations.push_back(std::move(create));
    NativeFileMutation write;write.operation=NativeFileOperation::write;write.path=name;write.data=inputs[i];
    mutations.push_back(std::move(write));
  }
  require(result.input<=1536U*1024,"aggregate source cap");
  auto start=std::chrono::steady_clock::now();
  {
    auto writer=NativeFilesystemWriter::initialize(ObjectStoreWriter::create(source,id(1),{mode,6,false}));
    writer.apply(mutations);
  }
  result.write_ns=elapsed(start);auto original=view(source);
  result.source=original.storage_info().physical_end;result.payload=original.storage_info().stored_payload_bytes;
  start=std::chrono::steady_clock::now();
  for(std::size_t i=0;i<inputs.size();++i)require(original.read("file"+std::to_string(i),0,256*1024)==inputs[i],
      "RAM source byte comparison");
  result.read_ns=elapsed(start);
  auto authority=NativeVolumeHeadWriter::create(head,id(90),original);
  start=std::chrono::steady_clock::now();
  {auto candidate=NativeFilesystemWriter::compact(original,target,id(2),{mode,6,false});}
  result.compact_ns=elapsed(start);auto candidate=view(target);result.compacted=candidate.storage_info().physical_end;
  authority.publish_compacted(original,candidate);
  auto selected=NativeVolumeHeadView::open(head,id(90),[&](const auto& selected_id)->std::shared_ptr<StoreByteIo>{
    require(selected_id==id(2),"selected wrong RAM image");return target;
  });
  for(std::size_t i=0;i<inputs.size();++i)require(
      selected.filesystem().read("file"+std::to_string(i),0,256*1024)==inputs[i],"RAM selected byte comparison");
  require(authority.info().selection.epoch==2&&head->extent()==kVolumeHeadBytes,"RAM handoff authority");
  source->verify();target->verify();head->verify();return result;
}
void require_job() {
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
  require(QueryInformationJobObject(nullptr,JobObjectExtendedLimitInformation,&limits,sizeof(limits),nullptr)&&
      QueryInformationJobObject(nullptr,JobObjectCpuRateControlInformation,&cpu,sizeof(cpu),nullptr),"missing bounded job");
  constexpr DWORD required=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_ACTIVE_PROCESS|
      JOB_OBJECT_LIMIT_PROCESS_MEMORY|JOB_OBJECT_LIMIT_JOB_MEMORY|JOB_OBJECT_LIMIT_PRIORITY_CLASS;
  require((limits.BasicLimitInformation.LimitFlags&required)==required&&
      !(limits.BasicLimitInformation.LimitFlags&(JOB_OBJECT_LIMIT_BREAKAWAY_OK|JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK))&&
      limits.BasicLimitInformation.ActiveProcessLimit==1&&limits.BasicLimitInformation.PriorityClass==BELOW_NORMAL_PRIORITY_CLASS&&
      limits.ProcessMemoryLimit==128ULL*1024*1024&&limits.JobMemoryLimit==limits.ProcessMemoryLimit&&
      cpu.ControlFlags==(JOB_OBJECT_CPU_RATE_CONTROL_ENABLE|JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP)&&cpu.CpuRate==2000,
      "RAM probe process limits mismatch");
}
void sample(std::ostream& out,const char* name,const Sample& s) {
  out<<'"'<<name<<"\":{\"input\":"<<s.input<<",\"source\":"<<s.source<<",\"payload\":"<<s.payload
      <<",\"compacted\":"<<s.compacted<<",\"write_ns\":"<<s.write_ns<<",\"read_ns\":"<<s.read_ns
      <<",\"compact_ns\":"<<s.compact_ns<<"}";
}
void create_report(const std::filesystem::path& path,const std::string& text) {
  local_path(path);require(text.size()<=2048,"RAM report cap");
  Handle h{CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr)};
  require(h.value!=INVALID_HANDLE_VALUE,"new report creation");DWORD n=0;
  require(WriteFile(h.value,text.data(),static_cast<DWORD>(text.size()),&n,nullptr)&&n==text.size()&&FlushFileBuffers(h.value),
      "bounded report write");
}
std::string read_report(const std::filesystem::path& path) {
  auto bytes=input(path,2048);
  return {reinterpret_cast<const char*>(bytes.data()),bytes.size()};
}
int child(int argc,wchar_t** argv) {
  require_job();ram::headroom();DWORD before=0,after=0;
  require(GetProcessHandleCount(GetCurrentProcess(),&before),"RAM handle baseline");
  std::vector<Bytes> real;for(int i=3;i<argc;++i)real.push_back(input(argv[i]));
  require(real.size()<=6,"real source count cap");
  std::vector<Bytes> synthetic(2,Bytes(65536,std::byte{65}));std::uint64_t x=0x872635a8ULL;
  for(auto& b:synthetic[1]){x^=x<<13;x^=x>>7;x^=x<<17;b=static_cast<std::byte>(x>>21);}
  require(store_zstd_available(),"qualified RAM probe requires bounded Zstandard");
  Sample sr,sa,rr,ra;
  {
    ram::WorkingSet working;
    sr=exercise(synthetic,StoreCompressionMode::raw);require(!ram::locked_bytes.load(),"synthetic raw RAM release");
    sa=exercise(synthetic,StoreCompressionMode::automatic);require(!ram::locked_bytes.load(),"synthetic codec RAM release");
    if(!real.empty()){
      rr=exercise(real,StoreCompressionMode::raw);require(!ram::locked_bytes.load(),"real raw RAM release");
      ra=exercise(real,StoreCompressionMode::automatic);require(!ram::locked_bytes.load(),"real codec RAM release");
    }
    working.restore();
  }
  require(ram::cleanup_ok.load()&&!ram::locked_bytes.load(),"locked RAM/working-set cleanup");
  require(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after,"RAM probe handle leak");
  std::ostringstream out;out<<"{\"experimental\":true,\"commit\":\""<<FLITZFS_RAM_SOURCE_COMMIT
      <<"\",\"backing\":\"locked_RAM_model\",\"mounted\":false,\"gpu\":false,\"real_files\":"<<real.size()
      <<",\"zstd_version\":"<<store_zstd_version()<<",\"head_bytes\":"<<kVolumeHeadBytes
      <<",\"locked_peak\":"<<ram::peak_locked_bytes.load()<<",\"locked_after\":"<<ram::locked_bytes.load()
      <<",\"handles_before\":"<<before<<",\"handles_after\":"<<after<<",";
  sample(out,"synthetic_raw",sr);out<<",";sample(out,"synthetic_auto",sa);
  out<<",";sample(out,"real_raw",rr);out<<",";sample(out,"real_auto",ra);
  out<<",\"cpu_verified\":true,\"resident_verified\":true,\"complete\":true}\n";
  create_report(argv[2],out.str());return 0;
}
}
int wmain(int argc,wchar_t** argv){try{
  require(argc>=3&&argc<=9,"usage: --run NEW_REPORT [up to six bounded read-only source files]");
  const std::wstring_view mode(argv[1]);
  if(mode==L"--child")return child(argc,argv);
  require(mode==L"--run","explicit bounded RAM run required");
  ram::headroom();local_path(argv[2]);require(!std::filesystem::exists(argv[2]),"report must not exist");
  std::vector<std::wstring> args{L"--child"};for(int i=2;i<argc;++i)args.emplace_back(argv[i]);
  const auto result=run_bounded_process(executable(),args,{128,2000,15000});
  require(result.exit_confirmed&&!result.timed_out&&!result.cancelled,
      "RAM child timed out/not retired; retain evidence, no retry");
  const auto report=read_report(argv[2]);
  if(result.exit_code!=0){std::cerr<<report;throw std::runtime_error("RAM child failed; no retry");}
  require(report.find("\"commit\":\"" FLITZFS_RAM_SOURCE_COMMIT "\"")!=std::string::npos&&
      report.find("\"complete\":true")!=std::string::npos,"RAM report provenance/completion");
  std::cout<<report<<"RAM supervisor: exit="<<result.exit_code<<" exit_confirmed="<<result.exit_confirmed
      <<" peak_commit_bytes="<<result.peak_committed_bytes<<"\n";return 0;
}catch(const std::exception& e){
  if(argc>=3&&std::wstring_view(argv[1])==L"--child") {
    try{
      std::string error;for(std::size_t i=0;i<256&&e.what()[i];++i){
        const auto c=static_cast<unsigned char>(e.what()[i]);
        if(c=='"'||c=='\\')error+='\\';if(c>=32&&c<127)error+=static_cast<char>(c);
      }
      std::ostringstream report;report<<"{\"experimental\":true,\"commit\":\""<<FLITZFS_RAM_SOURCE_COMMIT
          <<"\",\"complete\":false,\"error\":\""<<error<<"\",\"locked_after\":"<<ram::locked_bytes.load()
          <<",\"cleanup_ok\":"<<(ram::cleanup_ok.load()?"true":"false")<<"}\n";
      create_report(argv[2],report.str());
    }catch(...){}
  }
  std::cerr<<"RAM probe failure: "<<e.what()<<"\n";return 1;}}
