#include "flitzfs/catalog.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
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
void require(bool pass, const char* message) {
  if (!pass) throw std::runtime_error(message);
}
void refuse(const std::function<void()>& run, const char* message) {
  try { run(); } catch (const std::exception&) { return; }
  throw std::runtime_error(message);
}
struct Fixtures {
  fs::path dir;
  std::vector<fs::path> files;
  Fixtures() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int i = 0; i < 100 && dir.empty(); ++i) {
      const auto candidate = fs::temp_directory_path() /
          ("flitzfs-catalog-test-" + std::to_string(stamp) + "-" + std::to_string(i));
      if (fs::create_directory(candidate)) dir = candidate;
    }
    if (dir.empty()) throw std::runtime_error("cannot make isolated test directory");
  }
  fs::path file(const std::string& name) {
    auto path = dir / name;
    files.push_back(path);
    return path;
  }
  ~Fixtures() {
    for (const auto& f : files) { std::error_code ec; fs::remove(f, ec); }
    std::error_code ec; fs::remove(dir, ec);
  }
};
void write_fixture(const fs::path& path, const std::vector<unsigned char>& bytes) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  require(static_cast<bool>(f), "cannot create fixture");
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  require(static_cast<bool>(f), "cannot write fixture");
}
void compare(flitzfs::ReadOnlyCatalog& catalog, const std::string& virtual_path,
             const std::vector<unsigned char>& expected, std::uint64_t pos, std::uint32_t n) {
  const auto bytes = catalog.read(virtual_path, pos, n);
  require(bytes.size() == n, "catalog read size mismatch");
  for (std::uint32_t i = 0; i < n; ++i)
    require(std::to_integer<unsigned char>(bytes[i]) == expected[static_cast<std::size_t>(pos) + i],
            "catalog read bytes mismatch");
}
}  // namespace

int main() {
  try {
    Fixtures fixture;
    const auto src1 = fixture.file("alpha-src");
    const auto src2 = fixture.file("beta-src");
    const auto f1 = fixture.file("alpha.flfs");
    const auto f2 = fixture.file("beta.flfs");
    std::vector<unsigned char> alpha(260'317u);
    for (std::size_t i = 0; i < alpha.size(); ++i) alpha[i] = static_cast<unsigned char>('A' + i % 7u);
    std::vector<unsigned char> beta(15'000u);
    for (std::size_t i = 0; i < beta.size(); ++i) beta[i] = static_cast<unsigned char>(i % 251u);
    write_fixture(src1, alpha);
    write_fixture(src2, beta);
    (void)flitzfs::pack_new(src1, f1);
    (void)flitzfs::pack_new(src2, f2);

    const std::vector<flitzfs::CatalogSource> sources = {
      {"Docs/ALPHA.txt", f1}, {"docs/Nested/Beta.bin", f2}};
    auto catalog = flitzfs::ReadOnlyCatalog::create(sources);
    require(catalog.stat("").directory, "root must be directory");
    require(catalog.stat("DOCS").directory, "case-insensitive directory lookup");
    require(catalog.stat("docs/alpha.TXT").logical_bytes == alpha.size(), "file metadata size");
    require(catalog.stat("docs/nested/beta.bin").logical_bytes == beta.size(), "nested file metadata size");
    const auto root = catalog.list("");
    require(root.size() == 1 && root[0].path == "Docs" && root[0].directory, "root listing");
    const auto docs = catalog.list("DOCS");
    require(docs.size() == 2, "docs contains file and nested directory");
    const auto nested = catalog.list("docs/NESTED");
    require(nested.size() == 1 && nested[0].logical_bytes == beta.size(), "nested directory listing");
    compare(catalog, "docs/alpha.TXT", alpha, 128u*1024u - 17u, 700u);
    compare(catalog, "docs/nested/BETA.bin", beta, 500u, 2'000u);
    require(catalog.read("docs/alpha.txt", alpha.size(), 0).empty(), "EOF empty read");
    refuse([&] { (void)catalog.read("docs/alpha.txt", alpha.size(), 1); }, "out-of-bounds accepted");
    refuse([&] { (void)catalog.read("docs", 0, 1); }, "directory read accepted");
    refuse([&] { (void)catalog.list("docs/alpha.txt"); }, "file as directory accepted");
    refuse([&] { (void)catalog.stat("docs/notpresent"); }, "missing file accepted");

    for (const auto& invalid : {"/absolute", "../up", "a/../b", "a/./b", "a//b", "a/",
                                "C:/drive", "a\\windows", "a:b", "CON.txt", "COM1", "nul",
                                "name.", "name ", "folder/PRN.any", "folder\\relative", "folder//dup"}) {
      refuse([&] { (void)flitzfs::ReadOnlyCatalog::create({{invalid, f1}}); },
             "malformed or reserved virtual path accepted");
    }
    refuse([&] { (void)flitzfs::ReadOnlyCatalog::create({{"Data.X", f1}, {"data.x", f2}}); },
           "case-folded path collision accepted");
    refuse([&] { (void)flitzfs::ReadOnlyCatalog::create({{"docs", f1}, {"Docs/child", f2}}); },
           "file-directory name conflict accepted");
    refuse([&] { (void)flitzfs::ReadOnlyCatalog::create({{"docs/a", f1}, {"DOCS", f2}}); },
           "directory-file name conflict accepted");
    refuse([&] { (void)flitzfs::ReadOnlyCatalog::create({{"Missing", fixture.dir / "missing.flfs"}}); },
           "missing backing file accepted");
    std::vector<flitzfs::CatalogSource> too_many;
    for (std::size_t i = 0; i < flitzfs::kMaxCatalogFiles + 1; ++i)
      too_many.push_back({"file" + std::to_string(i), f1});
    refuse([&] { (void)flitzfs::ReadOnlyCatalog::create(too_many); }, "file budget bypassed");
    require(flitzfs::ReadOnlyCatalog::create({}).list("").empty(), "empty catalog root");
    require(fs::file_size(f1) != 0 && fs::file_size(f2) != 0, "source archives unchanged");

    std::cout << "PASS: read-only path catalog, ASCII case-folded lookup, collision and bounds checks\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
