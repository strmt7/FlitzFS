#include "flitzfs/container.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <cwctype>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#ifdef FLITZFS_HAS_ZSTD
  #include <zstd.h>
#endif
#ifdef _WIN32
  #define NOMINMAX
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <unistd.h>
#endif

namespace flitzfs {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::array<std::uint8_t, 8> kMagic = {'F','L','F','S','0','0','0','1'};
constexpr std::array<std::uint8_t, 8> kFooterMagic = {'F','L','F','S','I','D','X','1'};
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kHeaderBytes = 40;
constexpr std::size_t kIndexEntryBytes = 32;
constexpr std::size_t kFooterBytes = 32;
constexpr std::uint32_t kMaxEncodedBytes = 2u * 1024u * 1024u;
constexpr std::uint64_t kMaxIndexBytes = kMaxChunks * kIndexEntryBytes;

struct WireEntry {
  std::uint64_t offset{};
  std::uint32_t stored{};
  std::uint32_t decoded{};
  std::uint32_t crc{};
  std::uint8_t codec{};
};

[[noreturn]] void fail(const std::string& message) {
  throw std::runtime_error("FlitzFS: " + message);
}

void append_u32(Bytes& out, std::uint32_t n) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
}
void append_u64(Bytes& out, std::uint64_t n) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
}
std::uint32_t get_u32(const Bytes& a, std::size_t p) {
  if (p > a.size() || a.size() - p < 4) fail("truncated 32-bit field");
  std::uint32_t n = 0;
  for (int i = 0; i < 4; ++i) n |= std::uint32_t(a[p + static_cast<std::size_t>(i)]) << (8 * i);
  return n;
}
std::uint64_t get_u64(const Bytes& a, std::size_t p) {
  if (p > a.size() || a.size() - p < 8) fail("truncated 64-bit field");
  std::uint64_t n = 0;
  for (int i = 0; i < 8; ++i) n |= std::uint64_t(a[p + static_cast<std::size_t>(i)]) << (8 * i);
  return n;
}

constexpr auto kCrcTable = [] {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (unsigned b = 0; b < 8; ++b) c = (c >> 1) ^ ((c & 1U) ? 0xedb88320U : 0U);
    table[i] = c;
  }
  return table;
}();
std::uint32_t crc32(const std::uint8_t* data, std::size_t length) noexcept {
  std::uint32_t crc = 0xffffffffU;
  for (std::size_t i = 0; i < length; ++i)
    crc = kCrcTable[(crc ^ data[i]) & 0xffU] ^ (crc >> 8);
  return ~crc;
}

void read_exact(std::ifstream& file, Bytes& bytes, std::size_t length) {
  bytes.resize(length);
  if (length == 0) return;
  if (length > static_cast<std::size_t>((std::numeric_limits<std::streamsize>::max)())) fail("read size exceeds stream limit");
  file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length));
  if (file.gcount() != static_cast<std::streamsize>(length) || !file) fail("short read or damaged backing file");
}
void seek_exact(std::ifstream& file, std::uint64_t offset) {
  if (offset > static_cast<std::uint64_t>((std::numeric_limits<std::streamoff>::max)())) fail("offset exceeds seek range");
  file.clear();
  file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!file) fail("unable to seek backing file");
}

void validate_chunk_size(std::uint32_t value) {
  if (value < kMinChunkBytes || value > kMaxChunkBytes || (value & (value - 1U)) != 0U)
    fail("chunk size must be a power of two between 4 KiB and 1 MiB");
}

