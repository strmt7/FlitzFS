#include "locked_ram_fixture.hpp"
#include "gpu_probe_budget.hpp"
#include "worker_exchange.hpp"
#include "hip_worker_exit.hpp"
#include "flitzfs/native_session.hpp"
#include "flitzfs/match_worker_wire.hpp"
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <locale>
#include <new>
#include <span>
#include <vector>
#include <optional>
#include <sstream>
#include <string_view>
#ifndef FLITZFS_GPU_RAM_SOURCE_COMMIT
#error Exact experimental source identity required
#endif
namespace {
using namespace flitzfs;
using Bytes=std::vector<std::byte>;
namespace ram=locked_ram_fixture;
namespace fs=std::filesystem;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void local_path(const fs::path& path) {
  const auto text=path.native();
  require(text.size()>=4 && text.size()<=1024 &&
      ((text[0]>=L'A'&&text[0]<=L'Z')||(text[0]>=L'a'&&text[0]<=L'z')) &&
      text[1]==L':' && text[2]==L'\\' && text.find(L':',2)==text.npos,"ordinary absolute local path required");
  for(const auto& part:path)require(part!=L"." && part!=L"..","relative path component");
  const auto drive=GetDriveTypeW(path.root_path().c_str());
  require(drive==DRIVE_FIXED || drive==DRIVE_RAMDISK,"local fixed/RAM path required");
  const auto attrs=GetFileAttributesW(path.parent_path().c_str());
  require(attrs!=INVALID_FILE_ATTRIBUTES && (attrs&FILE_ATTRIBUTE_DIRECTORY) &&
      !(attrs&FILE_ATTRIBUTE_REPARSE_POINT),"ordinary trusted parent required");
}
class Report final {
 public:
  explicit Report(const fs::path& path) {
    local_path(path);
    const auto leaf=path.filename().native();
    require(leaf.size()>12 && leaf.size()<=128 && leaf.starts_with(L"native-") &&
        leaf.ends_with(L".json"),"new report must be named native-*.json");
    for(const auto c:leaf)require((c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||
        (c>=L'0'&&c<=L'9')||c==L'-'||c==L'.',"ordinary report filename required");
    handle_=CreateFileW(path.c_str(),GENERIC_WRITE|FILE_READ_ATTRIBUTES,0,nullptr,CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    require(handle_!=INVALID_HANDLE_VALUE,"new report creation refused");
    BY_HANDLE_FILE_INFORMATION info{};
    if(GetFileType(handle_)!=FILE_TYPE_DISK || !GetFileInformationByHandle(handle_,&info) ||
        info.nNumberOfLinks!=1 || (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))) {
      CloseHandle(handle_);handle_=INVALID_HANDLE_VALUE;
      throw std::runtime_error("new ordinary report file not proven");
    }
  }
  ~Report(){if(handle_!=INVALID_HANDLE_VALUE)CloseHandle(handle_);}
  void write(const std::string& text) {
    require(!written_ && text.size()<=8192,"single bounded report required");
    written_=true;DWORD bytes=0;
    require(WriteFile(handle_,text.data(),static_cast<DWORD>(text.size()),&bytes,nullptr) &&
        bytes==text.size() && FlushFileBuffers(handle_),"report publication failed");
  }
 private:
  HANDLE handle_=INVALID_HANDLE_VALUE;bool written_=false;
};
StoreIdentity identity(unsigned n){StoreIdentity id{};id[0]=static_cast<std::byte>(n);id[1]=std::byte{73};return id;}
std::uint64_t elapsed(std::chrono::steady_clock::time_point start) {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now()-start).count());
}
bool lower_hex(std::wstring_view text,std::size_t length) {
  if(text.size()!=length)return false;
  for(const auto c:text)if(!((c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f')))return false;
  return true;
}
struct Worker {
  fs::path executable;
  std::vector<std::wstring> arguments,warmup;
  WorkerExecution expected=WorkerExecution::cpu_semantics;
  bool driver=false;std::uint32_t timeout=5000;
};
Worker options(int argc,wchar_t** argv) {
  const std::wstring_view mode=argc>1?argv[1]:L"";
  Worker w;
  if(mode==L"--cpu-fixture") {
    require(argc==5,"CPU fixture requires NEW_REPORT TRUSTED_HELPER FIXTURE_MODE");
    const auto* actions=std::getenv("GITHUB_ACTIONS");
    const auto* runner=std::getenv("RUNNER_ENVIRONMENT");
    require(actions && std::string_view(actions)=="true" && runner &&
        std::string_view(runner)=="github-hosted","CPU fixture is disposable-hosted only");
    const std::wstring_view fixture(argv[4]);
    require(fixture==L"match" || fixture==L"match-invalid" ||
        fixture==L"match-hip-declaration" || fixture==L"match-hold","unreviewed CPU fixture mode");
    w.arguments={std::wstring(fixture)};w.warmup={L"match"};
    if(fixture==L"match-hold")w.timeout=100;
  } else {
    require(mode==L"--driver-once" && argc==7,
        "explicit --driver-once NEW_REPORT TRUSTED_WORKER ORDINAL LUID RUNTIME_SHA256 required");
    const std::wstring_view ordinal(argv[4]),luid(argv[5]),digest(argv[6]);
    require((ordinal.size()==1&&ordinal[0]>=L'0'&&ordinal[0]<=L'9') ||
        (ordinal.size()==2&&ordinal>=L"10"&&ordinal<=L"63"&&ordinal[1]>=L'0'&&ordinal[1]<=L'9'),
        "bounded explicit HIP ordinal required");
    require(lower_hex(luid,16)&&luid!=L"0000000000000000"&&lower_hex(digest,64),"invalid adapter/runtime identity");
    w.driver=true;w.expected=WorkerExecution::hip;
    w.arguments={L"--driver-match-adapter",argv[4],argv[5],L"1",argv[6]};w.warmup=w.arguments;
  }
  w.executable=argv[3];local_path(w.executable);
  require(w.executable.filename()==(w.driver?L"flitzfs_hip_worker.exe":L"flitzfs_worker_exchange_tests.exe"),
      "unexpected trusted helper filename");
  return w;
}
void warm_transport(const Worker& worker) {
  // Invalid match framing is rejected before runtime/module initialization.
  // Warm only CPU transport/lifetime bookkeeping, never a compute request.
  const std::array<std::byte,32> invalid{};
  const auto result=run_worker_exchange(worker.executable,worker.warmup,invalid,{128,2000,5000});
  require(result.code==WorkerExchangeCode::worker_failed && result.process.exit_confirmed &&
      !result.process.timed_out && !result.process.cancelled && result.process.exit_code!=0 &&
      result.response.empty() && !result.publication_observed && WorkerExchangeSession::available(),
      "CPU-only transport warmup/retirement failed");
}
struct MatchOutcome {
  std::vector<StoreMatchProposal> proposals;
  bool attempted=false,accepted=false,exit_confirmed=false,timed_out=false;
  std::uint32_t worker_exit=0;std::uint64_t prepare_ns=0,exchange_ns=0,validation_ns=0,device_bytes=0,peak_commit_bytes=0;
  const char* reason="not_requested";
};
MatchOutcome find_matches(const Worker& worker,std::span<const std::byte> bytes) {
  MatchOutcome out;out.attempted=true;
  const auto start=std::chrono::steady_clock::now();
  const auto search=PreparedGpuMatchSearch::prepare(bytes);
  const auto request=encode_match_worker_request(search,1);
  out.prepare_ns=elapsed(start);out.device_bytes=search.device_buffer_bytes();
  const auto transfer=std::chrono::steady_clock::now();
  const auto result=run_worker_exchange(worker.executable,worker.arguments,request,{128,2000,worker.timeout});
  out.exchange_ns=elapsed(transfer);out.worker_exit=result.process.exit_code;
  out.peak_commit_bytes=result.process.peak_committed_bytes;
  out.exit_confirmed=result.process.exit_confirmed;out.timed_out=result.process.timed_out;
  // An unknown live helper is terminal for the experiment. Never start another.
  require(out.exit_confirmed && WorkerExchangeSession::available(),"worker/supervisor retirement unconfirmed");
  if(result.code!=WorkerExchangeCode::completed || result.process.cancelled || out.timed_out ||
      out.worker_exit!=0) {
    require(result.response.empty(),"failed worker exposed candidate bytes");
    out.reason=out.timed_out?"timeout":"worker_failure";return out;
  }
  const auto validation=std::chrono::steady_clock::now();
  try {
    out.proposals=accept_match_worker_reply(result.response,search,1,worker.expected);
    out.accepted=true;out.reason="verified";
  }catch(const std::bad_alloc&){out.reason="allocation_pressure";}
  catch(const std::invalid_argument&){out.reason="invalid_reply";}
  out.validation_ns=elapsed(validation);return out;
}
struct Sample {
  std::uint64_t input_bytes=0,source_bytes=0,target_bytes=0,head_bytes=0;
  std::uint64_t source_payload=0,target_payload=0,cpu_baseline_payload=0;
  std::uint64_t records=0,supplied_matches=0,forwarded_matches=0,selected_records=0;
  std::uint64_t write_ns=0,prepare_ns=0,native_copy_ns=0,publish_ns=0,verify_ns=0,total_ns=0;
  MatchOutcome match;
};
Bytes synthetic_input() {
  Bytes prefix(8193);std::uint64_t x=0x872635a8ULL;
  for(auto& b:prefix){x^=x<<13;x^=x>>7;x^=x<<17;b=static_cast<std::byte>(x>>21);}
  auto input=prefix;input.insert(input.end(),prefix.begin(),prefix.end());input.resize(kNativeExtentBytes);
  return input;
}
Sample exercise(const Bytes& input,const Worker* worker) {
  const auto whole=std::chrono::steady_clock::now();
  auto source=std::make_shared<ram::Store>(2U*1024*1024);
  auto target=std::make_shared<ram::Store>(2U*1024*1024);
  auto head=std::make_shared<ram::Store>(kVolumeHeadBytes);
  Sample out;out.input_bytes=input.size();out.head_bytes=kVolumeHeadBytes;
  const auto writing=std::chrono::steady_clock::now();
  auto session=NativeFileSession::create_volume(head,identity(90),source,identity(1),
      {StoreCompressionMode::raw,6,false});
  session.create_entry("sample");
  const auto handle=session.open("sample",{true,true,false});
  session.write(handle,0,input);session.flush(handle);out.write_ns=elapsed(writing);
  const auto original=session.snapshot();
  const auto generation=original.storage_info().generation;
  out.source_bytes=source->extent();out.source_payload=original.storage_info().stored_payload_bytes;
  require(out.source_bytes<=2U*1024*1024,"source capture cap");
  Bytes preserved(static_cast<std::size_t>(out.source_bytes));source->read_exact(0,preserved);
  auto point=std::chrono::steady_clock::now();
  session.begin_recompression(target,identity(2),{StoreCompressionMode::automatic,6,true});
  out.prepare_ns=elapsed(point);
  using Progress=NativeFileSession::RecompressionProgress;
  auto progress=Progress::copying;
  const auto objects=original.storage_info().objects;
  require(objects>0&&objects<=8,"synthetic object-count cap");
  bool found=false;
  for(std::size_t step=0;step<objects;++step) {
    std::uint64_t object=0;
    std::span<const StoreMatchProposal> hints;
    if(worker) {
      if(auto work=session.next_recompression_input();work && work->bytes==input) {
        require(!found,"duplicate matching extent; no second worker permitted");found=true;
        out.match=find_matches(*worker,work->bytes);object=work->object_id;hints=out.match.proposals;
      }
    }
    point=std::chrono::steady_clock::now();
    progress=session.advance_recompression(object,hints);out.native_copy_ns+=elapsed(point);
    require(progress==(step+1==objects?Progress::ready:Progress::copying),"native candidate progress");
    const auto feedback=session.recompression_feedback();
    require(feedback && feedback->source_image==identity(1) && feedback->source_generation==generation &&
        feedback->reencoded,"missing/mismatched provisional native feedback");
    require(feedback->stored_bytes<=feedback->cpu_baseline_bytes,"native encoding exceeded CPU baseline");
    ++out.records;out.supplied_matches+=feedback->supplied_matches;
    out.forwarded_matches+=feedback->forwarded_matches;
    out.selected_records+=feedback->match_encoding_selected?1:0;
    out.cpu_baseline_payload+=feedback->cpu_baseline_bytes;
    out.target_payload+=feedback->stored_bytes;
  }
  require(!worker || found,"native match input capability missing");
  point=std::chrono::steady_clock::now();
  const auto published=session.publish_recompression();out.publish_ns=elapsed(point);
  require(published.selection.epoch==2 && published.selection.image_identity==identity(2) &&
      !session.recompression_feedback(),"native handoff/feedback retirement");
  const auto candidate=session.snapshot();out.target_bytes=candidate.storage_info().physical_end;
  require(out.target_payload==candidate.storage_info().stored_payload_bytes,"native payload accounting mismatch");
  point=std::chrono::steady_clock::now();
  require(session.read(handle,0,kNativeExtentBytes)==input &&
      original.read("sample",0,kNativeExtentBytes)==input,"retained-context/pinned-source comparison");
  session.flush(handle);session.cleanup(handle,false);session.close(handle);
  require(session.stats().contexts==0&&!session.poisoned(),"native context retirement");
  target->crash_to_checkpoint();head->crash_to_checkpoint();
  const auto selected=NativeVolumeHeadView::open(head,identity(90),
      [&](const auto& id)->std::shared_ptr<StoreByteIo>{
        require(id==identity(2),"wrong selected native image");return target;
      });
  selected.filesystem().verify_all();
  require(selected.filesystem().read("sample",0,kNativeExtentBytes)==input,"independent CPU selected-image comparison");
  Bytes after(preserved.size());source->read_exact(0,after);
  require(after==preserved,"native source image changed");
  source->verify();target->verify();head->verify();
  out.verify_ns=elapsed(point);out.total_ns=elapsed(whole);return out;
}
void sample(std::ostream& out,const char* name,const Sample& s) {
  out<<'"'<<name<<"\":{\"input_bytes\":"<<s.input_bytes<<",\"source_bytes\":"<<s.source_bytes
      <<",\"target_bytes\":"<<s.target_bytes<<",\"head_bytes\":"<<s.head_bytes
      <<",\"active_plus_head_bytes\":"<<s.target_bytes+s.head_bytes
      <<",\"retained_images_plus_head_bytes\":"<<s.source_bytes+s.target_bytes+s.head_bytes
      <<",\"source_payload\":"<<s.source_payload<<",\"target_payload\":"<<s.target_payload
      <<",\"cpu_baseline_payload\":"<<s.cpu_baseline_payload<<",\"records\":"<<s.records
      <<",\"supplied_matches\":"<<s.supplied_matches<<",\"forwarded_matches\":"<<s.forwarded_matches
      <<",\"selected_records\":"<<s.selected_records<<",\"write_ns\":"<<s.write_ns
      <<",\"prepare_ns\":"<<s.prepare_ns<<",\"native_copy_ns\":"<<s.native_copy_ns
      <<",\"publish_ns\":"<<s.publish_ns<<",\"verify_ns\":"<<s.verify_ns<<",\"total_ns\":"<<s.total_ns
      <<",\"worker_attempted\":"<<(s.match.attempted?"true":"false")
      <<",\"worker_accepted\":"<<(s.match.accepted?"true":"false")
      <<",\"worker_exit_confirmed\":"<<(s.match.exit_confirmed?"true":"false")
      <<",\"worker_timed_out\":"<<(s.match.timed_out?"true":"false")
      <<",\"worker_exit\":"<<s.match.worker_exit
      <<",\"worker_category\":"<<hip_worker_exit_category(s.match.worker_exit)
      <<",\"worker_stage\":\""<<hip_worker_stage_name(hip_worker_exit_stage(s.match.worker_exit))
      <<"\",\"worker_reason\":\""<<s.match.reason
      <<"\",\"match_prepare_ns\":"<<s.match.prepare_ns<<",\"exchange_ns\":"<<s.match.exchange_ns
      <<",\"reply_validation_ns\":"<<s.match.validation_ns<<",\"device_bytes\":"<<s.match.device_bytes
      <<",\"worker_peak_commit_bytes\":"<<s.match.peak_commit_bytes
      <<",\"proposals\":"<<s.match.proposals.size()<<"}";
}
}
int wmain(int argc,wchar_t** argv) {
  std::unique_ptr<Report> report;
  std::array<DWORD,10> handle_trace{};
  const auto count_handles=[&](std::size_t phase){
    require(phase<handle_trace.size() && GetProcessHandleCount(GetCurrentProcess(),&handle_trace[phase]),
        "handle phase observation failed");
  };
  try {
    const auto worker=options(argc,argv);
    report=std::make_unique<Report>(argv[2]); // Atomic CREATE_NEW before any work.
    const gpu_probe_fixture::Budget budget;
    const auto warming=std::chrono::steady_clock::now();
    warm_transport(worker);
    const auto warmup_ns=elapsed(warming);
    DWORD before=0,after=0;
    require(GetProcessHandleCount(GetCurrentProcess(),&before),"handle baseline");handle_trace[0]=before;
    require(store_zstd_available(),"native GPU/RAM probe requires qualified Zstandard");
    Sample cpu,proposed;
    const auto pairing=std::chrono::steady_clock::now();
    {
      ram::WorkingSet resident;count_handles(1);
      const auto input=synthetic_input();
      auto timing=std::chrono::steady_clock::now();
      cpu=exercise(input,nullptr);cpu.total_ns=elapsed(timing);count_handles(2);
      require(!ram::locked_bytes.load(),"CPU baseline locked-memory retirement");
      timing=std::chrono::steady_clock::now();
      proposed=exercise(input,&worker);proposed.total_ns=elapsed(timing);count_handles(3);
      require(!ram::locked_bytes.load(),"proposed locked-memory retirement");
      resident.restore();
    }
    count_handles(4);
    require(ram::cleanup_ok.load()&&!ram::locked_bytes.load()&&WorkerExchangeSession::available(),
        "resident/worker cleanup unconfirmed");
    require(GetProcessHandleCount(GetCurrentProcess(),&after),"final handle observation");
    handle_trace[5]=after;
    if(before!=after && !worker.driver) {
      // Diagnostics ONLY after the strict gate has already failed, hosted CPU
      // fixtures only. Preserve the original assertion; never promote this run.
      Worker diagnostic=worker;diagnostic.arguments={L"match"};diagnostic.timeout=5000;
      ram::WorkingSet resident;
      const auto input=synthetic_input();
      for(std::size_t attempt=0;attempt<2;++attempt) {
        (void)exercise(input,nullptr);count_handles(6+2*attempt);
        (void)exercise(input,&diagnostic);count_handles(7+2*attempt);
      }
      resident.restore();
    }
    require(before==after,"native GPU/RAM handle growth");
    std::ostringstream out;out.imbue(std::locale::classic());
    out<<"{\"schema\":1,\"experimental\":true,\"commit\":\""<<FLITZFS_GPU_RAM_SOURCE_COMMIT
        <<"\",\"backing\":\"locked_RAM_model\",\"mounted\":false,\"gpu_requested\":"<<(worker.driver?"true":"false")
        <<",\"gpu_result_accepted\":"<<(worker.driver&&proposed.match.accepted?"true":"false")
        <<",\"cpu_fixture\":"<<(!worker.driver?"true":"false")<<",\"zstd_version\":"<<store_zstd_version()
        <<",\"locked_peak\":"<<ram::peak_locked_bytes.load()<<",\"locked_after\":"<<ram::locked_bytes.load()
        <<",\"handles_before\":"<<before<<",\"handles_after\":"<<after
        <<",\"controller_peak_commit_bytes\":"<<budget.peak_commit()
        <<",\"transport_warmup_ns\":"<<warmup_ns<<",\"pair_total_ns\":"<<elapsed(pairing)<<",";
    sample(out,"cpu_baseline",cpu);out<<',';sample(out,"proposed",proposed);
    out<<",\"cpu_verified\":true,\"source_preserved\":true,\"retained_context_verified\":true,"
        "\"resident_verified\":true,\"power_loss_qualified\":false,\"complete\":true}\n";
    report->write(out.str());std::cout<<out.str();
    return worker.driver&&!proposed.match.accepted?2:0;
  }catch(const std::exception& error) {
    std::string text;
    for(std::size_t i=0;i<192&&error.what()[i];++i) {
      const auto c=static_cast<unsigned char>(error.what()[i]);
      if(c=='"'||c=='\\')text+='\\';
      if(c>=32&&c<127)text+=static_cast<char>(c);
    }
    if(report)try {
      std::ostringstream out;out.imbue(std::locale::classic());out<<"{\"schema\":1,\"experimental\":true,\"commit\":\""
          <<FLITZFS_GPU_RAM_SOURCE_COMMIT<<"\",\"complete\":false,\"error\":\""<<text
          <<"\",\"locked_after\":"<<ram::locked_bytes.load()<<",\"cleanup_ok\":"
          <<(ram::cleanup_ok.load()?"true":"false")<<",\"handle_trace\":[";
      for(std::size_t i=0;i<handle_trace.size();++i){if(i)out<<',';out<<handle_trace[i];}
      out<<"]}\n";
      report->write(out.str());
    }catch(...){}
    std::cerr<<"Native GPU/RAM probe failed: "<<text<<'\n';return 1;
  }
}
