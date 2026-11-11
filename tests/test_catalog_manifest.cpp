#include "flitzfs/catalog_manifest.hpp"
#include "flitzfs/catalog_error.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
void require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
template<class F> void rejects(F&& action) {
  try { action(); } catch (const std::exception&) { return; }
  throw std::runtime_error("malformed manifest was accepted");
}
struct Fixture {
  fs::path root;
  Fixture() {
    root = fs::temp_directory_path() / ("flitzfs-manifest-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    require(fs::create_directory(root), "exclusive fixture required");
  }
  ~Fixture() {
    std::error_code ignored;
    for (const auto* name : {"source.bin", "a.flfs", "collection.flcatalog", "large.flcatalog",
        "invalid.flcatalog", "manifest-link", "backing-link.flfs", "link.flcatalog"})
      fs::remove(root / name, ignored);
    fs::remove(root, ignored); // Never recursively remove anything.
  }
};
void write_fixture(const fs::path& path, const std::string& data) {
  // Used only with previously unused names inside this exclusively owned fixture.
  require(!fs::exists(path), "test attempted to overwrite a fixture");
  std::ofstream out(path, std::ios::binary);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  require(bool(out), "fixture write failed");
}
}
int main() {
  try {
    using flitzfs::ManifestEntry;
    using flitzfs::parse_catalog_manifest;
    using flitzfs::serialize_catalog_manifest;
    const std::vector<ManifestEntry> expected{{"Folder/Example.bin", "a.flfs"}, {"Alias.bin", "a.flfs"}};
    const auto text = serialize_catalog_manifest(expected);
    require(text == "FLITZFS-CATALOG-1\t2\nFolder/Example.bin\ta.flfs\nAlias.bin\ta.flfs\n", "wire format differs");
    require(parse_catalog_manifest(text) == expected, "manifest round trip");
    std::string crlf;
    for (const auto c : text) { if (c == '\n') crlf += '\r'; crlf += c; }
    require(parse_catalog_manifest(crlf) == expected, "Windows CRLF manifest rejected");
    require(parse_catalog_manifest("FLITZFS-CATALOG-1\t0\n").empty(), "empty catalog rejected");
    require(serialize_catalog_manifest({}) == "FLITZFS-CATALOG-1\t0\n", "empty encoding");
    for (std::size_t i = 0; i < text.size(); ++i)
      rejects([&] { (void)parse_catalog_manifest(std::string_view(text).substr(0, i)); });
    for (const auto* bad : {"", "FLITZFS-CATALOG-2\t0\n", "FLITZFS-CATALOG-1\t-1\n",
        "FLITZFS-CATALOG-1\t+1\n", "FLITZFS-CATALOG-1\t01\n", "FLITZFS-CATALOG-1\t 0\n",
        "FLITZFS-CATALOG-1\t17\n", "FLITZFS-CATALOG-1\t9999999999999999999999999\n",
        "FLITZFS-CATALOG-1\t0\nextra\n", "FLITZFS-CATALOG-1\t1\nx\ta.flfs\textra\n",
        "FLITZFS-CATALOG-1\t1\nx\ta.flfs\n\n", "FLITZFS-CATALOG-1\t1\n\ta.flfs\n",
        "FLITZFS-CATALOG-1\t1\nx\t\n", "FLITZFS-CATALOG-1\t1\nx\ta.flfs"})
      rejects([&] { (void)parse_catalog_manifest(bad); });
    for (const auto* leaf : {"../a.flfs", "sub/a.flfs", "sub\\a.flfs", "C:a.flfs", "a.flfs:stream",
        "a.zip", "NUL.flfs", "COM1.flfs", "NUL .flfs", "COM0.flfs", "a*.flfs", "a.flfs ", ".flfs"})
      rejects([&] { (void)serialize_catalog_manifest({{"okay", leaf}}); });
    for (const auto* name : {"../a", "/root", "C:a", "a\\b", "a/b.", "NUL.bin", "a.x:y", "a//b"})
      rejects([&] { (void)serialize_catalog_manifest({{name, "a.flfs"}}); });
    rejects([&] { (void)serialize_catalog_manifest({{"a", "a.flfs"}, {"A", "a.flfs"}}); });
    rejects([&] { (void)serialize_catalog_manifest({{"a", "a.flfs"}, {"a/b", "a.flfs"}}); });
    rejects([&] { (void)serialize_catalog_manifest({{"a/b", "a.flfs"}, {"A", "a.flfs"}}); });
    rejects([&] { (void)serialize_catalog_manifest({{std::string("a\0b", 3), "a.flfs"}}); });
    rejects([&] { (void)serialize_catalog_manifest({{"a", std::string("a\0.flfs", 7)}}); });
    rejects([&] { (void)serialize_catalog_manifest({{std::string(1, static_cast<char>(255)), "a.flfs"}}); });
    rejects([&] { (void)parse_catalog_manifest(std::string(flitzfs::kMaxManifestBytes + 1, 'x')); });
    std::vector<ManifestEntry> many;
    for (unsigned i = 0; i < 16; ++i) many.push_back({"f" + std::to_string(i), "a.FLFS"});
    require(parse_catalog_manifest(serialize_catalog_manifest(many)) == many, "maximum file count rejected");
    many.push_back({"f16", "a.flfs"});
    rejects([&] { (void)serialize_catalog_manifest(many); });
    // Deterministic mutation coverage; valid mutations must still canonicalize.
    for (std::size_t i = 0; i < text.size(); ++i) for (const auto value : {0, 9, 10, 13, 32, 65, 127, 255}) {
      auto mutated = text;
      mutated[i] = static_cast<char>(value);
      std::vector<ManifestEntry> decoded;
      try { decoded = parse_catalog_manifest(mutated); }
      catch (const std::exception&) { continue; }
      require(parse_catalog_manifest(serialize_catalog_manifest(decoded)) == decoded, "mutation canonicalization");
    }
    Fixture fixture;
    const std::string bytes(32769, 'm');
    write_fixture(fixture.root / "source.bin", bytes);
    (void)flitzfs::pack_new(fixture.root / "source.bin", fixture.root / "a.flfs");
    const auto manifest = fixture.root / "collection.flcatalog";
    write_fixture(manifest, text);
    const auto sources = flitzfs::read_catalog_manifest_sources(manifest);
    require(sources.size() == 2 && sources[0].container.is_absolute() &&
        sources[0].container == fixture.root / "a.flfs", "sibling binding differs");
    auto catalog = flitzfs::load_catalog_manifest(manifest);
    catalog.verify_all();
    require(catalog.list("").size() == 2 && catalog.stat("folder/EXAMPLE.bin").logical_bytes == bytes.size(), "persistent namespace");
    const auto recovered = catalog.read("alias.bin", 0, static_cast<std::uint32_t>(bytes.size()));
    require(std::string(reinterpret_cast<const char*>(recovered.data()), recovered.size()) == bytes, "persistent byte parity");
    write_fixture(fixture.root / "large.flcatalog", std::string(flitzfs::kMaxManifestBytes + 1, 'x'));
    rejects([&] { (void)flitzfs::load_catalog_manifest(fixture.root / "large.flcatalog"); });
    // Invalid layout must fail before the nonexistent first backing file is opened.
    write_fixture(fixture.root / "invalid.flcatalog", "FLITZFS-CATALOG-1\t2\na\tmissing.flfs\na/b\tmissing.flfs\n");
    bool path_error = false;
    try { (void)flitzfs::load_catalog_manifest(fixture.root / "invalid.flcatalog"); }
    catch (const flitzfs::CatalogError& e) { path_error = e.code() == flitzfs::CatalogErrorCode::invalid_path; }
    require(path_error, "namespace rejection happened after backing access");
#ifndef _WIN32
    fs::create_symlink(manifest, fixture.root / "manifest-link");
    rejects([&] { (void)flitzfs::load_catalog_manifest(fixture.root / "manifest-link"); });
    fs::create_symlink(fixture.root / "a.flfs", fixture.root / "backing-link.flfs");
    write_fixture(fixture.root / "link.flcatalog", serialize_catalog_manifest({{"x", "backing-link.flfs"}}));
    rejects([&] { (void)flitzfs::load_catalog_manifest(fixture.root / "link.flcatalog"); });
#endif
    std::cout << "Bounded persistent manifests, mutation rejection, aliases and read parity passed.\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