void validate_output_path(const std::filesystem::path& path) {
  if (path.empty() || path.filename().empty() || path == path.root_path())
    fail("output must be a specific new ordinary file");
#ifdef _WIN32
  // Refuse direct Win32/NT device namespaces, as well as DOS-reserved device names.
  // Extended-length paths are deliberately disabled in this conservative prototype.
  const auto s = path.wstring();
  if (s.rfind(L"\\\\.\\", 0) == 0 || s.rfind(L"\\\\?\\", 0) == 0 ||
      s.rfind(L"\\??\\", 0) == 0) fail("device/extended namespace output is forbidden");
  if (path.has_root_name() && !path.has_root_directory())
    fail("drive-relative Windows output paths are forbidden");
  auto filename = path.filename().wstring();
  if (!filename.empty() && (filename.back() == L'.' || filename.back() == L' '))
    fail("Windows output filename must not end with a dot or space");
  if (filename.find(L':') != std::wstring::npos)
    fail("NTFS alternate data stream output is forbidden");
  if (s.rfind(L"\\\\", 0) == 0)
    fail("network and device-style output paths are forbidden in this prototype");
  while (!filename.empty() && (filename.back() == L'.' || filename.back() == L' ')) filename.pop_back();
  if (filename.empty()) fail("blank Windows output filename is forbidden");
  const auto dot = filename.find(L'.');
  auto stem = filename.substr(0, dot);
  std::transform(stem.begin(), stem.end(), stem.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
  if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
      stem == L"CONIN$" || stem == L"CONOUT$" || stem == L"CLOCK$" ||
      (stem.size() == 4 && ((stem.substr(0, 3) == L"COM") || (stem.substr(0, 3) == L"LPT")) &&
       stem[3] >= L'1' && stem[3] <= L'9'))
    fail("reserved Windows device filename is forbidden");

#else
  const auto s = path.lexically_normal().string();
  if (s == "/dev" || s.rfind("/dev/", 0) == 0 ||
      s == "/proc" || s.rfind("/proc/", 0) == 0 ||
      s == "/sys" || s.rfind("/sys/", 0) == 0)
    fail("device and virtual-system outputs are forbidden");
#endif
}

void ensure_capacity_for_new_file(const std::filesystem::path& output, std::uint64_t maximum_output_bytes) {
  validate_output_path(output);
  const auto parent = output.parent_path().empty() ? std::filesystem::current_path() : output.parent_path();
  std::error_code ec;
  const auto stats = std::filesystem::space(parent, ec);
  if (ec || stats.available == static_cast<std::uintmax_t>(-1))
    fail("unable to verify backing-volume free space; refusing to write");
  if (maximum_output_bytes > (std::numeric_limits<std::uint64_t>::max)() - kFreeSpaceReserveBytes ||
      stats.available < maximum_output_bytes + kFreeSpaceReserveBytes)
    fail("insufficient available space with 1 GiB protected headroom; refusing to write");
}

class NewFileWriter {
 public:
  explicit NewFileWriter(const std::filesystem::path& path) {
    validate_output_path(path);
#ifdef _WIN32
    handle_ = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) fail("cannot create output exclusively (Windows error " + std::to_string(GetLastError()) + ")");
#else
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd_ < 0) fail("cannot create output exclusively: " + std::string(std::strerror(errno)));
#endif
  }
  NewFileWriter(const NewFileWriter&) = delete;
  NewFileWriter& operator=(const NewFileWriter&) = delete;
  ~NewFileWriter() {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
    if (fd_ >= 0) ::close(fd_);
#endif
  }
  void write(const std::uint8_t* data, std::size_t n) {
    while (n != 0) {
#ifdef _WIN32
      const auto amount = static_cast<DWORD>((std::min)(n, static_cast<std::size_t>(0x7ffff000U)));
      DWORD actual = 0;
      if (!WriteFile(handle_, data, amount, &actual, nullptr) || actual == 0) fail("output write failed");
      const std::size_t used = actual;
#else
      const auto amount = (std::min)(n, static_cast<std::size_t>(0x7ffff000U));
      const auto actual = ::write(fd_, data, amount);
      if (actual < 0 && errno == EINTR) continue;
      if (actual <= 0) fail("output write failed");
      const std::size_t used = static_cast<std::size_t>(actual);
#endif
      data += used;
      n -= used;
    }
  }
  void write(const Bytes& bytes) { write(bytes.data(), bytes.size()); }
  void flush() {
#ifdef _WIN32
    if (!FlushFileBuffers(handle_)) fail("failed to flush output");
#else
    if (::fsync(fd_) != 0) fail("failed to fsync output");
#endif
  }
 private:
#ifdef _WIN32
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int fd_ = -1;
#endif
};

