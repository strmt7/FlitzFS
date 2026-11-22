#include "flitzfs/native_hybrid_backing.hpp"
#include "folded_checkpoint.hpp"
#include "protected_stripe_image.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace flitzfs {
namespace {
using detail::Digest;
using detail::ProtectedStripeImage;
using Bytes=std::vector<std::byte>;
constexpr std::uint64_t roots=detail::kFoldedMirrorRoots,tail_start=2*roots;
constexpr std::size_t root_size=detail::kPagedStoreCheckpointWriteBytes/2;
constexpr std::uint64_t maximum_io=static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
constexpr std::string_view binding_magic="FLTXHB01";
using BindingRecord=std::array<std::byte,128>;

[[noreturn]] void fail(const char* reason) {throw std::runtime_error(reason);}

struct Binding {
  std::uint64_t unit=0,base_end=0,base_occupied=0;
  StoreIdentity identity{};
  Digest frozen_roots{};
  std::uint64_t virtual_limit=0;
  bool operator==(const Binding&) const=default;

  std::uint64_t folded_extent() const noexcept {return detail::folded_mirror_extent(base_end);}
};

std::uint64_t get64(std::span<const std::byte> bytes,std::size_t offset) noexcept {
  std::uint64_t value=0;
  for(unsigned i=0;i<8;++i)value|=std::to_integer<std::uint64_t>(bytes[offset+i])<<(8*i);
  return value;
}
void put64(std::span<std::byte> bytes,std::size_t offset,std::uint64_t value) noexcept {
  for(unsigned i=0;i<8;++i)bytes[offset+i]=static_cast<std::byte>((value>>(8*i))&255);
}
template<std::size_t N>std::array<std::byte,N> field(std::span<const std::byte> bytes,std::size_t offset) noexcept {
  std::array<std::byte,N> result{};
  std::copy_n(bytes.begin()+offset,N,result.begin());
  return result;
}
BindingRecord encode(const Binding& binding) noexcept {
  BindingRecord bytes{};
  std::ranges::copy(std::as_bytes(std::span(binding_magic)),bytes.begin());
  put64(bytes,8,binding.unit);
  put64(bytes,16,binding.base_end);
  put64(bytes,24,binding.base_occupied);
  std::ranges::copy(binding.identity,bytes.begin()+32);
  std::ranges::copy(binding.frozen_roots,bytes.begin()+48);
  put64(bytes,80,binding.virtual_limit);
  std::ranges::copy(detail::record_digest(std::span(bytes).first(96)),bytes.begin()+96);
  return bytes;
}
std::optional<Binding> read_binding(const StoreByteIo& tail,std::uint64_t offset) {
  BindingRecord bytes{};
  try {tail.read_exact(offset,bytes);}
  catch(const std::bad_alloc&){throw;}
  catch(const std::exception&){return {};}
  const auto front=std::span(bytes).first(96);
  if(detail::record_digest(front)!=field<32>(bytes,96))return {};
  if(!std::ranges::equal(front.first(8),std::as_bytes(std::span(binding_magic))) || get64(bytes,88))
    fail("unsupported intact hybrid backing binding");
  return Binding{get64(bytes,8),get64(bytes,16),get64(bytes,24),field<16>(bytes,32),
      field<32>(bytes,48),get64(bytes,80)};
}
std::pair<Binding,bool> load_binding(const StoreByteIo& tail) {
  const auto first=read_binding(tail,0);
  const auto second=read_binding(tail,kStoreCommitUnit);
  if(!first && !second)fail("missing hybrid backing binding");
  if(first && second && *first!=*second)fail("conflicting hybrid backing bindings");
  return {first?*first:*second,!(first && second)};
}
void check_limits(StoreIdentity identity,std::uint64_t virtual_limit,std::uint64_t physical_limit) {
  if(identity==StoreIdentity{} || virtual_limit<=roots || virtual_limit>maximum_io ||
      physical_limit<tail_start || physical_limit>maximum_io)
    fail("invalid hybrid backing limits");
}
void check_geometry(const Binding& binding) {
  const auto unit=binding.unit;
  if(unit<root_size || unit>kStoreCommitUnit || !std::has_single_bit(unit) ||
      binding.base_end<roots+2*unit || binding.base_end>binding.virtual_limit ||
      (binding.base_end-roots)%(2*unit))
    fail("invalid hybrid base geometry");
}
Digest root_binding(const StoreByteIo& source) {
  std::array<std::byte,root_size> first{},second{};
  source.read_exact(0,first);
  source.read_exact(kStoreCommitUnit,second);
  return detail::record_digest(first,second);
}

} // namespace

struct NativeHybridBacking::Impl {
  struct Lease {
    Lease(std::shared_ptr<StoreByteIo> backing,bool writer):io(std::move(backing)) {
      if(!io || !(writer?io->acquire_writer():io->acquire_exclusive_use()))
        fail("hybrid backing already owned or unavailable");
    }
    ~Lease(){io->release_writer();}
    Lease(const Lease&)=delete;
    Lease& operator=(const Lease&)=delete;
    std::shared_ptr<StoreByteIo> io;
  };

