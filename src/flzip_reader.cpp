#include "flitzfs/flzip_reader.hpp"
#include "flitzfs/catalog_error.hpp"
#include "flitzfs/read_only_file.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <utility>
#ifdef FLITZFS_HAS_ZSTD
#include <zstd.h>
#endif
#ifdef FLITZFS_HAS_ZLIB
#include <zlib.h>
#endif

namespace flitzfs {
namespace {
using Bytes = std::vector<std::byte>;
[[noreturn]] void bad(const char* message) { throw FlzipError(FlzipErrorCode::malformed, message); }
[[noreturn]] void limit(const char* message) { throw FlzipError(FlzipErrorCode::limit, message); }
[[noreturn]] void corrupt(const char* message) { throw FlzipError(FlzipErrorCode::corrupt, message); }
void admit(std::size_t bytes, std::uint64_t budget) {
  if (bytes > kMaxFlzipSnapshotBytes || budget == 0 || budget > kMaxFlzipLogicalBytes)
    limit("FLZIP snapshot or logical budget exceeds preview limits");
  if (bytes < 36) bad("FLZIP archive is too short");
}
struct Cursor {
  std::span<const std::byte> data;
  std::size_t pos = 0;
  std::size_t remaining() const noexcept { return data.size() - pos; }
  std::span<const std::byte> take(std::size_t n) {
    if (n > remaining()) bad("FLZIP metadata is truncated");
    const auto out = data.subspan(pos, n); pos += n; return out;
  }
  std::uint64_t number(std::size_t width) {
    const auto bytes = take(width);
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i)
      value |= std::uint64_t(std::to_integer<unsigned>(bytes[i])) << (8U * i);
    return value;
  }
};
constexpr auto crc_table = [] {
  std::array<std::uint32_t, 256> out{};
  for (std::uint32_t i = 0; i < out.size(); ++i) {
    auto v = i;
    for (unsigned b = 0; b < 8; ++b) v = (v >> 1) ^ ((v & 1U) ? 0xedb88320U : 0U);
    out[i] = v;
  }
  return out;
}();
std::uint32_t update_crc(std::uint32_t prior, std::span<const std::byte> data) noexcept {
  auto crc = ~prior;
  for (const auto value : data) crc = crc_table[(crc ^ std::to_integer<unsigned>(value)) & 255U] ^ (crc >> 8);
  return ~crc;
}
void admit_codec(std::uint8_t kind, std::uint32_t version) {
  constexpr unsigned introduced[] = {1,1,1,1,2,3,4,5,6,7,8,9,10,11};
  if (kind >= std::size(introduced) || version < introduced[kind])
    bad("FLZIP block kind is invalid for its declared version");
  if (!flzip_codec_available(kind))
    throw FlzipError(FlzipErrorCode::unsupported_codec, "FLZIP block codec is not available in this CPU preview");
}
} // namespace

bool flzip_codec_available(std::uint8_t kind) noexcept {
  if (kind == 0 || kind == 1 || kind == 3) return true;
#ifdef FLITZFS_HAS_ZLIB
  if (kind == 2) return true;
#endif
#ifdef FLITZFS_HAS_ZSTD
  if (kind == 8) return true;
#endif
  return false;
}

struct FlzipReader::Impl {
  struct Block {
    std::uint8_t kind = 0, fill = 0;
    std::uint32_t decoded = 0, encoded = 0;
    std::uint64_t absolute = 0, logical = 0;
  };
  struct Node {
    CatalogEntry entry;
    std::uint32_t crc = 0;
    std::vector<Block> blocks;
  };
  Bytes snapshot;
  FlzipInfo info;
  std::map<std::string, Node> nodes;
  Bytes cached;
  const Block* cached_block = nullptr;

