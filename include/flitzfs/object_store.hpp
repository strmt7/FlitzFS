#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace flitzfs {
// Experimental immutable-object transaction core. No filesystem/GPU/driver routing.
using StoreIdentity = std::array<std::byte,16>;
inline constexpr std::uint64_t kStoreMaxPhysicalBytes=64ULL*1024*1024;
inline constexpr std::uint32_t kStoreMaxObjectBytes=1024*1024;
inline constexpr std::uint32_t kStoreMaxTransactionBytes=4*1024*1024;
inline constexpr std::size_t kStoreMaxObjects=16;
inline constexpr std::uint64_t kStoreCommitUnit=65536;

// These are externally qualified BACKEND ASSUMPTIONS, not hardware attestation.
// Zero/unknown is rejected before writing. A successful flush must persist prior
// writes; damage during a failed write must stay within its failure units.
// The file adapter cannot discover/guarantee a device's power-loss failure unit.
struct StoreIoContract {
  std::uint32_t write_failure_unit_bytes=0;
  bool successful_flush_persists=false;
};

// Positional exact I/O; implementations throw on any short/failed operation.
// One control thread; do not race calls through writers or snapshots.
class StoreByteIo {
 public:
  virtual ~StoreByteIo()=default;
  virtual std::uint64_t extent() const=0;
  virtual void read_exact(std::uint64_t offset,std::span<std::byte> output) const=0;
  virtual void write_exact(std::uint64_t offset,std::span<const std::byte> input)=0;
  virtual void flush()=0;
  // Read-only resource preflight; throws before writes on unknown/low headroom.
  virtual void admit_write(std::uint64_t projected_extent)=0;
  virtual StoreIoContract contract() const noexcept=0;
};

enum class StoreCompressionMode { raw,automatic };
struct StoreCompressionOptions {
  StoreCompressionMode mode=StoreCompressionMode::automatic;
  int zstd_level=6; // 1..9, bounded single-threaded tables; no GPU dependency.
  bool require_zstd=false; // Fail before creation if bounded codec support is absent.
};
bool store_zstd_available() noexcept;
std::uint32_t store_zstd_version() noexcept; // Runtime library version, zero if not compiled.

struct StoreObjectInfo {
  std::uint64_t id=0,generation=0;
  std::uint32_t logical_bytes=0,stored_payload_bytes=0;
};
struct StoreMutation {
  std::uint64_t id=0;
  bool erase=false;
  std::vector<std::byte> bytes;
};
struct StoreCommitInfo {
  std::uint64_t generation=0,physical_end=0,logical_bytes=0;
  std::size_t objects=0;
  bool degraded_root=false; // Only one valid root: report explicitly; no mounted routing.
  std::uint64_t stored_payload_bytes=0; // Active payload only; excludes metadata/padding/history.
};

class ObjectStoreSnapshot final {
 public:
  static ObjectStoreSnapshot open(std::shared_ptr<StoreByteIo> image);
  static ObjectStoreSnapshot open_file(const std::filesystem::path& path);
  ObjectStoreSnapshot(ObjectStoreSnapshot&&) noexcept;
  ObjectStoreSnapshot& operator=(ObjectStoreSnapshot&&) noexcept;
  ObjectStoreSnapshot(const ObjectStoreSnapshot&)=delete;
  ObjectStoreSnapshot& operator=(const ObjectStoreSnapshot&)=delete;
  ~ObjectStoreSnapshot();
  StoreIdentity identity() const noexcept;
  StoreCommitInfo info() const noexcept;
  std::vector<StoreObjectInfo> list() const;
  // Complete object, at most 1 MiB. Header/identity/content verified every time.
  std::vector<std::byte> read(std::uint64_t id) const;
  void verify_all() const;
 private:
  friend class ObjectStoreWriter;
  struct Impl;
  explicit ObjectStoreSnapshot(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};

class ObjectStoreWriter final {
 public:
  // Only an empty, exclusively owned backend. There is no reopen-existing-write API.
  static ObjectStoreWriter create(std::shared_ptr<StoreByteIo> empty_image,StoreIdentity id,
      StoreCompressionOptions compression={});
  // New ordinary file only; never replaces a path or deletes a partial new file.
  // Contract is explicit: this API does NOT qualify a physical device.
  static ObjectStoreWriter create_file(const std::filesystem::path& path,
      StoreIdentity id,StoreIoContract contract,StoreCompressionOptions compression={});
  ObjectStoreWriter(ObjectStoreWriter&&) noexcept;
  ObjectStoreWriter& operator=(ObjectStoreWriter&&) noexcept;
  ObjectStoreWriter(const ObjectStoreWriter&)=delete;
  ObjectStoreWriter& operator=(const ObjectStoreWriter&)=delete;
  ~ObjectStoreWriter();
  // Atomic mutation batch: unique nonzero IDs, <=16 ops / 4 MiB new bytes.
  // Argument/budget errors occur before writes. Any I/O failure terminally
  // poisons the writer. A successful result means backend flush completed;
  // physical durability remains conditional on the declared backend contract.
  StoreCommitInfo commit(std::span<const StoreMutation> mutations);
  ObjectStoreSnapshot snapshot() const;
  bool poisoned() const noexcept;
 private:
  struct Impl;
  explicit ObjectStoreWriter(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};
} // namespace flitzfs