  Impl(std::shared_ptr<StoreByteIo> base_io,std::shared_ptr<StoreByteIo> tail_io,std::uint64_t cap)
      :writable_tail(valid_store_io_contract(tail_io->contract()) && tail_io->allows_writer()),
       base_owner(std::move(base_io),false),tail(std::move(tail_io),writable_tail),physical_limit(cap) {}

  std::uint64_t tail_extent() const {
    const auto extent=tail.io->extent();
    if(extent<tail_start || extent>physical_limit-binding.base_occupied ||
        extent-tail_start>binding.virtual_limit-binding.base_end)
      fail("hybrid writable tail outside admitted bounds");
    return extent;
  }
  std::uint64_t tail_offset(std::uint64_t at) const noexcept {
    return tail_start+(at-binding.base_end);
  }

  const bool writable_tail;
  Lease base_owner,tail;
  std::shared_ptr<ProtectedStripeImage> base;
  const std::uint64_t physical_limit;
  Binding binding;
  mutable bool degraded=false;
};

NativeHybridBacking::NativeHybridBacking(std::unique_ptr<Impl> state) noexcept:impl_(std::move(state)) {}
NativeHybridBacking::~NativeHybridBacking()=default;

std::shared_ptr<NativeHybridBacking> NativeHybridBacking::open(std::shared_ptr<StoreByteIo> base,
    std::shared_ptr<StoreByteIo> tail,StoreIdentity expected,std::uint64_t virtual_limit,std::uint64_t physical_limit) {
  check_limits(expected,virtual_limit,physical_limit);
  if(!base || !tail || base==tail || tail->extent()<tail_start || tail->extent()>physical_limit)
    fail("hybrid image needs a bounded tail and a distinct base");
  auto state=std::make_unique<Impl>(std::move(base),std::move(tail),physical_limit);
  auto [binding,degraded]=load_binding(*state->tail.io);
  if(binding.identity!=expected || binding.virtual_limit!=virtual_limit)
    fail("hybrid image identity or capacity differs");
  check_geometry(binding);
  if(state->writable_tail && state->tail.io->contract().write_failure_unit_bytes>binding.unit)
    fail("hybrid tail failure unit exceeds the indexed geometry");
  state->base=ProtectedStripeImage::open(state->base_owner.io,physical_limit-tail_start);
  if(state->base->identity()!=expected || state->base->extent()!=binding.folded_extent() ||
      state->base->occupied()!=binding.base_occupied ||
      root_binding(*state->base)!=binding.frozen_roots)
    fail("hybrid base does not match the checked binding");
  state->binding=binding;
  state->degraded=degraded;
  (void)state->tail_extent();
  return std::shared_ptr<NativeHybridBacking>(new NativeHybridBacking(std::move(state)));
}

std::shared_ptr<NativeHybridBacking> NativeHybridBacking::create(const NativeFilesystemSnapshot& source,
    std::shared_ptr<StoreByteIo> workspace,std::shared_ptr<StoreByteIo> base,std::shared_ptr<StoreByteIo> tail,
    StoreIdentity identity,std::uint64_t physical_limit,StoreCompressionOptions compression) {
  const auto source_storage=source.image_storage();
  check_limits(identity,source_storage.maximum_physical_bytes,physical_limit);
  if(source_storage.kind!=NativeImageKind::indexed_paged || source.needs_session_recovery() ||
      identity==source.identity() || !workspace || !base || !tail || workspace==base || workspace==tail || base==tail ||
      workspace->extent() || base->extent() || tail->extent() ||
      !valid_store_io_contract(workspace->contract()) || !valid_store_io_contract(base->contract()) ||
      !valid_store_io_contract(tail->contract()))
    fail("hybrid creation needs a closed indexed source and three qualified empty backings");
  const auto unit=(std::max<std::uint64_t>)(root_size,workspace->contract().write_failure_unit_bytes);
  if(tail->contract().write_failure_unit_bytes>unit)fail("hybrid tail needs the compacted mirror's failure unit");
  const auto packed=source.build_folded_workspace(workspace,identity,compression);
  Binding binding{packed.unit,packed.virtual_end,0,identity,packed.roots,source_storage.maximum_physical_bytes};
  check_geometry(binding);
  if(binding.unit!=unit || binding.folded_extent()!=workspace->extent())
    fail("hybrid folded workspace checkpoint differs");
  binding.base_occupied=ProtectedStripeImage::projected_occupied(identity,binding.folded_extent(),
      base->contract().write_failure_unit_bytes,physical_limit-tail_start);
  if(binding.base_occupied+tail_start>=binding.base_end)
    fail("hybrid base and tail do not reduce the protected footprint");

  const auto sealed=ProtectedStripeImage::seal(workspace,base,identity,physical_limit-tail_start);
  if(sealed->occupied()!=binding.base_occupied || root_binding(*sealed)!=binding.frozen_roots)
    fail("hybrid checked base differs from the staged checkpoint");
  {
    const Impl::Lease tail_owner(tail,true);
    if(tail->extent())fail("hybrid tail is no longer empty");
    tail->admit_write(tail_start);
    Bytes bytes(roots);
    tail->write_exact(0,bytes);
    sealed->read_exact(0,bytes);
    tail->write_exact(roots,bytes);
    tail->flush();
    Bytes copied(roots);
    tail->read_exact(roots,copied);
    if(copied!=bytes || tail->extent()!=tail_start)fail("hybrid mutable roots differ");
    const auto publication=encode(binding);
    tail->write_exact(0,publication);
    tail->flush();
    tail->write_exact(kStoreCommitUnit,publication);
    tail->flush();
  }
  auto result=open(std::move(base),std::move(tail),identity,binding.virtual_limit,physical_limit);
  auto checked=NativeFilesystemSnapshot::open_image(result,source_storage);
  checked.verify_all();
  if(result->backing_degraded() || checked.storage_info().degraded_checkpoint ||
      checked.storage_info().generation!=packed.generation)
    fail("hybrid checkpoint did not verify after publication");
  return result;
}