  explicit Impl(Bytes bytes, std::uint64_t budget) : snapshot(std::move(bytes)) {
    admit(snapshot.size(), budget);
    Cursor footer{std::span<const std::byte>(snapshot).last(24)};
    if (footer.number(4) != 0x465a5553U) bad("FLZIP footer magic missing");
    info.version = static_cast<std::uint32_t>(footer.number(4));
    if (info.version < 1 || info.version > 11)
      throw FlzipError(FlzipErrorCode::unsupported_version, "unsupported FLZIP version");
    const auto index_start = footer.number(8), index_size = footer.number(8);
    const auto footer_start = snapshot.size() - 24;
    if (index_start > footer_start || index_size != footer_start - index_start || index_size < 12)
      bad("FLZIP index extent or framing is invalid");
    if (index_size > kMaxFlzipIndexBytes) limit("FLZIP index exceeds 1 MiB");
    Cursor index{std::span<const std::byte>(snapshot).subspan(static_cast<std::size_t>(index_start), static_cast<std::size_t>(index_size))};
    if (index.number(4) != 0x505a5553U || index.number(4) != info.version)
      bad("FLZIP index magic/version mismatch");
    const auto records = index.number(4);
    if (records > kMaxFlzipRecords) limit("FLZIP entry count exceeds 256");
    if (records > index.remaining() / 35) bad("FLZIP entry count cannot fit index");
    nodes.emplace("", Node{{"", true, 0}, 0, {}});
    std::set<std::string> explicit_paths;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> intervals;
    for (std::uint64_t i = 0; i < records; ++i) {
      const auto path_len = index.number(2);
      if (!path_len || path_len > 240) bad("FLZIP path exceeds preview namespace");
      const auto path_bytes = index.take(static_cast<std::size_t>(path_len));
      std::string path(reinterpret_cast<const char*>(path_bytes.data()), path_bytes.size());
      const auto key = canonical_catalog_path(path);
      if (!explicit_paths.insert(key).second) bad("FLZIP duplicate path");
      const auto directory = index.number(1);
      if (directory > 1) bad("FLZIP invalid directory flag");
      const auto logical = index.number(8), payload_start = index.number(8), payload_size = index.number(8);
      const auto crc = static_cast<std::uint32_t>(index.number(4));
      const auto count = index.number(4);
      if (payload_start > index_start || payload_size > index_start - payload_start)
        bad("FLZIP payload overlaps metadata");
      if (count > kMaxFlzipBlocks - info.blocks) limit("FLZIP block count exceeds 8192");
      if (count > index.remaining() / 18) bad("FLZIP block count cannot fit index");
      if (logical > budget - info.logical_bytes) limit("FLZIP decoded collection exceeds admitted budget");
      if (directory && (logical || payload_size || count || crc)) bad("FLZIP directory has data");
      if (!directory && ++info.files > kMaxCatalogFiles) limit("FLZIP file count exceeds 16");
      info.logical_bytes += logical;
      info.blocks += static_cast<std::size_t>(count);
      if (payload_size) intervals.emplace_back(payload_start, payload_start + payload_size);
      for (std::size_t j = 0; j < key.size(); ++j) if (key[j] == '/') {
        const auto prefix = key.substr(0, j);
        auto [it, inserted] = nodes.try_emplace(prefix, Node{{path.substr(0, j), true, 0}, 0, {}});
        if (!inserted && !it->second.entry.directory) bad("FLZIP file/directory conflict");
        if (nodes.size() > 257) limit("FLZIP virtual node count exceeds 257");
      }
      Node node{{path, directory == 1, logical}, crc, {}};
      node.blocks.reserve(static_cast<std::size_t>(count));
      std::uint64_t payload_cursor = 0, decoded_cursor = 0;
      for (std::uint64_t j = 0; j < count; ++j) {
        Block b;
        b.kind = static_cast<std::uint8_t>(index.number(1));
        b.fill = static_cast<std::uint8_t>(index.number(1));
        b.decoded = static_cast<std::uint32_t>(index.number(4));
        const auto relative = index.number(8);
        b.encoded = static_cast<std::uint32_t>(index.number(4));
        admit_codec(b.kind, info.version);
        if (!b.decoded || b.decoded > kMaxFlzipBlockBytes || b.decoded > logical - decoded_cursor)
          bad("FLZIP invalid decoded block length");
        b.logical = decoded_cursor;
        decoded_cursor += b.decoded;
        if (b.kind == 1) {
          if (b.encoded) bad("FLZIP fill block has payload");
          // Upstream intentionally ignores fill encoded_offset and non-fill fill_value.
        } else {
          if (relative != payload_cursor || b.encoded > payload_size - payload_cursor)
            bad("FLZIP block payload is sparse, overlapping or out of range");
          if (b.kind == 0 && b.encoded != b.decoded) bad("FLZIP raw length mismatch");
          if (b.kind != 0 && (!b.encoded || b.encoded >= b.decoded)) bad("FLZIP compressed length mismatch");
          if (b.kind == 3 && (b.encoded < 2 || b.encoded > (info.version < 4 ? 256U : 16384U)))
            bad("FLZIP pattern exceeds version bound");
          b.absolute = payload_start + relative;
          payload_cursor += b.encoded;
        }
        node.blocks.push_back(b);
      }
      if (decoded_cursor != logical || payload_cursor != payload_size) bad("FLZIP entry totals mismatch");
      const auto existing = nodes.find(key);
      if (existing != nodes.end()) {
        if (!directory || !existing->second.entry.directory) bad("FLZIP file/directory conflict");
        existing->second = std::move(node); // Explicit directory may follow its children.
      } else nodes.emplace(key, std::move(node));
      if (nodes.size() > 257) limit("FLZIP virtual node count exceeds 257");
    }
    if (index.remaining()) bad("FLZIP index contains trailing bytes");
    std::sort(intervals.begin(), intervals.end());
    for (std::size_t i = 1; i < intervals.size(); ++i)
      if (intervals[i].first < intervals[i-1].second) bad("FLZIP entry payload windows overlap");
    info.archive_bytes = snapshot.size();
    verify(); // Before any caller can observe names, ranges or decoded data.
  }
  const Node& lookup(std::string_view path, bool root = true) const {
    const auto it = nodes.find(canonical_catalog_path(path, root));
    if (it == nodes.end()) throw CatalogError(CatalogErrorCode::not_found, "FLZIP path not found");
    return it->second;
  }
  Bytes decode(const Block& b) const {
    const auto source = std::span<const std::byte>(snapshot).subspan(static_cast<std::size_t>(b.absolute), b.encoded);
    Bytes out(b.decoded);
    if (b.kind == 0) std::memcpy(out.data(), source.data(), out.size());
    else if (b.kind == 1) std::fill(out.begin(), out.end(), static_cast<std::byte>(b.fill));
    else if (b.kind == 3) {
      for (std::size_t i = 0; i < out.size(); ++i) out[i] = source[i % source.size()];
    }
#ifdef FLITZFS_HAS_ZLIB
    else if (b.kind == 2) {
      uLongf decoded = b.decoded; uLong consumed = b.encoded;
      const auto status = uncompress2(reinterpret_cast<Bytef*>(out.data()), &decoded,
          reinterpret_cast<const Bytef*>(source.data()), &consumed);
      if (status != Z_OK || decoded != b.decoded || consumed != b.encoded)
        corrupt("FLZIP Deflate requires one exact zlib stream");
    }
#endif
#ifdef FLITZFS_HAS_ZSTD
    else if (b.kind == 8) {
      if (source.size() < 4 || source[0] != std::byte{0x28} || source[1] != std::byte{0xb5} ||
          source[2] != std::byte{0x2f} || source[3] != std::byte{0xfd} ||
          ZSTD_getFrameContentSize(source.data(), source.size()) != b.decoded ||
          ZSTD_getDictID_fromFrame(source.data(), source.size()) != 0)
        corrupt("FLZIP Zstandard frame header mismatch");
      const auto frame_size = ZSTD_findFrameCompressedSize(source.data(), source.size());
      if (ZSTD_isError(frame_size) || frame_size != source.size()) corrupt("FLZIP Zstandard frame boundary mismatch");
      const auto decoded = ZSTD_decompress(out.data(), out.size(), source.data(), source.size());
      if (ZSTD_isError(decoded) || decoded != out.size()) corrupt("FLZIP Zstandard decoded length mismatch");
    }
#endif
    else throw FlzipError(FlzipErrorCode::unsupported_codec, "unavailable FLZIP decoder");
    return out;
  }
  void verify() {
    cached_block = nullptr; Bytes{}.swap(cached);
    for (const auto& [key, node] : nodes) {
      (void)key;
      if (node.entry.directory) continue;
      std::uint32_t crc = 0;
      for (const auto& b : node.blocks) crc = update_crc(crc, decode(b));
      if (crc != node.crc) corrupt("FLZIP decoded file CRC mismatch");
    }
  }
};

