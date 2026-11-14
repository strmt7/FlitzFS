#include "worker_exchange.hpp"
#include "hip_worker_exit.hpp"
#include "gpu_budget.hpp"
#include "../gpu_probe_budget.hpp"
#include <chrono>
#include <limits>
#include <string_view>
#include "flitzfs/lz4_worker_wire.hpp"
#include <windows.h>
#include <array>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
using namespace flitzfs;
namespace fs=std::filesystem;
using Bytes=std::vector<std::byte>;
unsigned checks=0,exits=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
constexpr ProcessBudget budget{128,2000,15000};
void child(const fs::path& worker,std::vector<std::wstring> args,HipWorkerExit expected,
    bool success=false){
  const auto r=run_bounded_process(worker,args,budget);++exits;
  require(r.process_id && r.exit_confirmed && !r.timed_out && !r.cancelled &&
      r.peak_committed_bytes<=128ULL*1024*1024,"Compression metadata child did not retire");
  require(success?r.exit_code==0:
      hip_worker_exit_category(r.exit_code)==static_cast<std::uint32_t>(expected),
      "Unexpected compression metadata exit");
}
void exchange(const fs::path& worker,std::span<const std::byte> frame,HipWorkerExit expected,
    std::vector<std::wstring> args={},bool runtime_digest_refusal=false){
  if(args.empty())args={L"--driver-lz4-adapter",L"0",L"0100000000000000",L"1",std::wstring(64,L'0')};
  const auto r=run_worker_exchange(worker,args,frame,budget);++exits;
  require(r.process.process_id && r.process.exit_confirmed && !r.process.timed_out &&
      !r.process.cancelled && r.process.peak_committed_bytes<=128ULL*1024*1024,
      "Compression exchange child did not retire");
  const auto category=hip_worker_exit_category(r.process.exit_code);
  // A disposable runner can have no installed AMD runtime or a different
  // installed file. Both must refuse the deliberately zero digest before load.
  const bool expected_exit=category==static_cast<std::uint32_t>(expected) ||
      (runtime_digest_refusal && category==static_cast<std::uint32_t>(HipWorkerExit::integrity_failure));
  require(r.code==WorkerExchangeCode::worker_failed && r.response.empty() &&
      !r.publication_observed && expected_exit,
      "Rejected compression request exposed a reply");
  require(WorkerExchangeSession::available(),"Compression supervisor not quiescent");
}
Bytes read(const fs::path& path){
  const auto length=fs::file_size(path);
  require(length && length<=32ULL*1024*1024,"Fixture copy input size");
  std::ifstream in(path,std::ios::binary);
  require(in.is_open(),"Fixture source open");
  Bytes bytes(static_cast<std::size_t>(length));
  require(bool(in.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()))),
      "Fixture source changed");return bytes;
}
void write_new(const fs::path& path,std::span<const std::byte> bytes){
  const auto handle=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL,nullptr);
  require(handle!=INVALID_HANDLE_VALUE,"New fixture destination required");
  DWORD actual=0;
  const auto ok=WriteFile(handle,bytes.data(),static_cast<DWORD>(bytes.size()),&actual,nullptr);
  const auto closed=CloseHandle(handle);
  require(ok && actual==bytes.size() && closed,"Fixture copy failed");
}

