#include "protected_stripe_image.hpp"
#include "store_read_scope.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace flitzfs::detail {
namespace {
using Bytes=std::vector<std::byte>;
using Layout=ProtectedStripeLayout;
constexpr std::uint64_t stripe_data_units=kProtectedStripeDataUnits,minimum_unit=4096;
constexpr std::size_t root_bytes=128,descriptor_hashes=64;
constexpr std::string_view root_magic="FLTXSP01",compact_root_magic="FLTXSP02",descriptor_magic="FLTXSD01";
constexpr auto maximum_io=static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());

[[noreturn]] void fail(const char* why) {throw std::runtime_error(why);}

std::uint64_t get64(std::span<const std::byte> bytes,std::size_t offset) noexcept {
  std::uint64_t result=0;
  for(unsigned i=0;i<8;++i)result|=std::to_integer<std::uint64_t>(bytes[offset+i])<<(8*i);
  return result;
}
void put64(std::span<std::byte> bytes,std::size_t offset,std::uint64_t value) noexcept {
  for(unsigned i=0;i<8;++i)bytes[offset+i]=static_cast<std::byte>((value>>(8*i))&255);
}
template<std::size_t N>std::array<std::byte,N> field(std::span<const std::byte> bytes,std::size_t offset) {
  std::array<std::byte,N> result{};
  std::copy_n(bytes.begin()+offset,N,result.begin());
  return result;
}
void write_magic(std::span<std::byte> bytes,std::string_view magic) {
  std::ranges::copy(std::as_bytes(std::span(magic)),bytes.begin());
}
bool has_magic(std::span<const std::byte> bytes,std::string_view magic) {
  return std::ranges::equal(bytes.first(magic.size()),std::as_bytes(std::span(magic)));
}
bool zero(std::span<const std::byte> bytes) {
  return std::ranges::all_of(bytes,[](auto b){return b==std::byte{};});
}
void write_checksum(std::span<std::byte> bytes) noexcept {
  const auto digest=record_digest(bytes.first(bytes.size()-Digest{}.size()));
  std::ranges::copy(digest,bytes.end()-digest.size());
}
bool matches_checksum(std::span<const std::byte> bytes) noexcept {
  const auto offset=bytes.size()-Digest{}.size();
  return field<32>(bytes,offset)==record_digest(bytes.first(offset));
}
std::uint64_t blocks(const Layout& layout) noexcept {
  return 1+(layout.logical_bytes-1)/layout.unit;
}
std::uint64_t stripes(const Layout& layout) noexcept {
  return 1+(blocks(layout)-1)/stripe_data_units;
}
Layout make_layout(StoreIdentity id,std::uint64_t logical,std::uint64_t unit,std::uint64_t limit,bool compact) {
  if(id==StoreIdentity{} || !logical || limit>maximum_io ||
      unit<minimum_unit || unit>kStoreCommitUnit || !std::has_single_bit(unit))
    fail("invalid protected image geometry");
  Layout layout{id,logical,0,unit,compact?unit:kStoreCommitUnit};
  const auto prefix=2*layout.root_slot_bytes;
  if(limit<prefix)fail("protected image capacity");
  const auto data=blocks(layout),extra=2*stripes(layout);
  const auto available=(limit-prefix)/unit;
  if(data>available || extra>available-data)fail("protected image capacity");
  layout.physical_bytes=prefix+(data+extra)*unit;
  return layout;
}

