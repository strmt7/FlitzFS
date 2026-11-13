#pragma once
#include "memory_volume.hpp"
#include "flitzfs/native_session.hpp"
#include <string>
#include <array>
namespace flitzfs {
// Serialized application owner. No mount/driver or GPU initialization.
// Call only from its controller thread; snapshots never escape that thread.
struct VolumeView {
  bool ready=false,fenced=false;
  std::uint64_t allocated=0,capacity=0,physical_bytes=0,logical_bytes=0,payload_bytes=0,epoch=0;
  NativeOptimizationInfo optimization;
  std::vector<NativeFileInfo> files;
};
class VolumeModel final {
 public:
  static constexpr std::size_t text_limit=16384;
  VolumeModel()=default;
  ~VolumeModel();
  void create();
  void create_text(std::string_view name,std::string_view utf8);
  std::string read_text(std::string_view name) const;
  enum class AdviceMaintenance { ready,rebuilt,unavailable };
  // Explicit serialized maintenance opportunity, not ordinary file I/O.
  // One real audit and at most one new-area attempt; never a retry loop.
  AdviceMaintenance maintain_optimization_state();
  NativeOptimizationInfo optimization_info() const noexcept;
  bool optimize();
  VolumeView view() const;
  // Explicit destruction of a temporary volume, never automatic on WM_CLOSE.
  void discard();
  bool ready() const noexcept {return session_!=nullptr;}
 private:
  // Test-binary friend only; no runtime fault command/provider substitution.
  friend struct VolumeModelQualification;
  // Destruction order: session, backing references, then working-set lease.
  std::unique_ptr<MemoryVolumeDomain> domain_;
  std::shared_ptr<StoreByteIo> head_,image_;
  std::array<std::shared_ptr<StoreByteIo>,NativeFileSession::max_optimization_bindings> advice_;
  // Fixed preflighted slot; shared-pointer adoption after core publication
  // must not allocate, query a backend or release its retained old owner.
  void adopt_attempt(unsigned,std::shared_ptr<StoreByteIo>&) noexcept;
  std::unique_ptr<NativeFileSession> session_;
};
} // namespace flitzfs