unsigned number(std::wstring_view value,unsigned limit){
  require(!value.empty() && value.size()<=10,"Bounded adapter number required");
  unsigned result=0;
  for(const auto c:value){
    require(c>=L'0' && c<=L'9',"Adapter decimal digit");
    const auto digit=static_cast<unsigned>(c-L'0');
    require(digit<=limit && result<=(limit-digit)/10,"Adapter number overflow");
    result=result*10+digit;
  }return result;
}
std::array<std::byte,8> luid_value(std::wstring_view text){
  require(text.size()==16,"Adapter LUID length");
  const auto digit=[](wchar_t c){
    if(c>=L'0' && c<=L'9')return static_cast<unsigned>(c-L'0');
    require(c>=L'a' && c<=L'f',"Adapter hexadecimal digit");
    return static_cast<unsigned>(c-L'a'+10);
  };
  std::array<std::byte,8> result{};
  for(std::size_t i=0;i<8;++i)result[i]=static_cast<std::byte>(digit(text[2*i])*16+digit(text[2*i+1]));
  require(std::any_of(result.begin(),result.end(),[](auto x){return x!=std::byte{0};}),"Zero adapter LUID");
  return result;
}
int gpu_once(int argc,wchar_t** argv,const gpu_probe_fixture::Budget& controller){
  require(argc==7,"Explicit GPU worker, ordinal, LUID, node mask and runtime hash required");
  const fs::path worker(argv[2]);require(worker.is_absolute(),"Absolute trusted compression worker required");
  const auto ordinal=number(argv[3],63);
  const auto luid=luid_value(argv[4]);
  const auto mask=number(argv[5],(std::numeric_limits<unsigned>::max)());
  require(mask && !(mask&(mask-1)),"Single physical GPU node required");
  const std::wstring_view digest(argv[6]);
  require(digest.size()==64 && std::all_of(digest.begin(),digest.end(),[](wchar_t c){
      return (c>=L'0' && c<=L'9')||(c>=L'a' && c<=L'f');}),"Exact installed runtime digest required");
  Bytes first(2048),second(2048);
  for(std::size_t i=0;i<first.size();++i){
    first[i]=static_cast<std::byte>(i%31);
    second[i]=static_cast<std::byte>((i/17)%13);
  }
  const std::array<Lz4WorkerBytes,2> views{first,second};
  const auto source=PreparedLz4WorkerBatch::prepare(views);
  const auto before=query_windows_gpu_budget(luid,mask);
  require(before.status==WindowsGpuBudgetStatus::success &&
      check_gpu_memory_budget(before.snapshot,luid,mask,GetTickCount64(),
          source.device_layout().total_bytes)==GpuGateCode::admitted,
      "Fresh compression GPU budget unavailable; no worker launched");
  const auto frame=encode_lz4_worker_request(source,519);
  const auto started=std::chrono::steady_clock::now();
  const auto result=run_worker_exchange(worker,{L"--driver-lz4-adapter",std::to_wstring(ordinal),
      argv[4],std::to_wstring(mask),std::wstring(digest)},frame,budget);
  const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now()-started).count();
  std::uint64_t encoded=0;
  bool verified=false;
  if(result.code==WorkerExchangeCode::completed && result.process.exit_confirmed &&
      result.process.exit_code==0 && !result.process.timed_out && !result.process.cancelled){
    const auto after=query_windows_gpu_budget(luid,mask);
    require(after.status==WindowsGpuBudgetStatus::success &&
        check_gpu_memory_transition(before.snapshot,after.snapshot,luid,mask,GetTickCount64(),
            source.device_layout().total_bytes)==GpuGateCode::admitted,
        "Compression result memory transition refused");
    const auto payloads=accept_lz4_worker_reply(result.response,source,519,WorkerExecution::hip);
    for(const auto& bytes:payloads)encoded+=bytes.size();
    verified=true;
  }
  require(source.input(0).size()==first.size() &&
      std::equal(first.begin(),first.end(),source.input(0).begin()) &&
      std::equal(second.begin(),second.end(),source.input(1).begin()),"Independent CPU source changed");
  std::cout<<"LZ4 GPU one-shot: {\"gpu_result_verified\":"<<(verified?"true":"false")
      <<",\"cpu_selected\":"<<(verified?"false":"true")<<",\"source_bytes\":"<<source.input_bytes()
      <<",\"candidate_bytes\":"<<encoded<<",\"device_bytes\":"<<source.device_layout().total_bytes
      <<",\"exchange_ns\":"<<elapsed<<",\"worker_exit\":"<<result.process.exit_code
      <<",\"worker_category\":"<<hip_worker_exit_category(result.process.exit_code)
      <<",\"worker_stage\":\""<<hip_worker_stage_name(hip_worker_exit_stage(result.process.exit_code))
      <<"\",\"exit_confirmed\":"<<(result.process.exit_confirmed?"true":"false")
      <<",\"publication_observed\":"<<(result.publication_observed?"true":"false")
      <<",\"timed_out\":"<<(result.process.timed_out?"true":"false")
      <<",\"peak_commit_bytes\":"<<result.process.peak_committed_bytes
      <<",\"controller_job_peak_commit_bytes\":"<<controller.peak_commit()
      <<",\"transport_quiescent\":"<<(WorkerExchangeSession::available()?"true":"false")<<"}\n";
  return verified?0:2;
}
}
int wmain(int argc,wchar_t** argv){
  try{
    gpu_probe_fixture::Budget controller;
    if(argc>1 && std::wstring_view(argv[1])==L"--gpu-once")return gpu_once(argc,argv,controller);
    require(argc==3,"Explicit trusted worker and NEW fixture required");
    const fs::path worker(argv[1]),root(argv[2]);
    require(worker.is_absolute() && root.is_absolute() && root.has_filename() &&
        root.filename()!=L"." && root.filename()!=L"..","Absolute compression fixture paths required");
    require(!fs::exists(root) && fs::create_directory(root),"New exclusive compression fixture required");
    require(WorkerExchangeSession::available(),"Compression probe entered occupied transport");
    const auto source=read(worker);
    const auto module_path=worker.parent_path()/L"hip-lz4"/L"flitzfs_rocm_lz4_candidate.dll";
    const auto module=read(module_path);
    child(worker,{L"--verify-lz4-kernel-bundle"},HipWorkerExit::invalid_request,true);
    const auto missing=root/L"missing";require(fs::create_directory(missing),"New missing fixture");
    write_new(missing/worker.filename(),source);
    child(missing/worker.filename(),{L"--verify-lz4-kernel-bundle"},HipWorkerExit::runtime_unavailable);
    const auto tampered=root/L"tampered";require(fs::create_directory(tampered),"New tamper fixture");
    require(fs::create_directory(tampered/L"hip-lz4"),"New private module directory");
    write_new(tampered/worker.filename(),source);
    auto changed=module;changed.back()^=std::byte{1};
    write_new(tampered/L"hip-lz4"/L"flitzfs_rocm_lz4_candidate.dll",changed);
    child(tampered/worker.filename(),{L"--verify-lz4-kernel-bundle"},HipWorkerExit::integrity_failure);
    Bytes first(17,std::byte{0x51}),second(33,std::byte{0x62});
    const std::array<Lz4WorkerBytes,2> views{first,second};
    const auto prepared=PreparedLz4WorkerBatch::prepare(views);
    const auto original=Bytes(prepared.plaintext().begin(),prepared.plaintext().end());
    const auto frame=encode_lz4_worker_request(prepared,519);
    exchange(worker,frame,HipWorkerExit::runtime_unavailable,{},true);
    for(const auto offset:{std::size_t{0},std::size_t{24},std::size_t{32}}){
      auto invalid=frame;invalid[offset]^=std::byte{1};
      exchange(worker,invalid,HipWorkerExit::invalid_request);
    }
    exchange(worker,std::span<const std::byte>(frame).first(12),HipWorkerExit::invalid_request);
    exchange(worker,frame,HipWorkerExit::invalid_request,
        {L"--driver-lz4-adapter",L"0",L"0100000000000000",L"3",std::wstring(64,L'0')});
    exchange(worker,frame,HipWorkerExit::invalid_request,
        {L"--driver-lz4-adapter",L"0",L"0100000000000000",L"1",L"bad"});
    require(original==Bytes(prepared.plaintext().begin(),prepared.plaintext().end()),
        "CPU fallback source changed");
    require(read(worker)==source && read(module_path)==module,"Trusted input changed");
    std::cout<<"LZ4 runtime admission: {\"checks\":"<<checks<<",\"exits_confirmed\":"<<exits
        <<",\"bundle_verified\":true,\"tamper_refused\":true,\"CPU_source_unchanged\":true,"
        "\"hip_runtime_loaded\":false,\"gpu_executed\":false,\"complete\":true}\n";
    return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

