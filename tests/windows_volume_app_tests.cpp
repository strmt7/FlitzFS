#include "volume_controller.hpp"
#include "bounded_process.hpp"
#include "gpu_probe_budget.hpp"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>
#include <thread>
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
void model_tests() {
  DWORD before=0,after=0;
  require(GetProcessHandleCount(GetCurrentProcess(),&before)!=FALSE,"Handle baseline");
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
    store->write_exact(7,bytes);store->flush();store->read_exact(7,out);
    require(out==bytes && store->extent()==10,"Exact memory I/O");
    store->write_exact(65536,{});require(store->extent()==10,"Empty write extended image");
    rejects([&]{store->write_exact(UINT64_MAX,bytes);},"Overflow write accepted");
    rejects([&]{store->read_exact(UINT64_MAX,out);},"Overflow read accepted");
    rejects([&]{store->admit_write(65537);},"Capacity preflight accepted");
    // Domain wrapper retirement must not restore its lease while a store lives.
    auto survivor=store;
    store.reset();require(domain.usage().allocated==65536,"Shared store accounting");
    survivor.reset();require(domain.usage().allocated==0 && domain.usage().stores==0,"Store cleanup");
  }
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
    model.discard();require(!model.ready(),"Discard retained engine");
  }
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
  SIZE_T final_min=0,final_max=0;DWORD final_flags=0;
  require(GetProcessWorkingSetSizeEx(GetCurrentProcess(),&final_min,&final_max,&final_flags)!=FALSE &&
      final_min==ws_min && final_max==ws_max && final_flags==ws_flags,"Working-set lease not restored");
  require(MemoryVolumeDomain::cleanup_healthy(),"Memory cleanup health");
  require(GetProcessHandleCount(GetCurrentProcess(),&after)!=FALSE && after==before,"Controller handle leak");
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
  require(SendMessageTimeoutW(window,message,w,l,SMTO_ABORTIFHUNG|SMTO_BLOCK,1000,&result)!=0,"GUI control timeout");
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
    auto button=[&](int id){auto h=GetDlgItem(window,id);require(h!=nullptr,"Missing native button");send(h,BM_CLICK);};
    auto status=GetDlgItem(window,113);
    auto wait_status=[&](std::wstring_view expected){
      until([&]{return text(status).find(expected)!=std::wstring::npos;},deadline);
    };
    require(IsWindowVisible(window)!=FALSE,"GUI did not appear");
    button(101);wait_status(L"Memory volume created");
    send(GetDlgItem(window,111),WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"qualification.txt"));
    const std::wstring payload=L"FlitzFS native GUI byte verification. 1234567890";
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
    button(106);
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
    if(argc==2 && std::wstring_view(argv[1])==L"--model")model_tests();
    else if(argc==3 && std::wstring_view(argv[1])==L"--gui")gui_tests(std::filesystem::path(argv[2]));
    else throw std::runtime_error("Explicit --model or --gui absolute-app-path required");
    std::cout<<"Windows volume application checks: "<<checks
        <<"; real CPU engine, bounded process-lifetime memory, no mount/GPU/driver.\n";return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
