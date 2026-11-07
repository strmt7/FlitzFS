#include "flitzfs/container.hpp"

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

struct TempScope {
  fs::path dir;
  std::vector<fs::path> created;
  TempScope() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int i = 0; i < 50; ++i) {
      const auto candidate = fs::temp_directory_path() /
        ("flitzfs-owned-test-" + std::to_string(stamp) + "-" + std::to_string(i));
      if (fs::create_directory(candidate)) { dir = candidate; break; }
    }
    if (dir.empty()) throw std::runtime_error("unable to create exclusive test directory");
  }
  fs::path name(const std::string& filename) {
    fs::path p = dir / filename;
    created.push_back(p);
    return p;
  }
  ~TempScope() {
    // Delete only individually recorded files inside our exclusively-created directory.
    // Never call recursive remove_all, never delete a user-supplied path.
    for (const auto& p : created) { std::error_code ec; fs::remove(p, ec); }
    std::error_code ec; fs::remove(dir, ec);
  }
};

void require(bool result, const std::string& description) {
  if (!result) throw std::runtime_error("TEST FAILED: " + description);
}
void expect_failure(const std::function<void()>& action, const std::string& description) {
  try { action(); }
  catch (const std::exception&) { return; }
  throw std::runtime_error("TEST FAILED (accepted unsafe input): " + description);
}
void put(const fs::path& p, const std::vector<unsigned char>& bytes) {
  std::ofstream output(p, std::ios::binary | std::ios::trunc);
  require(bool(output), "opening test source");
  output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  require(bool(output), "writing test source");
}
std::vector<unsigned char> load(const fs::path& p) {
  std::ifstream input(p, std::ios::binary | std::ios::ate);
  require(bool(input), "opening test result");
  const auto n = input.tellg();
  require(n >= 0, "test result size");
  std::vector<unsigned char> result(static_cast<std::size_t>(n));
  input.seekg(0);
  input.read(reinterpret_cast<char*>(result.data()), n);
  require(bool(input) || n == 0, "reading test result");
  return result;
}
void alter_byte(const fs::path& p, std::uint64_t position) {
  std::fstream f(p, std::ios::binary | std::ios::in | std::ios::out);
  require(bool(f), "open test file for intentional corruption");
  f.seekg(static_cast<std::streamoff>(position));
  char value = 0;
  f.get(value);
  require(bool(f), "read test byte");
  f.seekp(static_cast<std::streamoff>(position));
  value = static_cast<char>(static_cast<unsigned char>(value) ^ 0xa5u);
  f.put(value);
  require(bool(f), "mutate test byte");
}
std::uint64_t test_u64le(const std::vector<unsigned char>& data, std::size_t at) {
  std::uint64_t out = 0;
  for (unsigned i = 0; i < 8; ++i) out |= std::uint64_t(data[at + i]) << (8 * i);
  return out;
}
std::uint32_t test_crc32(const unsigned char* data, std::size_t length) {
  std::uint32_t crc = 0xffffffffU;
  for (std::size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (unsigned b = 0; b < 8; ++b) crc = (crc >> 1) ^ ((crc & 1U) ? 0xedb88320U : 0U);
  }
  return ~crc;
}
void forge_index_field(const fs::path& original, const fs::path& target, std::size_t field) {
  auto bytes = load(original);
  const std::size_t footer = bytes.size() - 32;
  const auto start = static_cast<std::size_t>(test_u64le(bytes, footer + 8));
  const auto size = static_cast<std::size_t>(test_u64le(bytes, footer + 16));
  require(start + size == footer && field < size, "index fixture boundaries");
  bytes[start + field] ^= 0x5bu;
  const auto crc = test_crc32(bytes.data() + start, size);
  for (unsigned i = 0; i < 4; ++i) bytes[footer + 24 + i] = static_cast<unsigned char>(crc >> (i * 8));
  put(target, bytes);
}

