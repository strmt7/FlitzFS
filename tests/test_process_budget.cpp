#include "flitzfs/process_budget.hpp"
#include <iostream>
#include <stdexcept>
#include <limits>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F&& f) {
  try { f(); } catch (const std::invalid_argument&) { return; }
  throw std::runtime_error("invalid child command accepted");
}
}
int main() {
  try {
    using flitzfs::windows_child_command;
    flitzfs::ProcessBudget b;
    require(flitzfs::valid_process_budget(b), "default budget rejected");
    for (const auto memory : {0U, 63U, 257U, 0xffffffffU}) {
      b = {}; b.memory_mib = memory;
      require(!flitzfs::valid_process_budget(b), "memory ceiling bypass");
    }
    for (const auto rate : {0U, 99U, 2001U, 10000U}) {
      b = {}; b.cpu_rate = rate;
      require(!flitzfs::valid_process_budget(b), "CPU ceiling bypass");
    }
    for (const auto ms : {0U, 40001U, 0xffffffffU}) {
      b = {}; b.timeout_ms = ms;
      require(!flitzfs::valid_process_budget(b), "timeout ceiling bypass");
    }
    // Metadata-only simulated hosts: no allocation, launch, or hardware model.
    constexpr std::uint64_t gib=1024ULL*1024*1024,mib=1024*1024;
    const auto small=flitzfs::plan_resource_process_budget(300*mib,4*gib,8*gib,16,2000,15000);
    const auto large=flitzfs::plan_resource_process_budget(300*mib,64*gib,128*gib,16,2000,15000);
    require(!flitzfs::valid_process_budget(small) && flitzfs::valid_process_budget(large) &&
        large.memory_mib==300 && large.memory_share_divisor==16,"adaptive process memory did not scale");
    require(flitzfs::valid_process_budget(flitzfs::plan_resource_process_budget(
        2*gib+1,64*gib,128*gib,16,1000,5000)),"legacy MiB ceiling retained in adaptive route");
    const auto rounded=flitzfs::plan_resource_process_budget(64*mib+1,64*gib,128*gib,16,1000,5000);
    require(rounded.memory_mib==65,"adaptive byte allowance rounded down");
    for(unsigned fault=0;fault<9;++fault){
      auto required=300*mib,physical=64*gib,commit=128*gib;unsigned divisor=16,cpu=2000,timeout=15000;
      if(fault==0)required=0;
      if(fault==1)physical=0;
      if(fault==2)commit=512*mib;
      if(fault==3)divisor=1;
      if(fault==4)cpu=10000;
      if(fault==5)timeout=0;
      if(fault==6)required=~std::uint64_t{0};
      if(fault==7)physical=gib;
      if(fault==8)commit=gib;
      require(!flitzfs::valid_process_budget(flitzfs::plan_resource_process_budget(
          required,physical,commit,divisor,cpu,timeout)),"invalid adaptive process evidence admitted");
    }
    b=large;b.memory_share_divisor=15;require(!flitzfs::valid_process_budget(b),"unsafe adaptive share admitted");
    b=large;b.memory_mib=(std::numeric_limits<std::size_t>::max)();
    require(!flitzfs::valid_process_budget(b),"adaptive process byte overflow admitted");
    require(!flitzfs::valid_process_budget(flitzfs::plan_resource_process_budget(
        300*mib,8*gib,16*gib,64,1000,5000)),"background work share ignored");
    const std::wstring exe = L"C:\\Program Files\\FlitzFS\\helper.exe";
    const auto prefix = L"\"" + exe + L"\"";
    require(windows_child_command(exe, {}) == prefix, "executable quoting");
    const std::wstring generic=L"C:/Program Files/FlitzFS/helper.exe";
    require(windows_child_command(generic,{L"a b"})==prefix+L" \"a b\"",
        "valid generic Windows executable path rejected or misquoted");
    require(generic==L"C:/Program Files/FlitzFS/helper.exe","command encoder mutated borrowed source");
    require(windows_child_command(L"C:/Program Files\\FlitzFS/helper.exe",{})==prefix,
        "mixed valid Windows separators changed executable identity");
    for(const auto* invalid:{L"C:/NUL.exe",L"C:/dir/..",L"C:/helper.exe:stream",
        L"C://helper.exe",L"//server/helper.exe",L"C:helper.exe"})
      rejects([&]{(void)windows_child_command(invalid,{});});
    require(windows_child_command(exe, {L""}) == prefix + L" \"\"", "empty argument");
    require(windows_child_command(exe, {L"a b", L"a\tb"}) == prefix + L" \"a b\" \"a\tb\"", "spaces/tabs");
    require(windows_child_command(exe, {L"a\"b"}) == prefix + L" \"a\\\"b\"", "embedded quote");
    require(windows_child_command(exe, {L"path\\"}) == prefix + L" \"path\\\\\"", "trailing slash");
    require(windows_child_command(exe, {L"a\\\"b"}) == prefix + L" \"a\\\\\\\"b\"", "slash before quote");
    for (const auto* path : {L"helper.exe", L"C:helper.exe", L"\\\\server\\helper.exe",
        L"\\\\.\\device", L"C:\\NUL.exe", L"C:\\helper.exe:stream"})
      rejects([&] { (void)windows_child_command(path, {}); });
    rejects([&] { (void)windows_child_command(exe, {std::wstring(L"a\0b", 3)}); });
    rejects([&] { (void)windows_child_command(exe, {std::wstring(513, L'x')}); });
    rejects([&] { (void)windows_child_command(exe, std::vector<std::wstring>(17)); });
    rejects([&] { (void)windows_child_command(exe, std::vector<std::wstring>(16, std::wstring(512, L'x'))); });
    std::cout << "Portable process budgets and CRT command encoding passed.\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
