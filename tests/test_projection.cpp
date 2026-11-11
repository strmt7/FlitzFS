#include "flitzfs/projection.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {
void require(bool pass, const char* message) {
  if (!pass) throw std::runtime_error(message);
}
void reject(const std::function<void()>& action, const char* message) {
  try { action(); } catch (const std::exception&) { return; }
  throw std::runtime_error(message);
}
struct Fixture {
  fs::path folder;
  std::vector<fs::path> cleanup;
  Fixture() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned i = 0; i < 100 && folder.empty(); ++i) {
      auto candidate = fs::temp_directory_path() /
          ("flitzfs-projection-" + std::to_string(stamp) + "-" + std::to_string(i));
      if (fs::create_directory(candidate)) folder = candidate;
    }
    if (folder.empty()) throw std::runtime_error("isolated fixture creation failed");
  }
  fs::path path(const std::string& filename) {
    const auto out = folder / filename;
    cleanup.push_back(out);
    return out;
  }
  ~Fixture() {
    for (auto& file : cleanup) { std::error_code ec; fs::remove(file, ec); }
    std::error_code ec; fs::remove(folder, ec);
  }
};
} // namespace

int main() {
  try {
    Fixture fx;
    auto source = fx.path("source.bin");
    auto archive = fx.path("source.flfs");
    std::vector<unsigned char> expected(260'503U);
    for (std::size_t i = 0; i < expected.size(); ++i)
      expected[i] = static_cast<unsigned char>((i * 29U + i / 17U) & 255U);
    { std::ofstream stream(source, std::ios::binary | std::ios::trunc);
      require(static_cast<bool>(stream), "fixture input creation failed");
      stream.write(reinterpret_cast<const char*>(expected.data()), static_cast<std::streamsize>(expected.size()));
      require(static_cast<bool>(stream), "fixture input write failed");
    }
    (void)flitzfs::pack_new(source, archive);
    auto projection = flitzfs::ReadOnlyProjection::create({{"Docs/Alpha.bin", archive}});
    require(projection.stat(L"\\").directory, "virtual root type");
    require(projection.stat(L"\\DOCS").directory, "virtual parent type");
    require(projection.stat(L"\\docs\\ALPHA.bin").logical_bytes == expected.size(), "file stat size");
    require(projection.list(L"\\").size() == 1, "root listing size");
    require(projection.list(L"\\docs").size() == 1, "nested listing size");
    {
      const auto read = projection.read(L"\\docs\\alpha.bin", 128U * 1024U - 111U, 1024U);
      require(read.size() == 1024U, "cross-chunk read length");
      for (std::size_t i = 0; i < read.size(); ++i)
        require(std::to_integer<unsigned char>(read[i]) == expected[128U * 1024U - 111U + i], "data mismatch");
    }
    {
      const auto read = projection.read(L"\\DOCS\\ALPHA.bin", expected.size() - 101U, 4096U);
      require(read.size() == 101U, "read spanning EOF must be short");
      for (std::size_t i = 0; i < read.size(); ++i)
        require(std::to_integer<unsigned char>(read[i]) == expected[expected.size() - 101U + i], "EOF data mismatch");
    }
    require(projection.read(L"\\docs\\alpha.bin", expected.size(), 10).empty(), "exact EOF read");
    require(projection.read(L"\\docs\\alpha.bin", (std::numeric_limits<std::uint64_t>::max)(), 128U).empty(), "beyond EOF read");
    require(projection.read(L"\\docs\\alpha.bin", 0, 0).empty(), "zero-byte read");
    reject([&] { (void)projection.read(L"\\docs\\alpha.bin", 0, flitzfs::kMaxSingleReadBytes + 1U); }, "oversized read accepted");
    reject([&] { (void)projection.read(L"\\docs", 0, 12); }, "directory read accepted");
    reject([&] { (void)projection.read(L"\\", 0, 12); }, "root read accepted");
    reject([&] { (void)projection.read(L"\\missing", 0, 12); }, "nonexistent file read accepted");
    for (const auto& name : {L"", L"C:\\Windows", L"relative", L"\\\\unc", L"\\docs\\..\\escape",
                             L"\\docs\\.\\Alpha.bin", L"\\docs\\", L"\\docs//alpha", L"\\docs\\alpha.bin/extra",
                             L"\\docs:stream", L"\\docs\\bad?", L"\\docs\\\x03b1.txt", L"\\docs\\\\double"})
      reject([&] { (void)projection.stat(name); }, "unsafe virtual Windows path accepted");
    const wchar_t nul_path[] = {L'\\',L'd',L'o',L'c',L's',0,L'x'};
    reject([&] { (void)projection.stat(std::wstring_view(nul_path, 7)); }, "NUL virtual path accepted");
    projection.verify_all();
    reject([&] { projection.verify_all(expected.size() - 1U); }, "verification budget bypassed");
    require(fs::file_size(archive) > 0, "backing container modified unexpectedly");
    std::cout << "PASS: Windows virtual path projection, bounded EOF semantics and no writes\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
