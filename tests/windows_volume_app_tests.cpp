#include "volume_controller.hpp"
#include "bounded_process.hpp"
#include "windows_app_budget.hpp"
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
    const auto use=m.domain_->usage();return m.domain_->create_store(use.capacity-use.allocated);
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
template<class F>void until(F&& predicate,std::uint64_t deadline) {
  while(!predicate()) {
    if(GetTickCount64()>=deadline)throw std::runtime_error("Application qualification deadline exceeded");
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
          model.optimization_info().settled_input_bytes==0&&model.view().allocated==baseline.allocated,
          "healthy audit rewrote/rebound exhausted advice");
    }
    for(unsigned rebuilt=1;rebuilt<NativeFileSession::max_optimization_bindings;++rebuilt) {
      const auto old=Q::current(model);Q::damage(model);
      const auto damaged=Q::bytes(old);
      const auto unfunded=model.view();
      for(unsigned repeat=0;repeat<8;++repeat)
        require(model.maintain_optimization_state()==Result::unavailable&&
            model.view().allocated==unfunded.allocated&&model.view().physical_bytes==unfunded.physical_bytes&&
            model.optimization_info().binding_attempts==rebuilt&&Q::current(model)==old,
            "unfunded helper allocated/admitted advice or retried historical input");
      Q::earn(model,2*kOptimizationAreaBytes);const auto funded=model.view();
      require(model.maintain_optimization_state()==Result::rebuilt,"helper did not rebuild failed advice");
      const auto view=model.view();const auto info=view.optimization;
      require(info.phase==NativeOptimizationPhase::ready&&info.sequence==1&&
          info.binding_attempts==rebuilt+1&&info.owned_area_bytes==(rebuilt+1)*kOptimizationAreaBytes&&
          !info.remaining_publications&&info.budgeted&&
          info.settled_input_bytes==Q::workload(model).committed_input_bytes&&!view.fenced&&
          view.allocated==baseline.allocated+rebuilt*kOptimizationAreaBytes&&
          view.physical_bytes==funded.physical_bytes+kOptimizationAreaBytes,
          "helper replacement mapping/accounting");
      require(Q::current(model)!=old&&Q::bytes(old)==damaged&&model.read_text("keep.txt")==text&&
          Q::read(model,handle)==expected&&Q::stats(model).contexts==1&&Q::stats(model).cleaned==1,
          "helper replaced source bytes, lost a live cleaned handle or reused an area");
    }
    Q::damage(model);
    const auto allocated=model.view().allocated;
    for(unsigned repeat=0;repeat<8;++repeat)
      require(model.maintain_optimization_state()==Result::unavailable&&
          model.view().allocated==allocated&&model.optimization_info().binding_attempts==
            NativeFileSession::max_optimization_bindings,"exhausted helper retried/leaked a new area");
    const auto funded_input=text.size()+(NativeFileSession::max_optimization_bindings-1)*2*kOptimizationAreaBytes;
    require(Q::workload(model)==NativeWorkloadInfo{true,funded_input},"advice loss or maintenance minted input");
    require(!model.optimize()&&model.view().allocated==allocated&&!model.view().fenced,
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
      require(use.allocated==use.capacity,"bounded pressure fixture did not fill its own domain");
      for(unsigned repeat=0;repeat<3;++repeat)
        rejects([&]{model.maintain_optimization_state();},"provider capacity refusal hidden");
      require(model.optimization_info().binding_attempts==before&&!model.view().fenced&&
          model.view().allocated==use.allocated&&model.read_text("keep.txt")==text&&
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
        !failed_view.fenced&&failed_view.allocated==baseline.allocated+kOptimizationAreaBytes&&
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
          model.view().allocated==insufficient.allocated&&model.optimization_info().binding_attempts==2,
          "failed creation reused already admitted input or leaked a replacement");
    Q::earn(model,2*kOptimizationAreaBytes);
    const auto recovery=model.view();
    require(model.maintain_optimization_state()==Result::rebuilt&&
        model.optimization_info().binding_attempts==3&&
        model.optimization_info().owned_area_bytes==3*kOptimizationAreaBytes&&
        Q::current(model)!=area&&area->extent()==written&&Q::bytes(old)==damaged&&
        (!cut||Q::bytes(area)==partial)&&
        model.view().allocated==baseline.allocated+2*kOptimizationAreaBytes&&
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
    require(store->extent()==0 && domain.usage().allocated==65536,"New store/accounting");
    std::array<std::byte,3> bytes{std::byte{1},std::byte{2},std::byte{3}},out{};
    store->write_exact(7,bytes);store->flush();store->read_exact(7,out);domain.verify_resident();
    require(out==bytes && store->extent()==10,"Exact memory I/O");
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
    store.reset();require(domain.usage().allocated==65536,"Shared store accounting");
    survivor.reset();domain.verify_resident();require(domain.usage().allocated==0 && domain.usage().stores==0,"Store cleanup");
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
    const auto image_capacity=initial.allocated-262144;
    require(initial.ready && image_capacity>=4*1024*1024 && image_capacity%65536==0 &&
        initial.capacity<=MemoryVolumeDomain::maximum_capacity &&
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
    const auto allocated=model.view().allocated;
    require(!model.optimize() && model.view().allocated==allocated,"Unchanged optimization repeated or leaked");
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
    require(observation.cpu_total_ticks==1000000 && observation.cpu_busy_ticks<=1000000 &&
        observation.cpu_window_ms>=1000 && observation.cpu_window_ms<=2000 &&
        observation.physical_total && observation.physical_available<=observation.physical_total &&
        observation.commit_limit && observation.commit_available<=observation.commit_limit,"Invalid live resource ratios");
    std::cout<<"Live maintenance telemetry: busy_ppm="<<observation.cpu_busy_ticks<<" window_ms="<<observation.cpu_window_ms
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
          <<" quanta="<<completed.maintenance_quanta<<" elapsed_ns="<<completed.maintenance_elapsed_ns
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
void gui_tests(const std::filesystem::path& app,const app_fixture::Budget& budget) {
  auto observer=std::make_shared<ProcessObserver>();ProcessResult result;
  std::exception_ptr failure;
  std::atomic<bool> supervisor_failed{false};
  const auto child_memory_mib=budget.child_memory_mib();
  std::thread supervisor([&]{try{result=run_bounded_process(app,{},ProcessBudget{child_memory_mib,2000,25000,16},observer);}
      catch(...){failure=std::current_exception();supervisor_failed.store(true,std::memory_order_release);}});
  std::exception_ptr test_failure;
  try {
    const auto deadline=GetTickCount64()+20000;
    HWND window=nullptr;
    until([&]{if(supervisor_failed.load(std::memory_order_acquire))throw std::runtime_error("GUI child launch failed");
      window=find_window(observer->process_id());return window!=nullptr;},deadline);
    auto button=[&](int id){std::cout<<"GUI command: "<<id<<"\n";auto h=GetDlgItem(window,id);require(h!=nullptr,"Missing native button");send(h,BM_CLICK);};
    HWND status=nullptr;
    auto wait_status=[&](std::wstring_view expected){
      until([&]{return text(status).find(expected)!=std::wstring::npos;},deadline);
    };
    until([&]{
      const auto observation=observer->wait_for_exit();
      require(observation!=ProcessObservation::exited && observation!=ProcessObservation::wait_failed,
          "GUI exited or became unobservable before readiness");
      return IsWindowVisible(window)!=FALSE && GetDlgItem(window,111) &&
          GetDlgItem(window,112) && GetDlgItem(window,113) &&
          IsWindowEnabled(GetDlgItem(window,101))!=FALSE;
    },deadline);
    status=GetDlgItem(window,113);require(status!=nullptr,"Ready status control missing");
    button(101);wait_status(L"Memory volume created");
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
  if(test_failure)std::rethrow_exception(test_failure);
  require(!result.timed_out && !result.cancelled && result.exit_code==0,"GUI child did not retire cleanly");
  std::cout<<"GUI child peak commit: "<<result.peak_committed_bytes<<" bytes\n";
}
}
int wmain(int argc,wchar_t** argv) {
  try {
    app_fixture::Budget budget;
    if(argc==2 && std::wstring_view(argv[1])==L"--model") {
      native_initialization();
      for(unsigned cycle=0;cycle<3;++cycle)model_tests();
    }
    else if(argc==2 && std::wstring_view(argv[1])==L"--maintenance")maintenance_tests();
    else if(argc==3 && std::wstring_view(argv[1])==L"--gui")gui_tests(std::filesystem::path(argv[2]),budget);
    else throw std::runtime_error("Explicit --model, --maintenance or --gui absolute-app-path required");
    std::cout<<"Windows volume application checks: "<<checks
        <<"; real CPU engine, bounded process-lifetime memory, no mount/GPU/driver.\n";return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