void read_backing(const StoreByteIo& io,std::uint64_t offset,std::span<std::byte> bytes) {
  StoreCaptureReadScope::require_active();
  io.read_exact(offset,bytes);
  StoreCaptureReadScope::require_active();
}
bool read_copy(const StoreByteIo& io,std::uint64_t offset,std::span<std::byte> bytes) {
  try {read_backing(io,offset,bytes);return true;}
  catch(const std::bad_alloc&){throw;}
  catch(const StoreCaptureInterrupted&){throw;}
  catch(const std::exception&){return false;}
}
std::array<std::byte,root_bytes> make_root(const Layout& layout) {
  std::array<std::byte,root_bytes> root{};
  write_magic(root,layout.root_slot_bytes==kStoreCommitUnit?root_magic:compact_root_magic);
  put64(root,8,layout.unit);
  put64(root,16,layout.logical_bytes);
  put64(root,24,layout.physical_bytes);
  std::ranges::copy(layout.identity,root.begin()+32);
  write_checksum(root);
  return root;
}
std::optional<Layout> read_root(const StoreByteIo& io,std::uint64_t offset,std::uint64_t limit,bool probing=false) {
  std::array<std::byte,root_bytes> root{};
  if(offset>io.extent() || root.size()>io.extent()-offset)return {};
  if(!read_copy(io,offset,root) || !matches_checksum(root))return {};
  if(probing && !has_magic(root,"FLTXSP"))return {};
  const auto unit=get64(root,8);
  const bool compact=has_magic(root,compact_root_magic),legacy=has_magic(root,root_magic);
  const auto spacing=compact?unit:kStoreCommitUnit;
  if(probing && (compact || legacy) && offset!=spacing)return {};
  if((!legacy && !compact) || (compact && unit>=kStoreCommitUnit) || (offset && offset!=spacing) ||
      !zero(std::span(root).subspan(48,48)))
    fail("unsupported intact protected image root");
  const auto candidate=make_layout(field<16>(root,32),get64(root,16),unit,limit,compact);
  if(candidate.physical_bytes!=get64(root,24) || candidate.physical_bytes!=io.extent())
    fail("protected image root extent differs");
  return candidate;
}
std::pair<Layout,bool> load_roots(const StoreByteIo& io,std::uint64_t limit) {
  if(io.extent()<2*minimum_unit || io.extent()>limit)fail("protected image extent");
  const auto first=read_root(io,0,limit);
  std::optional<Layout> second;
  if(first)second=read_root(io,first->root_slot_bytes,limit);
  else {
    for(auto offset=minimum_unit;offset<=kStoreCommitUnit;offset*=2) {
      if(auto recovered=read_root(io,offset,limit,true)) {
        if(second)fail("ambiguous protected image peer");
        second=std::move(recovered);
      }
    }
  }
  if(!first && !second)fail("no protected image recovery root");
  if(first && second && *first!=*second)fail("conflicting protected image roots");
  return {first?*first:*second,!(first && second)};
}
void merge_xor(std::span<std::byte> output,std::span<const std::byte> input) noexcept {
  for(std::size_t i=0;i<output.size();++i)output[i]^=input[i];
}
struct Stripe {
  const Layout& layout;
  std::uint64_t index;
  std::uint64_t first_data() const noexcept {return index*stripe_data_units;}
  std::uint64_t count() const noexcept {return (std::min)(stripe_data_units,blocks(layout)-first_data());}
  std::uint64_t data_at(std::uint64_t slot) const noexcept {
    return 2*layout.root_slot_bytes+(index*(stripe_data_units+2)+slot)*layout.unit;
  }
  std::uint64_t descriptor_at() const noexcept {return data_at(count());}
  std::uint64_t parity_at() const noexcept {return data_at(count()+1);}
  std::uint64_t logical_at(std::uint64_t slot) const noexcept {return (first_data()+slot)*layout.unit;}
};