std::uint64_t NativeHybridBacking::occupied() const {
  return impl_->binding.base_occupied+impl_->tail_extent();
}
std::uint64_t NativeHybridBacking::extent() const {
  return impl_->binding.base_end+(impl_->tail_extent()-tail_start);
}
void NativeHybridBacking::read_exact(std::uint64_t at,std::span<std::byte> output) const {
  const auto& state=*impl_;
  const auto& binding=state.binding;
  if(at>binding.virtual_limit || output.size()>binding.virtual_limit-at)fail("hybrid read bounds");
  if(at<roots && !output.empty()) {
    const auto count=(std::min<std::uint64_t>)(output.size(),roots-at);
    state.tail.io->read_exact(roots+at,output.first(count));
    output=output.subspan(count);
    at+=count;
  }
  while(!output.empty() && at<binding.base_end) {
    const auto count=(std::min<std::uint64_t>)(output.size(),binding.unit-at%binding.unit);
    const auto folded=detail::folded_mirror_offset(at,binding.unit);
    try {state.base->read_exact(folded,output.first(count));}
    catch(const std::bad_alloc&){throw;}
    catch(const std::exception&){state.degraded=true;throw;}
    output=output.subspan(count);
    at+=count;
  }
  if(!output.empty())state.tail.io->read_exact(state.tail_offset(at),output);
}
void NativeHybridBacking::write_exact(std::uint64_t at,std::span<const std::byte> input) {
  if(!has_writer() || !allows_writer())fail("hybrid backing has no healthy writer");
  const auto& state=*impl_;
  if(at<roots && input.size()<=roots-at) {
    state.tail.io->write_exact(roots+at,input);
    return;
  }
  const auto& binding=state.binding;
  if(at<binding.base_end || at>binding.virtual_limit || input.size()>binding.virtual_limit-at)
    fail("hybrid writes cannot overlap the sealed base");
  const auto offset=state.tail_offset(at);
  const auto available=state.physical_limit-binding.base_occupied;
  if(offset>available || input.size()>available-offset)fail("hybrid write exceeds physical backing budget");
  state.tail.io->write_exact(offset,input);
}
void NativeHybridBacking::admit_write(std::uint64_t projected) {
  const auto& state=*impl_;
  if(!has_writer() || !allows_writer() || projected<extent() || projected>state.binding.virtual_limit)
    fail("hybrid write admission refused");
  const auto projected_tail=state.tail_offset(projected);
  if(projected_tail>state.physical_limit-state.binding.base_occupied)fail("hybrid physical backing budget");
  state.tail.io->admit_write(projected_tail);
}
void NativeHybridBacking::flush() {
  if(!allows_writer())fail("hybrid backing requires recovery before flush");
  impl_->tail.io->flush();
}
StoreIoContract NativeHybridBacking::contract() const noexcept {
  const auto tail=impl_->tail.io->contract();
  const auto base=impl_->base_owner.io->contract();
  if(!impl_->writable_tail || !impl_->tail.io->allows_writer() || !valid_store_io_contract(tail) ||
      !valid_store_io_contract(base) || tail.write_failure_unit_bytes>impl_->binding.unit)
    return {};
  return {tail.write_failure_unit_bytes,true,false};
}
bool NativeHybridBacking::backing_degraded() const noexcept {
  return impl_->degraded || impl_->base->degraded() ||
      impl_->base_owner.io->backing_degraded() || impl_->tail.io->backing_degraded();
}
bool NativeHybridBacking::allows_writer() const noexcept {
  return !backing_degraded() && valid_store_io_contract(contract());
}
void NativeHybridBacking::verify_backing() const {
  auto& state=*impl_;
  try {
    const auto [observed,degraded]=load_binding(*state.tail.io);
    if(observed!=state.binding)fail("hybrid backing binding changed");
    if(degraded)state.degraded=true;
    (void)state.tail_extent();
    state.base->verify_all();
    if(root_binding(*state.base)!=state.binding.frozen_roots)fail("hybrid frozen roots changed");
  }catch(const std::bad_alloc&){throw;}
  catch(const std::exception&){state.degraded=true;throw;}
}

} // namespace flitzfs
