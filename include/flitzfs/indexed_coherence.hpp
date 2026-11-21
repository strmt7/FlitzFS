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

struct IndexedCoherenceAlternate {
  std::uint64_t cursor=0;
  std::optional<OptimizationGroupFeature> representative;
};

// Independent advisory backing for one native image. Identified catalogs bind
// the paged store to a backing ID and the typed root to its volume and image.
class IndexedCoherenceSnapshot final {
 public:
  static IndexedCoherenceSnapshot open(std::shared_ptr<StoreByteIo>,
      StoreIdentity volume,StoreIdentity image,PagedStoreLimits,StoreIdentity backing={});
  std::optional<OptimizationGroupFeature> find(std::uint32_t band,std::uint64_t fingerprint) const;
  // A cursor is valid for the same snapshot, band and fingerprint. A hash
  // collision advances it without returning a nomination.
  std::optional<IndexedCoherenceAlternate> next_alternate(std::uint32_t band,
      std::uint64_t fingerprint,std::uint64_t after=0) const;
  bool has_projected_keys() const noexcept {return band_schema_>=2;}
  bool has_modular_keys() const noexcept {return band_schema_>=3;}
  bool has_alternates() const noexcept {return schema_>=4;}
  PagedStoreInfo info() const noexcept {return store_.info();}
  StoreIdentity volume() const noexcept {return volume_;}
  StoreIdentity image() const noexcept {return image_;}
  StoreIdentity backing_identity() const noexcept {return backing_;}

 private:
  friend class IndexedCoherenceWriter;
  IndexedCoherenceSnapshot(PagedObjectStoreSnapshot,StoreIdentity volume,StoreIdentity image,StoreIdentity backing);
  PagedObjectStoreSnapshot store_;
  StoreIdentity volume_,image_,backing_;
  std::uint8_t schema_=1,band_schema_=1;
};

class IndexedCoherenceWriter final {
 public:
  // Creation requires a new backing reserved exclusively for this catalog.
  static IndexedCoherenceWriter create(std::shared_ptr<StoreByteIo>,
      StoreIdentity volume,StoreIdentity image,PagedStoreLimits);
  static IndexedCoherenceWriter resume(std::shared_ptr<StoreByteIo>,
      StoreIdentity volume,StoreIdentity image,std::uint64_t expected_generation,PagedStoreLimits);
  // Keep the latest observed binding and fund distinct alternate extents
  // within the same publication. Older replays and exact duplicates cost zero.
  PagedStoreWriteResult publish(std::span<const IndexedCoherencePosting>,std::uint64_t maximum_write_bytes);
  IndexedCoherenceSnapshot snapshot() const;
  bool poisoned() const noexcept {return store_.poisoned();}

 private:
  friend class NativeFileSession;
  friend class IndexedCoherenceStorage;
  static IndexedCoherenceWriter create_reserved(std::shared_ptr<StoreByteIo>,StoreIdentity volume,
      StoreIdentity image,PagedStoreLimits,std::span<const IndexedCoherencePosting>,
      std::uint64_t maximum_write_bytes,const std::function<bool(std::uint64_t)>& reserve,
      StoreIdentity backing={});
  static IndexedCoherenceWriter resume_selected(const IndexedCoherenceSnapshot&);
  PagedStoreWriteResult publish_reserved(std::span<const IndexedCoherencePosting>,
      std::uint64_t maximum_write_bytes,const std::function<bool(std::uint64_t)>& reserve);
  // Prepare a filtered copy on separate backing. The original stays selected
  // through complete staged verification and the funded write reservation.
  PagedObjectStoreWriter::Rewrite prepare_compaction(std::shared_ptr<StoreByteIo>,
      std::function<bool(const OptimizationGroupFeature&)>,
      std::function<bool(std::uint64_t)>,StoreIdentity replacement_backing) const;
  bool advance_compaction(PagedObjectStoreWriter::Rewrite& work) const {return work.advance(store_);}
  IndexedCoherenceWriter finish_compaction(PagedObjectStoreWriter::Rewrite& work) const {
    const auto backing=backing_==StoreIdentity{}?StoreIdentity{}:work.identity();
    return IndexedCoherenceWriter(work.finish(store_),volume_,image_,backing);
  }
  IndexedCoherenceWriter(PagedObjectStoreWriter,StoreIdentity volume,StoreIdentity image,StoreIdentity backing={});
  PagedObjectStoreWriter store_;
  StoreIdentity volume_,image_,backing_;
};

// Trusted internal storage reservation and retained writer lease. The callback
// supplies only a newly reserved catalog backend during a funded idle scan.
// Attach one serialized session at a time. Keep this owner across an in-process
// remount to reuse the exact writer without a whole-catalog verification pass.
class IndexedCoherenceStorage final {
 public:
  using Reserve=std::function<std::shared_ptr<StoreByteIo>()>;
  IndexedCoherenceStorage(PagedStoreLimits,Reserve);
  // Adopt an independently reopened writer and a provider for future copies.
  IndexedCoherenceStorage(PagedStoreLimits,Reserve,IndexedCoherenceWriter recovered);
  struct Reservation {
    StoreIdentity identity{};
    std::shared_ptr<StoreByteIo> backing;
  };
  using ReserveIdentified=std::function<Reservation()>;
  using Resolve=std::function<std::shared_ptr<StoreByteIo>(StoreIdentity)>;
  // A fresh nonaliasing reservation with a never-reused ID. The provider keeps
  // the mapping across restart and preserves backings through uncertain
  // selection outcomes and until all readers release their pins.
  IndexedCoherenceStorage(PagedStoreLimits,ReserveIdentified,Resolve);
  IndexedCoherenceStorage(const IndexedCoherenceStorage&)=delete;
  IndexedCoherenceStorage& operator=(const IndexedCoherenceStorage&)=delete;

 private:
  friend class NativeFileSession;
  friend struct detail::NativeMaintenanceSource;
  PagedStoreLimits limits_;
  ReserveIdentified reserve_;
  Resolve resolve_;
  std::shared_ptr<IndexedCoherenceWriter> retained_;
  std::weak_ptr<const void> session_;
};
}