void initialize_descriptor(std::span<std::byte> descriptor,const Stripe& stripe) {
  std::ranges::fill(descriptor,std::byte{});
  write_magic(descriptor,descriptor_magic);
  put64(descriptor,8,stripe.index);
  std::ranges::copy(stripe.layout.identity,descriptor.begin()+16);
  put64(descriptor,32,stripe.count());
}
bool valid_descriptor(std::span<const std::byte> bytes,const Stripe& stripe) {
  const auto end=descriptor_hashes+stripe.count()*Digest{}.size();
  return matches_checksum(bytes) && has_magic(bytes,descriptor_magic) && get64(bytes,8)==stripe.index &&
      field<16>(bytes,16)==stripe.layout.identity && get64(bytes,32)==stripe.count() &&
      zero(bytes.subspan(40,24)) && zero(bytes.subspan(end,bytes.size()-end-Digest{}.size()));
}
Digest data_digest(std::span<const std::byte> descriptor,std::uint64_t slot) {
  return field<32>(descriptor,descriptor_hashes+slot*Digest{}.size());
}
bool check_data(std::span<const std::byte> bytes,std::span<const std::byte> descriptor,std::uint64_t slot) noexcept {
  return record_digest(bytes)==data_digest(descriptor,slot);
}
bool read_descriptor(const StoreByteIo& io,const Stripe& stripe,std::span<std::byte> descriptor,Bytes& scratch) {
  if(read_copy(io,stripe.descriptor_at(),descriptor) && valid_descriptor(descriptor,stripe))return false;
  read_backing(io,stripe.parity_at(),descriptor);
  scratch.resize(stripe.layout.unit);
  for(std::uint64_t i=0;i<stripe.count();++i) {
    read_backing(io,stripe.data_at(i),scratch);
    merge_xor(descriptor,scratch);
  }
  if(!valid_descriptor(descriptor,stripe))fail("unrecoverable protected stripe descriptor");
  return true;
}
void recover_data(const StoreByteIo& io,const Stripe& stripe,std::uint64_t missing,
    std::span<const std::byte> descriptor,std::span<std::byte> bytes,std::span<std::byte> scratch) {
  read_backing(io,stripe.parity_at(),bytes);
  merge_xor(bytes,descriptor);
  for(std::uint64_t i=0;i<stripe.count();++i) {
    if(i==missing)continue;
    read_backing(io,stripe.data_at(i),scratch);
    if(!check_data(scratch,descriptor,i))fail("more than one protected data unit is unavailable");
    merge_xor(bytes,scratch);
  }
  if(!check_data(bytes,descriptor,missing))fail("unrecoverable protected data unit");
}
struct StripeBuffers {
  explicit StripeBuffers(std::size_t unit):descriptor(unit),data(unit),scratch(unit),parity(unit) {}
  Bytes descriptor,data,scratch,parity;
};

bool verify_stripe(const StoreByteIo& stored,const Stripe& stripe,StripeBuffers& buffers,
    const StoreByteIo* clean_source) {
  auto& [descriptor,data,scratch,parity]=buffers;
  bool damaged=read_descriptor(stored,stripe,descriptor,scratch);
  if(damaged && clean_source)fail("damaged descriptor in newly sealed image");
  std::ranges::copy(descriptor,parity.begin());
  for(std::uint64_t i=0;i<stripe.count();++i) {
    if(!read_copy(stored,stripe.data_at(i),data) || !check_data(data,descriptor,i)) {
      if(damaged || clean_source)fail("damaged data in protected image verification");
      recover_data(stored,stripe,i,descriptor,data,scratch);
      damaged=true;
    }
    const auto offset=stripe.logical_at(i);
    const auto length=(std::min)(stripe.layout.unit,stripe.layout.logical_bytes-offset);
    if(!zero(std::span(data).subspan(length)))fail("protected image nonzero tail");
    if(clean_source) {
      clean_source->read_exact(offset,std::span(scratch).first(length));
      if(!std::ranges::equal(std::span(scratch).first(length),std::span(data).first(length)))
        fail("new protected image differs from the source");
    }
    merge_xor(parity,data);
  }
  if(!read_copy(stored,stripe.parity_at(),scratch) || scratch!=parity) {
    if(damaged || clean_source)fail("damaged parity in protected image verification");
    damaged=true;
  }
  return damaged;
}
} // namespace

