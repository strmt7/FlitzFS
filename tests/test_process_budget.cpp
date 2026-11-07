#include "flitzfs/process_budget.hpp"
#include <iostream>
#include <stdexcept>

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
    const std::wstring exe = L"C:\\Program Files\\FlitzFS\\helper.exe";
    const auto prefix = L"\"" + exe + L"\"";
    require(windows_child_command(exe, {}) == prefix, "executable quoting");
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
