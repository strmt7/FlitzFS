#pragma once
#include "flitzfs/optimization_state.hpp"
#include "flitzfs/paged_store.hpp"

namespace flitzfs {
namespace detail {struct NativeMaintenanceSource;}
inline constexpr std::uint32_t kCoherenceMaskedBand=kOptimizationGroupBands;
inline constexpr std::uint32_t kCoherenceModularBand=kCoherenceMaskedBand+kOptimizationXorProjections;
inline constexpr std::uint32_t kCoherenceModularWidths=4;
inline constexpr std::uint32_t kCoherenceBandCount=kCoherenceModularBand+kCoherenceModularWidths;

// A nomination only. The publisher supplies a verified feature sample.
// A consumer rechecks the selected extent binding against its live volume.
struct IndexedCoherencePosting {
  std::uint32_t band=0;
  OptimizationGroupFeature representative;
  // Masked byte and modular word bands carry one full key.
  std::uint64_t projected_fingerprint=0;
};

// Independent advisory backing for one native image. The volume ID binds
// the paged store and the typed root binds the contributing image ID.
class IndexedCoherenceSnapshot final {
 public:
  static IndexedCoherenceSnapshot open(std::shared_ptr<StoreByteIo>,
      StoreIdentity volume,StoreIdentity image,PagedStoreLimits);
  std::optional<OptimizationGroupFeature> find(std::uint32_t band,std::uint64_t fingerprint) const;
  bool has_projected_keys() const noexcept {return schema_>=2;}
  bool has_modular_keys() const noexcept {return schema_>=3;}
  PagedStoreInfo info() const noexcept {return store_.info();}
  StoreIdentity volume() const noexcept {return store_.identity();}
  StoreIdentity image() const noexcept {return image_;}

 private:
  friend class IndexedCoherenceWriter;
  IndexedCoherenceSnapshot(PagedObjectStoreSnapshot,StoreIdentity volume,StoreIdentity image);
  PagedObjectStoreSnapshot store_;
  StoreIdentity image_;
  std::uint8_t schema_=1;
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
  static IndexedCoherenceWriter create_reserved(std::shared_ptr<StoreByteIo>,StoreIdentity volume,
      StoreIdentity image,PagedStoreLimits,std::span<const IndexedCoherencePosting>,
      std::uint64_t maximum_write_bytes,const std::function<bool(std::uint64_t)>& reserve);
  PagedStoreWriteResult publish_reserved(std::span<const IndexedCoherencePosting>,
      std::uint64_t maximum_write_bytes,const std::function<bool(std::uint64_t)>& reserve);
  // Prepare a filtered copy on separate backing. The original stays selected
  // through complete staged verification and the funded write reservation.
  PagedObjectStoreWriter::Rewrite prepare_compaction(std::shared_ptr<StoreByteIo>,
      std::function<bool(const OptimizationGroupFeature&)>,
      std::function<bool(std::uint64_t)>) const;
  bool advance_compaction(PagedObjectStoreWriter::Rewrite& work) const {return work.advance(store_);}
  IndexedCoherenceWriter finish_compaction(PagedObjectStoreWriter::Rewrite& work) const {
    return IndexedCoherenceWriter(work.finish(store_),volume_,image_);
  }
  IndexedCoherenceWriter(PagedObjectStoreWriter,StoreIdentity volume,StoreIdentity image);
  PagedObjectStoreWriter store_;
  StoreIdentity volume_,image_;
};

// Trusted internal storage reservation and retained writer lease. The callback
// supplies only a newly reserved catalog backend during a funded idle scan.
// Attach one serialized session at a time. Keep this owner across an in-process
// remount to reuse the exact writer without a whole-catalog verification pass.
class IndexedCoherenceStorage final {
 public:
  using Reserve=std::function<std::shared_ptr<StoreByteIo>()>;
  IndexedCoherenceStorage(PagedStoreLimits,Reserve);
  IndexedCoherenceStorage(const IndexedCoherenceStorage&)=delete;
  IndexedCoherenceStorage& operator=(const IndexedCoherenceStorage&)=delete;

 private:
  friend class NativeFileSession;
  friend struct detail::NativeMaintenanceSource;
  PagedStoreLimits limits_;
  Reserve reserve_;
  std::shared_ptr<IndexedCoherenceWriter> retained_;
  std::weak_ptr<const void> session_;
};
}