struct ProtectedStripeImage::Lease {
  StoreByteIo& io;
  Lease(StoreByteIo& backing,bool writable):io(backing) {
    if(!(writable?io.acquire_writer():io.acquire_exclusive_use()))fail("protected image backing is already owned");
  }
  ~Lease(){io.release_writer();}
  Lease(const Lease&)=delete;
  Lease& operator=(const Lease&)=delete;
};
ProtectedStripeImage::ProtectedStripeImage(std::shared_ptr<StoreByteIo> backing,Layout layout,
    std::uint64_t maximum,bool degraded):backing_(std::move(backing)),layout_(layout),
    physical_limit_(maximum),degraded_(degraded) {}

std::uint64_t ProtectedStripeImage::projected_occupied(StoreIdentity id,std::uint64_t logical,
    std::uint32_t failure_unit,std::uint64_t physical_limit,bool compact_roots) {
  return make_layout(id,logical,(std::max<std::uint64_t>)(minimum_unit,failure_unit),
      physical_limit,compact_roots).physical_bytes;
}
std::shared_ptr<ProtectedStripeImage> ProtectedStripeImage::open(std::shared_ptr<StoreByteIo> backing,
    std::uint64_t physical_limit) {
  if(!backing)fail("missing protected image backing");
  const auto [layout,degraded]=load_roots(*backing,physical_limit);
  return std::shared_ptr<ProtectedStripeImage>(
      new ProtectedStripeImage(std::move(backing),layout,physical_limit,degraded));
}
struct ProtectedStripeImage::SealWork::Impl {
  enum class Phase { reserve,copy,flush,verify,first_root,second_root,ready };

  static Layout prepare_layout(const StoreByteIo& source,const StoreByteIo& output,
      StoreIdentity id,std::uint64_t maximum,bool compact_roots) {
    if(output.extent() || !valid_store_io_contract(output.contract()))
      fail("protected image requires an empty qualified target");
    return make_layout(id,source.extent(),(std::max<std::uint64_t>)(minimum_unit,output.contract().write_failure_unit_bytes),
        maximum,compact_roots);
  }
  Impl(std::shared_ptr<StoreByteIo> source,std::shared_ptr<StoreByteIo> target,StoreIdentity id,std::uint64_t maximum,
      bool compact_roots)
      :input(std::move(source)),output(std::move(target)),input_lease(*input,false),output_lease(*output,true),
       limit(maximum),layout(prepare_layout(*input,*output,id,maximum,compact_roots)),buffers(layout.unit) {}

  void copy_stripe(const Stripe& stripe) {
    auto& descriptor=buffers.descriptor;
    auto& data=buffers.data;
    auto& parity=buffers.parity;
    initialize_descriptor(descriptor,stripe);
    std::ranges::fill(parity,std::byte{});
    for(std::uint64_t i=0;i<stripe.count();++i) {
      const auto offset=stripe.logical_at(i);
      const auto length=(std::min)(layout.unit,layout.logical_bytes-offset);
      input->read_exact(offset,std::span(data).first(length));
      std::ranges::fill(std::span(data).subspan(length),std::byte{});
      std::ranges::copy(record_digest(data),descriptor.begin()+descriptor_hashes+i*Digest{}.size());
      merge_xor(parity,data);
      output->write_exact(stripe.data_at(i),data);
    }
    write_checksum(descriptor);
    merge_xor(parity,descriptor);
    output->write_exact(stripe.descriptor_at(),descriptor);
    output->write_exact(stripe.parity_at(),parity);
  }

