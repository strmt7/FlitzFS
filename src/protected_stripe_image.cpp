#include "protected_stripe_image.hpp"
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
constexpr std::uint64_t stripe_data_units=kProtectedStripeDataUnits,prefix_bytes=2*kStoreCommitUnit;
constexpr std::size_t root_bytes=128,descriptor_hashes=64;
constexpr std::string_view root_magic="FLTXSP01",descriptor_magic="FLTXSD01";
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
Layout make_layout(StoreIdentity id,std::uint64_t logical,std::uint64_t unit,std::uint64_t limit) {
  if(id==StoreIdentity{} || !logical || limit>maximum_io || limit<prefix_bytes ||
      unit<4096 || unit>kStoreCommitUnit || !std::has_single_bit(unit))
    fail("invalid protected image geometry");
  Layout layout{id,logical,0,unit};
  const auto data=blocks(layout),extra=2*stripes(layout);
  const auto available=(limit-prefix_bytes)/unit;
  if(data>available || extra>available-data)fail("protected image capacity");
  layout.physical_bytes=prefix_bytes+(data+extra)*unit;
  return layout;
}

bool read_copy(const StoreByteIo& io,std::uint64_t offset,std::span<std::byte> bytes) {
  try {io.read_exact(offset,bytes);return true;}
  catch(const std::bad_alloc&){throw;}
  catch(const std::exception&){return false;}
}
std::array<std::byte,root_bytes> make_root(const Layout& layout) {
  std::array<std::byte,root_bytes> root{};
  write_magic(root,root_magic);
  put64(root,8,layout.unit);
  put64(root,16,layout.logical_bytes);
  put64(root,24,layout.physical_bytes);
  std::ranges::copy(layout.identity,root.begin()+32);
  write_checksum(root);
  return root;
}
std::optional<Layout> read_root(const StoreByteIo& io,std::uint64_t offset,std::uint64_t limit) {
  std::array<std::byte,root_bytes> root{};
  if(!read_copy(io,offset,root) || !matches_checksum(root))return {};
  if(!has_magic(root,root_magic) || !zero(std::span(root).subspan(48,48)))
    fail("unsupported intact protected image root");
  const auto candidate=make_layout(field<16>(root,32),get64(root,16),get64(root,8),limit);
  if(candidate.physical_bytes!=get64(root,24) || candidate.physical_bytes!=io.extent())
    fail("protected image root extent differs");
  return candidate;
}
std::pair<Layout,bool> load_roots(const StoreByteIo& io,std::uint64_t limit) {
  if(io.extent()<prefix_bytes || io.extent()>limit)fail("protected image extent");
  const auto first=read_root(io,0,limit),second=read_root(io,kStoreCommitUnit,limit);
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
    return prefix_bytes+(index*(stripe_data_units+2)+slot)*layout.unit;
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
  io.read_exact(stripe.parity_at(),descriptor);
  scratch.resize(stripe.layout.unit);
  for(std::uint64_t i=0;i<stripe.count();++i) {
    io.read_exact(stripe.data_at(i),scratch);
    merge_xor(descriptor,scratch);
  }
  if(!valid_descriptor(descriptor,stripe))fail("unrecoverable protected stripe descriptor");
  return true;
}
void recover_data(const StoreByteIo& io,const Stripe& stripe,std::uint64_t missing,
    std::span<const std::byte> descriptor,std::span<std::byte> bytes,std::span<std::byte> scratch) {
  io.read_exact(stripe.parity_at(),bytes);
  merge_xor(bytes,descriptor);
  for(std::uint64_t i=0;i<stripe.count();++i) {
    if(i==missing)continue;
    io.read_exact(stripe.data_at(i),scratch);
    if(!check_data(scratch,descriptor,i))fail("more than one protected data unit is unavailable");
    merge_xor(bytes,scratch);
  }
  if(!check_data(bytes,descriptor,missing))fail("unrecoverable protected data unit");
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
    std::uint32_t failure_unit,std::uint64_t physical_limit) {
  return make_layout(id,logical,(std::max)(4096U,failure_unit),physical_limit).physical_bytes;
}
std::shared_ptr<ProtectedStripeImage> ProtectedStripeImage::open(std::shared_ptr<StoreByteIo> backing,
    std::uint64_t physical_limit) {
  if(!backing)fail("missing protected image backing");
  const auto [layout,degraded]=load_roots(*backing,physical_limit);
  return std::shared_ptr<ProtectedStripeImage>(
      new ProtectedStripeImage(std::move(backing),layout,physical_limit,degraded));
}
std::shared_ptr<ProtectedStripeImage> ProtectedStripeImage::seal(std::shared_ptr<StoreByteIo> source,
    std::shared_ptr<StoreByteIo> target,StoreIdentity id,std::uint64_t physical_limit) {
  if(!source || !target || source==target)fail("protected sealing requires separate owned backings");
  const Lease input(*source,false),output(*target,true);
  if(target->extent() || !valid_store_io_contract(target->contract()))
    fail("protected image requires an empty qualified target");
  const auto contract=target->contract();
  const auto layout=make_layout(id,source->extent(),(std::max)(4096U,contract.write_failure_unit_bytes),physical_limit);
  target->admit_write(layout.physical_bytes);
  {
    const Bytes reservation(kStoreCommitUnit);
    target->write_exact(0,reservation);
    target->write_exact(kStoreCommitUnit,reservation);
  }
  Bytes descriptor(layout.unit),parity(layout.unit),data(layout.unit);
  for(std::uint64_t n=0;n<stripes(layout);++n) {
    const Stripe stripe{layout,n};
    initialize_descriptor(descriptor,stripe);
    std::ranges::fill(parity,std::byte{});
    for(std::uint64_t i=0;i<stripe.count();++i) {
      const auto offset=stripe.logical_at(i);
      const auto length=(std::min)(layout.unit,layout.logical_bytes-offset);
      source->read_exact(offset,std::span(data).first(length));
      std::ranges::fill(std::span(data).subspan(length),std::byte{});
      std::ranges::copy(record_digest(data),descriptor.begin()+descriptor_hashes+i*Digest{}.size());
      merge_xor(parity,data);
      target->write_exact(stripe.data_at(i),data);
    }
    write_checksum(descriptor);
    merge_xor(parity,descriptor);
    target->write_exact(stripe.descriptor_at(),descriptor);
    target->write_exact(stripe.parity_at(),parity);
  }
  target->flush();
  if(target->extent()!=layout.physical_bytes || source->extent()!=layout.logical_bytes ||
      !valid_store_io_contract(target->contract()) || target->contract().write_failure_unit_bytes>layout.unit)
    fail("protected image backing changed during sealing");
  const ProtectedStripeImage unpublished(target,layout,physical_limit,false);
  unpublished.verify_stripes(source.get());

  const auto root=make_root(layout);
  target->write_exact(0,root);
  target->flush();
  target->write_exact(kStoreCommitUnit,root);
  target->flush();
  auto published=open(target,physical_limit);
  if(published->layout_!=layout || published->degraded())fail("protected image publication did not verify");
  return published;
}
const ProtectedStripeImage::Checksums& ProtectedStripeImage::stripe_checksums(std::uint64_t index) const {
  const auto hit=std::ranges::find(checked_,index,&CheckedStripe::stripe);
  if(hit!=checked_.end()) {
    std::rotate(checked_.begin(),hit,hit+1);
    return checked_.front().checksums;
  }

  const Stripe stripe{layout_,index};
  Bytes descriptor(layout_.unit),scratch;
  if(read_descriptor(*backing_,stripe,descriptor,scratch))degraded_=true;
  CheckedStripe loaded{index};
  for(std::uint64_t i=0;i<stripe.count();++i)loaded.checksums[i]=data_digest(descriptor,i);
  std::move_backward(checked_.begin(),checked_.end()-1,checked_.end());
  checked_.front()=loaded;
  return checked_.front().checksums;
}
const Bytes& ProtectedStripeImage::checked_block(std::uint64_t block) const {
  const auto hit=std::ranges::find(blocks_,block,&CheckedBlock::ordinal);
  if(hit!=blocks_.end()) {
    std::rotate(blocks_.begin(),hit,hit+1);
    return blocks_.front().bytes;
  }
  const Stripe stripe{layout_,block/stripe_data_units};
  const auto slot=block%stripe_data_units;
  const auto expected=stripe_checksums(stripe.index)[slot];
  auto& victim=blocks_.back();
  victim.ordinal=UINT64_MAX;
  auto& data=victim.bytes;
  data.resize(layout_.unit);
  if(!read_copy(*backing_,stripe.data_at(slot),data) || record_digest(data)!=expected) {
    Bytes descriptor(layout_.unit),scratch(layout_.unit);
    (void)read_descriptor(*backing_,stripe,descriptor,scratch);
    if(data_digest(descriptor,slot)!=expected)fail("protected stripe descriptor changed");
    recover_data(*backing_,stripe,slot,descriptor,data,scratch);
    degraded_=true;
  }
  victim.ordinal=block;
  std::rotate(blocks_.begin(),blocks_.end()-1,blocks_.end());
  return blocks_.front().bytes;
}

