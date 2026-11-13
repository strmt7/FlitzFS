#include "volume_controller.hpp"
#include "bounded_process.hpp"
#include "gpu_probe_budget.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>
#include <thread>
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
  static NativeSessionStats stats(const VolumeModel& m){return m.session_->stats();}
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
}

void advice_application_tests() {
  using Q=VolumeModelQualification;
  using Result=VolumeModel::AdviceMaintenance;
  const std::string text="retained bytes";
  const auto expected=std::vector<std::byte>(reinterpret_cast<const std::byte*>(text.data()),
      reinterpret_cast<const std::byte*>(text.data()+text.size()));
  {
    VolumeModel model;rejects([&]{model.maintain_optimization_state();},"uncreated advice maintenance accepted");
    model.create();model.create_text("keep.txt",text);
    const auto baseline=model.view();const auto handle=Q::open_cleaned(model);
    {
      const auto area=Q::current(model);const auto bytes=Q::bytes(area);
      require(model.maintain_optimization_state()==Result::ready&&
          model.maintain_optimization_state()==Result::ready&&Q::bytes(area)==bytes&&
          model.optimization_info().binding_attempts==1&&
          !model.optimization_info().remaining_publications&&model.view().allocated==baseline.allocated,
          "healthy audit rewrote/rebound exhausted advice");
    }
    for(unsigned rebuilt=1;rebuilt<NativeFileSession::max_optimization_bindings;++rebuilt) {
      const auto old=Q::current(model);Q::damage(model);
      const auto damaged=Q::bytes(old);
      require(model.maintain_optimization_state()==Result::rebuilt,"helper did not rebuild failed advice");
      const auto view=model.view();const auto info=view.optimization;
      require(info.phase==NativeOptimizationPhase::ready&&info.sequence==1&&
          info.binding_attempts==rebuilt+1&&info.owned_area_bytes==(rebuilt+1)*kOptimizationAreaBytes&&
          !info.remaining_publications&&!view.fenced&&
          view.allocated==baseline.allocated+rebuilt*kOptimizationAreaBytes&&
          view.physical_bytes==baseline.physical_bytes+rebuilt*kOptimizationAreaBytes,
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
    require(!model.optimize()&&model.view().allocated==allocated&&!model.view().fenced,
        "unavailable advice started image work or fenced data");
    model.create_text("after.txt","still writable");
    require(model.read_text("after.txt")=="still writable"&&model.read_text("keep.txt")==text&&
        Q::read(model,handle)==expected,"advice exhaustion stopped real file access");
    Q::close(model,handle);model.discard();
  }
  {
    VolumeModel model;model.create();model.create_text("keep.txt",text);
    const auto handle=Q::open_cleaned(model);Q::damage(model);
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
  {
    VolumeModel model;model.create();model.create_text("keep.txt",text);
    Q::damage(model,1);
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
    require(initial.ready && initial.allocated==4*1024*1024+262144 &&
        initial.capacity==12*1024*1024 && initial.files.empty(),"Initial real volume");
    rejects([&]{model.create();},"Duplicate create replaced volume");
    std::string data(8192,'A');for(std::size_t i=0;i<data.size();i+=37)data[i]='B';
    model.create_text("first.txt",data);
    require(model.read_text("first.txt")==data,"Real volume text readback");
    rejects([&]{model.create_text("first.txt","replacement");},"Existing file overwritten");
    require(model.read_text("first.txt")==data,"Duplicate create changed bytes");
    rejects([&]{model.create_text("../outside","x");},"Traversal accepted");
    rejects([&]{model.create_text("large",std::string(VolumeModel::text_limit+1,'x'));},"Editor cap accepted");
    (void)model.optimize();
    require(model.read_text("first.txt")==data && !model.view().fenced,"Optimization changed file");
    const auto allocated=model.view().allocated;
    require(!model.optimize() && model.view().allocated==allocated,"Unchanged optimization repeated or leaked");
    model.create_text("empty.txt","");require(model.read_text("empty.txt").empty(),"Empty file readback");
    const std::string unicode="UTF-8: \xCE\xB1\xCE\xB2 \xF0\x9F\x9A\x80";
    model.create_text("utf8.txt",unicode);
    require(model.read_text("utf8.txt")==unicode,"UTF-8 byte preservation");
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
void gui_tests(const std::filesystem::path& app) {
  auto observer=std::make_shared<ProcessObserver>();ProcessResult result;
  std::exception_ptr failure;
  std::thread supervisor([&]{try{result=run_bounded_process(app,{},ProcessBudget{128,2000,25000},observer);}
      catch(...){failure=std::current_exception();}});
  std::exception_ptr test_failure;
  try {
    const auto deadline=GetTickCount64()+20000;
    HWND window=nullptr;
    until([&]{window=find_window(observer->process_id());return window!=nullptr;},deadline);
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
    gpu_probe_fixture::Budget budget;
    if(argc==2 && std::wstring_view(argv[1])==L"--model") {
      native_initialization();
      for(unsigned cycle=0;cycle<3;++cycle)model_tests();
    }
    else if(argc==3 && std::wstring_view(argv[1])==L"--gui")gui_tests(std::filesystem::path(argv[2]));
    else throw std::runtime_error("Explicit --model or --gui absolute-app-path required");
    std::cout<<"Windows volume application checks: "<<checks
        <<"; real CPU engine, bounded process-lifetime memory, no mount/GPU/driver.\n";return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
