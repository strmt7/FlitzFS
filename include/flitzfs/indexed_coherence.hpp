#pragma once
#include "flitzfs/optimization_state.hpp"
#include "flitzfs/paged_store.hpp"

namespace flitzfs {
// A nomination only. The publisher supplies a verified feature sample.
// A consumer rechecks the selected extent binding against its live volume.
struct IndexedCoherencePosting {
  std::uint32_t band=0;
  OptimizationGroupFeature representative;
};

// Independent advisory backing for one native image. The volume ID binds
// the paged store and the typed root binds the contributing image ID.
class IndexedCoherenceSnapshot final {
 public:
  static IndexedCoherenceSnapshot open(std::shared_ptr<StoreByteIo>,
      StoreIdentity volume,StoreIdentity image,PagedStoreLimits);
  std::optional<OptimizationGroupFeature> find(std::uint32_t band,std::uint64_t fingerprint) const;
  PagedStoreInfo info() const noexcept {return store_.info();}
  StoreIdentity volume() const noexcept {return store_.identity();}
  StoreIdentity image() const noexcept {return image_;}

 private:
  friend class IndexedCoherenceWriter;
  IndexedCoherenceSnapshot(PagedObjectStoreSnapshot,StoreIdentity volume,StoreIdentity image);
  PagedObjectStoreSnapshot store_;
  StoreIdentity image_;
};

class IndexedCoherenceWriter final {
 public:
  // Creation requires a new backing reserved exclusively for this catalog.
  static IndexedCoherenceWriter create(std::shared_ptr<StoreByteIo>,
      StoreIdentity volume,StoreIdentity image,PagedStoreLimits);
  static IndexedCoherenceWriter resume(std::shared_ptr<StoreByteIo>,
      StoreIdentity volume,StoreIdentity image,std::uint64_t expected_generation,PagedStoreLimits);
  // A single lookup key retains the latest observed binding. Replaying older
  // samples or exact duplicates consumes no write budget.
  PagedStoreWriteResult publish(std::span<const IndexedCoherencePosting>,std::uint64_t maximum_write_bytes);
  IndexedCoherenceSnapshot snapshot() const;
  bool poisoned() const noexcept {return store_.poisoned();}

 private:
  friend class NativeFileSession;
  PagedStoreWriteResult publish_reserved(std::span<const IndexedCoherencePosting>,
      std::uint64_t maximum_write_bytes,const std::function<bool(std::uint64_t)>& reserve);
  IndexedCoherenceWriter(PagedObjectStoreWriter,StoreIdentity volume,StoreIdentity image);
  PagedObjectStoreWriter store_;
  StoreIdentity volume_,image_;
};
}
