#include "worker_exchange.hpp"
#include "lz4_native_storage_fixture.hpp"
#include "flitzfs/gpu_lz4_verify.hpp"
#include <cstdlib>
#include "hip_worker_exit.hpp"
#include "gpu_budget.hpp"
#include "../gpu_probe_budget.hpp"
#include <chrono>
#include <limits>
#include <string_view>
#include "flitzfs/lz4_worker_wire.hpp"
#include "flitzfs/uniform_lz4_wire.hpp"
#include "uniform_lz4_profile.hpp"
#include "uniform_lz4_sweep_profile.hpp"
#include "deferred_native_fixture.hpp"
#include "flitzfs/uniform_lz4_sweep.hpp"
#include "../native_lz4_fixture.hpp"
#include "../anchor_search_fixture.hpp"
#include "../../src/native_group_anchor.hpp"
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

void uniform_exchange(const fs::path& worker,const PreparedUniformLz4& batch,
    std::span<const std::byte> frame,HipWorkerExit expected,bool missing_runtime=false,
    unsigned fault=0,bool profile=false,bool sweep=false){
  const auto& limits=batch.limits();
  const auto reply_bytes=limits.reply_bytes+(profile?kUniformLz4ProfileBytes:0);
  std::vector<std::wstring> args{profile?L"--driver-uniform-lz4-profile-adapter":L"--driver-uniform-lz4-adapter",L"0",L"0100000000000000",L"1",
      std::wstring(64,L'0'),std::to_wstring(limits.request_bytes),std::to_wstring(reply_bytes),
      std::to_wstring(limits.chunks),std::to_wstring(limits.device_bytes),std::to_wstring(limits.chunk_bytes)};
  if(sweep){
    args[0]=profile?L"--driver-uniform-lz4-sweep-profile-adapter":L"--driver-uniform-lz4-sweep-adapter";
    uniform_lz4::Layout quantum{};require(uniform_lz4::make_layout(2U*limits.chunk_bytes,limits.chunk_bytes,quantum),"admission sweep quantum");
    args[7]=L"2";args[8]=std::to_wstring(quantum.total_bytes);
  }
  if(fault==5)args[7]=L"0";
  if(fault==6)args[8]=L"1";
  if(fault==1)args[7]=L"1"; // Actual 17-block request exceeds independently supplied grant.
  if(fault==2)args[8]=L"18446744073709551616";
  if(fault==3)args[6]=std::to_wstring(reply_bytes-1);
  if(fault==4)args[9]=L"32768";
  auto session=WorkerExchangeSession::start_sized(worker,args,frame,
      {static_cast<std::size_t>(limits.request_bytes),static_cast<std::size_t>(reply_bytes)},budget);
  const auto start=GetTickCount64();
  while(!session.try_result() && GetTickCount64()-start<17000)Sleep(1);
  const auto* result=session.try_result();
  require(result && !session.failure() && result->process.process_id && result->process.exit_confirmed &&
      !result->process.timed_out && !result->process.cancelled &&
      result->process.peak_committed_bytes<=128ULL*1024*1024,"Uniform admission child retirement");++exits;
  const auto category=hip_worker_exit_category(result->process.exit_code);
  require(result->code==WorkerExchangeCode::worker_failed && result->response.empty() &&
      !result->publication_observed && (category==static_cast<std::uint32_t>(expected) ||
      (missing_runtime && category==static_cast<std::uint32_t>(HipWorkerExit::integrity_failure))),
      "Uniform admission failure exposed candidate/incorrect category");
  while((!session.retirement_confirmed() || !WorkerExchangeSession::available()) &&
      GetTickCount64()-start<19000)Sleep(1);
  require(session.retirement_confirmed() && WorkerExchangeSession::available(),"Uniform supervisor not quiescent");
  for(const auto* suffix:{L".request",L".reply"}){
    const auto name=L"Local\\FlitzFS.worker."+result->channel_token+suffix;
    const auto mapping=OpenFileMappingW(FILE_MAP_READ,FALSE,name.c_str());
    const auto error=GetLastError();if(mapping)CloseHandle(mapping);
    require(!mapping && error==ERROR_FILE_NOT_FOUND,"Uniform admission retained a mapping");
  }
}
int uniform_admission(const fs::path& worker,const fs::path& root){
  require(worker.is_absolute() && root.is_absolute() && root.has_filename() &&
      root.filename()!=L"." && root.filename()!=L"..","Uniform absolute fixture paths");
  require(!fs::exists(root) && fs::create_directory(root),"NEW uniform fixture required");
  const auto binary=read(worker);
  const auto module_path=worker.parent_path()/L"hip-uniform-lz4"/L"flitzfs_rocm_uniform_lz4_candidate.dll";
  const auto module=read(module_path);
  child(worker,{L"--verify-uniform-lz4-kernel-bundle"},HipWorkerExit::invalid_request,true);
  const auto missing=root/L"missing";require(fs::create_directory(missing),"Uniform missing fixture");
  write_new(missing/worker.filename(),binary);
  child(missing/worker.filename(),{L"--verify-uniform-lz4-kernel-bundle"},HipWorkerExit::runtime_unavailable);
  const auto tamper=root/L"tamper";require(fs::create_directory(tamper),"Uniform tamper fixture");
  require(fs::create_directory(tamper/L"hip-uniform-lz4"),"Uniform tamper module directory");
  write_new(tamper/worker.filename(),binary);auto changed=module;changed.back()^=std::byte{1};
  write_new(tamper/L"hip-uniform-lz4"/module_path.filename(),changed);
  child(tamper/worker.filename(),{L"--verify-uniform-lz4-kernel-bundle"},HipWorkerExit::integrity_failure);
  Bytes source(16U*65536+37,std::byte{0x51});uniform_lz4::Layout layout{};
  require(uniform_lz4::make_layout(source.size(),65536,layout),"Uniform admission layout");
  const auto batch=PreparedUniformLz4::prepare(source,uniform_lz4_limits(layout));
  const auto frame=encode_uniform_lz4_request(batch,73519);
  uniform_exchange(worker,batch,frame,HipWorkerExit::runtime_unavailable,true);
  for(const auto at:{std::size_t{0},std::size_t{36},std::size_t{48}}){
    auto invalid=frame;invalid[at]^=std::byte{1};uniform_exchange(worker,batch,invalid,HipWorkerExit::invalid_request);
  }
  uniform_exchange(worker,batch,frame,HipWorkerExit::invalid_request,false,1);
  uniform_exchange(worker,batch,frame,HipWorkerExit::invalid_request,false,2);
  uniform_exchange(worker,batch,frame,HipWorkerExit::device_fault,false,3);
  uniform_exchange(worker,batch,frame,HipWorkerExit::invalid_request,false,4);
  uniform_exchange(worker,batch,frame,HipWorkerExit::runtime_unavailable,true,0,true);
  uniform_exchange(worker,batch,frame,HipWorkerExit::invalid_request,false,1,true);
  uniform_exchange(worker,batch,frame,HipWorkerExit::device_fault,false,3,true);
  auto invalid_profile=frame;invalid_profile[48]^=std::byte{1};
  uniform_exchange(worker,batch,invalid_profile,HipWorkerExit::invalid_request,false,0,true);
  uniform_exchange(worker,batch,frame,HipWorkerExit::runtime_unavailable,true,0,false,true);
  uniform_exchange(worker,batch,frame,HipWorkerExit::invalid_request,false,4,false,true);
  uniform_exchange(worker,batch,frame,HipWorkerExit::invalid_request,false,5,false,true);
  uniform_exchange(worker,batch,frame,HipWorkerExit::invalid_request,false,6,false,true);
  uniform_exchange(worker,batch,invalid_profile,HipWorkerExit::invalid_request,false,0,false,true);
  uniform_exchange(worker,batch,frame,HipWorkerExit::runtime_unavailable,true,0,true,true);
  uniform_exchange(worker,batch,frame,HipWorkerExit::invalid_request,false,4,true,true);
  uniform_exchange(worker,batch,frame,HipWorkerExit::invalid_request,false,5,true,true);
  uniform_exchange(worker,batch,frame,HipWorkerExit::invalid_request,false,6,true,true);
  uniform_exchange(worker,batch,invalid_profile,HipWorkerExit::invalid_request,false,0,true,true);
  require(read(worker)==binary && read(module_path)==module &&
      std::equal(source.begin(),source.end(),batch.plaintext().begin()),"Uniform trusted input/source changed");
  std::cout<<"Uniform LZ4 runtime admission: {\"checks\":"<<checks<<",\"exits_confirmed\":"<<exits
      <<",\"bundle_verified\":true,\"tamper_refused\":true,\"CPU_source_unchanged\":true,"
      "\"hip_runtime_loaded\":false,\"gpu_executed\":false,\"complete\":true}\n";
  return 0;
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
int native_cpu_fixture(const gpu_probe_fixture::Budget& controller){
  require(store_lz4_available() && gpu_lz4_cpu_verification_available(),"Independent native CPU LZ4 required");
  const auto plain=lz4_native_fixture::source();
  detail::StoreCodecEncoder encoder({StoreCompressionMode::fast,6,false});
  const auto candidate=encoder.encode(plain);
  require(candidate.codec==detail::kStoreLz4,"Native CPU candidate did not compress");
  std::uint64_t image_bytes=0,active_payload=0;
  for(const bool supplied:{true,false}){
    lz4_native_fixture::Volume volume;
    const auto selected=volume.publish(plain,supplied?std::span<const std::byte>(candidate.payload):
        std::span<const std::byte>{});
    require(selected==supplied,"Native candidate/CPU fallback selection mismatch");
    image_bytes=(std::max)(image_bytes,volume.image_bytes);
    active_payload=(std::max)(active_payload,volume.active_payload_bytes);
    volume.close();require(volume.closed,"Native working-set retirement not confirmed");
  }
  require(locked_ram_fixture::locked_bytes==0 && locked_ram_fixture::cleanup_ok,
      "Native CPU fixture leaked locked memory");
  std::cout<<"LZ4 native CPU fixture: {\"complete\":true,\"gpu_executed\":false,"
      "\"candidate_stored_exactly\":true,\"CPU_fallback_verified\":true,\"independent_native_readback\":true,"
      "\"source_bytes\":"<<plain.size()<<",\"image_bytes\":"<<image_bytes<<",\"active_payload_bytes\":"<<active_payload
      <<",\"locked_peak_bytes\":"<<locked_ram_fixture::peak_locked_bytes.load()
      <<",\"locked_after_bytes\":"<<locked_ram_fixture::locked_bytes.load()
      <<",\"controller_job_peak_commit_bytes\":"<<controller.peak_commit()
      <<",\"working_set_restored\":true,\"mounted\":false,\"physical_power_loss_qualified\":false}\n";
  return 0;
}
Bytes uniform_native_source(bool sweep){
  const auto block=lz4_native_fixture::source();
  const auto count=sweep?kStoreMaxObjectBytes/kNativeExtentBytes:2;
  Bytes plain;plain.reserve(count*block.size());
  for(std::uint32_t i=0;i<count;++i)plain.insert(plain.end(),block.begin(),block.end());
  return plain;
}
int uniform_native_cpu_fixture(bool sweep=false){
  const auto plain=uniform_native_source(sweep);
  const auto encoded=native_lz4_fixture::alternative_block(std::span(plain).first(kNativeExtentBytes));
  std::vector<NativeLz4ExtentCandidate> hints;
  for(std::uint32_t i=0;i<plain.size()/kNativeExtentBytes;++i)hints.push_back({i,encoded});
  lz4_native_fixture::Volume volume;const auto selected=volume.publish_uniform(plain,hints);
  require(selected,"uniform CPU fixture did not store exact external payload");
  const auto image=volume.image_bytes,payload=volume.active_payload_bytes;volume.close();
  require(locked_ram_fixture::locked_bytes==0 && locked_ram_fixture::cleanup_ok,"uniform CPU lock retirement");
  std::cout<<(sweep?"Uniform sweep native CPU fixture: {":"Uniform native CPU fixture: {")<<"\"complete\":true,\"source_bytes\":"<<plain.size()
      <<",\"image_bytes\":"<<image<<",\"active_payload_bytes\":"<<payload
      <<",\"candidate_stored_exactly\":true,\"independent_native_readback\":true,\"prior_preserved\":true,"
      "\"locked_peak_bytes\":"<<locked_ram_fixture::peak_locked_bytes.load()
      <<",\"locked_after_bytes\":0,\"working_set_restored\":true,\"gpu_executed\":false,\"mounted\":false}\n";
  return 0;
}
struct AnchorSearchInput {Bytes base,members;};
struct AnchorNativeResult {
  std::uint64_t compacted_bytes=0,work_ns=0;
  std::size_t selected_members=0;
  bool applied=false;
};
std::uint64_t nanos_since(std::chrono::steady_clock::time_point start) {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now()-start).count());
}
AnchorNativeResult anchor_native_trial(const AnchorSearchInput& input,const UniformLz4Candidates* candidates,
    const detail::AnchorMatchAdvice& direct={}) {
  const auto start=std::chrono::steady_clock::now();
  locked_ram_fixture::WorkingSet working;
  constexpr std::size_t capacity=4U*1024*1024;
  SYSTEM_INFO system{};GetSystemInfo(&system);
  const StoreCompressionOptions compression{StoreCompressionMode::automatic,6,false};
  const auto count=input.members.size()/kNativeExtentBytes;
  require(direct.empty() || (!candidates && direct.size()==count),"anchor advice extent count");
  const auto make_id=[](unsigned value){StoreIdentity id{};id[0]=static_cast<std::byte>(value);return id;};
  auto image=std::make_shared<locked_ram_fixture::Store>(capacity,system.dwPageSize);
  auto packed=std::make_shared<locked_ram_fixture::Store>(capacity,system.dwPageSize);
  std::optional<NativeFilesystemSnapshot> before,after;
  AnchorNativeResult outcome;
  const auto member_source=[&](std::size_t index) {
    return index?std::span<const std::byte>(input.members).subspan((index-1)*kNativeExtentBytes,kNativeExtentBytes):
        std::span<const std::byte>(input.base);
  };
  {
    auto writer=NativeFilesystemWriter::initialize_indexed(
        PagedObjectStoreWriter::create_mirrored(image,make_id(1),{capacity},compression));
    std::vector<NativeExtentSelection> selections;
    for(std::size_t i=0;i<=count;++i) {
      NativeFileMutation create;create.operation=NativeFileOperation::create_file;create.path="f"+std::to_string(i);
      const auto commit=writer.apply({&create,1});
      require(commit.created_inodes.size()==1,"anchor fixture inode");
      const auto inode=commit.created_inodes.front();
      writer.write_inode_storage_result(inode,0,member_source(i));
      selections.push_back({inode,0});
    }
    before.emplace(writer.snapshot());
    auto work=writer.prepare_shared_base(selections.front(),selections);
    std::size_t at=0;
    for(;;) {
      const auto index=work.progress().completed_members;
      std::vector<NativeSharedBaseCopyRange> advice;
      if(candidates && index) {
        std::array<UniformLz4Bytes,detail::kAnchorLz4BlocksPerMember> blocks;
        for(std::size_t block=0;block<blocks.size();++block) {
          const auto size=candidates->sizes.at((index-1)*blocks.size()+block);
          require(at<=candidates->payload.size() && size<=candidates->payload.size()-at,"anchor reply bounds");
          blocks[block]=std::span<const std::byte>(candidates->payload).subspan(at,size);at+=size;
        }
        advice=detail::native_anchor_copy_ranges(blocks);
      }
      const auto ranges=index && !direct.empty()?std::span<const NativeSharedBaseCopyRange>(direct.at(index-1)):
          std::span<const NativeSharedBaseCopyRange>(advice);
      if(writer.advance_shared_base(work,ranges))break;
    }
    outcome.selected_members=work.progress().selected_advice_members;
    outcome.applied=writer.finish_shared_base(work).applied;
    after.emplace(writer.snapshot());
  }
  (void)NativeFilesystemWriter::compact(*after,packed,make_id(2),compression);
  const auto reopened=NativeFilesystemSnapshot::open_image(packed,{NativeImageKind::indexed_paged,capacity});
  reopened.verify_all();
  for(std::size_t i=0;i<=count;++i) {
    const auto path="f"+std::to_string(i);
    require(std::ranges::equal(reopened.read(path,0,kNativeExtentBytes),member_source(i)) &&
        std::ranges::equal(before->read(path,0,kNativeExtentBytes),member_source(i)),
        "anchor compacted or pinned source differs");
  }
  image->verify();packed->verify();
  outcome.compacted_bytes=packed->extent();
  outcome.work_ns=nanos_since(start);
  return outcome;
}
int direct_anchor_gpu_once(wchar_t** argv,const AnchorSearchInput& input) {
  const fs::path worker(argv[2]);
  require(worker.is_absolute() && worker.filename()==L"flitzfs_hip_worker.exe","Trusted direct anchor worker required");
  const auto mask=number(argv[5],(std::numeric_limits<unsigned>::max)());
  (void)number(argv[3],63);
  const auto luid=luid_value(argv[4]);
  const std::wstring_view digest(argv[6]);
  require(mask && !(mask&(mask-1)) && digest.size()==64 &&
      std::all_of(digest.begin(),digest.end(),[](wchar_t c){
        return (c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f');}),"Direct anchor adapter/runtime binding invalid");

  const auto frame=detail::encode_anchor_match_request(input.base,input.members,1);
  const auto request=detail::decode_anchor_match_request(frame);
  const auto device_bytes=anchor_match::device_bytes(detail::anchor_match_members(request.source));
  const auto observed=query_windows_gpu_budget(luid,mask);
  const auto admitted=observed.status==WindowsGpuBudgetStatus::success &&
      check_gpu_memory_budget(observed.snapshot,luid,mask,GetTickCount64(),device_bytes)==GpuGateCode::admitted;
  const auto start=std::chrono::steady_clock::now();
  std::optional<WorkerExchangeResult> result;
  detail::AnchorMatchAdvice gpu_advice;
  bool verified=false;
  if(admitted) {
    result=run_worker_exchange(worker,{L"--driver-anchor-match",argv[3],argv[4],argv[5],argv[6]},frame,budget);
    require(result->process.exit_confirmed && WorkerExchangeSession::available(),
        "Direct anchor worker retirement unconfirmed");
    if(result->code==WorkerExchangeCode::completed && result->process.exit_code==0 &&
        !result->process.timed_out && !result->process.cancelled) {
      try {
        gpu_advice=detail::accept_anchor_match_reply(result->response,request);
        verified=true;
      } catch(const std::invalid_argument&) {
        gpu_advice.clear();
      }
    }
  }
  const auto exchange_ns=nanos_since(start);
  const auto cpu_start=std::chrono::steady_clock::now();
  const auto cpu_records=detail::cpu_anchor_match_records(request);
  const auto cpu_advice=detail::verify_anchor_match_records(request,cpu_records);
  const auto cpu_ns=nanos_since(cpu_start);
  const auto exact=verified && result->response==detail::encode_anchor_match_reply(request,cpu_records);
  const auto baseline=anchor_native_trial(input,nullptr);
  const auto cpu=anchor_native_trial(input,nullptr,cpu_advice);
  std::optional<AnchorNativeResult> gpu;
  if(exact)gpu=anchor_native_trial(input,nullptr,gpu_advice);
  const auto& selected=gpu?*gpu:cpu;
  require(locked_ram_fixture::locked_bytes==0 && locked_ram_fixture::cleanup_ok,
      "Direct anchor fixture leaked locked memory");
  std::cout<<std::boolalpha<<"Direct AMD shared base search: {\"gpu_executed\":"<<verified
      <<",\"admitted\":"<<admitted<<",\"cpu_fallback\":"<<!gpu
      <<",\"cpu_record_parity\":"<<exact
      <<",\"source_bytes\":"<<request.source.size()<<",\"worker_request_bytes\":"<<frame.size()
      <<",\"worker_reply_bytes\":"<<(result?result->response.size():0)<<",\"device_allocation_bytes\":"<<device_bytes
      <<",\"host_to_device_bytes\":"<<(verified?request.source.size():0)
      <<",\"device_to_host_bytes\":"<<(verified?cpu_records.size()*sizeof(cpu_records[0]):0)
      <<",\"cpu_search_ns\":"<<cpu_ns<<",\"exchange_ns\":"<<exchange_ns
      <<",\"cpu_baseline_compacted_bytes\":"<<baseline.compacted_bytes
      <<",\"cpu_direct_compacted_bytes\":"<<cpu.compacted_bytes
      <<",\"amd_direct_compacted_bytes\":"<<(gpu?std::to_string(gpu->compacted_bytes):"null")
      <<",\"amd_direct_applied\":"<<(gpu&&gpu->applied)<<",\"amd_prepared_members\":"<<(gpu?gpu->selected_members:0)
      <<",\"selected_compacted_bytes\":"<<selected.compacted_bytes
      <<",\"zstd_available\":"<<store_zstd_available()
      <<",\"worker_exit\":"<<(result?result->process.exit_code:0)
      <<",\"worker_stage\":\""<<(result?hip_worker_stage_name(hip_worker_exit_stage(result->process.exit_code)):"none")<<'"'
      <<",\"retirement_confirmed\":"<<(!result||result->process.exit_confirmed)
      <<",\"cpu_readback_verified\":true,\"locked_after_bytes\":0}\n";
  return verified&&exact?0:2;
}
UniformLz4Candidates cpu_anchor_candidates(const PreparedUniformLz4& batch) {
  detail::StoreCodecEncoder encoder({StoreCompressionMode::fast,1,false});
  UniformLz4Candidates result;
  for(std::uint32_t i=0;i<batch.layout().chunks;++i) {
    const auto bytes=batch.input(i);
    auto encoded=encoder.encode(Bytes(bytes.begin(),bytes.end()));
    const auto usable=encoded.codec==detail::kStoreLz4;
    result.sizes.push_back(usable?static_cast<std::uint32_t>(encoded.payload.size()):0);
    if(usable)result.payload.insert(result.payload.end(),encoded.payload.begin(),encoded.payload.end());
  }
  verify_uniform_lz4_candidates(batch,result);
  return result;
}
bool retire_uniform_exchange(const std::optional<WorkerExchangeSession>& session) {
  if(!session)return true;
  const auto started=GetTickCount64();
  do {
    if(session->retirement_confirmed() && WorkerExchangeSession::available())return true;
    Sleep(1);
  }while(GetTickCount64()-started<2000);
  return false;
}
int uniform_native_gpu_once(int argc,wchar_t** argv,const gpu_probe_fixture::Budget& controller,
    bool sweep=false,bool sweep_profile=false,const AnchorSearchInput* anchor=nullptr){
  require(argc==7 || (anchor && argc==8),"Uniform calibration needs explicit trusted worker/ordinal/LUID/node/runtime digest");
  require(store_lz4_available() && gpu_lz4_cpu_verification_available(),"Independent uniform CPU recovery required");
  const fs::path worker(argv[2]);
  require(worker.is_absolute() && worker.filename()==L"flitzfs_hip_lz4_worker.exe","Trusted uniform worker required");
  const auto ordinal=number(argv[3],63),mask=number(argv[5],(std::numeric_limits<unsigned>::max)());
  const auto luid=luid_value(argv[4]);const std::wstring_view digest(argv[6]);
  require(mask && !(mask&(mask-1)) && digest.size()==64 &&
      std::all_of(digest.begin(),digest.end(),[](wchar_t c){
        return (c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f');}),"Uniform adapter/runtime binding invalid");
  const auto prepare_start=std::chrono::steady_clock::now();
  const auto plain=anchor?Bytes{}:uniform_native_source(sweep);
  const auto input_bytes=anchor?anchor->members.size()*detail::kAnchorLz4BlocksPerMember:plain.size();
  uniform_lz4::Layout layout{};require(uniform_lz4::make_layout(input_bytes,65536,layout),"Uniform calibration layout");
  const auto layout_limits=uniform_lz4_limits(layout);
  const auto batch=anchor?detail::prepare_native_anchor_search(anchor->base,anchor->members,layout_limits):
      PreparedUniformLz4::prepare(plain,layout_limits);
  const auto prepare_ns=nanos_since(prepare_start);
  const UniformLz4ProfileBinding binding{71519,ordinal,mask,layout.chunks,layout.chunk_bytes,layout.total_bytes,luid};
  uniform_lz4::Layout quantum{};require(uniform_lz4::make_layout(2U*kNativeExtentBytes,kNativeExtentBytes,quantum),"native sweep quantum");
  const auto plan=plan_uniform_lz4_sweep(batch,2,quantum.total_bytes);
  const auto device_bytes=sweep?plan.device_peak_bytes:layout.total_bytes;
  const auto reply_bytes=batch.limits().reply_bytes+(sweep?(sweep_profile?kUniformLz4SweepProfileBytes:0):kUniformLz4ProfileBytes);
  const auto grant_chunks=sweep?plan.quantum_chunks:layout.chunks;
  std::optional<lz4_native_fixture::Volume> volume;
  if(!anchor)volume.emplace();
  const auto before=query_windows_gpu_budget(luid,mask);
  const bool admitted=before.status==WindowsGpuBudgetStatus::success &&
      check_gpu_memory_budget(before.snapshot,luid,mask,GetTickCount64(),device_bytes)==GpuGateCode::admitted;
  std::optional<WorkerExchangeSession> session;const WorkerExchangeResult* result=nullptr;
  bool verified=false;UniformLz4ProfiledReply accepted;UniformLz4SweepTrace sweep_trace;std::uint64_t exchange_ns=0;
  if(admitted){
    try{
    const auto frame=encode_uniform_lz4_request(batch,binding.request_id);const auto started=std::chrono::steady_clock::now();
    session.emplace(WorkerExchangeSession::start_sized(worker,{sweep?(sweep_profile?L"--driver-uniform-lz4-sweep-profile-adapter":L"--driver-uniform-lz4-sweep-adapter"):L"--driver-uniform-lz4-profile-adapter",
        std::to_wstring(ordinal),argv[4],std::to_wstring(mask),std::wstring(digest),
        std::to_wstring(batch.limits().request_bytes),std::to_wstring(reply_bytes),
        std::to_wstring(grant_chunks),std::to_wstring(device_bytes),std::to_wstring(layout.chunk_bytes)},
        frame,{static_cast<std::size_t>(batch.limits().request_bytes),
          static_cast<std::size_t>(reply_bytes)},{128,2000,5000}));
    const auto wait_started=GetTickCount64();
    while(!(result=session->try_result()) && GetTickCount64()-wait_started<7000)Sleep(1);
    exchange_ns=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-started).count());
    if(result && result->code==WorkerExchangeCode::completed && result->process.exit_confirmed &&
        !result->process.timed_out && !result->process.cancelled){
      try{
        const auto after=query_windows_gpu_budget(luid,mask);
        if(after.status==WindowsGpuBudgetStatus::success &&
            check_gpu_memory_transition(before.snapshot,after.snapshot,luid,mask,GetTickCount64(),device_bytes)==GpuGateCode::admitted){
          if(sweep_profile){
            const UniformLz4ProfileBinding sweep_binding{binding.request_id,ordinal,mask,grant_chunks,layout.chunk_bytes,device_bytes,luid};
            auto profiled=accept_uniform_lz4_sweep_profile_reply(result->response,batch,sweep_binding,WorkerExecution::hip);
            sweep_trace=profiled.trace;accepted.candidates=std::move(profiled.candidates);
          }else if(sweep)accepted.candidates=accept_uniform_lz4_reply(result->response,batch,binding.request_id,WorkerExecution::hip);
          else accepted=accept_uniform_lz4_profile_reply(result->response,batch,binding,WorkerExecution::hip);
          verified=true;
        }
      }catch(const std::exception&){} // Terminal independent CPU source remains available.
    }
    }catch(const std::exception&){verified=false;}
    if(!verified && session)(void)session->request_cancel();
  }
  if(anchor) {
    const auto retired=retire_uniform_exchange(session);
    require(retired,"Anchor transport retirement unconfirmed");
    if(session)result=session->try_result();
    const auto gpu_before_storage_ns=nanos_since(prepare_start);
    const auto plain_result=anchor_native_trial(*anchor,nullptr);
    const auto cpu_start=std::chrono::steady_clock::now();
    const auto cpu=cpu_anchor_candidates(batch);
    const auto cpu_search_ns=nanos_since(cpu_start);
    const auto cpu_result=anchor_native_trial(*anchor,&cpu);
    const auto gpu_result=verified?anchor_native_trial(*anchor,&accepted.candidates):plain_result;
    require(locked_ram_fixture::locked_bytes==0 && locked_ram_fixture::cleanup_ok,"Anchor fixture retirement");
    std::cout<<"Anchor collective comparison: {\"gpu_verified\":"<<(verified?"true":"false")
        <<",\"worker_started\":"<<(session.has_value()?"true":"false")
        <<",\"native_members\":"<<anchor->members.size()/kNativeExtentBytes
        <<",\"logical_shared_input_bytes\":"<<anchor->base.size()+anchor->members.size()
        <<",\"transport_input_bytes\":"<<batch.plaintext().size()
        <<",\"reply_payload_bytes\":"<<accepted.candidates.payload.size()
        <<",\"cpu_native_bytes\":"<<plain_result.compacted_bytes
        <<",\"cpu_tiled_bytes\":"<<cpu_result.compacted_bytes
        <<",\"gpu_tiled_bytes\":"<<gpu_result.compacted_bytes
        <<",\"cpu_tiled_selected_members\":"<<cpu_result.selected_members
        <<",\"gpu_tiled_selected_members\":"<<gpu_result.selected_members
        <<",\"gpu_group_applied\":"<<(gpu_result.applied?"true":"false")
        <<",\"prepare_ns\":"<<prepare_ns<<",\"exchange_ns\":"<<exchange_ns
        <<",\"cpu_native_ns\":"<<plain_result.work_ns
        <<",\"cpu_tiled_search_ns\":"<<cpu_search_ns
        <<",\"cpu_tiled_total_ns\":"<<prepare_ns+cpu_search_ns+cpu_result.work_ns
        <<",\"gpu_tiled_total_ns\":"<<gpu_before_storage_ns+gpu_result.work_ns
        <<",\"gpu_setup_ns\":"<<sweep_trace.wall_ns[0]
        <<",\"gpu_maximum_kernel_quantum_ns\":"<<sweep_trace.maximum_kernel_wall_ns
        <<",\"gpu_completed_windows\":"<<sweep_trace.completed_windows
        <<",\"worker_exit\":"<<(result?result->process.exit_code:0)
        <<",\"job_peak_bytes\":"<<controller.peak_commit()
        <<",\"locked_peak_bytes\":"<<locked_ram_fixture::peak_locked_bytes.load()
        <<",\"zstandard_available\":"<<(store_zstd_available()?"true":"false")
        <<",\"independent_native_readback\":true,\"transport_quiescent\":true}\n";
    return verified?0:2;
  }
  std::vector<NativeLz4ExtentCandidate> hints;
  if(verified){
    std::size_t at=0;for(std::uint32_t i=0;i<layout.chunks;++i){
      const auto size=accepted.candidates.sizes[i];
      hints.push_back({i,std::span<const std::byte>(accepted.candidates.payload).subspan(at,size)});at+=size;
    }
  }
  const auto stored=volume->publish_uniform(plain,hints);
  require(std::equal(plain.begin(),plain.end(),batch.plaintext().begin()),"Uniform retained source changed");
  const auto image=volume->image_bytes,payload=volume->active_payload_bytes;volume->close();
  const auto retired=retire_uniform_exchange(session);
  if(session)result=session->try_result(); // Report late exit status, never accept late output.
  require(locked_ram_fixture::locked_bytes==0 && locked_ram_fixture::cleanup_ok,"Uniform native lock retirement");
  const auto kernel=accepted.trace.wall_ns[static_cast<unsigned>(HipPhase::kernel_wait)];
  std::cout<<(sweep?"Uniform native sweep GPU: {":"Uniform native GPU calibration: {")<<"\"gpu_result_verified\":"<<(verified?"true":"false")
      <<",\"gpu_payload_stored_exactly\":"<<(stored?"true":"false")
      <<",\"CPU_fallback_selected\":"<<(!(verified && stored)?"true":"false")
      <<",\"source_bytes\":"<<plain.size()<<",\"chunks\":"<<layout.chunks<<",\"device_bytes\":"<<device_bytes
      <<",\"planned_windows\":"<<(sweep?plan.windows:1)<<",\"quantum_chunks\":"<<grant_chunks
      <<",\"worker_payload_bytes\":"<<accepted.candidates.payload.size()<<",\"image_bytes\":"<<image
      <<",\"active_payload_bytes\":"<<payload<<",\"exchange_ns\":"<<exchange_ns
      <<",\"phase_profile_supported\":"<<(sweep?"false":"true")
      <<",\"trace_total_wall_ns\":"<<accepted.trace.total_wall_ns<<",\"kernel_wait_wall_ns\":"<<kernel
      <<",\"phase_wall_ns\":[";
  for(unsigned i=0;i<hip_phase_count;++i){if(i)std::cout<<',';std::cout<<accepted.trace.wall_ns[i];}
  std::cout<<"],\"sweep_profile_supported\":"<<(sweep_profile?"true":"false")
      <<",\"sweep_trace_complete\":"<<(sweep_trace.finished?"true":"false")
      <<",\"sweep_trace_total_wall_ns\":"<<sweep_trace.total_wall_ns
      <<",\"sweep_completed_windows\":"<<sweep_trace.completed_windows
      <<",\"sweep_maximum_kernel_wall_ns\":"<<sweep_trace.maximum_kernel_wall_ns
      <<",\"sweep_minimum_kernel_wall_ns\":"<<sweep_trace.minimum_kernel_wall_ns
      <<",\"sweep_maximum_quantum_wall_ns\":"<<sweep_trace.maximum_quantum_wall_ns
      <<",\"sweep_phase_wall_ns\":[";
  for(unsigned i=0;i<uniform_sweep_phase_count;++i){if(i)std::cout<<',';std::cout<<sweep_trace.wall_ns[i];}
  std::cout<<"],\"trace_complete\":"<<(accepted.trace.finished?"true":"false")
      <<",\"worker_exit\":"<<(result?result->process.exit_code:0)
      <<",\"worker_peak_commit_bytes\":"<<(result?result->process.peak_committed_bytes:0)
      <<",\"controller_job_peak_commit_bytes\":"<<controller.peak_commit()
      <<",\"transport_quiescent\":"<<(retired?"true":"false")
      <<",\"independent_native_readback\":true,\"prior_preserved\":true,\"retained_source_verified\":true,"
      "\"locked_peak_bytes\":"<<locked_ram_fixture::peak_locked_bytes.load()
      <<",\"locked_after_bytes\":0,\"working_set_restored\":true,\"normal_io_route\":false,"
      "\"cost_profile_qualified\":false,\"mounted\":false,\"physical_power_loss_qualified\":false}\n";
  return verified && stored && retired?0:2;
}
int native_gpu_once(int argc,wchar_t** argv,const gpu_probe_fixture::Budget& controller){
  require(argc==7,"Native GPU composition needs explicit worker/ordinal/LUID/node/runtime hash");
  require(store_lz4_available() && gpu_lz4_cpu_verification_available(),"Independent native CPU recovery required");
  const fs::path worker(argv[2]);
  require(worker.is_absolute() && worker.filename()==L"flitzfs_hip_lz4_worker.exe",
      "Absolute trusted compression worker required");
  const auto ordinal=number(argv[3],63),mask=number(argv[5],(std::numeric_limits<unsigned>::max)());
  const auto luid=luid_value(argv[4]);
  require(mask && !(mask&(mask-1)),"Native composition needs one physical node");
  const std::wstring_view digest(argv[6]);
  require(digest.size()==64 && std::all_of(digest.begin(),digest.end(),[](wchar_t c){
      return (c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f');}),"Exact installed runtime digest required");
  const auto plain=lz4_native_fixture::source();
  const std::array<Lz4WorkerBytes,1> input{plain};
  const auto prepared=PreparedLz4WorkerBatch::prepare(input);
  lz4_native_fixture::Volume volume; // Preserve a CPU-readable acknowledged sentinel before GPU work.
  const auto before=query_windows_gpu_budget(luid,mask);
  require(before.status==WindowsGpuBudgetStatus::success &&
      check_gpu_memory_budget(before.snapshot,luid,mask,GetTickCount64(),
          prepared.device_layout().total_bytes)==GpuGateCode::admitted,"Native GPU headroom unavailable");
  const auto frame=encode_lz4_worker_request(prepared,521);
  const auto started=std::chrono::steady_clock::now();
  const auto result=run_worker_exchange(worker,{L"--driver-lz4-adapter",std::to_wstring(ordinal),
      argv[4],std::to_wstring(mask),std::wstring(digest)},frame,{128,2000,5000});
  const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now()-started).count();
  require(result.process.process_id && result.process.exit_confirmed &&
      WorkerExchangeSession::available(),"Native worker/supervisor retirement unconfirmed; no retry");
  std::vector<Bytes> payloads;bool verified=false;
  if(result.code==WorkerExchangeCode::completed && result.process.exit_code==0 &&
      !result.process.timed_out && !result.process.cancelled){
    const auto after=query_windows_gpu_budget(luid,mask);
    require(after.status==WindowsGpuBudgetStatus::success &&
        check_gpu_memory_transition(before.snapshot,after.snapshot,luid,mask,GetTickCount64(),
            prepared.device_layout().total_bytes)==GpuGateCode::admitted,"Native GPU completion resource refusal");
    payloads=accept_lz4_worker_reply(result.response,prepared,521,WorkerExecution::hip);
    require(payloads.size()==1,"Native composition returned wrong block count");verified=true;
  }
  const std::span<const std::byte> candidate=verified?std::span<const std::byte>(payloads.front()):
      std::span<const std::byte>{};
  const auto selected=volume.publish(plain,candidate);
  require(std::equal(plain.begin(),plain.end(),prepared.input(0).begin(),prepared.input(0).end()),
      "Native GPU source changed");
  const auto image_bytes=volume.image_bytes,active_payload=volume.active_payload_bytes;
  volume.close();
  require(volume.closed && locked_ram_fixture::locked_bytes==0 && locked_ram_fixture::cleanup_ok,
      "Native GPU fixture cleanup failed");
  std::cout<<"LZ4 native GPU composition: {\"gpu_result_verified\":"<<(verified?"true":"false")
      <<",\"gpu_payload_stored_exactly\":"<<(selected?"true":"false")<<",\"CPU_fallback_selected\":"<<(!selected?"true":"false")
      <<",\"independent_native_readback\":true,\"retained_source_verified\":true,\"source_bytes\":"<<plain.size()
      <<",\"worker_payload_bytes\":"<<candidate.size()<<",\"image_bytes\":"<<image_bytes
      <<",\"active_payload_bytes\":"<<active_payload<<",\"device_bytes\":"<<prepared.device_layout().total_bytes
      <<",\"exchange_ns\":"<<elapsed<<",\"worker_exit\":"<<result.process.exit_code
      <<",\"worker_category\":"<<hip_worker_exit_category(result.process.exit_code)
      <<",\"worker_stage\":\""<<hip_worker_stage_name(hip_worker_exit_stage(result.process.exit_code))
      <<"\",\"worker_exit_confirmed\":true,\"transport_quiescent\":true,\"worker_timed_out\":"
      <<(result.process.timed_out?"true":"false")<<",\"worker_peak_commit_bytes\":"<<result.process.peak_committed_bytes
      <<",\"controller_job_peak_commit_bytes\":"<<controller.peak_commit()
      <<",\"locked_peak_bytes\":"<<locked_ram_fixture::peak_locked_bytes.load()
      <<",\"locked_after_bytes\":0,\"working_set_restored\":true,\"mounted\":false,"
      "\"physical_power_loss_qualified\":false}\n";
  return verified && selected?0:2;
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
    if(argc==2&&std::wstring_view(argv[1])==L"--deferred-native-cpu-fixture")
      return deferred_native_fixture::run({},controller);
    if(argc==2&&std::wstring_view(argv[1])==L"--deferred-native-cpu-once"){
      deferred_native_fixture::Configuration configuration;
      configuration.invocation=deferred_native_fixture::Invocation::explicit_cpu;
      return deferred_native_fixture::run(std::move(configuration),controller);
    }
    if(argc==2&&std::wstring_view(argv[1])==L"--deferred-native-cpu-series")
      return deferred_native_fixture::run_comparison({},controller,deferred_native_fixture::Comparison::cpu_only);
    if(argc>1&&(std::wstring_view(argv[1])==L"--gpu-deferred-native-once"||
        std::wstring_view(argv[1])==L"--gpu-deferred-native-compare")){
      require(argc==7,"Deferred native GPU needs trusted worker/ordinal/LUID/node/runtime digest");
      deferred_native_fixture::Configuration configuration;
      configuration.invocation=deferred_native_fixture::Invocation::gpu;
      configuration.worker.helper=argv[2];
      require(configuration.worker.helper.is_absolute()&&
          configuration.worker.helper.filename()==L"flitzfs_hip_lz4_worker.exe","Trusted deferred worker required");
      configuration.worker.ordinal=static_cast<int>(number(argv[3],63));
      configuration.luid=luid_value(argv[4]);
      configuration.node_mask=number(argv[5],(std::numeric_limits<unsigned>::max)());
      require(configuration.node_mask&&!(configuration.node_mask&(configuration.node_mask-1)),
          "Deferred GPU requires one corroborated node");
      const std::wstring_view runtime(argv[6]);
      require(runtime.size()==64&&std::all_of(runtime.begin(),runtime.end(),[](wchar_t c){
        return (c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f');}),"Deferred runtime digest invalid");
      const auto digit=[](wchar_t c){return static_cast<unsigned>(c<=L'9'?c-L'0':c-L'a'+10);};
      for(std::size_t i=0;i<configuration.worker.runtime_sha256.size();++i)
        configuration.worker.runtime_sha256[i]=static_cast<std::byte>((digit(runtime[2*i])<<4)|digit(runtime[2*i+1]));
      if(std::wstring_view(argv[1])==L"--gpu-deferred-native-compare")
        return deferred_native_fixture::run_comparison(std::move(configuration),controller,
            deferred_native_fixture::Comparison::matched);
      return deferred_native_fixture::run(std::move(configuration),controller);
    }
    if(argc==4 && std::wstring_view(argv[1])==L"--uniform-admission")return uniform_admission(argv[2],argv[3]);
    if(argc==2 && std::wstring_view(argv[1])==L"--uniform-sweep-native-cpu-fixture")return uniform_native_cpu_fixture(true);
    const auto mode=argc>1?std::wstring_view(argv[1]):std::wstring_view{};
    if(mode==L"--gpu-indexed-anchor-once" || mode==L"--gpu-indexed-direct-anchor-once" ||
        mode==L"--gpu-indexed-tiled-anchor-once") {
      require(argc==7 || argc==8,"Anchor search needs the exact worker binding and optional public extent");
      AnchorSearchInput input;
      if(argc==8) {
        const fs::path corpus(argv[7]);
        require(corpus.is_absolute() && fs::file_size(corpus)==kNativeExtentBytes,"public anchor extent size");
        input.base=read(corpus);
      } else {
        input.base.resize(kNativeExtentBytes);
        std::uint64_t state=0x679a210fed44c3abULL;
        for(auto& value:input.base){state^=state<<13;state^=state>>7;state^=state<<17;value=static_cast<std::byte>(state>>21);}
      }
      const auto members=mode==L"--gpu-indexed-anchor-once"?4:anchor_match::max_members;
      for(unsigned i=1;i<=members;++i) {
        const auto member=anchor_search_fixture::fragmented(input.base,i);
        input.members.insert(input.members.end(),member.begin(),member.end());
      }
      if(mode==L"--gpu-indexed-direct-anchor-once")return direct_anchor_gpu_once(argv,input);
      return uniform_native_gpu_once(argc,argv,controller,true,true,&input);
    }
    if(argc>1 && std::wstring_view(argv[1])==L"--gpu-uniform-sweep-profile-native-once")return uniform_native_gpu_once(argc,argv,controller,true,true);
    if(argc>1 && std::wstring_view(argv[1])==L"--gpu-uniform-sweep-native-once")return uniform_native_gpu_once(argc,argv,controller,true);
    if(argc==2 && std::wstring_view(argv[1])==L"--uniform-native-cpu-fixture")return uniform_native_cpu_fixture();
    if(argc>1 && std::wstring_view(argv[1])==L"--gpu-uniform-native-once")return uniform_native_gpu_once(argc,argv,controller);
    if(argc==2 && std::wstring_view(argv[1])==L"--native-cpu-fixture")return native_cpu_fixture(controller);
    if(argc>1 && std::wstring_view(argv[1])==L"--gpu-native-once")return native_gpu_once(argc,argv,controller);
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
    // A frame below the common transport header is rejected before launch.
    // Keep that separate from the worker's truncated LZ4-directory rejection.
    bool short_transport_refused=false;
    try{
      (void)run_worker_exchange(worker,{},std::span<const std::byte>(frame).first(12),budget);
    }catch(const std::invalid_argument&){
      short_transport_refused=true;
    }
    require(short_transport_refused && WorkerExchangeSession::available(),
        "Short transport frame was admitted or consumed a worker slot");
    exchange(worker,std::span<const std::byte>(frame).first(kLz4WorkerHeaderBytes),
        HipWorkerExit::invalid_request);
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