Bytes make_header(std::uint32_t chunk, std::uint64_t logical, std::uint64_t count) {
  Bytes b(kMagic.begin(), kMagic.end());
  append_u32(b, kVersion);
  append_u32(b, chunk);
  append_u64(b, logical);
  append_u64(b, count);
  append_u64(b, 0);
  if (b.size() != kHeaderBytes) fail("internal header size mismatch");
  return b;
}
void serialize_entry(Bytes& index, const WireEntry& e) {
  append_u64(index, e.offset);
  append_u32(index, e.stored);
  append_u32(index, e.decoded);
  append_u32(index, e.crc);
  index.push_back(e.codec);
  index.push_back(0);
  index.push_back(0);
  index.push_back(0);
  append_u64(index, 0);
}

#ifdef FLITZFS_HAS_ZSTD
Bytes maybe_compress(const Bytes& decoded, int level, std::uint8_t& codec) {
  Bytes result(ZSTD_compressBound(decoded.size()));
  const auto n = ZSTD_compress(result.data(), result.size(), decoded.data(), decoded.size(), level);
  if (ZSTD_isError(n)) fail("Zstd compressor failed");
  // Only store Zstandard if savings exceed a small overhead allowance.
  if (n + 16 < decoded.size()) {
    result.resize(n);
    codec = 1;
    return result;
  }
  codec = 0;
  return decoded;
}
#endif

Bytes decode_payload(const Bytes& stored, std::size_t expected, std::uint8_t codec) {
  if (codec == 0) {
    if (stored.size() != expected) fail("raw block size mismatch");
    return stored;
  }
  if (codec != 1) fail("unknown codec");
#ifdef FLITZFS_HAS_ZSTD
  Bytes decoded(expected);
  ZSTD_DCtx* raw = ZSTD_createDCtx();
  if (!raw) fail("cannot allocate Zstd decoder context");
  std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> ctx(raw, &ZSTD_freeDCtx);
  const auto param = ZSTD_DCtx_setParameter(ctx.get(), ZSTD_d_windowLogMax, 20);
  if (ZSTD_isError(param)) fail("cannot cap Zstd window size");
  const auto n = ZSTD_decompressDCtx(ctx.get(), decoded.data(), decoded.size(), stored.data(), stored.size());
  if (ZSTD_isError(n) || n != expected) fail("Zstd block decompression failed");
  return decoded;
#else
  (void)stored; (void)expected;
  fail("container needs Zstandard; this build was compiled without it");
#endif
}
} // namespace

ContainerInfo pack_new(const std::filesystem::path& input, const std::filesystem::path& output,
                       const PackOptions& options) {
  validate_chunk_size(options.chunk_bytes);
  if (options.zstd_level < 1 || options.zstd_level > 5) fail("CPU Zstd level limited to [1,5] in the safety prototype");
  if (options.max_input_bytes > kMaxExplicitInputBytes || options.max_input_bytes == 0)
    fail("input budget must be between 1 byte and 4 GiB");
  std::error_code source_ec;
  const auto source_status = std::filesystem::symlink_status(input, source_ec);
  if (source_ec || !std::filesystem::is_regular_file(source_status))
    fail("input must be an ordinary non-symlink file");
  std::ifstream source(input, std::ios::binary | std::ios::ate);
  if (!source) fail("cannot open source for reading");
  const auto end = source.tellg();
  if (end < 0) fail("cannot determine source length");
  const auto logical = static_cast<std::uint64_t>(end);
  if (logical > options.max_input_bytes) fail("input exceeds explicitly configured size budget");
  const auto count = logical / options.chunk_bytes + (logical % options.chunk_bytes != 0 ? 1U : 0U);
  if (count > kMaxChunks) fail("source exceeds chunk-index resource budget");
  source.seekg(0, std::ios::beg);
  if (!source) fail("cannot rewind input");

  // Check worst-case raw payload + index before exclusive creation.
  // Compression will never increase stored payload size because raw fallback is used.
  ensure_capacity_for_new_file(output, logical + count * kIndexEntryBytes + kHeaderBytes + kFooterBytes);
  // Exclusive creation happens after validation; no path is replaced or deleted.
  NewFileWriter target(output);
  target.write(make_header(options.chunk_bytes, logical, count));
  Bytes index;
  index.reserve(static_cast<std::size_t>(count * kIndexEntryBytes));
  std::uint64_t next_offset = kHeaderBytes;
  std::uint64_t zstd_count = 0;
  std::uint64_t remaining = logical;
  for (std::uint64_t i = 0; i < count; ++i) {
    const auto n = static_cast<std::uint32_t>((std::min)(remaining, std::uint64_t(options.chunk_bytes)));
    Bytes decoded;
    read_exact(source, decoded, n);
    std::uint8_t codec = 0;
    Bytes encoded;
#ifdef FLITZFS_HAS_ZSTD
    encoded = maybe_compress(decoded, options.zstd_level, codec);
#else
    encoded = decoded;
#endif
    if (encoded.empty() || encoded.size() > kMaxEncodedBytes) fail("invalid encoded block size");
    WireEntry e{next_offset, static_cast<std::uint32_t>(encoded.size()), n, crc32(decoded.data(), decoded.size()), codec};
    target.write(encoded);
    serialize_entry(index, e);
    next_offset += encoded.size();
    remaining -= n;
    if (codec == 1) ++zstd_count;
  }
  if (remaining != 0 || index.size() != count * kIndexEntryBytes) fail("internal index size mismatch");
  const auto index_offset = next_offset;
  target.write(index);
  Bytes footer(kFooterMagic.begin(), kFooterMagic.end());
  append_u64(footer, index_offset);
  append_u64(footer, static_cast<std::uint64_t>(index.size()));
  append_u32(footer, crc32(index.data(), index.size()));
  append_u32(footer, 0);
  if (footer.size() != kFooterBytes) fail("internal footer size mismatch");
  target.write(footer);
  target.flush();
  return {options.chunk_bytes, logical, next_offset + index.size() + footer.size(), count, zstd_count};
}