void ProtectedStripeImage::read_exact(std::uint64_t offset,std::span<std::byte> output) const {
  if(offset>extent() || output.size()>extent()-offset)fail("protected image read outside logical bounds");
  while(!output.empty()) {
    const auto& data=checked_block(offset/layout_.unit);
    const auto first=offset%layout_.unit;
    const auto length=(std::min<std::uint64_t>)(output.size(),layout_.unit-first);
    std::ranges::copy(std::span(data).subspan(first,length),output.begin());
    output=output.subspan(length);
    offset+=length;
  }
}
void ProtectedStripeImage::verify_stripes(const StoreByteIo* clean_source) const {
  Bytes descriptor(layout_.unit),data(layout_.unit),scratch(layout_.unit),computed(layout_.unit);
  for(std::uint64_t n=0;n<stripes(layout_);++n) {
    const Stripe stripe{layout_,n};
    bool damaged=read_descriptor(*backing_,stripe,descriptor,scratch);
    if(damaged && clean_source)fail("damaged descriptor in newly sealed image");
    std::ranges::copy(descriptor,computed.begin());
    for(std::uint64_t i=0;i<stripe.count();++i) {
      if(!read_copy(*backing_,stripe.data_at(i),data) || !check_data(data,descriptor,i)) {
        if(damaged || clean_source)fail("damaged data in protected image verification");
        recover_data(*backing_,stripe,i,descriptor,data,scratch);
        damaged=true;
      }
      const auto offset=stripe.logical_at(i);
      const auto length=(std::min)(layout_.unit,layout_.logical_bytes-offset);
      if(!zero(std::span(data).subspan(length)))fail("protected image nonzero tail");
      if(clean_source) {
        clean_source->read_exact(offset,std::span(scratch).first(length));
        if(!std::ranges::equal(std::span(scratch).first(length),std::span(data).first(length)))
          fail("new protected image differs from the source");
      }
      merge_xor(computed,data);
    }
    if(!read_copy(*backing_,stripe.parity_at(),scratch) || scratch!=computed) {
      if(damaged || clean_source)fail("damaged parity in protected image verification");
      damaged=true;
    }
    if(damaged)degraded_=true;
  }
}
void ProtectedStripeImage::verify_all() const {
  checked_.fill({});
  for(auto& block:blocks_)block.ordinal=UINT64_MAX;
  const auto [checked,damaged]=load_roots(*backing_,physical_limit_);
  if(checked!=layout_)fail("protected image roots changed");
  if(damaged)degraded_=true;
  verify_stripes();
}
void ProtectedStripeImage::write_exact(std::uint64_t,std::span<const std::byte>) {fail("protected image is immutable");}
void ProtectedStripeImage::admit_write(std::uint64_t) {fail("protected image is immutable");}
void ProtectedStripeImage::flush() {fail("protected image is immutable");}

}