  bool step() {
    const auto contract=output->contract();
    if(!valid_store_io_contract(contract) || contract.write_failure_unit_bytes>layout.unit)
      fail("protected image contract changed during sealing");
    switch(phase) {
      case Phase::reserve: {
        output->admit_write(2*layout.root_slot_bytes);
        const Bytes reservation(layout.root_slot_bytes);
        output->write_exact(0,reservation);
        output->write_exact(layout.root_slot_bytes,reservation);
        phase=Phase::copy;
        break;
      }
      case Phase::copy: {
        const Stripe stripe{layout,next_stripe};
        output->admit_write(stripe.parity_at()+layout.unit);
        copy_stripe(stripe);
        if(++next_stripe==stripes(layout))phase=Phase::flush;
        break;
      }
      case Phase::flush:
        output->flush();
        if(output->extent()!=layout.physical_bytes || input->extent()!=layout.logical_bytes ||
            !valid_store_io_contract(output->contract()) || output->contract().write_failure_unit_bytes>layout.unit)
          fail("protected image backing changed during sealing");
        next_stripe=0;
        phase=Phase::verify;
        break;
      case Phase::verify:
        (void)verify_stripe(*output,{layout,next_stripe++},buffers,input.get());
        if(next_stripe==stripes(layout))phase=Phase::first_root;
        break;
      case Phase::first_root:
        output->write_exact(0,make_root(layout));
        output->flush();
        phase=Phase::second_root;
        break;
      case Phase::second_root:
        output->write_exact(layout.root_slot_bytes,make_root(layout));
        output->flush();
        published=ProtectedStripeImage::open(output,limit);
        if(published->layout_!=layout || published->degraded())fail("protected image publication did not verify");
        phase=Phase::ready;
        break;
      case Phase::ready:
        break;
    }
    return phase==Phase::ready;
  }

  std::shared_ptr<StoreByteIo> input,output;
  Lease input_lease,output_lease;
  std::uint64_t limit;
  Layout layout;
  StripeBuffers buffers;
  std::uint64_t next_stripe=0;
  Phase phase=Phase::reserve;
  std::shared_ptr<ProtectedStripeImage> published;
};

ProtectedStripeImage::SealWork::SealWork(std::unique_ptr<Impl> value) noexcept:impl_(std::move(value)) {}
ProtectedStripeImage::SealWork::SealWork(SealWork&&) noexcept=default;
ProtectedStripeImage::SealWork& ProtectedStripeImage::SealWork::operator=(SealWork&&) noexcept=default;
ProtectedStripeImage::SealWork::~SealWork()=default;

ProtectedStripeImage::SealWork ProtectedStripeImage::begin_seal(std::shared_ptr<StoreByteIo> source,
    std::shared_ptr<StoreByteIo> target,StoreIdentity id,std::uint64_t physical_limit,bool compact_roots) {
  if(!source || !target || source==target)fail("protected sealing requires separate owned backings");
  return SealWork(std::make_unique<SealWork::Impl>(std::move(source),std::move(target),id,physical_limit,compact_roots));
}
bool ProtectedStripeImage::SealWork::advance() {
  if(!impl_)fail("protected sealing is unavailable");
  try {return impl_->step();}
  catch(...){impl_.reset();throw;}
}
bool ProtectedStripeImage::SealWork::can_continue_preparation() const noexcept {
  return impl_ && (impl_->phase==Impl::Phase::copy || impl_->phase==Impl::Phase::verify);
}
std::shared_ptr<ProtectedStripeImage> ProtectedStripeImage::SealWork::finish() {
  if(!impl_ || !impl_->published)fail("protected sealing is incomplete");
  auto result=std::move(impl_->published);
  impl_.reset();
  return result;
}

