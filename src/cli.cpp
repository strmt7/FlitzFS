#include "flitzfs/container.hpp"
#include "flitzfs/catalog_manifest.hpp"

#include <charconv>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
void usage() {
  std::cout <<
    "FlitzFS experimental filesystem-image diagnostics (NOT a mounted filesystem)\n"
    "Operations never overwrite, format, partition, delete, or modify input files.\n"
    "This executable has no GPU, encryption, driver-install or mount commands.\n\n"
    "Usage:\n"
    "  flitzfs pack    <existing-input-file> <NEW-output.flfs> [--chunk-kib N] [--level N] [--max-mib N]\n"
    "  flitzfs info    <existing-input.flfs>\n"
    "  flitzfs verify  <existing-input.flfs> [--max-mib N]\n"
    "  flitzfs probe   <existing-input.flfs> <offset> <length>\n"
    "  flitzfs extract <existing-input.flfs> <NEW-output-file> [--max-mib N]\n\n"
    "  flitzfs catalog-list   <existing.flcatalog> [virtual-directory]\n"
    "  flitzfs catalog-verify <existing.flcatalog>\n"
    "  flitzfs catalog-probe  <existing.flcatalog> <virtual-file> <offset> <length>\n\n"
    "Catalog manifests: at most 8 KiB, 16 sibling .flfs files; plaintext, NOT authenticated.\n"
    "Maximum probe length: 4 MiB. Pack chunk size: 4..1024 KiB, power of two.\n"
    "Default pack/extract budget: 256 MiB. Explicit cap: 4096 MiB.\n"
    "Requires at least 1 GiB of free disk space after projected output size.\n"
    "A failed pack may leave a PARTIAL NEW file; it will never replace an old one.\n";
}
std::uint64_t parse_u64(std::string_view text) {
  std::uint64_t value = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 10);
  if (ec != std::errc{} || end != text.data() + text.size())
    throw std::runtime_error("expected a nonnegative integer: " + std::string(text));
  return value;
}
void print_info(const flitzfs::ContainerInfo& info) {
  std::cout << "Logical bytes: " << info.logical_bytes << "\n"
            << "Container bytes: " << info.container_bytes << "\n"
            << "Chunk bytes: " << info.chunk_bytes << "\n"
            << "Chunk count: " << info.chunk_count << "\n"
            << "CPU Zstandard chunks: " << info.zstd_chunk_count << "\n";
  if (info.logical_bytes != 0) {
    std::cout << "Stored/original: " << std::fixed << std::setprecision(3)
              << static_cast<double>(info.container_bytes) / static_cast<double>(info.logical_bytes) << "\n";
  }
}
} // namespace

int main(int argc, char** argv) {
  try {
    if (argc <= 1 || std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "help") {
      usage();
      return 0;
    }
    const auto command = std::string_view(argv[1]);
    if (command == "catalog-list" && (argc == 3 || argc == 4)) {
      auto catalog = flitzfs::load_catalog_manifest(argv[2]);
      for (const auto& entry : catalog.list(argc == 4 ? argv[3] : ""))
        std::cout << (entry.directory ? "directory" : "file") << '\t'
                  << entry.logical_bytes << '\t' << entry.path << '\n';
      std::cout << "Metadata only; use catalog-verify for bounded payload verification.\n";
      return 0;
    }
    if (command == "catalog-verify" && argc == 3) {
      auto catalog = flitzfs::load_catalog_manifest(argv[2]);
      catalog.verify_all();
      std::cout << "Catalog payloads verified under the 32 MiB total budget; NOT authenticated.\n";
      return 0;
    }
    if (command == "catalog-probe" && argc == 6) {
      const auto offset = parse_u64(argv[4]);
      const auto size = parse_u64(argv[5]);
      if (size > flitzfs::kMaxSingleReadBytes) throw std::runtime_error("probe length exceeds 4 MiB");
      auto catalog = flitzfs::load_catalog_manifest(argv[2]);
      const auto bytes = catalog.read(argv[3], offset, static_cast<std::uint32_t>(size));
      std::cout << "Read and CRC-checked " << bytes.size() << " catalog bytes.\n";
      return 0;
    }
    if (command == "pack") {
      if (argc < 4) { usage(); return 2; }
      flitzfs::PackOptions options;
      for (int i = 4; i < argc; i += 2) {
        if (i + 1 >= argc) throw std::runtime_error("option requires a value");
        const auto key = std::string_view(argv[i]);
        const auto raw = parse_u64(argv[i+1]);
        if (key == "--chunk-kib") {
          if (raw > (std::numeric_limits<std::uint32_t>::max)() / 1024ULL)
            throw std::runtime_error("chunk size overflow");
          options.chunk_bytes = static_cast<std::uint32_t>(raw * 1024ULL);
        } else if (key == "--max-mib") {
          if (raw == 0 || raw > 4096) throw std::runtime_error("max-mib must be 1..4096");
          options.max_input_bytes = raw * 1024ULL * 1024ULL;
        } else if (key == "--level") {
          if (raw > 5) throw std::runtime_error("level out of range");
          options.zstd_level = static_cast<int>(raw);
        } else throw std::runtime_error("unknown option: " + std::string(key));
      }
      print_info(flitzfs::pack_new(argv[2], argv[3], options));
      std::cout << "Created NEW container successfully.\n";
      return 0;
    }
    if (command == "info" && argc == 3) {
      print_info(flitzfs::Reader::open(argv[2]).info());
      std::cout << "Note: info validates metadata, not all compressed payloads. Use verify.\n";
      return 0;
    }
    if (command == "verify" && (argc == 3 || argc == 5)) {
      auto reader = flitzfs::Reader::open(argv[2]);
      auto max_bytes = flitzfs::kDefaultMaxInputBytes;
      if (argc == 5) {
        if (std::string_view(argv[3]) != "--max-mib") throw std::runtime_error("unknown verify option");
        const auto n = parse_u64(argv[4]);
        if (n == 0 || n > 4096) throw std::runtime_error("max-mib must be 1..4096");
        max_bytes = n * 1024ULL * 1024ULL;
      }
      if (reader.info().logical_bytes > max_bytes)
        throw std::runtime_error("verification exceeds explicitly configured size budget");
      reader.verify_all();
      std::cout << "Every chunk decoded and checked (CRC32 detects accidental corruption, NOT tampering).\n";
      return 0;
    }
    if (command == "probe" && argc == 5) {
      auto reader = flitzfs::Reader::open(argv[2]);
      const auto offset = parse_u64(argv[3]);
      const auto size = parse_u64(argv[4]);
      if (size > flitzfs::kMaxSingleReadBytes) throw std::runtime_error("probe length exceeds 4 MiB");
      const auto bytes = reader.read(offset, static_cast<std::uint32_t>(size));
      std::cout << "Read and integrity-checked " << bytes.size() << " logical bytes.\n";
      return 0;
    }
    if (command == "extract" && (argc == 4 || argc == 6)) {
      auto reader = flitzfs::Reader::open(argv[2]);
      std::uint64_t max_bytes = flitzfs::kDefaultMaxInputBytes;
      if (argc == 6) {
        if (std::string_view(argv[4]) != "--max-mib") throw std::runtime_error("unknown extraction option");
        const auto n = parse_u64(argv[5]);
        if (n == 0 || n > 4096) throw std::runtime_error("max-mib must be 1..4096");
        max_bytes = n * 1024ULL * 1024ULL;
      }
      reader.extract_new(argv[3], max_bytes);
      std::cout << "Extracted to a NEW file; no existing file was replaced.\n";
      return 0;
    }
    usage();
    return 2;
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
}
