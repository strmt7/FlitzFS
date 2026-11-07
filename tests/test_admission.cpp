#include "flitzfs/catalog.hpp"
#include "flitzfs/catalog_error.hpp"
#include "flitzfs/projection.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {
void require(bool pass, const char* what) { if (!pass) throw std::runtime_error(what); }
void rejects(const std::function<void()>& run, const char* reason) {
  try { run(); }
  catch (const std::exception& error) {
    require(std::string(error.what()).find(reason) != std::string::npos, "unexpected rejection reason");
    return;
  }
  throw std::runtime_error("admission unexpectedly passed");
}
void typed(const std::function<void()>& run, flitzfs::CatalogErrorCode code) {
  try { run(); }
  catch (const flitzfs::CatalogError& error) { require(error.code() == code, "wrong typed error"); return; }
  throw std::runtime_error("typed rejection missing");
}
struct Fixture {
  fs::path root;
  std::vector<fs::path> files;
  Fixture() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned i = 0; i < 100 && root.empty(); ++i) {
      const auto p = fs::temp_directory_path() / ("flitzfs-admit-" + std::to_string(stamp) + "-" + std::to_string(i));
      if (fs::create_directory(p)) root = p;
    }
    require(!root.empty(), "isolated fixture creation failed");
  }
  fs::path file(const char* name) { auto p = root / name; files.push_back(p); return p; }
  ~Fixture() {
    for (const auto& p : files) { std::error_code ec; fs::remove(p, ec); }
    std::error_code ec; fs::remove(root, ec);
  }
};
}
int main() {
  try {
    Fixture fixture;
    const auto source = fixture.file("source.bin");
    const auto packed = fixture.file("source.flfs");
    { std::ofstream f(source, std::ios::binary); f << "bounded metadata"; require(bool(f), "fixture write"); }
    (void)flitzfs::pack_new(source, packed);
    require(flitzfs::Reader::open(packed, 32).info().chunk_count == 1, "exact index cap rejected");
    rejects([&] { (void)flitzfs::Reader::open(packed, 31); }, "index admission budget");
    rejects([&] { (void)flitzfs::Reader::open(packed, 0); }, "index admission budget");
    rejects([&] { (void)flitzfs::Reader::open(packed, 32ULL * 1024 * 1024 + 1); }, "hard limit");
    const auto empty = fixture.file("empty.bin");
    const auto empty_packed = fixture.file("empty.flfs");
    { std::ofstream f(empty, std::ios::binary); require(bool(f), "empty fixture write"); }
    (void)flitzfs::pack_new(empty, empty_packed);
    require(flitzfs::Reader::open(empty_packed, 0).info().chunk_count == 0, "empty index must fit zero cap");
    // Only 72 physical bytes. A plausible large header must hit the admission
    // check BEFORE looking for its absent footer/index, let alone allocating it.
    std::array<unsigned char, 72> bytes{'F','L','F','S','0','0','0','1'};
    auto field = [&](std::size_t at, std::uint64_t n, unsigned length) {
      for (unsigned i = 0; i < length; ++i) bytes[at + i] = static_cast<unsigned char>(n >> (8U * i));
    };
    field(8, 1, 4); field(12, 4096, 4);
    field(16, 4096ULL * flitzfs::kMaxChunks, 8); field(24, flitzfs::kMaxChunks, 8);
    const auto forged = fixture.file("oversized-index.flfs");
    { std::ofstream f(forged, std::ios::binary); f.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); require(bool(f), "forged fixture write"); }
    rejects([&] { (void)flitzfs::Reader::open(forged, 4096); }, "index admission budget");
    rejects([&] { (void)flitzfs::ReadOnlyCatalog::create({{"huge.bin", forged}}); }, "index admission budget");
    auto projection = flitzfs::ReadOnlyProjection::create({{"Docs/a.bin", packed}});
    using Code = flitzfs::CatalogErrorCode;
    typed([&] { (void)projection.stat(L"\\missing"); }, Code::not_found);
    typed([&] { (void)projection.stat(L"\\Docs\\a.bin:stream"); }, Code::invalid_path);
    typed([&] { (void)projection.list(L"\\Docs\\a.bin"); }, Code::not_directory);
    typed([&] { (void)projection.read(L"\\Docs", 0, 1); }, Code::is_directory);
    typed([&] { (void)projection.read(L"\\Docs\\a.bin", 0, flitzfs::kMaxSingleReadBytes + 1); }, Code::limit);
    std::cout << "PASS: pre-allocation index admission and typed callback errors\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