Reader Reader::open(const std::filesystem::path& container) {
  std::error_code source_ec;
  const auto source_status = std::filesystem::symlink_status(container, source_ec);
  if (source_ec || !std::filesystem::is_regular_file(source_status))
    fail("container must be an ordinary non-symlink file");
  std::ifstream f(container, std::ios::binary | std::ios::ate);
  if (!f) fail("cannot open container");
  const auto end = f.tellg();
  if (end < 0 || end < static_cast<std::streamoff>(kHeaderBytes + kFooterBytes))
    fail("container is too short");
  const auto file_size = static_cast<std::uint64_t>(end);

  seek_exact(f, 0);
  Bytes header;
  read_exact(f, header, kHeaderBytes);
  if (!std::equal(kMagic.begin(), kMagic.end(), header.begin())) fail("bad container magic");
  if (get_u32(header, 8) != kVersion) fail("unsupported container version");
  const auto chunk_size = get_u32(header, 12);
  validate_chunk_size(chunk_size);
  const auto logical = get_u64(header, 16);
  const auto count = get_u64(header, 24);
  if (get_u64(header, 32) != 0) fail("unsupported header flags");
  if (count > kMaxChunks || count != logical / chunk_size + (logical % chunk_size != 0 ? 1U : 0U))
    fail("invalid or excessive chunk count");

  seek_exact(f, file_size - kFooterBytes);
  Bytes footer;
  read_exact(f, footer, kFooterBytes);
  if (!std::equal(kFooterMagic.begin(), kFooterMagic.end(), footer.begin())) fail("bad or missing commit footer");
  const auto index_offset = get_u64(footer, 8);
  const auto index_size = get_u64(footer, 16);
  const auto index_crc = get_u32(footer, 24);
  if (get_u32(footer, 28) != 0 || index_size > kMaxIndexBytes ||
      index_size != count * kIndexEntryBytes || index_offset < kHeaderBytes ||
      index_offset > file_size - kFooterBytes ||
      index_size != (file_size - kFooterBytes) - index_offset)
    fail("index boundary or reserved field invalid");

  seek_exact(f, index_offset);
  Bytes index;
  read_exact(f, index, static_cast<std::size_t>(index_size));
  if (crc32(index.data(), index.size()) != index_crc) fail("index checksum mismatch");
  std::vector<Entry> entries;
  entries.reserve(static_cast<std::size_t>(count));
  std::uint64_t expected_offset = kHeaderBytes;
  std::uint64_t decoded_total = 0;
  std::uint64_t zstd_count = 0;
  for (std::uint64_t i = 0; i < count; ++i) {
    const auto p = static_cast<std::size_t>(i * kIndexEntryBytes);
    const auto offset = get_u64(index, p);
    const auto stored = get_u32(index, p + 8);
    const auto decoded = get_u32(index, p + 12);
    const auto check = get_u32(index, p + 16);
    const auto codec = index[p + 20];
    const auto expected_decoded = static_cast<std::uint32_t>((std::min)(logical - decoded_total, std::uint64_t(chunk_size)));
    if (offset != expected_offset || stored == 0 || stored > kMaxEncodedBytes ||
        decoded != expected_decoded || (codec != 0 && codec != 1) ||
        (codec == 0 && stored != decoded) ||
        index[p + 21] != 0 || index[p + 22] != 0 || index[p + 23] != 0 ||
        get_u64(index, p + 24) != 0 ||
        expected_offset > index_offset || stored > index_offset - expected_offset)
      fail("invalid block descriptor");
    entries.push_back({offset, stored, decoded, check, codec});
    expected_offset += stored;
    decoded_total += decoded;
    if (codec == 1) ++zstd_count;
  }
  if (expected_offset != index_offset || decoded_total != logical) fail("index spans do not match container");
  ContainerInfo info{chunk_size, logical, file_size, count, zstd_count};
  return Reader(std::move(f), info, std::move(entries));
}

