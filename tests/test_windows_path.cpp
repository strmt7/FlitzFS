#include "flitzfs/windows_path.hpp"
#include "flitzfs/directory_order.hpp"
#include <array>
#include <iostream>
#include <initializer_list>
#include <stdexcept>
#include <string>

int main() {
  try {
    for (const auto* path : {L"file.bin", L"C:\\temp\\file.flfs", L"D:/new.bin",
        L".\\fixture\\file", L"../fixture/file", L"\\temp\\file", L"folder/.hidden",
        L"folder/a.b.c", L"file.name with spaces", L"C:/data/\u0394\u03bf\u03ba\u03b9\u03bc\u03ae.bin"}) {
      if (!flitzfs::ordinary_windows_file_path(path)) throw std::runtime_error("ordinary path rejected");
    }
    for (const auto* path : {L"", L".", L"..", L"C:", L"C:relative", L"C:\\", L"folder/",
        L"\\\\.\\PhysicalDrive0", L"\\\\?\\C:\\data", L"\\??\\C:\\data", L"//server/share/file",
        L"\\/server/share/file", L"C://file", L"folder//file", L"file:stream", L"dir:stream/file",
        L"file\"name", L"file<name", L"file>name", L"file|name", L"file?name", L"file*name",
        L"CON", L"NUL.txt", L"PrN.tar.gz", L"Aux .txt", L"CONIN$", L"CONOUT$", L"CLOCK$",
        L"COM0", L"COM1.txt", L"LPT9", L"COM\u00b9.bin", L"LPT\u00b2.log", L"COM\u00b3",
        L"folder/NUL/file", L"folder./file", L"folder /file", L"file.", L"file ", L"folder/.."}) {
      if (flitzfs::ordinary_windows_file_path(path)) throw std::runtime_error("unsafe path accepted");
    }
    const auto nul = std::wstring(L"safe.bin") + wchar_t(0) + L"other.bin";
    if (flitzfs::ordinary_windows_file_path(nul)) throw std::runtime_error("embedded NUL accepted");
    for (wchar_t control = 1; control < 32; ++control)
      if (flitzfs::ordinary_windows_file_path(std::wstring(L"safe") + control + L".bin"))
        throw std::runtime_error("control character accepted");
    if (flitzfs::ordinary_windows_file_path(std::wstring(256, L'a')) ||
        flitzfs::ordinary_windows_file_path(std::wstring(32768, L'a')))
      throw std::runtime_error("name or path budget bypassed");
    constexpr std::array<std::string_view, 8> ordered{
        ".", "..", " a.bin", "(a).bin", "-a.bin", ".hidden", "Alpha.bin", "Beta.bin"};
    for (std::size_t i = 0; i < ordered.size(); ++i)
      for (std::size_t j = 0; j < ordered.size(); ++j)
        if (flitzfs::directory_name_after(ordered[i], ordered[j]) != (i > j))
          throw std::runtime_error("directory marker ordering skipped or repeated an entry");
    if (flitzfs::directory_name_after("ALPHA.bin", "alpha.BIN") ||
        !flitzfs::directory_name_after("beta.bin", "ALPHA.bin"))
      throw std::runtime_error("directory marker case folding");
    std::cout << "PASS: lexical host path gate, device aliases, namespaces, streams and NUL; no OS I/O\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
