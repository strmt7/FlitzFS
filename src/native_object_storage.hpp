#pragma once
#include "flitzfs/native_filesystem.hpp"
#include "flitzfs/paged_store.hpp"
#include <variant>

namespace flitzfs::detail {
// Internal dispatch only: native mutation/session logic and format validation
// remain shared. No backend discovery, mount, worker or I/O retry lives here.
inline StoreCommitInfo native_paged_info(PagedStoreInfo value) noexcept {
  StoreCommitInfo result;
  result.generation=value.generation;result.physical_end=value.physical_end;
  result.logical_bytes=value.logical_bytes;result.objects=static_cast<std::size_t>(value.objects);
  result.degraded_root=value.degraded_roots;result.stored_payload_bytes=value.stored_payload_bytes;
  result.checkpoint_generation=value.checkpoint_generation;result.replicated_checkpoint=true;
  result.degraded_checkpoint=value.degraded_roots || value.degraded_metadata || value.degraded_payloads;
  return result;
}
class NativeObjectSnapshot final {
 public:
  NativeObjectSnapshot(ObjectStoreSnapshot value):value_(std::move(value)){}
  NativeObjectSnapshot(PagedObjectStoreSnapshot value):value_(std::move(value)){}
  StoreIdentity identity() const noexcept {
    return std::visit([](const auto& value){return value.identity();},value_);
  }
  bool paged() const noexcept {return std::holds_alternative<PagedObjectStoreSnapshot>(value_);}
  bool mirrored() const noexcept {const auto* value=paged_if();return value && value->info().mirrored_payloads;}
  std::optional<StoreObjectInfo> stat(std::uint64_t id) const {
    if(const auto* value=paged_if())return value->stat(id);
    for(const auto& object:std::get<ObjectStoreSnapshot>(value_).list())if(object.id==id)return object;
    return std::nullopt;
  }
  const PagedObjectStoreSnapshot* paged_if() const noexcept {return std::get_if<PagedObjectStoreSnapshot>(&value_);}
  StoreCommitInfo info() const noexcept {
    if(const auto* flat=std::get_if<ObjectStoreSnapshot>(&value_))return flat->info();
    return native_paged_info(std::get<PagedObjectStoreSnapshot>(value_).info());
  }
  std::vector<StoreObjectInfo> list(std::size_t maximum=kStoreMaxObjects) const {
    if(const auto* flat=std::get_if<ObjectStoreSnapshot>(&value_))return flat->list();
    const auto& paged=std::get<PagedObjectStoreSnapshot>(value_);
    if(maximum>4*kStoreMaxObjects || paged.info().objects>maximum)
      throw NativeFsError(NativeFsErrorCode::limit,"paged inventory exceeds current native namespace bounds");
    return paged.list_page(0,maximum);
  }
  std::vector<std::byte> read(std::uint64_t id) const {
    return std::visit([&](const auto& value){return value.read(id);},value_);
  }
  void verify_all() const {std::visit([](const auto& value){value.verify_all();},value_);}
  const ObjectStoreSnapshot& flat() const {
    const auto* result=std::get_if<ObjectStoreSnapshot>(&value_);
    if(!result)throw NativeFsError(NativeFsErrorCode::invalid_operation,
        "this volume-head/recompression operation requires the flat storage backend");
    return *result;
  }
 private:
  std::variant<ObjectStoreSnapshot,PagedObjectStoreSnapshot> value_;
};
class NativeObjectWriter final {
 public:
  static PagedObjectStoreWriter rebuild(const PagedObjectStoreSnapshot& source,
      std::shared_ptr<StoreByteIo> target,StoreIdentity id,std::span<const std::byte> root,
      StoreCompressionOptions compression,bool reencode) {
    return PagedObjectStoreWriter::rebuild_replacing(source,std::move(target),id,source.limits(),
        compression,1,root,reencode);
  }
  NativeObjectWriter(ObjectStoreWriter value):value_(std::move(value)){}
  NativeObjectWriter(PagedObjectStoreWriter value):value_(std::move(value)){}
  bool paged() const noexcept {return std::holds_alternative<PagedObjectStoreWriter>(value_);}
  PagedObjectStoreWriter* paged_if() noexcept {return std::get_if<PagedObjectStoreWriter>(&value_);}
  const PagedObjectStoreWriter* paged_if() const noexcept {return std::get_if<PagedObjectStoreWriter>(&value_);}
  PagedObjectStoreWriter::Rewrite prepare_paged_rebuild(std::shared_ptr<StoreByteIo> target,
      StoreIdentity id,std::span<const std::byte> root,StoreCompressionOptions compression,bool reencode) const {
    const auto* source=paged_if();
    if(!source)throw NativeFsError(NativeFsErrorCode::invalid_operation,"staged paged rebuild needs paged storage");
    return source->prepare_rebuild_replacing(std::move(target),id,compression,root,reencode);
  }
  const ObjectStoreWriter* flat_if() const noexcept {return std::get_if<ObjectStoreWriter>(&value_);}
  bool poisoned() const noexcept {
    return std::visit([](const auto& value){return value.poisoned();},value_);
  }
  NativeObjectSnapshot snapshot() const {
    return std::visit([](const auto& value)->NativeObjectSnapshot{return value.snapshot();},value_);
  }
  NativeObjectSnapshot snapshot_for_staging(bool query_cache=false) const {
    if(const auto* flat=flat_if())return flat->snapshot_for_staging();
    return std::get<PagedObjectStoreWriter>(value_).snapshot_for_staging(query_cache);
  }
  std::uint64_t generation_for_staging() const noexcept {
    return std::visit([](const auto& value){return value.generation_for_staging();},value_);
  }
  std::uint64_t physical_end_for_staging() const noexcept {
    const auto* paged=paged_if();return paged?paged->physical_end_for_staging():0;
  }
  StoreCommitInfo commit_replicated(std::span<const StoreMutation> changes,bool directory_indexes=false,bool shared_bases=false) {
    if(auto* flat=std::get_if<ObjectStoreWriter>(&value_))return flat->commit_replicated(changes);
    return native_paged_info(std::get<PagedObjectStoreWriter>(value_).commit_native(changes,directory_indexes,shared_bases));
  }
  StoreCommitInfo flush_checkpoint() {
    if(auto* flat=std::get_if<ObjectStoreWriter>(&value_))return flat->flush_checkpoint();
    return native_paged_info(std::get<PagedObjectStoreWriter>(value_).flush_checkpoint());
  }
  ObjectStoreWriter::Rewrite prepare_recompression(std::shared_ptr<StoreByteIo> target,
      StoreIdentity id,std::uint64_t replacement_id,std::span<const std::byte> replacement,
      StoreCompressionOptions compression,StoreCheckpointOptions checkpoints) const {
    const auto* flat=std::get_if<ObjectStoreWriter>(&value_);
    if(!flat)throw NativeFsError(NativeFsErrorCode::invalid_operation,
        "staged native recompression requires the flat storage backend");
    return flat->prepare_recompression(std::move(target),id,replacement_id,replacement,compression,checkpoints);
  }
 private:
  std::variant<ObjectStoreWriter,PagedObjectStoreWriter> value_;
};
} // namespace flitzfs::detail
