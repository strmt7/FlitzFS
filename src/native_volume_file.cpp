#include "flitzfs/native_volume_file.hpp"
#include "store_file.hpp"
#include <algorithm>
#include <limits>
#include <utility>

namespace flitzfs {
namespace {
constexpr auto image_offset=kNativeVolumeFileImageOffset;
constexpr auto maximum_file_bytes=static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
static_assert(image_offset%kStoreCommitUnit==0);
using Error=NativeFsErrorCode;
[[noreturn]] void fail(Error code,const char* message){throw NativeFsError(code,message);}
void check_image_capacity(std::uint64_t bytes,Error code) {
  if(bytes<kVolumeHeadBytes || bytes>maximum_file_bytes-image_offset)
    fail(code,"invalid indexed volume-file capacity");
}
}
struct NativeVolumeFile::Owner {
  std::shared_ptr<StoreByteIo> backing;
  const bool writable;
  explicit Owner(std::shared_ptr<StoreByteIo> file,bool write):backing(std::move(file)),writable(write) {
    if(!backing)fail(Error::invalid_operation,"missing volume-file backing");
    if(writable && (!valid_store_io_contract(backing->contract()) || !backing->acquire_writer()))
      fail(Error::invalid_operation,"volume file requires an exclusive qualified writer");
  }
  ~Owner(){if(writable)backing->release_writer();}
};

struct NativeVolumeFile::Region final:StoreByteIo {
  const std::shared_ptr<Owner> owner;
  const std::uint64_t offset,capacity;
  const bool tail;
  std::uint64_t fixed_extent;