FlzipReader::FlzipReader(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
FlzipReader::~FlzipReader() = default;
FlzipReader::FlzipReader(FlzipReader&&) noexcept = default;
FlzipReader& FlzipReader::operator=(FlzipReader&&) noexcept = default;
FlzipReader FlzipReader::open(const std::filesystem::path& input, std::uint64_t budget) {
  auto file = ReadOnlyFile::open(input);
  if (file.size() > kMaxFlzipSnapshotBytes) limit("FLZIP file exceeds snapshot budget");
  admit(static_cast<std::size_t>(file.size()), budget);
  Bytes bytes(static_cast<std::size_t>(file.size()));
  file.read_exact_at(0, bytes.data(), bytes.size());
  return FlzipReader(std::make_unique<Impl>(std::move(bytes), budget));
}
FlzipReader FlzipReader::from_bytes(std::span<const std::byte> input, std::uint64_t budget) {
  admit(input.size(), budget);
  return FlzipReader(std::make_unique<Impl>(Bytes(input.begin(), input.end()), budget));
}
const FlzipInfo& FlzipReader::info() const noexcept { return impl_->info; }
CatalogEntry FlzipReader::stat(std::string_view path) const { return impl_->lookup(path).entry; }
std::vector<CatalogEntry> FlzipReader::list(std::string_view path) const {
  const auto& parent = impl_->lookup(path);
  if (!parent.entry.directory) throw CatalogError(CatalogErrorCode::not_directory, "FLZIP path is not a directory");
  const auto key = canonical_catalog_path(path, true);
  std::vector<CatalogEntry> result;
  for (const auto& [name, node] : impl_->nodes) {
    if (name.empty()) continue;
    const auto slash = name.rfind('/');
    if ((slash == name.npos ? std::string{} : name.substr(0, slash)) == key) result.push_back(node.entry);
  }
  return result;
}
std::vector<std::byte> FlzipReader::read(std::string_view path, std::uint64_t offset, std::uint32_t length) {
  const auto& node = impl_->lookup(path, false);
  if (node.entry.directory) throw CatalogError(CatalogErrorCode::is_directory, "FLZIP cannot read directory bytes");
  if (length > kMaxSingleReadBytes || offset > node.entry.logical_bytes || length > node.entry.logical_bytes - offset)
    throw CatalogError(CatalogErrorCode::limit, "FLZIP read range exceeds bounds");
  Bytes result(length);
  if (!length) return result;
  const auto end = offset + length;
  for (const auto& b : node.blocks) {
    if (b.logical >= end) break;
    if (b.logical + b.decoded <= offset) continue;
    if (impl_->cached_block != &b) {
      impl_->cached_block = nullptr; Bytes{}.swap(impl_->cached);
      impl_->cached = impl_->decode(b);
      impl_->cached_block = &b;
    }
    const auto begin = (std::max)(offset, b.logical);
    const auto stop = (std::min)(end, b.logical + b.decoded);
    std::memcpy(result.data() + static_cast<std::size_t>(begin - offset),
        impl_->cached.data() + static_cast<std::size_t>(begin - b.logical), static_cast<std::size_t>(stop - begin));
  }
  return result;
}
void FlzipReader::verify_all() { impl_->verify(); }
} // namespace flitzfs