std::vector<unsigned char> make_pattern(std::size_t size) {
  std::vector<unsigned char> bytes(size);
  for (std::size_t i = 0; i < size; ++i) {
    bytes[i] = static_cast<unsigned char>((i % 8192 < 5000) ? 'A' + (i % 7) : '0' + (i % 10));
  }
  return bytes;
}
std::vector<unsigned char> make_noise(std::size_t size) {
  std::vector<unsigned char> bytes(size);
  std::uint32_t x = 0xf2133c17u;
  for (auto& c : bytes) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; c = static_cast<unsigned char>(x); }
  return bytes;
}
void compare_range(flitzfs::Reader& r, const std::vector<unsigned char>& source,
                   std::uint64_t offset, std::uint32_t n) {
  const auto result = r.read(offset, n);
  require(result.size() == n, "range size");
  for (std::uint32_t i = 0; i < n; ++i)
    require(std::to_integer<unsigned char>(result[i]) == source[static_cast<std::size_t>(offset) + i], "range contents");
}
int main() {
  try {
    TempScope tmp;
    const auto empty = tmp.name("empty-input");
    const auto empty_container = tmp.name("empty.flfs");
    const auto empty_result = tmp.name("empty-extracted");
    put(empty, {});
    const auto empty_info = flitzfs::pack_new(empty, empty_container);
    require(empty_info.chunk_count == 0 && empty_info.logical_bytes == 0, "empty info");
    auto r0 = flitzfs::Reader::open(empty_container);
    r0.verify_all();
    require(r0.read(0, 0).empty(), "empty range");
    expect_failure([&] { (void)r0.read(0, 1); }, "empty range beyond EOF");
    r0.extract_new(empty_result);
    require(fs::file_size(empty_result) == 0, "empty extraction");

    const auto source = tmp.name("original-data");
    const auto archive = tmp.name("original.flfs");
    const auto restored = tmp.name("restored-data");
    const auto pattern = make_pattern(512u * 1024u + 137u);
    put(source, pattern);
    auto info = flitzfs::pack_new(source, archive);
    require(info.logical_bytes == pattern.size() && info.chunk_count == 5, "chunking");
    auto reader = flitzfs::Reader::open(archive);
    reader.verify_all();
    compare_range(reader, pattern, 0, 33);
    compare_range(reader, pattern, 128u * 1024u - 19u, 110);
    compare_range(reader, pattern, pattern.size() - 17u, 17);
    compare_range(reader, pattern, 0, static_cast<std::uint32_t>(pattern.size()));
    // Bounded property-style check of randomly placed independent requests.
    std::uint32_t x = 0x87b4d5a1u;
    for (unsigned trial = 0; trial < 120; ++trial) {
      x ^= x << 13; x ^= x >> 17; x ^= x << 5;
      const auto offset = static_cast<std::uint64_t>(x % pattern.size());
      const auto n = static_cast<std::uint32_t>((std::min)(std::uint64_t(1 + ((x >> 3) % 16384)),
                                                         static_cast<std::uint64_t>(pattern.size()) - offset));
      compare_range(reader, pattern, offset, n);
    }
    require(reader.read(pattern.size(), 0).empty(), "EOF zero range");
    expect_failure([&] { (void)reader.read(pattern.size(), 1); }, "range beyond EOF");
    expect_failure([&] { (void)reader.read(0, flitzfs::kMaxSingleReadBytes + 1u); }, "unbounded read");
    reader.extract_new(restored);
    require(load(restored) == pattern, "lossless extraction");

    const auto source_noise = tmp.name("noise-input");
    const auto archive_noise = tmp.name("noise.flfs");
    const auto noise = make_noise(245u * 1024u + 19u);
    put(source_noise, noise);
    auto noisy_info = flitzfs::pack_new(source_noise, archive_noise);
    require(noisy_info.logical_bytes == noise.size(), "noise size");
    auto rnoise = flitzfs::Reader::open(archive_noise);
    rnoise.verify_all();
    compare_range(rnoise, noise, 117u * 1024u, 60u * 1024u);

    const auto sentinel = tmp.name("sentinel-exists");
    put(sentinel, {1, 2, 3, 4});
    expect_failure([&] { (void)flitzfs::pack_new(source, sentinel); }, "pack must not overwrite");
    expect_failure([&] { reader.extract_new(sentinel); }, "extract must not overwrite");
    expect_failure([&] { (void)flitzfs::pack_new(source, source); }, "same input and output");
    require(load(sentinel) == std::vector<unsigned char>({1, 2, 3, 4}), "existing target stays unchanged");
    require(load(source) == pattern, "source stays unchanged");
    flitzfs::PackOptions tiny_budget;
    tiny_budget.max_input_bytes = 16;
    const auto refused_archive = tmp.dir / "budget-refused.flfs";
    expect_failure([&] { (void)flitzfs::pack_new(source, refused_archive, tiny_budget); }, "pack budget guard");
    require(!fs::exists(refused_archive), "budget failure creates no target");
    const auto refused_extract = tmp.dir / "extract-budget-refused";
    expect_failure([&] { reader.extract_new(refused_extract, 16); }, "extract budget guard");
    require(!fs::exists(refused_extract), "extract budget failure creates no target");
    for (std::uint32_t chunk : {4u*1024u, 64u*1024u, 1024u*1024u}) {
      flitzfs::PackOptions setting; setting.chunk_bytes = chunk;
      const auto variable = tmp.name("chunk-" + std::to_string(chunk) + ".flfs");
      (void)flitzfs::pack_new(source, variable, setting);
      auto v = flitzfs::Reader::open(variable);
      v.verify_all();
      compare_range(v, pattern, 2000, 17000);
    }

    const auto broken_data = tmp.name("broken-payload.flfs");
    fs::copy_file(archive, broken_data, fs::copy_options::none);
    alter_byte(broken_data, 40);
    auto broken_reader = flitzfs::Reader::open(broken_data);
    expect_failure([&] { broken_reader.verify_all(); }, "payload corruption detected");

    const auto broken_index = tmp.name("broken-index.flfs");
    fs::copy_file(archive, broken_index, fs::copy_options::none);
    alter_byte(broken_index, fs::file_size(broken_index) - 32u - 3u);
    expect_failure([&] { (void)flitzfs::Reader::open(broken_index); }, "index/footer corruption detected");

    // Forge index entries AND fix their CRC: descriptor checks must reject them
    // even when their accidental-corruption checksum is valid.
    for (std::size_t position : {0u, 8u, 12u, 20u, 21u, 24u}) {
      const auto forged = tmp.name("forged-descriptor-" + std::to_string(position) + ".flfs");
      forge_index_field(archive, forged, position);
      expect_failure([&] { (void)flitzfs::Reader::open(forged); }, "forged index descriptor rejection");
    }
    const auto forged_crc = tmp.name("forged-block-crc.flfs");
    forge_index_field(archive, forged_crc, 16);
    auto wrong_crc = flitzfs::Reader::open(forged_crc);
    expect_failure([&] { wrong_crc.verify_all(); }, "forged block CRC rejected on decode");

#ifndef _WIN32
    const auto source_link = tmp.name("source-symlink");
    fs::create_symlink(source, source_link);
    expect_failure([&] { (void)flitzfs::pack_new(source_link, tmp.dir / "symlink-output.flfs"); },
                   "symbolic input refused");
    const auto archive_link = tmp.name("archive-symlink");
    fs::create_symlink(archive, archive_link);
    expect_failure([&] { (void)flitzfs::Reader::open(archive_link); }, "symbolic container refused");
    const auto output_link = tmp.name("output-symlink");
    fs::create_symlink(sentinel, output_link);
    expect_failure([&] { (void)flitzfs::pack_new(source, output_link); }, "symbolic output never overwritten");
    require(load(sentinel) == std::vector<unsigned char>({1, 2, 3, 4}), "symbolic output target unchanged");
#endif

    const auto incomplete = tmp.name("incomplete.flfs");
    fs::copy_file(archive, incomplete, fs::copy_options::none);
    fs::resize_file(incomplete, fs::file_size(incomplete) - 15u);
    expect_failure([&] { (void)flitzfs::Reader::open(incomplete); }, "missing commit footer");

    const auto fake_count = tmp.name("fake-count.flfs");
    fs::copy_file(archive, fake_count, fs::copy_options::none);
    alter_byte(fake_count, 26);
    expect_failure([&] { (void)flitzfs::Reader::open(fake_count); }, "tampered count rejected before allocation");

    for (int j = 0; j < 12; ++j) {
      auto mutation = tmp.name("mutated-" + std::to_string(j) + ".flfs");
      fs::copy_file(archive, mutation, fs::copy_options::none);
      alter_byte(mutation, static_cast<std::uint64_t>((j % 2 == 0) ? j : fs::file_size(mutation) - 1u - j));
      expect_failure([&] { (void)flitzfs::Reader::open(mutation); }, "damaged metadata rejection");
    }

    expect_failure([&] { flitzfs::PackOptions bad; bad.chunk_bytes = 12345; (void)flitzfs::pack_new(source, tmp.dir / "bad.flfs", bad); }, "bad chunk size");
    expect_failure([&] { flitzfs::PackOptions bad; bad.zstd_level = 99; (void)flitzfs::pack_new(source, tmp.dir / "badlevel.flfs", bad); }, "unbounded codec level");

    std::cout << "PASS: empty, lossless, random reads, CPU codecs, no-clobber, resource bounds, and corruption tests\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
