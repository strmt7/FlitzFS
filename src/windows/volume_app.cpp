#include "volume_controller.hpp"
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cwchar>
#include <string>
#include <vector>
#include <stdexcept>

namespace {
using namespace flitzfs;
constexpr UINT tray_message=WM_APP+42;
constexpr int create_id=101,save_id=102,read_id=103,optimize_id=104,discard_id=105,exit_id=106;
constexpr int files_id=110,name_id=111,text_id=112,status_id=113,show_id=114;
std::wstring wide(std::string_view text) {
  if(text.empty())return {};
  const auto n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),
      static_cast<int>(text.size()),nullptr,0);
  if(n<=0)throw std::runtime_error("Invalid UTF-8 display text");
  std::wstring result(static_cast<std::size_t>(n),L'\0');
  if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),
      result.data(),n)!=n)throw std::runtime_error("UTF-8 conversion failed");
  return result;
}
std::string utf8(HWND control,std::size_t limit) {
  const auto n=GetWindowTextLengthW(control);
  if(n<0 || static_cast<std::size_t>(n)>limit)throw std::runtime_error("Editor character limit");
  std::wstring text(static_cast<std::size_t>(n)+1,L'\0');
  const auto actual=GetWindowTextW(control,text.data(),static_cast<int>(text.size()));
  if(actual!=n)throw std::runtime_error("Editor read failed");
  if(!n)return {};
  const auto bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),n,nullptr,0,nullptr,nullptr);
  if(bytes<=0 || static_cast<std::size_t>(bytes)>limit)throw std::runtime_error("Editor UTF-8 byte limit");
  std::string result(static_cast<std::size_t>(bytes),'\0');
  if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),n,result.data(),bytes,nullptr,nullptr)!=bytes)
    throw std::runtime_error("Editor encoding failed");
  return result;
}
struct App {
  HWND window=nullptr,heading=nullptr,summary=nullptr,status=nullptr,files=nullptr,name=nullptr,text=nullptr;
  std::array<HWND,6> buttons{};
  std::unique_ptr<VolumeController> controller;
  NOTIFYICONDATAW tray{};bool tray_added=false,quitting=false;
  UINT taskbar_created=0;
  std::uint64_t rendered=UINT64_MAX;
  std::vector<std::string> paths;
  HWND control(const wchar_t* kind,const wchar_t* label,DWORD style,int id) {
    auto h=CreateWindowExW(0,kind,label,WS_CHILD|WS_VISIBLE|style,0,0,1,1,window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    if(!h)throw std::runtime_error("Native control creation failed");
    SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);
    return h;
  }
  void add_tray() {
    tray={};tray.cbSize=sizeof(tray);tray.hWnd=window;tray.uID=1;
    tray.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;tray.uCallbackMessage=tray_message;
    tray.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
    wcscpy_s(tray.szTip,L"FlitzFS experimental volume manager");
    tray_added=Shell_NotifyIconW(NIM_ADD,&tray)!=FALSE;
    if(tray_added) {
      tray.uVersion=NOTIFYICON_VERSION_4;
      if(!Shell_NotifyIconW(NIM_SETVERSION,&tray)) {
        if(Shell_NotifyIconW(NIM_DELETE,&tray))tray_added=false;
        ShowWindow(window,SW_SHOW);
        throw std::runtime_error("Tray callback version could not be established");
      }
    }
    if(!tray_added)ShowWindow(window,SW_SHOW);
    // Without a tray, Close keeps a live volume visible rather than stranding it.
  }
  void remove_tray() noexcept {
    if(tray_added && Shell_NotifyIconW(NIM_DELETE,&tray))tray_added=false;
  }
  ~App(){remove_tray();}
  void initialize(HWND h) {
    window=h;
    heading=control(L"STATIC",L"FlitzFS  /  Volume manager",SS_LEFT,120);
    summary=control(L"STATIC",L"Experimental  |  CPU engine  |  Not mounted",SS_LEFT,121);
    buttons[0]=control(L"BUTTON",L"Create memory volume",WS_TABSTOP,create_id);
    buttons[1]=control(L"BUTTON",L"Save new file",WS_TABSTOP,save_id);
    buttons[2]=control(L"BUTTON",L"Read selected",WS_TABSTOP,read_id);
    buttons[3]=control(L"BUTTON",L"Optimize",WS_TABSTOP,optimize_id);
    buttons[4]=control(L"BUTTON",L"Discard temporary volume",WS_TABSTOP,discard_id);
    buttons[5]=control(L"BUTTON",L"Exit",WS_TABSTOP,exit_id);
    files=control(L"LISTBOX",L"Volume files",WS_TABSTOP|WS_BORDER|LBS_NOTIFY|WS_VSCROLL,files_id);
    name=control(L"EDIT",L"note.txt",WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL,name_id);
    text=control(L"EDIT",L"",WS_TABSTOP|WS_BORDER|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL,text_id);
    SendMessageW(name,EM_SETLIMITTEXT,127,0);
    SendMessageW(text,EM_SETLIMITTEXT,VolumeModel::text_limit,0);
    status=control(L"STATIC",L"No volume. Create a memory volume to begin.",SS_LEFT,status_id);
    taskbar_created=RegisterWindowMessageW(L"TaskbarCreated");
    if(!taskbar_created)throw std::runtime_error("Tray restart notification unavailable");
    add_tray();controller=std::make_unique<VolumeController>(window);layout();refresh();
  }
  void layout() noexcept {
    RECT r{};GetClientRect(window,&r);
    const int dpi=static_cast<int>(GetDpiForWindow(window));
    auto d=[dpi](int n){return MulDiv(n,dpi?dpi:96,96);};
    const int w=r.right,h=r.bottom,p=d(16),gap=d(8),row=d(32);
    const int usable=(std::max)(1,w-2*p),left=(std::min)(d(260),usable/3);
    MoveWindow(heading,p,p,usable,d(28),TRUE);
    MoveWindow(summary,p,p+d(30),usable,d(44),TRUE);
    const int button_width=(std::max)(1,(usable-2*gap)/3);
    for(int i=0;i<6;++i)MoveWindow(buttons[static_cast<std::size_t>(i)],
        p+(i%3)*(button_width+gap),d(94)+(i/3)*(row+gap),button_width,row,TRUE);
    const int top=d(184),body=(std::max)(d(60),h-top-d(88));
    MoveWindow(files,p,top,left,body,TRUE);
    MoveWindow(name,p+left+gap,top,(std::max)(1,usable-left-gap),row,TRUE);
    MoveWindow(text,p+left+gap,top+row+gap,(std::max)(1,usable-left-gap),
        (std::max)(1,body-row-gap),TRUE);
    MoveWindow(status,p,h-d(78),usable,d(64),TRUE);
  }
  void refresh() {
    if(!controller)return;
    const auto v=controller->view();
    const bool idle=!v.busy&&!v.stopping&&!v.stopped;
    EnableWindow(buttons[0],idle&&!v.volume.ready);
    for(std::size_t i=1;i<4;++i)EnableWindow(buttons[i],idle&&v.volume.ready&&!v.volume.fenced);
    EnableWindow(buttons[4],idle&&v.volume.ready);
    EnableWindow(buttons[5],(idle&&!v.volume.ready)||v.stopped);
    if(v.generation!=rendered) {
      rendered=v.generation;paths.clear();SendMessageW(files,LB_RESETCONTENT,0,0);
      for(const auto& f:v.volume.files) {
        paths.push_back(f.path);const auto label=wide(f.path)+L"  ["+std::to_wstring(f.logical_bytes)+L" B]";
        SendMessageW(files,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
      }
      if(v.text)SetWindowTextW(text,wide(*v.text).c_str());
    }
    std::wstring state=L"Experimental | CPU engine | Not mounted";
    if(v.volume.ready)state+=L"\r\nFile bytes: "+std::to_wstring(v.volume.logical_bytes)+
        L" | Image/head/advice bytes: "+std::to_wstring(v.volume.physical_bytes)+
        L" | Locked allocation: "+std::to_wstring(v.volume.allocated)+L"/"+std::to_wstring(v.volume.capacity);
    SetWindowTextW(summary,state.c_str());
    const auto message=wide(v.message)+(v.busy?L"\r\nWorking...":L"");
    SetWindowTextW(status,message.c_str());
    if(v.stopped && quitting) {remove_tray();DestroyWindow(window);}
  }
  void command(int id) {
    if(!controller)return;
    if(id==show_id){ShowWindow(window,SW_RESTORE);SetForegroundWindow(window);return;}
    if(id==exit_id) {
      if(controller->view().stopped){quitting=true;remove_tray();DestroyWindow(window);return;}
      if(controller->stop(false)){quitting=true;refresh();}
      return;
    }
    VolumeCommand cmd{VolumeCommandKind::create,{},{}};
    switch(id) {
      case create_id:break;
      case save_id:cmd={VolumeCommandKind::create_text,utf8(name,127),utf8(text,VolumeModel::text_limit)};break;
      case read_id:{
        const auto selected=SendMessageW(files,LB_GETCURSEL,0,0);
        if(selected==LB_ERR || static_cast<std::size_t>(selected)>=paths.size())return;
        cmd={VolumeCommandKind::read_text,paths[static_cast<std::size_t>(selected)],{}};
        SetWindowTextW(name,wide(cmd.name).c_str());break;
      }
      case optimize_id:cmd.kind=VolumeCommandKind::optimize;break;
      case discard_id:cmd.kind=VolumeCommandKind::discard;break;
      default:return;
    }
    if(controller->submit(std::move(cmd)))refresh();
  }
  void tray_event(LPARAM value) {
    const auto event=LOWORD(value);
    if(event==NIN_SELECT || event==NIN_KEYSELECT || event==WM_LBUTTONUP)command(show_id);
    if(event==WM_CONTEXTMENU) {
      HMENU menu=CreatePopupMenu();if(!menu)return;
      AppendMenuW(menu,MF_STRING,show_id,L"Show FlitzFS");
      const auto v=controller->view();
      AppendMenuW(menu,MF_STRING|((v.volume.ready||v.busy)?MF_GRAYED:0),exit_id,L"Exit");
      POINT p{};GetCursorPos(&p);SetForegroundWindow(window);
      const auto selected=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,window,nullptr);
      DestroyMenu(menu);if(selected)command(static_cast<int>(selected));
    }
  }
};
LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM w,LPARAM l) {
  auto* app=reinterpret_cast<App*>(GetWindowLongPtrW(window,GWLP_USERDATA));
  if(message==WM_NCCREATE) {
    app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
    SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));
  }
  if(!app)return DefWindowProcW(window,message,w,l);
  try {
    if(app->taskbar_created && message==app->taskbar_created) {
      app->tray_added=false;app->add_tray();return 0;
    }
    switch(message) {
      case WM_CREATE:app->initialize(window);return 0;
      case WM_SIZE:app->layout();return 0;
      case WM_GETMINMAXINFO:{
        auto* limits=reinterpret_cast<MINMAXINFO*>(l);
        const auto dpi=GetDpiForWindow(window);
        limits->ptMinTrackSize={MulDiv(760,dpi?dpi:96,96),MulDiv(560,dpi?dpi:96,96)};
        return 0;
      }
      case WM_DPICHANGED:{
        const auto* r=reinterpret_cast<RECT*>(l);
        SetWindowPos(window,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);
        app->layout();return 0;
      }
      case WM_COMMAND:app->command(LOWORD(w));return 0;
      case VolumeController::completion_message:app->refresh();return 0;
      case tray_message:app->tray_event(l);return 0;
      case WM_CLOSE:
        if(app->controller && app->controller->view().volume.ready) {
          if(app->tray_added)ShowWindow(window,SW_HIDE);
          else SetWindowTextW(app->status,L"A temporary volume is active. Discard it explicitly before exit.");
        } else app->command(exit_id);
        return 0;
      case WM_QUERYENDSESSION:
        return !(app->controller && (app->controller->view().volume.ready||app->controller->view().busy));
      case WM_DESTROY:PostQuitMessage(0);return 0;
      case WM_NCDESTROY:SetWindowLongPtrW(window,GWLP_USERDATA,0);break;
    }
  } catch(...) {
    if(message==WM_CREATE)return -1;
    if(app->status)SetWindowTextW(app->status,L"Operation could not complete. The volume remains owned.");
    return 0;
  }
  return DefWindowProcW(window,message,w,l);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR command_line,int) {
  // No unattended mode, autostart registration, elevation or dependency installer.
  if(command_line && *command_line)return 2;
  App app;WNDCLASSW type{};type.lpfnWndProc=procedure;type.hInstance=instance;
  type.lpszClassName=L"FlitzFS.VolumeApp.Experimental";type.hCursor=LoadCursorW(nullptr,IDC_ARROW);
  type.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
  if(!RegisterClassW(&type))return 3;
  const auto window=CreateWindowExW(0,type.lpszClassName,L"FlitzFS — Experimental volume manager",
      WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,1000,680,nullptr,nullptr,instance,&app);
  if(!window)return 4;
  ShowWindow(window,SW_SHOW);UpdateWindow(window);
  MSG message{};int result=0;
  while((result=GetMessageW(&message,nullptr,0,0))>0) {
    if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}
  }
  // Controller destructor joins its already-retired worker; all tray/UI objects
  // are process-local and no service, scheduled task or registry state is used.
  return result<0?5:(app.tray_added?6:(MemoryVolumeDomain::cleanup_healthy()?0:7));
}