std::vector<std::byte> Reader::read_chunk(std::size_t index) {
  if (index >= entries_.size()) fail("chunk out of range");
  const auto& e = entries_[index];
  seek_exact(file_, e.offset);
  Bytes stored;
  read_exact(file_, stored, e.stored_bytes);
  auto decoded = decode_payload(stored, e.decoded_bytes, e.codec);
  if (crc32(decoded.data(), decoded.size()) != e.crc32) fail("chunk CRC mismatch");
  std::vector<std::byte> result(decoded.size());
  if (!decoded.empty()) std::memcpy(result.data(), decoded.data(), decoded.size());
  return result;
}

std::vector<std::byte> Reader::read(std::uint64_t offset, std::uint32_t length) {
  if (length > kMaxSingleReadBytes) fail("read request exceeds 4 MiB budget");
  if (offset > info_.logical_bytes || length > info_.logical_bytes - offset)
    fail("read exceeds logical end of container");
  std::vector<std::byte> result(length);
  std::uint64_t current = offset;
  std::size_t destination = 0;
  while (destination < result.size()) {
    const auto block_index = static_cast<std::size_t>(current / info_.chunk_bytes);
    const auto within = static_cast<std::size_t>(current % info_.chunk_bytes);
    if (cached_index_ != block_index) {
      auto new_chunk = read_chunk(block_index);
      cached_chunk_ = std::move(new_chunk);
      cached_index_ = block_index;
    }
    if (within >= cached_chunk_.size()) fail("read offset outside decoded block");
    const auto amount = (std::min)(result.size() - destination, cached_chunk_.size() - within);
    std::memcpy(result.data() + destination, cached_chunk_.data() + within, amount);
    destination += amount;
    current += amount;
  }
  return result;
}

void Reader::verify_all() {
  // Always touch the backing payloads rather than relying on the small read cache.
  cached_index_ = static_cast<std::size_t>(-1);
  cached_chunk_.clear();
  for (std::size_t i = 0; i < entries_.size(); ++i) (void)read_chunk(i);
}

void Reader::extract_new(const std::filesystem::path& output, std::uint64_t max_output_bytes) {
  if (max_output_bytes == 0 || max_output_bytes > kMaxExplicitInputBytes ||
      info_.logical_bytes > max_output_bytes) fail("extraction exceeds configured size budget");
  ensure_capacity_for_new_file(output, info_.logical_bytes);
  // Verify first: on known corruption, refuse before creating any output.
  // Concurrent modification of the backing file remains outside this preview's guarantees.
  verify_all();
  NewFileWriter target(output);
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    const auto decoded = read_chunk(i);
    target.write(reinterpret_cast<const std::uint8_t*>(decoded.data()), decoded.size());
  }
  target.flush();
}

} // namespace flitzfs