  Region(std::shared_ptr<Owner> file,std::uint64_t base,std::uint64_t limit,
      std::uint64_t initialized,bool follows_end=false)
      :owner(std::move(file)),offset(base),capacity(limit),tail(follows_end),fixed_extent(initialized){}
  std::uint64_t extent() const override {
    if(!tail)return fixed_extent;
    const auto end=owner->backing->extent();
    if(end>offset+capacity)fail(Error::corrupt,"volume file exceeds its image extent limit");
    return end<=offset?0:end-offset;
  }
  void read_exact(std::uint64_t at,std::span<std::byte> out) const override {
    const auto end=extent();
    if(at>end || out.size()>end-at)fail(Error::corrupt,"volume-file read exceeds its region");
    if(!out.empty())owner->backing->read_exact(offset+at,out);
  }
  void write_exact(std::uint64_t at,std::span<const std::byte> bytes) override {
    if(!owner->writable)fail(Error::invalid_operation,"read-only volume file");
    if(at>capacity || bytes.size()>capacity-at)fail(Error::limit,"volume-file write exceeds its region");
    if(bytes.empty())return;
    owner->backing->write_exact(offset+at,bytes);
    if(!tail)fixed_extent=(std::max)(fixed_extent,at+static_cast<std::uint64_t>(bytes.size()));
  }
  void admit_write(std::uint64_t projected) override {
    if(!owner->writable)fail(Error::invalid_operation,"read-only volume file");
    if(projected<extent() || projected>capacity)fail(Error::limit,"volume-file admission exceeds its region");
    owner->backing->admit_write((std::max)(owner->backing->extent(),offset+projected));
  }
  void flush() override {
    if(!owner->writable)fail(Error::invalid_operation,"read-only volume file");
    owner->backing->flush();
  }
  StoreIoContract contract() const noexcept override {return owner->backing->contract();}
 protected:
  bool allows_writer() const noexcept override {return owner->writable;}
};

NativeVolumeImageResolver NativeVolumeFile::Regions::resolver() const {
  return [retained=image,expected=selection.image_identity](const StoreIdentity& requested) {
    return requested==expected?retained:nullptr;
  };
}
std::uint64_t NativeVolumeFile::Regions::advice_sequence() const {
  const auto observed=read_optimization_state(*advice,selection.volume_identity);
  return observed.code==OptimizationReadCode::available && observed.state.settled_input_bytes?observed.sequence:0;
}
NativeVolumeFile::Regions NativeVolumeFile::new_regions(std::shared_ptr<StoreByteIo> file,PagedStoreLimits limits) {
  check_image_capacity(limits.maximum_physical_bytes,Error::invalid_operation);
  auto owner=std::make_shared<Owner>(std::move(file),true);
  if(owner->backing->extent())fail(Error::invalid_operation,"volume-file creation requires empty backing");
  return {std::make_shared<Region>(owner,0,kVolumeHeadBytes,0),
          std::make_shared<Region>(owner,image_offset,limits.maximum_physical_bytes,0,true),
          std::make_shared<Region>(owner,kVolumeHeadBytes,kOptimizationAreaBytes,0),{}};
}
NativeVolumeFile::Regions NativeVolumeFile::open_regions(std::shared_ptr<StoreByteIo> file,bool writable,
    std::optional<StoreIdentity> expected) {
  auto owner=std::make_shared<Owner>(std::move(file),writable);
  const auto end=owner->backing->extent();
  if(end<image_offset+kVolumeHeadBytes || end>maximum_file_bytes)
    fail(Error::corrupt,"truncated or oversized volume file");
  auto head=std::make_shared<Region>(owner,0,kVolumeHeadBytes,kVolumeHeadBytes);
  const auto selected=NativeVolumeHeadView::inspect_selector(*head,expected).selection;
  if(selected.storage.kind!=NativeImageKind::indexed_paged || selected.epoch!=1)
    fail(Error::corrupt,"head does not select an indexed volume-file image");
  check_image_capacity(selected.storage.maximum_physical_bytes,Error::corrupt);
  if(end-image_offset>selected.storage.maximum_physical_bytes)
    fail(Error::corrupt,"volume file exceeds its declared image capacity");
  return {std::move(head),
          std::make_shared<Region>(owner,image_offset,selected.storage.maximum_physical_bytes,0,true),
          std::make_shared<Region>(owner,kVolumeHeadBytes,kOptimizationAreaBytes,kOptimizationAreaBytes),selected};
}
NativeFileSession NativeVolumeFile::create(std::shared_ptr<StoreByteIo> empty,StoreIdentity volume,
    StoreIdentity image,PagedStoreLimits limits,StoreCompressionOptions foreground,
    NativeDispositionPolicy disposition,std::optional<NativeVolumeRootMetadata> root) {
  auto region=new_regions(std::move(empty),limits);
  auto session=NativeFileSession::create_indexed_volume(std::move(region.head),volume,std::move(region.image),
      image,limits,foreground,disposition,std::move(root));
  session.create_budgeted_optimization_area(std::move(region.advice));
  return session;
}
NativeFileSession NativeVolumeFile::resume(std::shared_ptr<StoreByteIo> existing,
    std::optional<StoreIdentity> expected,StoreCompressionOptions foreground,NativeDispositionPolicy disposition) {
  auto region=open_regions(std::move(existing),true,expected);
  const auto& selected=region.selection;
  auto session=NativeFileSession::resume_closed_indexed_volume(std::move(region.head),selected.volume_identity,
      selected.epoch,region.resolver(),foreground,disposition);
  if(const auto sequence=region.advice_sequence())
    session.resume_budgeted_optimization_area(std::move(region.advice),sequence);
  return session;
}
NativeVolumeHeadView NativeVolumeFile::inspect(std::shared_ptr<StoreByteIo> existing,
    std::optional<StoreIdentity> expected) {
  auto region=open_regions(std::move(existing),false,expected);
  return NativeVolumeHeadView::open_indexed_recovery(std::move(region.head),region.selection.volume_identity,region.resolver());
}
#ifdef _WIN32
std::shared_ptr<StoreByteIo> NativeVolumeFile::create_backing(const std::filesystem::path& path,StoreIoContract contract,
    PagedStoreLimits limits) {
  check_image_capacity(limits.maximum_physical_bytes,Error::invalid_operation);
  return detail::new_volume_file(path,contract,image_offset+limits.maximum_physical_bytes);
}
std::shared_ptr<StoreByteIo> NativeVolumeFile::reopen_backing(const std::filesystem::path& path,StoreIoContract contract) {
  return detail::reopen_volume_file(path,contract,maximum_file_bytes);
}
NativeFileSession NativeVolumeFile::create(const std::filesystem::path& path,StoreIoContract contract,StoreIdentity volume,
    StoreIdentity image,PagedStoreLimits limits,StoreCompressionOptions foreground,
    NativeDispositionPolicy disposition,std::optional<NativeVolumeRootMetadata> root) {
  return create(create_backing(path,contract,limits),volume,image,limits,foreground,disposition,std::move(root));
}
NativeFileSession NativeVolumeFile::resume(const std::filesystem::path& path,StoreIoContract contract,
    std::optional<StoreIdentity> expected,StoreCompressionOptions foreground,NativeDispositionPolicy disposition) {
  return resume(reopen_backing(path,contract),expected,foreground,disposition);
}
NativeVolumeHeadView NativeVolumeFile::inspect(const std::filesystem::path& path,std::optional<StoreIdentity> expected) {
  return inspect(detail::read_store_file(path,maximum_file_bytes),expected);
}
#endif
} // namespace flitzfs