std::shared_ptr<ProtectedStripeImage> ProtectedStripeImage::seal(std::shared_ptr<StoreByteIo> source,
    std::shared_ptr<StoreByteIo> target,StoreIdentity id,std::uint64_t physical_limit,bool compact_roots) {
  auto build=begin_seal(std::move(source),std::move(target),id,physical_limit,compact_roots);
  while(!build.advance()) {}
  return build.finish();
}
Digest ProtectedStripeImage::data_checksum(std::uint64_t index,std::uint64_t slot,
    std::uint64_t cache_generation) const {
  {
    std::lock_guard lock(cache_mutex_);
    const auto hit=std::ranges::find(checked_,index,&CheckedStripe::stripe);
    if(cache_generation==cache_generation_ && hit!=checked_.end()) {
      std::rotate(checked_.begin(),hit,hit+1);
      return checked_.front().checksums[slot];
    }
  }

  const Stripe stripe{layout_,index};
  Bytes descriptor(layout_.unit),scratch;
  if(read_descriptor(*backing_,stripe,descriptor,scratch))degraded_.store(true,std::memory_order_release);
  CheckedStripe loaded{index};
  for(std::uint64_t i=0;i<stripe.count();++i)loaded.checksums[i]=data_digest(descriptor,i);
  {
    std::lock_guard lock(cache_mutex_);
    if(cache_generation==cache_generation_ && std::ranges::find(checked_,index,&CheckedStripe::stripe)==checked_.end()) {
      std::move_backward(checked_.begin(),checked_.end()-1,checked_.end());
      checked_.front()=loaded;
    }
  }
  return loaded.checksums[slot];
}
void ProtectedStripeImage::read_block(std::uint64_t block,std::size_t first,std::span<std::byte> output) const {
  StoreCaptureReadScope::require_active();
  std::uint64_t generation;
  {
    std::lock_guard lock(cache_mutex_);
    const auto hit=std::ranges::find(blocks_,block,&CheckedBlock::ordinal);
    if(hit!=blocks_.end()) {
      std::ranges::copy(std::span(hit->bytes).subspan(first,output.size()),output.begin());
      std::rotate(blocks_.begin(),hit,hit+1);
      return;
    }
    generation=cache_generation_;
  }

  const Stripe stripe{layout_,block/stripe_data_units};
  const auto slot=block%stripe_data_units;
  const auto expected=data_checksum(stripe.index,slot,generation);
  Bytes data(layout_.unit);
  if(!read_copy(*backing_,stripe.data_at(slot),data) || record_digest(data)!=expected) {
    Bytes descriptor(layout_.unit),scratch(layout_.unit);
    (void)read_descriptor(*backing_,stripe,descriptor,scratch);
    if(data_digest(descriptor,slot)!=expected)fail("protected stripe descriptor changed");
    recover_data(*backing_,stripe,slot,descriptor,data,scratch);
    degraded_.store(true,std::memory_order_release);
  }
  std::ranges::copy(std::span(data).subspan(first,output.size()),output.begin());
  {
    std::lock_guard lock(cache_mutex_);
    if(generation==cache_generation_ && std::ranges::find(blocks_,block,&CheckedBlock::ordinal)==blocks_.end()) {
      auto& victim=blocks_.back();
      victim.bytes.swap(data);
      victim.ordinal=block;
      std::rotate(blocks_.begin(),blocks_.end()-1,blocks_.end());
    }
  }
}

void ProtectedStripeImage::read_exact(std::uint64_t offset,std::span<std::byte> output) const {
  if(offset>extent() || output.size()>extent()-offset)fail("protected image read outside logical bounds");
  while(!output.empty()) {
    const auto first=offset%layout_.unit;
    const auto length=(std::min<std::uint64_t>)(output.size(),layout_.unit-first);
    read_block(offset/layout_.unit,first,output.first(length));
    output=output.subspan(length);
    offset+=length;
  }
}
void ProtectedStripeImage::verify_stripes() const {
  StripeBuffers buffers(layout_.unit);
  for(std::uint64_t n=0;n<stripes(layout_);++n) {
    if(verify_stripe(*backing_,{layout_,n},buffers,nullptr))degraded_.store(true,std::memory_order_release);
  }
}
void ProtectedStripeImage::verify_all() const {
  {
    std::lock_guard lock(cache_mutex_);
    ++cache_generation_;
    checked_.fill({});
    for(auto& block:blocks_)block.ordinal=UINT64_MAX;
  }
  const auto [checked,damaged]=load_roots(*backing_,physical_limit_);
  if(checked!=layout_)fail("protected image roots changed");
  if(damaged)degraded_.store(true,std::memory_order_release);
  verify_stripes();
}
void ProtectedStripeImage::write_exact(std::uint64_t,std::span<const std::byte>) {fail("protected image is immutable");}
void ProtectedStripeImage::admit_write(std::uint64_t) {fail("protected image is immutable");}
void ProtectedStripeImage::flush() {fail("protected image is immutable");}

}
