#include "volume_controller.hpp"
#include "bounded_process.hpp"
#include "windows_app_budget.hpp"
#include "flitzfs/native_volume_file.hpp"
#include "flitzfs/read_only_file.hpp"
#include "../src/record_digest.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <pdh.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <new>
#include <set>
#include <string>
#include <exception>
#include <iostream>
#include <thread>
// Test executable only: fail one selected 64-KiB allocation on the calling
// thread, then disarm before exception unwinding. No global memory pressure,
// product fault switch, changed provider or failure in the GUI child is used.
namespace advice_allocation_fixture {
thread_local bool armed=false,hit=false,capturing=false;
thread_local std::size_t allocation_bytes=0,observed_bytes=0;
thread_local unsigned remaining=0,admitted_binding=0;
thread_local const flitzfs::VolumeModel* admitted_model=nullptr;
bool refuse(std::size_t size) noexcept {
  if(capturing)observed_bytes=(std::max)(observed_bytes,size);
  if(!armed||size!=allocation_bytes||!admitted_model||
      admitted_model->optimization_info().binding_attempts!=admitted_binding)return false;
  if(remaining){--remaining;return false;}
  armed=false;hit=true;return true;
}
struct Failure {
  Failure(unsigned skip,const flitzfs::VolumeModel& model,unsigned binding) noexcept {
    remaining=skip;hit=false;admitted_model=&model;admitted_binding=binding;armed=true;
  }
  ~Failure(){armed=false;admitted_model=nullptr;}
  Failure(const Failure&)=delete;
  Failure& operator=(const Failure&)=delete;
  bool reached() const noexcept{return hit;}
};
}
void* operator new(std::size_t size) {
  if(advice_allocation_fixture::refuse(size))throw std::bad_alloc();
  // Preserve ordinary zero-size and new-handler behavior outside the one-shot
  // test fault. Aligned allocations retain their CRT allocation/deletion pair.
  for(;;) {
    if(void* p=std::malloc(size?size:1))return p;
    const auto handler=std::get_new_handler();
    if(!handler)throw std::bad_alloc();
    handler();
  }
}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete(void* p,std::size_t) noexcept {::operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept {::operator delete(p);}

namespace flitzfs {
// Qualification binary only. Faults touch its freshly allocated private RAM;
// no production command, flag, provider override or arbitrary path exists.
struct VolumeModelQualification {
  static std::shared_ptr<StoreByteIo> current(VolumeModel& m) {
    const auto n=m.session_->optimization_info().binding_attempts;
    if(!n||n>m.advice_.size()||!m.advice_[n-1])throw std::runtime_error("Qualification mapping");
    return m.advice_[n-1];
  }
  static std::vector<std::byte> bytes(const std::shared_ptr<StoreByteIo>& io) {
    if(io->extent()!=kOptimizationAreaBytes)throw std::runtime_error("Qualification area extent");
    std::vector<std::byte> out(kOptimizationAreaBytes);io->read_exact(0,out);return out;
  }
  static void damage(VolumeModel& m,unsigned copies=2) {
    auto area=current(m);
    for(unsigned slot=0;slot<copies;++slot) {
      std::array<std::byte,1> byte{};
      const auto at=slot*kStoreCommitUnit+96;
      area->read_exact(at,byte);byte[0]^=std::byte{1};area->write_exact(at,byte);
    }
    area->flush();
  }
  static std::shared_ptr<StoreByteIo> fill_remaining(VolumeModel& m) {
    const auto use=m.domain_->usage();return m.domain_->create_store(use.capacity-use.reserved);
  }
  static NativeFileHandle open_cleaned(VolumeModel& m) {
    auto file=m.session_->open("keep.txt",{true,false,false},NativeOpenKind::file);
    m.session_->cleanup(file,false);return file;
  }
  static std::vector<std::byte> read(VolumeModel& m,const NativeFileHandle& f) {
    return m.session_->read(f,0,32);
  }
  static void close(VolumeModel& m,const NativeFileHandle& f){m.session_->close(f);}
  // Exercise the normal mounted-session-sized write on the same helper
  // backing without requiring a larger limit in the experimental text editor.
  static void create_extent(VolumeModel& model,std::string_view name,std::string_view contents) {
    model.create_text(name,{});
    const auto handle=model.session_->open(name,{true,true,false},NativeOpenKind::file);
    model.session_->write(handle,0,std::as_bytes(std::span(contents.data(),contents.size())));
    model.session_->flush(handle);
    model.session_->cleanup(handle,false);model.session_->close(handle);
  }
  static std::string read_extent(VolumeModel& model,std::string_view name) {
    const auto data=model.session_->snapshot().read(name,0,kNativeExtentBytes);
    if(data.empty())return {};
    return {reinterpret_cast<const char*>(data.data()),data.size()};
  }
  static NativeSessionStats stats(const VolumeModel& m){return m.session_->stats();}
  static NativeWorkloadInfo workload(const VolumeModel& m){return m.session_->workload_info();}
  // Actual accepted engine writes, never a forged counter or product bypass.
  // The helper editor remains bounded; qualification also exercises the shared
  // engine's larger synthetic files through its existing private test friend.
  static void earn(VolumeModel& m,std::size_t n) {
    const auto before=workload(m);
    if(!before.enabled||!n||n>2*kOptimizationAreaBytes||n>UINT64_MAX-before.committed_input_bytes)throw std::runtime_error("Funding input limit");
    const auto name="qualification-funding-"+std::to_string(before.committed_input_bytes)+".bin";
    std::vector<std::byte> payload(n,std::byte{0x41});
    m.session_->create_entry(name);
    const auto file=m.session_->open(name,{true,true,false},NativeOpenKind::file);
    try {
      m.session_->write(file,0,payload);m.session_->flush(file);
      if(m.session_->read(file,0,static_cast<std::uint32_t>(n))!=payload||
          workload(m)!=NativeWorkloadInfo{true,before.committed_input_bytes+n})
        throw std::runtime_error("Actual funding bytes/provenance mismatch");
      m.session_->cleanup(file,false);m.session_->close(file);
    }catch(...) {
      if(!m.session_->poisoned())try{m.session_->cleanup(file,false);m.session_->close(file);}catch(...){m.session_->fence();}
      throw;
    }
  }
};
}
using namespace flitzfs;
namespace {
unsigned checks=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F>void rejects(F&& f,const char* why) {
  ++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error(why);
}
template<class F>void until(F&& predicate,std::uint64_t deadline,
    const char* timeout_message="Application qualification deadline exceeded") {
  while(!predicate()) {
    if(GetTickCount64()>=deadline)throw std::runtime_error(timeout_message);
    MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}
    Sleep(5);
  }
}
void handles(const char* phase) {
  DWORD count=0;require(GetProcessHandleCount(GetCurrentProcess(),&count)!=FALSE,"Handle observation");
  std::cout<<"Application handle phase: "<<phase<<" handles="<<count<<"\n";
}
DWORD handle_count() {
  DWORD value=0;require(GetProcessHandleCount(GetCurrentProcess(),&value)!=FALSE,"Native handle observation");
  return value;
}
void native_initialization() {
  // Reproduce cold platform initialization without our provider/model/controller.
  // Every repeated primitive and every later full model cycle must balance.
  handles("native-cold");
  std::array<unsigned char,16> random{};
  DWORD rng_baseline=0;
  for(unsigned i=0;i<32;++i) {
    require(BCryptGenRandom(nullptr,random.data(),static_cast<ULONG>(random.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0,"Native RNG qualification");
    const auto current=handle_count();
    std::cout<<"Native RNG repetition "<<i<<" handles="<<current<<"\n";
    if(i>=16)require(current==rng_baseline,"Measured native RNG handle growth");
    rng_baseline=current;
  }
  handles("native-rng-stable");
  DWORD window_baseline=0;
  for(unsigned i=0;i<32;++i) {
    auto window=CreateWindowExW(0,L"STATIC",L"FlitzFS native initialization qualification",0,
        0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);
    require(window!=nullptr,"Native initialization window");
    bool posted=false;
    std::thread sender([&]{posted=PostMessageW(window,WM_APP+40,0,0)!=FALSE;});
    sender.join();require(posted,"Native initialization notification");
    MSG message{};while(PeekMessageW(&message,window,0,0,PM_REMOVE)) {
      TranslateMessage(&message);DispatchMessageW(&message);
    }
    require(DestroyWindow(window)!=FALSE,"Native initialization window cleanup");
    const auto current=handle_count();
    std::cout<<"Native window/thread repetition "<<i<<" handles="<<current<<"\n";
    if(i>=16)require(current==window_baseline,"Measured native window/thread handle growth");
    window_baseline=current;
  }
  handles("native-window-thread-stable");
  // Separate native PDH cold initialization from product ownership. Every query
  // must close, and repeated native-only cycles must stabilize before full
  // controller cycles are required to return to their exact handle baseline.
  DWORD counter_baseline=0;
  for(unsigned i=0;i<32;++i) {
    PDH_HQUERY query=nullptr;PDH_HCOUNTER counter=nullptr;
    require(PdhOpenQueryW(nullptr,0,&query)==ERROR_SUCCESS,"Native PDH query creation");
    const auto added=PdhAddEnglishCounterW(query,L"\\Processor Information(_Total)\\% Processor Time",0,&counter);
    const auto collected=added==ERROR_SUCCESS?PdhCollectQueryData(query):added;
    const auto closed=PdhCloseQuery(query);
    require(added==ERROR_SUCCESS && collected==ERROR_SUCCESS && closed==ERROR_SUCCESS,"Native PDH query lifecycle");
    const auto current=handle_count();std::cout<<"Native PDH repetition "<<i<<" handles="<<current<<"\n";
    if(i>=16)require(current==counter_baseline,"Measured native PDH handle growth");counter_baseline=current;
  }
  handles("native-pdh-stable");
}

void advice_application_tests() {
  using Q=VolumeModelQualification;
  using Result=VolumeModel::AdviceMaintenance;
  const std::string text="retained bytes";
  const auto expected=std::vector<std::byte>(reinterpret_cast<const std::byte*>(text.data()),
      reinterpret_cast<const std::byte*>(text.data()+text.size()));
  {
    VolumeModel model;rejects([&]{model.maintain_optimization_state();},"uncreated advice maintenance accepted");
    model.create();
    require(Q::workload(model)==NativeWorkloadInfo{true,0},"helper did not activate before exposure");
    model.create_text("keep.txt",text);
    require(Q::workload(model)==NativeWorkloadInfo{true,text.size()},"helper did not count actual saved bytes");
    const auto baseline=model.view();const auto handle=Q::open_cleaned(model);
    {
      const auto area=Q::current(model);const auto bytes=Q::bytes(area);
      require(model.maintain_optimization_state()==Result::ready&&
          model.maintain_optimization_state()==Result::ready&&Q::bytes(area)==bytes&&
          model.optimization_info().binding_attempts==1&&
          !model.optimization_info().remaining_publications&&model.optimization_info().budgeted&&
          model.optimization_info().settled_input_bytes==0&&model.view().reserved==baseline.reserved,
          "healthy audit rewrote/rebound exhausted advice");
    }
    for(unsigned rebuilt=1;rebuilt<NativeFileSession::max_optimization_bindings;++rebuilt) {
      const auto old=Q::current(model);Q::damage(model);
      const auto damaged=Q::bytes(old);
      const auto unfunded=model.view();
      for(unsigned repeat=0;repeat<8;++repeat)
        require(model.maintain_optimization_state()==Result::unavailable&&
            model.view().reserved==unfunded.reserved&&model.view().physical_bytes==unfunded.physical_bytes&&
            model.optimization_info().binding_attempts==rebuilt&&Q::current(model)==old,
            "unfunded helper allocated/admitted advice or retried historical input");
      Q::earn(model,2*kOptimizationAreaBytes);const auto funded=model.view();
      require(model.maintain_optimization_state()==Result::rebuilt,"helper did not rebuild failed advice");
      const auto view=model.view();const auto info=view.optimization;
      require(info.phase==NativeOptimizationPhase::ready&&info.sequence==1&&
          info.binding_attempts==rebuilt+1&&info.owned_area_bytes==(rebuilt+1)*kOptimizationAreaBytes&&
          !info.remaining_publications&&info.budgeted&&
          info.settled_input_bytes==Q::workload(model).committed_input_bytes&&!view.fenced&&
          view.reserved==baseline.reserved+rebuilt*kOptimizationAreaBytes&&
          view.physical_bytes==funded.physical_bytes+kOptimizationAreaBytes,
          "helper replacement mapping/accounting");
      require(Q::current(model)!=old&&Q::bytes(old)==damaged&&model.read_text("keep.txt")==text&&
          Q::read(model,handle)==expected&&Q::stats(model).contexts==1&&Q::stats(model).cleaned==1,
          "helper replaced source bytes, lost a live cleaned handle or reused an area");
    }
    Q::damage(model);
    const auto allocated=model.view().reserved;
    for(unsigned repeat=0;repeat<8;++repeat)
      require(model.maintain_optimization_state()==Result::unavailable&&
          model.view().reserved==allocated&&model.optimization_info().binding_attempts==
            NativeFileSession::max_optimization_bindings,"exhausted helper retried/leaked a new area");
    const auto funded_input=text.size()+(NativeFileSession::max_optimization_bindings-1)*2*kOptimizationAreaBytes;
    require(Q::workload(model)==NativeWorkloadInfo{true,funded_input},"advice loss or maintenance minted input");
    require(!model.optimize()&&model.view().reserved==allocated&&!model.view().fenced,
        "unavailable advice started image work or fenced data");
    model.create_text("after.txt","still writable");
    require(Q::workload(model)==NativeWorkloadInfo{true,funded_input+std::string_view("still writable").size()},
        "helper advice exhaustion lost actual input");
    require(model.read_text("after.txt")=="still writable"&&model.read_text("keep.txt")==text&&
        Q::read(model,handle)==expected,"advice exhaustion stopped real file access");
    Q::close(model,handle);model.discard();
  }
  {
    VolumeModel model;model.create();model.create_text("keep.txt",text);
    const auto handle=Q::open_cleaned(model);Q::damage(model);
    require(model.maintain_optimization_state()==Result::unavailable,"pressure seed reused historical input");
    Q::earn(model,2*kOptimizationAreaBytes);
    const auto before=model.optimization_info().binding_attempts;
    {
      auto filler=Q::fill_remaining(model);
      const auto use=model.view();
      require(use.reserved==use.capacity,"bounded pressure fixture did not fill its own domain");
      for(unsigned repeat=0;repeat<3;++repeat)
        rejects([&]{model.maintain_optimization_state();},"provider capacity refusal hidden");
      require(model.optimization_info().binding_attempts==before&&!model.view().fenced&&
          model.view().reserved==use.reserved&&model.read_text("keep.txt")==text&&
          Q::read(model,handle)==expected,"pressure changed mapping, leaked or blocked file access");
    }
    // New independently available capacity permits a distinct attempt, without
    // waiting, polling hardware, a timeout retry or resetting failed bytes.
    require(model.maintain_optimization_state()==Result::rebuilt&&
        model.optimization_info().binding_attempts==before+1&&Q::read(model,handle)==expected,
        "released provider capacity did not admit checked rebuilding");
    Q::close(model,handle);model.discard();
  }
  // Calibrate this exact STL/toolchain's ordinary vector allocation request.
  // MSVC may add alignment bookkeeping; never assume payload size is heap size.
  advice_allocation_fixture::observed_bytes=0;
  advice_allocation_fixture::capturing=true;
  try {
    std::vector<std::byte> probe(kStoreCommitUnit);
    probe.back()=std::byte{1};
    advice_allocation_fixture::capturing=false;
    require(probe.size()==kStoreCommitUnit&&probe.back()==std::byte{1},"allocator calibration bytes");
  }catch(...){advice_allocation_fixture::capturing=false;throw;}
  advice_allocation_fixture::allocation_bytes=advice_allocation_fixture::observed_bytes;
  require(advice_allocation_fixture::allocation_bytes>=kStoreCommitUnit&&
      advice_allocation_fixture::allocation_bytes<=kStoreCommitUnit+512,
      "allocator calibration outside bounded overhead");
  std::cout<<"Helper record heap request: "<<advice_allocation_fixture::allocation_bytes<<" bytes\n";
  // Four real allocator cuts after core binding admission: the initial empty
  // record, either prepared record, and final readback scratch. The last cut
  // leaves valid-looking bytes; the failed owner must still expose no policy.
  for(unsigned cut=0;cut<4;++cut) {
    VolumeModel model;model.create();model.create_text("keep.txt",text);
    const auto handle=Q::open_cleaned(model);const auto old=Q::current(model);
    Q::damage(model);const auto damaged=Q::bytes(old);
    require(model.maintain_optimization_state()==Result::unavailable,"allocator seed reused historical input");
    Q::earn(model,2*kOptimizationAreaBytes);
    {
      auto filler=Q::fill_remaining(model);
      rejects([&]{model.maintain_optimization_state();},"failure preparation did not refuse capacity");
      require(model.optimization_info().phase==NativeOptimizationPhase::terminal&&
          model.optimization_info().binding_attempts==1,"fault preparation changed binding");
    }
    const auto baseline=model.view();
    bool failed=false,reached=false;
    {
      advice_allocation_fixture::Failure fault(cut,model,2);
      try{(void)model.maintain_optimization_state();}
      catch(const std::bad_alloc&){failed=true;}
      reached=fault.reached();
    }
    if(!failed||!reached)throw std::runtime_error("post-admission allocator cut not reached/propagated: "+std::to_string(cut));
    require(failed&&reached,"post-admission allocator cut was not reached/propagated");
    const auto area=Q::current(model);const auto info=model.optimization_info();
    const auto written=cut? kOptimizationAreaBytes:0;
    const auto failed_view=model.view();
    require(area!=old&&area->extent()==written&&info.binding_attempts==2&&
        info.owned_area_bytes==2*kOptimizationAreaBytes&&info.sequence==0&&
        info.phase==NativeOptimizationPhase::unavailable&&info.session_ready&&
        !failed_view.fenced&&failed_view.reserved==baseline.reserved+kOptimizationAreaBytes&&
        failed_view.physical_bytes==baseline.physical_bytes+written,
        "throwing admitted area lost helper ownership/accounting or exposed authority");
    const auto partial=cut?Q::bytes(area):std::vector<std::byte>{};
    require(Q::bytes(old)==damaged&&model.read_text("keep.txt")==text&&
        Q::read(model,handle)==expected&&Q::stats(model).contexts==1&&Q::stats(model).cleaned==1,
        "allocator failure changed old bytes or a cleaned live context");
    model.create_text("after.txt","write after failed advice");
    require(model.read_text("after.txt")=="write after failed advice"&&
        Q::read(model,handle)==expected,"allocator failure stopped acknowledged file access");
    const auto insufficient=model.view();
    for(unsigned repeat=0;repeat<4;++repeat)
      require(model.maintain_optimization_state()==Result::unavailable&&
          model.view().reserved==insufficient.reserved&&model.optimization_info().binding_attempts==2,
          "failed creation reused already admitted input or leaked a replacement");
    Q::earn(model,2*kOptimizationAreaBytes);
    const auto recovery=model.view();
    require(model.maintain_optimization_state()==Result::rebuilt&&
        model.optimization_info().binding_attempts==3&&
        model.optimization_info().owned_area_bytes==3*kOptimizationAreaBytes&&
        Q::current(model)!=area&&area->extent()==written&&Q::bytes(old)==damaged&&
        (!cut||Q::bytes(area)==partial)&&
        model.view().reserved==baseline.reserved+2*kOptimizationAreaBytes&&
        model.view().physical_bytes==recovery.physical_bytes+kOptimizationAreaBytes,
        "later repair reused/mutated a failed area or lost retained reservations");
    Q::close(model,handle);model.discard();
    require(!model.ready()&&MemoryVolumeDomain::cleanup_healthy(),"allocator-fault cleanup");
    std::cout<<"Helper post-admission allocator cut: "<<cut<<" retained and retired\n";
  }
  {
    VolumeModel model;model.create();model.create_text("keep.txt",text);
    Q::damage(model,1);
    require(model.maintain_optimization_state()==Result::unavailable&&
        model.read_text("keep.txt")==text&&!model.view().fenced,"degraded unfunded advice blocked data");
    Q::earn(model,2*kOptimizationAreaBytes);
    require(model.maintain_optimization_state()==Result::rebuilt&&
        model.read_text("keep.txt")==text&&!model.view().fenced,
        "degraded advice did not remain isolated from data");
    model.discard();
  }
}

void model_tests() {
  DWORD before=0,after=0;
  require(GetProcessHandleCount(GetCurrentProcess(),&before)!=FALSE,"Handle baseline");
  handles("baseline");
  SIZE_T ws_min=0,ws_max=0;DWORD ws_flags=0;
  require(GetProcessWorkingSetSizeEx(GetCurrentProcess(),&ws_min,&ws_max,&ws_flags)!=FALSE,"Working-set baseline");
  {
    auto domain=MemoryVolumeDomain::create(4*1024*1024);
    rejects([]{(void)MemoryVolumeDomain::create(1024*1024);},"Duplicate domain accepted");
    rejects([&]{(void)domain.create_store(1);},"Unaligned memory store accepted");
    rejects([&]{(void)domain.create_store(5*1024*1024);},"Oversized store accepted");
    auto store=domain.create_store(65536);
    require(store->extent()==0 && domain.usage().reserved==65536 && domain.usage().resident==0,
        "New store accounting and lazy memory");
    std::array<std::byte,3> bytes{std::byte{1},std::byte{2},std::byte{3}},out{};
    store->write_exact(7,bytes);store->flush();store->read_exact(7,out);domain.verify_resident();
    require(out==bytes && store->extent()==10 && domain.usage().resident>0 &&
        domain.usage().resident<=65536,"Exact memory I/O and resident page accounting");
    store->write_exact(65536,{});require(store->extent()==10,"Empty write extended image");
    rejects([&]{store->write_exact(UINT64_MAX,bytes);},"Overflow write accepted");
    rejects([&]{store->read_exact(UINT64_MAX,out);},"Overflow read accepted");
    rejects([&]{store->admit_write(65537);},"Capacity preflight accepted");
    {
      std::array<std::shared_ptr<StoreByteIo>,63> peers;
      for(auto& peer:peers)peer=domain.create_store(4096);
      require(domain.usage().stores==64,"Bounded store registry accounting");
      rejects([&]{(void)domain.create_store(4096);},"Store registry cap accepted");
      domain.verify_resident();
    }
    require(domain.usage().stores==1,"Peer allocation retirement");
    // Domain wrapper retirement must not restore its lease while a store lives.
    auto survivor=store;
    store.reset();require(domain.usage().reserved==65536,"Shared store accounting");
    survivor.reset();domain.verify_resident();require(domain.usage().reserved==0 && domain.usage().resident==0 &&
        domain.usage().stores==0,"Store cleanup");
  }
  handles("provider-retired");
  {
    std::shared_ptr<StoreByteIo> survivor;
    {auto domain=MemoryVolumeDomain::create(1024*1024);survivor=domain.create_store(65536);}
    rejects([]{(void)MemoryVolumeDomain::create(1024*1024);},"Live backing lost domain lease");
    survivor.reset();
  }
  {
    VolumeModel model;require(!model.ready(),"Unexpected initial volume");
    model.create();const auto initial=model.view();
    const auto image_capacity=initial.reserved-262144;
    require(initial.ready && image_capacity>=4*1024*1024 && image_capacity%65536==0 &&
        initial.resident>0 && initial.resident<initial.reserved &&
        2*image_capacity+(NativeFileSession::max_optimization_bindings+1)*131072<=initial.capacity &&
        initial.files.empty(),"Initial admitted paged volume and maintenance reserve");
    rejects([&]{model.create();},"Duplicate create replaced volume");
    std::string data(8192,'A');for(std::size_t i=0;i<data.size();i+=37)data[i]='B';
    model.create_text("first.txt",data);
    require(model.read_text("first.txt")==data,"Real volume text readback");
    const auto written=VolumeModelQualification::workload(model);
    require(written==NativeWorkloadInfo{true,data.size()},"helper foreground input provenance");
    rejects([&]{model.create_text("first.txt","replacement");},"Existing file overwritten");
    require(model.read_text("first.txt")==data,"Duplicate create changed bytes");
    require(VolumeModelQualification::workload(model)==written,"rejected helper save minted input");
    rejects([&]{model.create_text("../outside","x");},"Traversal accepted");
    rejects([&]{model.create_text("large",std::string(VolumeModel::text_limit+1,'x'));},"Editor cap accepted");
    (void)model.optimize();
    require(model.read_text("first.txt")==data && !model.view().fenced,"Optimization changed file");
    const auto allocated=model.view().reserved;
    require(!model.optimize() && model.view().reserved==allocated,"Unchanged optimization repeated or leaked");
    require(VolumeModelQualification::workload(model)==written,"helper optimization minted/reset input");
    model.create_text("empty.txt","");require(model.read_text("empty.txt").empty(),"Empty file readback");
    require(VolumeModelQualification::workload(model)==written,"helper empty file minted input");
    const std::string unicode="UTF-8: \xCE\xB1\xCE\xB2 \xF0\x9F\x9A\x80";
    model.create_text("utf8.txt",unicode);
    require(model.read_text("utf8.txt")==unicode,"UTF-8 byte preservation");
    require(VolumeModelQualification::workload(model)==NativeWorkloadInfo{true,data.size()+unicode.size()},
        "helper UTF-8 byte count mismatch");
    model.discard();require(!model.ready(),"Discard retained engine");
  }
  advice_application_tests();
  handles("volume-retired");
  {
    auto window=CreateWindowExW(0,L"STATIC",L"FlitzFS controller qualification",0,
        0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);
    require(window!=nullptr,"Message-only controller window");
    {
      VolumeController controller(window);
      require(!controller.submit({VolumeCommandKind::stop,{},{}}),"Queued stop accepted");
      require(!controller.submit({VolumeCommandKind::create_text,{},std::string(16385,'x')}),"Oversized command accepted");
      require(controller.submit({VolumeCommandKind::create,{},{}}),"Create command refused");
      const auto deadline=GetTickCount64()+10000;
      until([&]{return !controller.view().busy;},deadline);
      require(controller.view().volume.ready,"Controller did not create real volume");
      require(!controller.stop(false),"Implicit temporary volume discard accepted");
      require(controller.submit({VolumeCommandKind::create_text,"owned.txt","controller bytes"}),"Write command refused");
      until([&]{return !controller.view().busy;},deadline);
      require(controller.submit({VolumeCommandKind::read_text,"owned.txt",{}}),"Read command refused");
      until([&]{return !controller.view().busy;},deadline);
      require(controller.view().text=="controller bytes","Controller readback");
      require(controller.stop(true),"Explicit stop refused");
      until([&]{return controller.view().stopped;},deadline);
      require(!controller.view().volume.ready && !controller.view().notification_failed,"Stop/notification retirement");
    }
    require(DestroyWindow(window)!=FALSE,"Controller window cleanup");
  }
  handles("controller-retired");
  SIZE_T final_min=0,final_max=0;DWORD final_flags=0;
  require(GetProcessWorkingSetSizeEx(GetCurrentProcess(),&final_min,&final_max,&final_flags)!=FALSE &&
      final_min==ws_min && final_max==ws_max && final_flags==ws_flags,"Working-set lease not restored");
  require(MemoryVolumeDomain::cleanup_healthy(),"Memory cleanup health");
  require(GetProcessHandleCount(GetCurrentProcess(),&after)!=FALSE,"Final handle observation");
  std::cout<<"Application handle balance: "<<before<<" -> "<<after<<"\n";
  require(after==before,"Controller handle leak");
}
std::array<std::string,3> related_extent_family() {
  // Large enough to cross an isolation-bank boundary even after Zstandard.
  // Keep the input valid text and each member distinct, with a shared block.
  std::string base(kNativeExtentBytes,'!');
  std::uint32_t state=0x8934cb57;
  for(auto& c:base) {
    state^=state<<13;state^=state>>17;state^=state<<5;
    c=static_cast<char>('!'+state%94);
  }
  std::array<std::string,3> files{base,base,base};
  for(std::size_t i=1;i<files.size();++i) {
    auto& c=files[i][i*911];
    c=c=='!'?'"':'!';
  }
  return files;
}
std::string family_name(std::size_t index) {return "family-"+std::to_string(index);}
void drive_quiet_maintenance(VolumeModel& model,NativeMaintenanceObservation& observation) {
  for(unsigned i=0;i<500 && model.maintenance_pending();++i) {
    ++observation.sequence;observation.captured_ms+=1000;
    model.maintain_groups(observation,observation.captured_ms);
  }
}
void maintenance_tests() {
  // Real OS collector and current-thread priority, without a provider, file,
  // process worker or remote counter. Failed collection is a test failure.
  {
    const auto priority=GetThreadPriority(GetCurrentThread());require(priority!=THREAD_PRIORITY_ERROR_RETURN,"Priority query");
    {MaintenanceThreadScope scope;require(scope.active(),"Background resource mode unavailable");
      require(scope.finish(),"Background resource priority restoration failed");}
    require(GetThreadPriority(GetCurrentThread())==priority,"Priority changed after background scope");
    WindowsMaintenanceHost host;auto observation=host.observe(false);
    require(!observation.valid,"Cold CPU rate observation admitted");
    const auto deadline=GetTickCount64()+6000;
    until([&]{
      if(host.status()==MaintenanceHostStatus::unavailable)throw std::runtime_error("Built-in CPU telemetry unavailable");
      if(host.next_delay_ms())return false;
      observation=host.observe(false);return observation.valid;
    },deadline);
    require(observation.cpu_total_ticks && observation.cpu_busy_ticks<=observation.cpu_total_ticks &&
        observation.cpu_window_ms>=1000 && observation.cpu_window_ms<=2000 &&
        observation.physical_total && observation.physical_available<=observation.physical_total &&
        observation.commit_limit && observation.commit_available<=observation.commit_limit,"Invalid live resource ratios");
    const auto busy_ppm=1'000'000.0*static_cast<double>(observation.cpu_busy_ticks)/
        static_cast<double>(observation.cpu_total_ticks);
    std::cout<<"Live maintenance telemetry: busy_ppm="<<busy_ppm<<" window_ms="<<observation.cpu_window_ms
        <<" physical_available="<<observation.physical_available<<" commit_available="<<observation.commit_available<<"\n";
  }
  auto observation=NativeMaintenanceObservation{};observation.valid=true;
  observation.physical_total=16ULL<<30;observation.physical_available=12ULL<<30;
  observation.commit_limit=32ULL<<30;observation.commit_available=24ULL<<30;
  observation.cpu_total_ticks=1000;observation.cpu_window_ms=1000;
  // Even a strong similarity match can be unprofitable after foreground Zstd.
  if(store_zstd_available()) {
    VolumeModel model;model.create();std::string a(8192,'K'),b=a,c=a;b[1]='L';c[5]='M';
    model.create_text("a",a);model.create_text("b",b);model.create_text("c",c);
    const auto before=model.view().physical_bytes;
    drive_quiet_maintenance(model,observation);
    const auto progress=model.maintenance_progress();
    require(!model.maintenance_pending() && progress.phase==NativeMaintenancePhase::idle &&
        progress.declined_groups>0 && progress.published_groups==0,"Already compact files should decline grouping");
    require(model.view().physical_bytes==before,"Declined group wrote to backing");
    require(model.read_text("a")==a && model.read_text("b")==b && model.read_text("c")==c,"Declined group changed file bytes");
    model.discard();require(MemoryVolumeDomain::cleanup_healthy(),"Declined group model did not retire");
    std::cout<<"Already compact groups declined="<<progress.declined_groups<<"\n";
  }
  const auto family=related_extent_family();
  // The real locked provider uses modeled quiet observations for this case.
  {
    VolumeModel model;model.create();
    for(std::size_t i=0;i<family.size();++i)
      VolumeModelQualification::create_extent(model,family_name(i),family[i]);
    drive_quiet_maintenance(model,observation);
    const auto progress=model.maintenance_progress();
    require(!model.maintenance_pending() && progress.phase==NativeMaintenancePhase::idle &&
        progress.published_groups==1 && progress.minimum_compacted_bytes_saved>0,
        "Windows provider did not automatically group a profitable file family");
    for(std::size_t i=0;i<family.size();++i)
      require(VolumeModelQualification::read_extent(model,family_name(i))==family[i],"Windows background grouping changed file bytes");
    require(model.optimize(),"Paged application maintenance failed");
    for(std::size_t i=0;i<family.size();++i)
      require(VolumeModelQualification::read_extent(model,family_name(i))==family[i],"Paged application maintenance changed grouped bytes");
    VolumeModelQualification::create_extent(model,"later",family.front());
    for(unsigned i=0;i<500 && model.maintenance_progress().owned_payload_bytes==0;++i) {
      ++observation.sequence;observation.captured_ms+=1000;
      model.maintain_groups(observation,observation.captured_ms);
    }
    require(model.maintenance_progress().owned_payload_bytes>0,"No helper private work to retire");
    model.stop_maintenance();
    require(!model.maintenance_pending() && model.maintenance_progress().owned_payload_bytes==0 &&
        VolumeModelQualification::read_extent(model,family_name(0))==family.front() &&
        VolumeModelQualification::read_extent(model,"later")==family.front(),
        "Stopping optional work retained buffers or changed data");
    model.discard();require(MemoryVolumeDomain::cleanup_healthy(),"Grouped model did not retire");
    std::cout<<"Profitable family groups="<<progress.published_groups
        <<" compacted_savings_lower_bound="<<progress.minimum_compacted_bytes_saved<<"\n";
  }
  // Real telemetry can admit or defer maintenance while foreground work proceeds.
  // Small text files may already be compact enough to decline grouping.
  {
    auto window=CreateWindowExW(0,L"STATIC",L"FlitzFS idle integration qualification",0,
        0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);
    require(window!=nullptr,"Maintenance controller window");
    {
      VolumeController controller(window);const auto deadline=GetTickCount64()+60000;
      auto command=[&](VolumeCommand c){require(controller.submit(std::move(c)),"Foreground refused during background maintenance");
        until([&]{return !controller.view().busy;},deadline);require(!controller.view().volume.fenced,"Maintenance fenced volume");};
      command({VolumeCommandKind::create,{},{}});
      std::array<std::string,3> texts;
      texts.fill(std::string(8192,'K'));texts[1][1]='L';texts[2][5]='M';
      for(std::size_t i=0;i<texts.size();++i)command({VolumeCommandKind::create_text,family_name(i),texts[i]});
      const auto resource_deferred=[](const VolumeControllerView& v) {
        const auto& p=v.volume.maintenance;
        return v.maintenance_host==MaintenanceHostStatus::ready && v.volume.maintenance_pending &&
            p.phase==NativeMaintenancePhase::deferred &&
            (p.deferral==NativeMaintenanceDeferral::cpu || p.deferral==NativeMaintenanceDeferral::memory);
      };
      until([&]{const auto v=controller.view();require(!v.maintenance_disabled,"Native background scheduling disabled");
        const auto& p=v.volume.maintenance;
        return !v.volume.maintenance_pending ||
            (p.phase==NativeMaintenancePhase::preparing && p.owned_payload_bytes>0) || resource_deferred(v);},deadline);
      const auto selected=controller.view();
      const bool interleaved=selected.volume.maintenance.phase==NativeMaintenancePhase::preparing &&
          selected.volume.maintenance.owned_payload_bytes>0;
      command({VolumeCommandKind::read_text,family_name(0),{}});
      require(controller.view().text==texts.front(),"Foreground read during maintenance");
      const auto content_generation=controller.view().content_generation;
      until([&]{const auto v=controller.view();require(!v.maintenance_disabled,"Native background scheduling disabled");
        return !v.volume.maintenance_pending || resource_deferred(v);},deadline);
      const auto completed=controller.view();
      const bool settled=!completed.volume.maintenance_pending;
      const bool published=completed.volume.maintenance.published_groups>0;
      if(settled) {
        if(store_zstd_available())
          require(!published && completed.volume.maintenance.declined_groups>0,"Live compact files should decline grouping");
        else require(published,"Live idle grouping did not publish");
      } else require(resource_deferred(completed),"Pending live maintenance lacks a qualified resource deferral");
      require(completed.content_generation==content_generation,"Background work changed foreground content generation");
      for(std::size_t i=0;i<texts.size();++i) {
        command({VolumeCommandKind::read_text,family_name(i),{}});
        require(controller.view().text==texts[i],"Live family readback");
      }
      require(controller.stop(true),"Maintenance controller stop refused");until([&]{return controller.view().stopped;},deadline);
      std::cout<<"Live idle groups="<<completed.volume.maintenance.published_groups<<" samples="<<completed.volume.maintenance.sampled_extents
          <<" control_generation="<<completed.content_generation<<" notifications="<<completed.generation
          <<" quanta="<<completed.maintenance_quanta<<" continued_steps="<<completed.continued_preparation_steps
          <<" elapsed_ns="<<completed.maintenance_elapsed_ns
          <<" maximum_quantum_ns="<<completed.maintenance_maximum_quantum_ns<<"\n";
      std::cout<<"Live maintenance outcome="<<(settled?"settled":"resource-deferred")
          <<" preparation_interleave="<<interleaved<<" publication_exercised="<<published
          <<" deferral="<<static_cast<unsigned>(completed.volume.maintenance.deferral)<<"\n";
    }
    require(DestroyWindow(window)!=FALSE && MemoryVolumeDomain::cleanup_healthy(),"Live maintenance controller retirement");
  }
}
HWND find_window(DWORD pid) {
  struct Context{DWORD pid;HWND window=nullptr;}context{pid};
  EnumWindows([](HWND window,LPARAM param)->BOOL {
    auto& c=*reinterpret_cast<Context*>(param);DWORD owner=0;GetWindowThreadProcessId(window,&owner);
    wchar_t kind[96]{};
    if(owner==c.pid && GetClassNameW(window,kind,96)>0 &&
        std::wstring_view(kind)==L"FlitzFS.VolumeApp.Experimental"){c.window=window;return FALSE;}
    return TRUE;
  },reinterpret_cast<LPARAM>(&context));
  return context.window;
}
LRESULT send(HWND window,UINT message,WPARAM w=0,LPARAM l=0) {
  DWORD_PTR result=0;
  const auto control=GetDlgCtrlID(window);
  SetLastError(ERROR_SUCCESS);const auto started=GetTickCount64();
  const auto dispatched=SendMessageTimeoutW(window,message,w,l,SMTO_ABORTIFHUNG|SMTO_BLOCK,1000,&result);
  if(!dispatched) {
    const auto error=GetLastError();
    throw std::runtime_error("GUI message failed: control="+std::to_string(control)+
        " message="+std::to_string(message)+" Win32="+std::to_string(error)+
        " elapsed_ms="+std::to_string(GetTickCount64()-started));
  }
  return static_cast<LRESULT>(result);
}
std::wstring text(HWND window) {
  const auto count=send(window,WM_GETTEXTLENGTH);
  require(count>=0 && count<=32768,"GUI text cap");
  std::wstring result(static_cast<std::size_t>(count)+1,L'\0');
  const auto actual=send(window,WM_GETTEXT,result.size(),reinterpret_cast<LPARAM>(result.data()));
  require(actual>=0 && actual<=count,"GUI text read");
  result.resize(static_cast<std::size_t>(actual));return result;
}
std::wstring disposable_mount_parent() {
  wchar_t nonce[64]{};
  const auto size=GetEnvironmentVariableW(L"FLITZFS_DISPOSABLE_GUEST_MOUNT",nonce,64);
  require(size==32 && std::all_of(nonce,nonce+size,[](wchar_t c) {
    return (c>=L'0' && c<=L'9') || (c>=L'a' && c<=L'f');
  }),"Mounted GUI trial requires disposable guest admission");
  wchar_t model[128]{};
  DWORD model_size=sizeof(model);
  require(RegGetValueW(HKEY_LOCAL_MACHINE,L"HARDWARE\\DESCRIPTION\\System\\BIOS",L"SystemProductName",
      RRF_RT_REG_SZ,nullptr,model,&model_size)==ERROR_SUCCESS && std::wstring_view(model)==L"Virtual Machine",
      "Mounted GUI trial requires a virtual guest");
  DWORD container=0,container_size=sizeof(container);
  require(RegGetValueW(HKEY_LOCAL_MACHINE,L"SYSTEM\\CurrentControlSet\\Control",L"ContainerType",RRF_RT_REG_DWORD,
      nullptr,&container,&container_size)==ERROR_SUCCESS && container==4,
      "Mounted GUI trial requires the disposable Windows Sandbox guest");
  const auto parent=L"C:\\FlitzFS-guest-"+std::wstring(nonce,size)+L"\\mount-parent";
  wchar_t temporary[MAX_PATH]{};
  const auto length=GetTempPath2W(static_cast<DWORD>(std::size(temporary)),temporary);
  require(length<std::size(temporary) && std::wstring_view(temporary,length)==parent+L"\\",
      "Mounted GUI trial requires a private temporary mount parent");
  return parent;
}

struct MountedTrialFile {
  HANDLE file;
  MountedTrialFile(const std::filesystem::path& name,DWORD access,DWORD disposition,DWORD share=FILE_SHARE_READ):
      file(CreateFileW(name.c_str(),access,share,nullptr,disposition,FILE_ATTRIBUTE_NORMAL,nullptr)) {
    if(file==INVALID_HANDLE_VALUE)
      throw std::runtime_error("Mounted test file open failed with Windows status "+std::to_string(GetLastError()));
  }
  ~MountedTrialFile(){CloseHandle(file);}
  MountedTrialFile(const MountedTrialFile&)=delete;
  MountedTrialFile& operator=(const MountedTrialFile&)=delete;
};

struct MountedFileActivation {
  std::filesystem::path path;
  VolumeFileAction action=VolumeFileAction::create_new;
  std::optional<DWORD> serial;
  bool expect_create_conflict=false;
  bool populated=false;
};

constexpr std::size_t mounted_trace_files=1024;
constexpr std::size_t mapped_trace_file=0,overwritten_trace_file=64,renamed_trace_file=13;
constexpr auto mounted_trace_directory=L"Population";
constexpr auto mounted_trace_seed=0x19e5ce9bU;

std::string mounted_trace_name(std::size_t i,bool edited) {
  if(edited && i==renamed_trace_file)return "retained-rename.bin";
  return "entry-"+std::to_string(i)+".bin";
}

std::wstring mounted_trace_path(const std::wstring& directory,std::size_t i,bool edited=false) {
  const auto name=mounted_trace_name(i,edited);
  return directory+L"\\"+std::wstring(name.begin(),name.end());
}

std::vector<std::byte> mounted_trace_bytes(std::size_t i,bool edited) {
  const auto size=i%64==0?kNativeExtentBytes+79:271+i%389;
  std::vector<std::byte> bytes(edited && i==overwritten_trace_file?93:size);
  auto state=mounted_trace_seed^static_cast<std::uint32_t>(i);
  for(auto& value:bytes) {
    state=state*1664525U+1013904223U;
    value=static_cast<std::byte>(state>>24);
  }
  if(edited && i==overwritten_trace_file)bytes.front()^=std::byte{0xc3};
  if(edited && i==mapped_trace_file)
    std::fill_n(bytes.begin()+kNativeExtentBytes-23,64,std::byte{0x6d});
  return bytes;
}

template<class Reader>
detail::Digest verify_mounted_trace(Reader read) {
  std::array<detail::Digest,mounted_trace_files> digests{};
  for(std::size_t i=0;i<digests.size();++i) {
    const auto expected=mounted_trace_bytes(i,true);
    const auto actual=read(i,static_cast<DWORD>(expected.size()));
    require(actual==expected,"Populated mount file payload differs");
    digests[i]=detail::record_digest(actual);
  }
  return detail::record_digest(std::as_bytes(std::span(digests)));
}

void print_mounted_trace_digest(const char* phase,const detail::Digest& digest) {
  constexpr char hex[]="0123456789abcdef";
  std::cout<<"mounted_population phase="<<phase<<" seed="<<mounted_trace_seed
      <<" files="<<mounted_trace_files<<" blake3=";
  for(const auto b:digest) {
    const auto value=std::to_integer<unsigned>(b);
    std::cout<<hex[value>>4]<<hex[value&15U];
  }
  std::cout<<'\n';
}

void write_mounted_trace(const std::wstring& directory) {
  require(CreateDirectoryW(directory.c_str(),nullptr)!=FALSE,"Populated directory creation failed");
  for(std::size_t i=0;i<mounted_trace_files;++i) {
    const auto bytes=mounted_trace_bytes(i,false);
    MountedTrialFile file(mounted_trace_path(directory,i),GENERIC_WRITE,CREATE_NEW);
    DWORD written=0;
    require(WriteFile(file.file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr) &&
        written==bytes.size() && FlushFileBuffers(file.file),"Populated create write or flush failed");
  }
  std::cout<<"mounted_population phase=seed_flushed files="<<mounted_trace_files<<std::endl;
  {
    const auto path=mounted_trace_path(directory,overwritten_trace_file);
    {
      MountedTrialFile reader(path,GENERIC_READ,OPEN_EXISTING,FILE_SHARE_READ);
      const auto conflict=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
      const auto error=GetLastError();
      if(conflict!=INVALID_HANDLE_VALUE)CloseHandle(conflict);
      require(conflict==INVALID_HANDLE_VALUE && error==ERROR_SHARING_VIOLATION,
          "Populated conflicting write returned the wrong sharing status");
    }
    MountedTrialFile replacement(path,GENERIC_WRITE,CREATE_ALWAYS);
    const auto bytes=mounted_trace_bytes(overwritten_trace_file,true);
    DWORD written=0;
    require(WriteFile(replacement.file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr) &&
        written==bytes.size() && FlushFileBuffers(replacement.file),"Populated overwrite or flush failed");
  }
  {
    MountedTrialFile file(mounted_trace_path(directory,mapped_trace_file),
        GENERIC_READ|GENERIC_WRITE,OPEN_EXISTING);
    struct Mapping {
      HANDLE section=nullptr;
      std::byte* view=nullptr;
      ~Mapping(){if(view)UnmapViewOfFile(view);if(section)CloseHandle(section);}
    } mapping;
    mapping.section=CreateFileMappingW(file.file,nullptr,PAGE_READWRITE,0,0,nullptr);
    require(mapping.section!=nullptr,"Mounted writable section creation failed");
    mapping.view=static_cast<std::byte*>(MapViewOfFile(mapping.section,FILE_MAP_WRITE,0,0,0));
    require(mapping.view!=nullptr,"Mounted writable view mapping failed");
    constexpr auto offset=kNativeExtentBytes-23;
    std::fill_n(mapping.view+offset,64,std::byte{0x6d});
    require(FlushViewOfFile(mapping.view+offset,64)!=FALSE,"Mounted dirty view flush failed");
    require(FlushFileBuffers(file.file)!=FALSE,"Mounted file flush after dirty view failed");
  }
  const auto old_path=mounted_trace_path(directory,renamed_trace_file);
  const auto new_path=mounted_trace_path(directory,renamed_trace_file,true);
  constexpr DWORD share=FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE;
  MountedTrialFile retained(old_path,GENERIC_READ,OPEN_EXISTING,share);
  require(MoveFileExW(old_path.c_str(),new_path.c_str(),0)!=FALSE,
      "Populated rename with retained handle failed");
  MountedTrialFile renamed(new_path,GENERIC_READ|GENERIC_WRITE,OPEN_EXISTING,share);
  BY_HANDLE_FILE_INFORMATION prior{},after{};
  require(GetFileInformationByHandle(retained.file,&prior) && GetFileInformationByHandle(renamed.file,&after) &&
      (prior.nFileIndexHigh || prior.nFileIndexLow) &&
      prior.dwVolumeSerialNumber==after.dwVolumeSerialNumber &&
      prior.nFileIndexHigh==after.nFileIndexHigh && prior.nFileIndexLow==after.nFileIndexLow,
      "Populated rename changed the retained file identity");
  require(FlushFileBuffers(renamed.file)!=FALSE,"Populated renamed file flush failed");
  std::cout<<"mounted_population phase=mutations_flushed denied_share_status="<<ERROR_SHARING_VIOLATION
      <<" overwrite=1 mapped_extents=2 retained_identity=1\n";
}

void verify_mounted_population(const std::wstring& directory,const char* phase) {
  std::set<std::wstring> remaining;
  for(std::size_t i=0;i<mounted_trace_files;++i) {
    const auto name=mounted_trace_name(i,true);
    remaining.emplace(name.begin(),name.end());
  }
  WIN32_FIND_DATAW entry{};
  struct Search {
    HANDLE handle;
    ~Search(){if(handle!=INVALID_HANDLE_VALUE)FindClose(handle);}
  } search{FindFirstFileW((directory+L"\\*").c_str(),&entry)};
  require(search.handle!=INVALID_HANDLE_VALUE,"Populated enumeration did not start");
  do {
    const std::wstring_view name(entry.cFileName);
    if(name!=L"." && name!=L"..")
      require(remaining.erase(entry.cFileName)==1,"Populated enumeration returned an unexpected or duplicate name");
  } while(FindNextFileW(search.handle,&entry));
  require(GetLastError()==ERROR_NO_MORE_FILES && remaining.empty(),"Populated enumeration missed file names");
  const auto digest=verify_mounted_trace([&](std::size_t i,DWORD length) {
    MountedTrialFile file(mounted_trace_path(directory,i,true),GENERIC_READ,OPEN_EXISTING);
    LARGE_INTEGER size{};
    require(GetFileSizeEx(file.file,&size) && size.QuadPart==length,"Populated file length differs");
    std::vector<std::byte> bytes(length);
    DWORD read=0;
    require(ReadFile(file.file,bytes.data(),length,&read,nullptr) && read==length,"Populated file read failed");
    return bytes;
  });
  print_mounted_trace_digest(phase,digest);
}

const std::wstring mounted_moved_directory=L"Archives-"+std::wstring(178,L'y');
constexpr auto mounted_renamed_file=L"R\u00e9vis\u00e9-\U0001f680.bin";

std::vector<std::byte> mounted_client_bytes() {
  std::vector<std::byte> bytes(2*kNativeExtentBytes+137);
  for(std::size_t i=0;i<bytes.size();++i)bytes[i]=static_cast<std::byte>((i/64+i%29)&255U);
  return bytes;
}

template<class Button,class Status>
void mounted_gui_workflow(HWND window,std::wstring_view parent,ProcessObserver& observer,
    Button button,Status wait_status,std::uint64_t deadline,MountedFileActivation* activation) {
  const std::wstring volume=activation?L"Volume file":L"Memory volume";
  const auto closed=activation?L"Volume file closed.":L"Temporary volume explicitly discarded.";
  const auto location=[&] {
    const auto display=text(GetDlgItem(window,121));
    const std::wstring_view heading=activation?L"Attached volume file\r\n":L"Attached temporary volume\r\n";
    require(display.starts_with(heading),"Attached GUI did not publish a mount location");
    auto target=display.substr(heading.size());
    if(activation) {
      const std::wstring expected=L"\r\nBacking file: "+activation->path.native();
      require(target.ends_with(expected),"Attached GUI did not identify its selected backing");
      target.resize(target.size()-expected.size());
    }
    const auto prefix=std::wstring(parent)+L"\\FlitzFS-";
    require(target.starts_with(prefix) && target.size()==prefix.size()+32 &&
        std::all_of(target.begin()+static_cast<std::ptrdiff_t>(prefix.size()),target.end(),[](wchar_t c) {
          return (c>=L'0' && c<=L'9') || (c>=L'a' && c<=L'f');
        }),"Mounted location is outside the private guest fixture");
    const auto attributes=GetFileAttributesW(target.c_str());
    require(attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_REPARSE_POINT),
        "Attached path lacks its mount junction");
    return target;
  };
  const auto serial=[](const std::wstring& mount) {
    wchar_t label[64]{};
    DWORD serial_number=0,flags=0;
    const auto root=mount+L"\\";
    require(GetVolumeInformationW(root.c_str(),label,64,&serial_number,nullptr,&flags,nullptr,0) &&
        std::wstring_view(label)==L"FlitzFS" && !(flags&FILE_READ_ONLY_VOLUME),"Mounted volume identity or access differs");
    return serial_number;
  };
  const auto await_detach=[&](const std::wstring& mount) {
    wait_status(volume+L" detached and retained.");
    until([&]{return GetFileAttributesW(mount.c_str())==INVALID_FILE_ATTRIBUTES &&
        GetLastError()==ERROR_FILE_NOT_FOUND;},deadline);
    require((IsWindowEnabled(GetDlgItem(window,106))!=FALSE)==(activation!=nullptr),
        "Detached volume exit protection differs");
  };
  const auto folder=L"Donn\u00e9es-"+std::wstring(174,L'x');
  const auto& moved=mounted_moved_directory;
  constexpr auto original=L"R\u00e9sum\u00e9-\U0001f680.bin";
  constexpr auto alias=L"R\u00c9SUM\u00c9-\U0001f680.BIN";
  constexpr auto renamed=mounted_renamed_file;
  const auto root=[](const std::wstring& mount){return L"\\\\?\\"+mount+L"\\";};
  const auto expected=mounted_client_bytes();
  const auto verify=[&](HANDLE file) {
    LARGE_INTEGER size{};
    require(GetFileSizeEx(file,&size) && size.QuadPart==static_cast<LONGLONG>(expected.size()),
        "Mounted readback length differs");
    std::vector<std::byte> bytes(expected.size());
    DWORD read=0;
    require(ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr) &&
        read==bytes.size() && bytes==expected,"Mounted file bytes differ");
  };
  const auto verify_moved=[&](const std::wstring& mount) {
    const auto path=root(mount)+moved+L"\\"+renamed;
    WIN32_FIND_DATAW entry{};
    const auto search=FindFirstFileW(path.c_str(),&entry);
    require(search!=INVALID_HANDLE_VALUE,"Mounted enumeration failed after the parent move");
    FindClose(search);
    require(std::wstring_view(entry.cFileName)==renamed,"Mounted enumeration changed the Unicode spelling");
    MountedTrialFile file(path,GENERIC_READ,OPEN_EXISTING);
    verify(file.file);
  };

  if(!activation)button(107);
  wait_status(volume+L" attached.");
  const auto first=location();
  const auto initial_serial=serial(first);
  if(activation && activation->action==VolumeFileAction::open_existing) {
    require(activation->serial==initial_serial,"A fresh process opened a different volume");
    verify_moved(first);
    if(activation->populated)
      verify_mounted_population(root(first)+mounted_trace_directory,"fresh_process");
    button(108);await_detach(first);
    button(105);wait_status(closed);
    return;
  }
  if(activation)activation->serial=initial_serial;
  const auto directory=root(first)+folder;
  const auto path=directory+L"\\"+original;
  require(path.size()>MAX_PATH && CreateDirectoryW(directory.c_str(),nullptr),
      "Mounted Unicode directory or extended path setup failed");
  {
    MountedTrialFile file(path,GENERIC_WRITE,CREATE_NEW);
    DWORD written=0;
    require(WriteFile(file.file,expected.data(),static_cast<DWORD>(expected.size()),&written,nullptr) &&
        written==expected.size() && FlushFileBuffers(file.file),"Mounted write or flush failed");
  }
  const auto alias_path=directory+L"\\"+alias;
  const auto collision=CreateFileW(alias_path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
  const auto collision_error=GetLastError();
  if(collision!=INVALID_HANDLE_VALUE)CloseHandle(collision);
  require(collision==INVALID_HANDLE_VALUE && collision_error==ERROR_FILE_EXISTS,
      "Mounted case-insensitive name admitted a second file");
  {
    MountedTrialFile file(alias_path,GENERIC_READ,OPEN_EXISTING,FILE_SHARE_READ|FILE_SHARE_DELETE);
    require(MoveFileExW(path.c_str(),(directory+L"\\"+renamed).c_str(),0),
        "Mounted rename with an open share-delete handle failed");
    verify(file.file);
  }
  require(MoveFileExW(directory.c_str(),(root(first)+moved).c_str(),0),
      "Mounted directory move failed");
  const auto old_attributes=GetFileAttributesW(path.c_str());
  const auto old_error=GetLastError();
  if(old_attributes!=INVALID_FILE_ATTRIBUTES || old_error!=ERROR_PATH_NOT_FOUND)
    throw std::runtime_error("Mounted old path returned Windows status "+std::to_string(old_error));
  const auto missing=root(first)+moved+L"\\missing.bin";
  require(GetFileAttributesW(missing.c_str())==INVALID_FILE_ATTRIBUTES &&
      GetLastError()==ERROR_FILE_NOT_FOUND,"Mounted missing name under an existing parent reported the wrong status");
  verify_moved(first);
  if(activation && activation->populated) {
    const auto population=root(first)+mounted_trace_directory;
    write_mounted_trace(population);
    verify_mounted_population(population,"before_detach");
  }
  send(window,WM_CLOSE);
  require(IsWindow(window) && observer.wait_for_exit()==ProcessObservation::running,
      "Window close ended ownership of the mounted volume");
  verify_moved(first);
  ShowWindow(window,SW_RESTORE);
  button(108);await_detach(first);

  button(107);
  wait_status(volume+L" attached.");
  const auto second=location();
  require(second!=first && serial(second)==initial_serial,"Reattachment changed the volume identity or reused its path");
  verify_moved(second);
  if(activation && activation->populated)
    verify_mounted_population(root(second)+mounted_trace_directory,"reattached");
  button(108);await_detach(second);
  button(105);wait_status(closed);
  std::cout<<"mounted_gui_roundtrip reattached=1 junction_cleanup_verified=1 "
      <<"serial_preserved=1 window_close_retained=1 external_cpu_readback=1 "
      <<"unicode_names=1 extended_path=1 alias_collision=1 live_handle_rename=1 directory_move=1\n";
}

enum class GuiRuntime { ordinary,missing,altered,mounted,qualified_file_controls };
void gui_tests(const std::filesystem::path& app,const app_fixture::Budget& budget,GuiRuntime runtime,
    MountedFileActivation* activation=nullptr) {
  const bool mounted=runtime==GuiRuntime::mounted;
  const auto mount_parent=mounted?disposable_mount_parent():std::wstring{};
  auto sdk=app.parent_path()/L"winfsp-x64.dll";
  std::optional<ReadOnlyFile> fixture_pin;
  if(runtime==GuiRuntime::missing) {
    require(GetFileAttributesW(sdk.c_str())==INVALID_FILE_ATTRIBUTES &&
        GetLastError()==ERROR_FILE_NOT_FOUND,"Missing-runtime test requires its own relocated fixture");
  } else if(runtime==GuiRuntime::altered) {
    constexpr std::string_view marker="FlitzFS deliberately invalid WinFsp fixture\n";
    fixture_pin.emplace(ReadOnlyFile::open(sdk));
    require(fixture_pin->size()>=marker.size(),"Altered-runtime fixture size differs");
    std::string bytes(marker.size(),'\0');
    fixture_pin->read_exact_at(0,bytes.data(),bytes.size());
    require(bytes==marker,"Altered-runtime fixture identity differs");
  }
  auto observer=std::make_shared<ProcessObserver>();ProcessResult result;
  std::exception_ptr failure;
  std::atomic<bool> supervisor_failed{false};
  const auto child_memory_mib=budget.child_memory_mib(mounted?64:128);
  const std::uint32_t timeout_ms=mounted?39000:25000;
  std::vector<std::wstring> arguments;
  if(activation)arguments={activation->action==VolumeFileAction::create_new?
      L"--create-volume-file":L"--open-volume-file",activation->path.native()};
  std::thread supervisor([&]{try{result=run_bounded_process(app,arguments,ProcessBudget{child_memory_mib,2000,timeout_ms,16},observer);}
      catch(...){failure=std::current_exception();supervisor_failed.store(true,std::memory_order_release);}});
  std::exception_ptr test_failure;
  try {
    const auto deadline=GetTickCount64()+(mounted?34000:20000);
    HWND window=nullptr;
    until([&]{
      if(supervisor_failed.load(std::memory_order_acquire))throw std::runtime_error("GUI child launch failed");
      const auto observation=observer->wait_for_exit();
      if(observation==ProcessObservation::exited || observation==ProcessObservation::wait_failed)
        throw std::runtime_error("GUI exited or became unobservable before its window was found");
      window=find_window(observer->process_id());return window!=nullptr;
    },deadline,"Application window was not found on the test desktop");
    auto button=[&](int id){std::cout<<"GUI command: "<<id<<"\n";auto h=GetDlgItem(window,id);require(h!=nullptr,"Missing native button");send(h,BM_CLICK);};
    HWND status=nullptr;
    auto wait_status=[&](std::wstring_view expected){
      until([&]{
        const auto current=text(status);
        if(current.find(expected)!=std::wstring::npos)return true;
        if(mounted && (current.starts_with(L"Operation failed:") || current.starts_with(L"Attachment failed.") ||
            current.starts_with(L"Could not submit the volume file action.") ||
            current.find(L"retained after attachment failure.")!=std::wstring::npos ||
            current.starts_with(L"Attachment retirement could not be confirmed."))){
          std::wcerr<<current<<L'\n';
          throw std::runtime_error("Mounted application reported a lifecycle failure");
        }
        return false;
      },deadline);
    };
    until([&]{
      const auto observation=observer->wait_for_exit();
      require(observation!=ProcessObservation::exited && observation!=ProcessObservation::wait_failed,
          "GUI exited or became unobservable before readiness");
      const bool visible=IsWindowVisible(window)!=FALSE;
      const bool editors=GetDlgItem(window,111) && GetDlgItem(window,112);
      const bool status_control=GetDlgItem(window,113)!=nullptr;
      const auto action=GetDlgItem(window,mounted?107:101);
      const bool enabled=action && IsWindowEnabled(action)!=FALSE;
      // The disposable guest exercises the controls on a headless desktop.
      const bool ready=(mounted || visible) && editors && status_control && (activation || enabled);
      if(!ready && GetTickCount64()>=deadline)
        std::cerr<<"GUI readiness visible="<<visible<<" editors="<<editors<<" status="<<status_control
            <<" action="<<(action!=nullptr)<<" enabled="<<enabled<<'\n';
      return ready;
    },deadline,"Application window did not expose enabled controls");
    status=GetDlgItem(window,113);require(status!=nullptr,"Ready status control missing");
    if(activation && activation->expect_create_conflict) {
      wait_status(L"Operation failed: FlitzFS store file: exclusive creation failed");
      require(!IsWindowEnabled(GetDlgItem(window,108)) && IsWindowEnabled(GetDlgItem(window,116)),
          "Refused creation retained an attachment or disabled file selection");
    } else if(mounted) {
      mounted_gui_workflow(window,mount_parent,*observer,button,wait_status,deadline,activation);
    } else {
      const bool qualified_file_controls=runtime==GuiRuntime::qualified_file_controls;
      const auto new_file=GetDlgItem(window,115),open_file=GetDlgItem(window,116);
      if(qualified_file_controls)
        require(new_file && open_file && IsWindowEnabled(new_file) && IsWindowEnabled(open_file),
            "Qualified file controls are unavailable in the empty app");
      if(runtime==GuiRuntime::missing || runtime==GuiRuntime::altered) {
        require(IsWindowEnabled(GetDlgItem(window,107))!=FALSE,"Attach control unavailable");
        button(107);
        wait_status(runtime==GuiRuntime::missing?L"WinFsp runtime is unavailable beside FlitzFS":
            L"WinFsp runtime differs from this FlitzFS build");
        require(observer->wait_for_exit()==ProcessObservation::running &&
            IsWindowEnabled(GetDlgItem(window,101))!=FALSE &&
            IsWindowEnabled(GetDlgItem(window,108))==FALSE,"Rejected SDK stranded the application");
        std::cout<<"gui_runtime_refusal kind="<<(runtime==GuiRuntime::missing?"missing":"altered")
            <<" preview_available=1\n";
      }
      button(101);wait_status(L"Memory volume created");
      if(qualified_file_controls)
        require(!IsWindowEnabled(new_file) && !IsWindowEnabled(open_file),
            "An existing preview allowed another backing selection");
      const auto summary=GetDlgItem(window,121);
      require(summary&&text(summary).find(L"Optimization state: ready")!=std::wstring::npos,
          "GUI optimization-state diagnostic missing");
      send(GetDlgItem(window,111),WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"qualification.txt"));
      const std::wstring payload=L"FlitzFS native GUI byte verification. 1234567890 \u03B1\u03B2 \U0001F680";
      send(GetDlgItem(window,112),WM_SETTEXT,0,reinterpret_cast<LPARAM>(payload.c_str()));
      button(102);wait_status(L"New file saved and byte-verified.");
      require(send(GetDlgItem(window,110),LB_GETCOUNT)==1,"GUI file list");
      send(GetDlgItem(window,110),LB_SETCURSEL,0);
      send(GetDlgItem(window,112),WM_SETTEXT,0,reinterpret_cast<LPARAM>(L""));
      button(103);wait_status(L"File read and integrity-checked.");
      require(text(GetDlgItem(window,112))==payload,"GUI content readback");
      const std::wstring draft=L"Unsaved editor changes must survive background notifications.";
      send(GetDlgItem(window,112),WM_SETTEXT,0,reinterpret_cast<LPARAM>(draft.c_str()));
      const auto edit_deadline=GetTickCount64()+3000;
      until([&]{send(window,VolumeController::completion_message);return GetTickCount64()>=edit_deadline;},deadline);
      require(text(GetDlgItem(window,112))==draft,"Background status replaced unsaved editor contents");
      send(window,WM_CLOSE);
      // Shell presence is not guaranteed on a hosted runner. Either a live tray
      // hides the window, or the application keeps the owner visible.
      require(IsWindow(window)!=FALSE && observer->wait_for_exit()==ProcessObservation::running,
          "Close destroyed a live temporary volume");
      ShowWindow(window,SW_RESTORE);
      button(104);
      until([&]{return IsWindowEnabled(GetDlgItem(window,105))!=FALSE;},deadline);
      require(text(status).find(L"Operation failed:")==std::wstring::npos,"GUI optimization failure");
      button(105);wait_status(L"Temporary volume explicitly discarded.");
      if(qualified_file_controls) {
        require(IsWindowEnabled(new_file) && IsWindowEnabled(open_file),
            "Closing the preview did not restore file selection");
        std::cout<<"gui_file_controls selectable_when_empty=1 refused_while_owned=1 restored_on_close=1\n";
      }
    }
    // Exit may destroy the button/window before a synchronous BM_CLICK reply.
    // Admit the click to its actual queue, then require exact process retirement.
    std::cout<<"GUI command: 106 (asynchronous exit)\n";
    const auto exit_button=GetDlgItem(window,106);
    require(exit_button && IsWindowEnabled(exit_button) &&
        PostMessageW(exit_button,BM_CLICK,0,0)!=FALSE,"GUI exit command admission");
    until([&]{return observer->wait_for_exit()==ProcessObservation::exited;},deadline);
  } catch(...) {test_failure=std::current_exception();observer->request_stop();}
  supervisor.join();
  if(failure)std::rethrow_exception(failure);
  require(result.exit_confirmed,"GUI child exit unconfirmed");
  if(test_failure) {
    std::cerr<<"Bounded GUI child exit="<<result.exit_code<<" cancelled="<<result.cancelled
        <<" timed_out="<<result.timed_out<<'\n';
    std::rethrow_exception(test_failure);
  }
  require(!result.timed_out && !result.cancelled && result.exit_code==0,"GUI child did not retire cleanly");
  if(!mounted)std::cout<<"GUI child peak commit: "<<result.peak_committed_bytes<<" bytes\n";
}

void mounted_file_gui_tests(const std::filesystem::path& app,const app_fixture::Budget& budget,bool populated) {
  const auto parent=std::filesystem::path(disposable_mount_parent()).parent_path();
  MountedFileActivation activation{parent/L"Volume \u00e9preuve.flfs"};
  activation.populated=populated;
  require(GetFileAttributesW(activation.path.c_str())==INVALID_FILE_ATTRIBUTES &&
      GetLastError()==ERROR_FILE_NOT_FOUND,"Mounted volume-file fixture already exists");
  const auto raw_name=(std::filesystem::path(mounted_moved_directory)/mounted_renamed_file).generic_u8string();
  const std::string name(reinterpret_cast<const char*>(raw_name.data()),raw_name.size());
  const auto expected=mounted_client_bytes();
  std::uint64_t last_epoch=0,last_checkpoint=0,last_namespace=0;
  std::array<std::uint64_t,mounted_trace_files> last_access{};
  const auto verify=[&](std::optional<StoreIdentity> identity,bool read_only_pass=false) {
    const auto view=NativeVolumeFile::inspect(activation.path,identity);
    require(view.filesystem().read(name,0,static_cast<std::uint32_t>(expected.size()))==expected,
        "Independent CPU inspection recovered different file bytes");
    if(populated) {
      const auto epoch=view.info().selection.epoch;
      const auto checkpoint=view.filesystem().storage_info().generation;
      const auto sequence=view.filesystem().sequence();
      require(sequence>=last_namespace &&
          (epoch>last_epoch || (epoch==last_epoch && checkpoint>=last_checkpoint)),
          "Recovered populated checkpoint went backwards");
      const auto sequence_delta=last_namespace?sequence-last_namespace:0;
      const auto retain_access=last_epoch!=0;
      last_epoch=epoch;last_checkpoint=checkpoint;last_namespace=sequence;
      std::size_t access_updates=0;
      const auto digest=verify_mounted_trace([&](std::size_t i,DWORD length) {
        const auto path="Population/"+mounted_trace_name(i,true);
        const auto info=view.filesystem().stat(path);
        const auto basic=view.filesystem().basic_metadata(info.inode);
        require(info.logical_bytes==length && basic.has_value(),"Independent CPU file metadata differs");
        const auto observed=basic->last_access_time;
        if(retain_access && observed!=last_access[i])++access_updates;
        last_access[i]=observed;
        return view.filesystem().read(path,0,length);
      });
      std::cout<<"mounted_population checkpoint="<<checkpoint<<" epoch="<<epoch
          <<" namespace_sequence="<<sequence<<" namespace_delta="<<sequence_delta
          <<" access_times_changed="<<access_updates<<'\n';
      print_mounted_trace_digest("independent_cpu",digest);
      if(read_only_pass)
        require(access_updates==0,"Recently recorded access times changed on a read-only pass");
    }
    return view.info().selection.volume_identity;
  };
  gui_tests(app,budget,GuiRuntime::mounted,&activation);
  const auto identity=verify(std::nullopt);
  activation.expect_create_conflict=true;
  gui_tests(app,budget,GuiRuntime::mounted,&activation);
  verify(identity);
  activation.expect_create_conflict=false;
  activation.action=VolumeFileAction::open_existing;
  gui_tests(app,budget,GuiRuntime::mounted,&activation);
  verify(identity,true);
  std::cout<<"mounted_file_activation exclusive_create=1 identity_preserved=1 "
      <<"fresh_process_reopen=1 independent_cpu_readback=1\n";
}
}
int wmain(int argc,wchar_t** argv) {
  try {
    const auto mode=argc>1?std::wstring_view(argv[1]):std::wstring_view{};
    const bool populated=mode==L"--gui-mounted-populated-file-sandbox";
    const bool mounted=argc==3 && (mode==L"--gui-mounted-sandbox" || mode==L"--gui-mounted-file-sandbox" || populated);
    app_fixture::Budget budget(!mounted);
    if(argc==2 && std::wstring_view(argv[1])==L"--model") {
      native_initialization();
      for(unsigned cycle=0;cycle<3;++cycle)model_tests();
    }
    else if(argc==2 && std::wstring_view(argv[1])==L"--maintenance")maintenance_tests();
    else if(argc==3) {
      const auto app=std::filesystem::path(argv[2]);
      if(mode==L"--gui")gui_tests(app,budget,GuiRuntime::ordinary);
      else if(mode==L"--gui-qualified-file-controls")gui_tests(app,budget,GuiRuntime::qualified_file_controls);
      else if(mode==L"--gui-missing-winfsp")gui_tests(app,budget,GuiRuntime::missing);
      else if(mode==L"--gui-altered-winfsp")gui_tests(app,budget,GuiRuntime::altered);
      else if(mode==L"--gui-mounted-sandbox")gui_tests(app,budget,GuiRuntime::mounted);
      else if(mode==L"--gui-mounted-file-sandbox" || populated)mounted_file_gui_tests(app,budget,populated);
      else throw std::runtime_error("Unknown GUI qualification mode");
    } else throw std::runtime_error("Explicit model, maintenance or GUI qualification mode required");
    std::cout<<"Windows volume application checks: "<<checks
        <<(mounted?". Disposable mounted workflow.\n":". Real CPU engine and bounded process-lifetime memory.\n");
    return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
