#include "native_indexed_volume.hpp"
#include "indexed_names.hpp"
#include "native_lz4_read.hpp"
#include "native_group_codec.hpp"
#include "native_group_read.hpp"
#include "native_object_storage.hpp"
#include "store_codec.hpp"
#include "flitzfs/gpu_anchor_match.hpp"
#include <algorithm>
#include <bit>
#include <iterator>
#include <limits>
#include <map>
#include <new>
#include <utility>

namespace flitzfs::detail {
namespace {
using Bytes=std::vector<std::byte>;
using Code=NativeFsErrorCode;
constexpr std::string_view root_magic="FLTXPV01",extent_magic="FLTXPE01";
constexpr std::string_view member_magic="FLTXPD01",packed_member_magic="FLTXPD02",base_magic="FLTXPA01";
constexpr std::uint64_t namespace_object=1,inode_domain=1ULL<<63;
constexpr std::uint64_t lifetime_domain=2ULL<<61,retirement_domain=5ULL<<61;
constexpr std::uint64_t ordinal_mask=kNativeIndexedInodeLimit-1;
constexpr std::size_t original_root_size=96,lifetime_root_size=120,root_size=kIndexedVolumeRootMaximumBytes;
constexpr auto extent_header=kIndexedExtentHeaderBytes;
constexpr std::size_t base_size=8+kNativeExtentBytes;
constexpr std::size_t packed_slice_size=80;
constexpr std::uint32_t anchor_copy_capability=4,packed_xor_capability=5,modular_delta_capability=6,sparse_xor_capability=7;
constexpr std::uint32_t prefixed_zstd_capability=8,packed_slice_capability=9;
constexpr std::uint32_t directory_shape_feature=1U<<8;
constexpr std::uint32_t directory_backfill_complete_feature=1U<<9;
constexpr std::uint32_t inline_file_feature=1U<<10;
constexpr std::uint32_t namespace_features=kIndexedOrdinalNames|directory_shape_feature|
    directory_backfill_complete_feature|inline_file_feature;
constexpr NativeGroupMethod available_group_method(std::uint32_t capability) noexcept {
  if(capability>=prefixed_zstd_capability)return NativeGroupMethod::prefixed_zstd;
  if(capability>=sparse_xor_capability)return NativeGroupMethod::sparse_xor;
  if(capability>=modular_delta_capability)return NativeGroupMethod::modular_delta;
  if(capability>=packed_xor_capability)return NativeGroupMethod::packed_xor;
  if(capability>=anchor_copy_capability)return NativeGroupMethod::anchor_copies;
  return NativeGroupMethod::aligned_patches;
}
static_assert(kNativeIndexedDataObject==kNativeGroupBaseBit);
constexpr std::size_t maximum_inode_bytes=104+kIndexedNameBytes+kNativeMaxSecurityBytes+
    (std::max)(kIndexedInlineFileBytes,
        kIndexedShapeHeaderBytes+kIndexedShapeBucketBytes*(kIndexedPathUnits+kIndexedPathDepth));

[[noreturn]] void fail(Code code,const char* message){throw NativeFsError(code,message);}
std::uint64_t add(std::uint64_t left,std::uint64_t right) {
  if(right>(std::numeric_limits<std::uint64_t>::max)()-left)fail(Code::corrupt,"indexed volume count overflow");
  return left+right;
}
void put(std::span<std::byte> bytes,std::size_t at,std::uint64_t value,unsigned width=8) {
  for(unsigned i=0;i<width;++i)bytes[at+i]=static_cast<std::byte>(value>>(8*i));
}
std::uint64_t get(std::span<const std::byte> bytes,std::size_t at,unsigned width=8) {
  std::uint64_t value=0;
  for(unsigned i=0;i<width;++i)value|=std::uint64_t(std::to_integer<unsigned char>(bytes[at+i]))<<(8*i);
  return value;
}
bool magic(std::span<const std::byte> bytes,std::string_view expected) noexcept {
  return bytes.size()>=expected.size() &&
      std::string_view(reinterpret_cast<const char*>(bytes.data()),expected.size())==expected;
}
void set_magic(std::span<std::byte> bytes,std::string_view value) {
  for(std::size_t i=0;i<value.size();++i)bytes[i]=static_cast<std::byte>(value[i]);
}
std::uint64_t domain(std::uint64_t id) noexcept {return id&~ordinal_mask;}
bool active_lifetime(IndexedLifetime state) noexcept {
  return state==IndexedLifetime::pending || state==IndexedLifetime::detached;
}
std::uint64_t marker_id(std::uint64_t id,IndexedLifetime state) noexcept {
  if(state==IndexedLifetime::linked)return 0;
  return (state==IndexedLifetime::retiring?retirement_domain:lifetime_domain)|id;
}
std::optional<Bytes> fetch(const PagedObjectStoreSnapshot& store,std::uint64_t id,std::size_t budget,
    std::optional<std::uint64_t> expected_generation={}) {
  try{return expected_generation?store.read_if_generation(id,*expected_generation,budget):
      store.read_if_present(id,budget);}
  catch(const std::bad_alloc&){throw;}
  catch(const std::exception&){fail(Code::storage_failure,"indexed object integrity or read failure");}
}
Bytes required(const PagedObjectStoreSnapshot& store,std::uint64_t id,std::size_t budget) {
  auto bytes=fetch(store,id,budget);
  if(!bytes)fail(Code::corrupt,"indexed object reference missing");
  return std::move(*bytes);
}
std::string name_key(std::string_view path,bool root=false) {
  return indexed_path_name(path,root).key;
}
std::uint64_t index_entries(const IndexedVolumeRoot& root) {
  return add(add(root.inodes-1-root.detached_inodes,root.extents),root.shared_members);
}
NativeIndexKind index_kind(const IndexedVolumeRoot& root) noexcept {
  return root.format>=3?NativeIndexKind::names_extents_and_references:NativeIndexKind::names_and_extents;
}
void validate_header_fields(const IndexedVolumeRoot& root) {
  if(root.directory_backfill_complete && !root.directory_shapes)
    fail(Code::corrupt,"completed indexed backfill lacks shape capability");
  if(root.inline_files && root.format<2)
    fail(Code::corrupt,"inline files require indexed lifetime support");
  if(root.identity==StoreIdentity{} || !root.sequence || root.next_inode<2 ||
      root.next_inode>kNativeIndexedInodeLimit || !root.next_extent ||
      root.next_extent>kNativeIndexedInodeLimit || (!root.workload.enabled && root.workload.committed_input_bytes))
    fail(Code::corrupt,"invalid indexed root identity or allocator");
  const auto non_root=root.inodes?root.inodes-1:0;
  if(root.format<1 || root.format>packed_slice_capability || root.name_profile>kIndexedOrdinalNames ||
      root.active_lifetimes>non_root ||
      root.retiring_inodes>non_root-root.active_lifetimes ||
      root.detached_inodes<root.retiring_inodes ||
      root.detached_inodes-root.retiring_inodes>root.active_lifetimes ||
      (root.format==1 && (root.active_lifetimes || root.detached_inodes || root.retiring_inodes)))
    fail(Code::corrupt,"invalid indexed lifetime counts");
  if(root.shared_members>root.extents || root.shared_bases>root.shared_members ||
      bool(root.shared_bases)!=bool(root.shared_members) ||
      (root.format<3 && (root.shared_bases || root.shared_members)))
    fail(Code::corrupt,"invalid indexed shared-base counts");
}
IndexedVolumeRoot decode_root(std::span<const std::byte> bytes) {
  if((bytes.size()!=original_root_size && bytes.size()!=lifetime_root_size && bytes.size()!=root_size) ||
      !magic(bytes,root_magic) ||
      get(bytes,12,4)>1 || get(bytes,88,4)!=kNativeExtentBytes || (get(bytes,92,4)&~std::uint64_t(namespace_features)))
    fail(Code::corrupt,"unsupported indexed native volume root");
  IndexedVolumeRoot root;
  root.format=static_cast<std::uint32_t>(get(bytes,8,4));
  const auto features=static_cast<std::uint32_t>(get(bytes,92,4));
  root.name_profile=features&kIndexedOrdinalNames;
  root.directory_shapes=(features&directory_shape_feature)!=0;
  root.directory_backfill_complete=(features&directory_backfill_complete_feature)!=0;
  root.inline_files=(features&inline_file_feature)!=0;
  const auto expected_size=root.format==1?original_root_size:root.format==2?lifetime_root_size:root_size;
  if(root.format<1 || root.format>packed_slice_capability || bytes.size()!=expected_size)
    fail(Code::corrupt,"indexed native volume capability and length differ");
  if(root.format>=2) {
    root.active_lifetimes=get(bytes,96);root.detached_inodes=get(bytes,104);root.retiring_inodes=get(bytes,112);
  }
  if(root.format>=3) {root.shared_bases=get(bytes,120);root.shared_members=get(bytes,128);}
  std::copy_n(bytes.begin()+kIndexedVolumeIdentityOffset,16,root.identity.begin());
  root.sequence=get(bytes,16);root.next_inode=get(bytes,24);root.next_extent=get(bytes,32);
  root.inodes=get(bytes,40);root.extents=get(bytes,48);root.charged_extents=get(bytes,56);
  root.workload={get(bytes,12,4)==1,get(bytes,64)};
  return root;
}
Bytes encode_root(const IndexedVolumeRoot& root) {
  Bytes bytes(root.format==1?original_root_size:root.format==2?lifetime_root_size:root_size);
  set_magic(bytes,root_magic);put(bytes,8,root.format,4);put(bytes,12,root.workload.enabled,4);
  if(root.format>=2) {
    put(bytes,96,root.active_lifetimes);put(bytes,104,root.detached_inodes);put(bytes,112,root.retiring_inodes);
  }
  if(root.format>=3) {put(bytes,120,root.shared_bases);put(bytes,128,root.shared_members);}
  put(bytes,16,root.sequence);put(bytes,24,root.next_inode);put(bytes,32,root.next_extent);
  put(bytes,40,root.inodes);put(bytes,48,root.extents);put(bytes,56,root.charged_extents);
  put(bytes,64,root.workload.committed_input_bytes);
  std::copy(root.identity.begin(),root.identity.end(),bytes.begin()+kIndexedVolumeIdentityOffset);
  const auto features=root.name_profile|(root.directory_shapes?directory_shape_feature:0)|
      (root.directory_backfill_complete?directory_backfill_complete_feature:0)|
      (root.inline_files?inline_file_feature:0);
  put(bytes,88,kNativeExtentBytes,4);put(bytes,92,features,4);
  return bytes;
}
IndexedInodeInput decode_inode_with_lifetime(const PagedObjectStoreSnapshot& store,const IndexedVolumeRoot& root,
    std::uint64_t id,std::span<const std::byte> bytes) {
  if(bytes.size()>maximum_inode_bytes)fail(Code::corrupt,"indexed inode frame exceeds its bound");
  auto inode=decode_indexed_inode(bytes,id,root);
  if(inode.lifetime!=IndexedLifetime::linked) {
    const auto active=fetch(store,lifetime_domain|id,1);
    const auto retirement=fetch(store,retirement_domain|id,1);
    if(active && retirement)fail(Code::corrupt,"indexed inode has two lifetime markers");
    if(active && *active==Bytes{static_cast<std::byte>(inode.lifetime)})return inode;
    if(!active && inode.lifetime==IndexedLifetime::detached &&
        retirement && *retirement==Bytes{static_cast<std::byte>(IndexedLifetime::retiring)}) {
      inode.lifetime=IndexedLifetime::retiring;
      return inode;
    }
    fail(Code::corrupt,"indexed inode and lifetime marker disagree");
  }
  return inode;
}
IndexedInodeInput load_inode(const PagedObjectStoreSnapshot& store,const IndexedVolumeRoot& root,std::uint64_t id) {
  if(!id || id>=root.next_inode)fail(Code::not_found,"indexed inode is outside the namespace");
  const auto bytes=fetch(store,inode_domain|id,maximum_inode_bytes);
  if(!bytes)fail(Code::not_found,"indexed inode does not exist");
  return decode_inode_with_lifetime(store,root,id,*bytes);
}
std::uint64_t extent_begin(const IndexedInodeInput& inode,std::uint64_t block) {
  if(inode.directory || !inode.size || block>(inode.size-1)/kNativeExtentBytes)
    fail(Code::corrupt,"indexed extent outside its file");
  return block*kNativeExtentBytes;
}
void validate_payload(const IndexedInodeInput& inode,std::uint64_t block,std::span<const std::byte> payload) {
  if(!inode.inline_data.empty())fail(Code::corrupt,"inline inode has an external extent");
  const auto begin=extent_begin(inode,block);
  if(payload.empty() || payload.size()>kNativeExtentBytes || payload.size()>inode.size-begin ||
      payload.back()==std::byte{0})
    fail(Code::corrupt,"invalid indexed extent bytes");
}
Bytes encode_extent(const IndexedInodeInput& inode,const IndexedExtentInput& extent) {
  validate_payload(inode,extent.block,extent.bytes);
  Bytes bytes(extent_header+extent.bytes.size());
  set_magic(bytes,extent_magic);put(bytes,8,inode.inode);put(bytes,16,extent.block);
  std::copy(extent.bytes.begin(),extent.bytes.end(),bytes.begin()+extent_header);
  return bytes;
}
std::span<const std::byte> unique_extent_candidate(std::span<const NativeLz4ExtentCandidate> candidates,
    std::uint64_t block) noexcept {
  const NativeLz4ExtentCandidate* found=nullptr;
  for(const auto& candidate:candidates)if(candidate.extent_index==block) {
    if(found)return {};
    found=&candidate;
  }
  return found?found->payload:std::span<const std::byte>{};
}
void apply_extent_candidate(StoreMutation& mutation,std::span<const std::byte> candidate) {
  if(candidate.empty() || candidate.size()>=mutation.bytes.size()-extent_header || !store_lz4_available())return;
  try {
    mutation.lz4_candidate.assign(candidate.begin(),candidate.end());
    mutation.lz4_literal_prefix_bytes=extent_header;
  }catch(const std::bad_alloc&) {
    // Optional acceleration leaves the authoritative CPU frame available.
  }
}
struct DecodedIndexedExtent {
  Bytes data;
  std::size_t payload_offset=0;
  std::uint64_t base=0,reusable_anchor_generation=0;
  bool packed_slice=false,tiled_group=false;
  std::span<const std::byte> payload() const {return std::span(data).subspan(payload_offset);}
};
void verify_base_frame(const Bytes& bytes) {
  if(bytes.size()!=base_size || !magic(bytes,base_magic))
    fail(Code::corrupt,"invalid indexed shared base");
}
struct IndexedPackedSlice {
  std::uint64_t base,generation;
  std::size_t offset,length;
};
IndexedPackedSlice read_packed_slice(std::span<const std::byte> frame) {
  if(frame.size()!=packed_slice_size)fail(Code::corrupt,"indexed packed slice field length");
  const IndexedPackedSlice slice{get(frame,24),get(frame,32),
      static_cast<std::size_t>(get(frame,40,4)),static_cast<std::size_t>(get(frame,44,4))};
  if(slice.base<=kNativeIndexedDataObject || domain(slice.base)!=kNativeIndexedDataObject || !slice.generation ||
      !slice.length || slice.offset>=kNativeExtentBytes || slice.length>kNativeExtentBytes-slice.offset)
    fail(Code::corrupt,"indexed packed slice identity or range");
  return slice;
}
std::span<const std::byte> checked_packed_payload(std::span<const std::byte> frame,
    const IndexedPackedSlice& slice,std::span<const std::byte> base) {
  const auto data=base.subspan(slice.offset,slice.length);
  if(!std::ranges::equal(record_digest(data),frame.subspan(48)))
    fail(Code::corrupt,"indexed packed slice digest differs");
  return data;
}
Bytes make_packed_slice(NativeExtentSelection extent,std::size_t offset,
    std::span<const std::byte> expected,std::span<const std::byte> base) {
  Bytes record(packed_slice_size);
  set_magic(record,packed_member_magic);
  put(record,8,extent.inode);
  put(record,16,extent.extent_index);
  put(record,24,kNativeGroupBaseBit+1);
  put(record,32,1);
  put(record,40,offset,4);
  put(record,44,expected.size(),4);
  const auto digest=record_digest(expected);
  std::copy(digest.begin(),digest.end(),record.begin()+48);
  const auto slice=read_packed_slice(record);
  if(!std::ranges::equal(checked_packed_payload(record,slice,base),expected))
    fail(Code::corrupt,"indexed packed slice private CPU comparison failed");
  return record;
}
DecodedIndexedExtent decode_extent(const PagedObjectStoreSnapshot& source,const NativeNameIndex& index,
    const IndexedVolumeRoot& root,std::uint64_t object,Bytes bytes,const IndexedInodeInput& inode,
    std::uint64_t block,bool compare_anchor=false,IndexedSharedBaseRead* cached_base=nullptr,
    IndexedSharedBaseCache* retained=nullptr) {
  if(bytes.size()<=extent_header || get(bytes,8)!=inode.inode || get(bytes,16)!=block)
    fail(Code::corrupt,"indexed extent owner or logical offset mismatch");
  if(magic(bytes,extent_magic)) {
    validate_payload(inode,block,std::span(bytes).subspan(extent_header));
    return {std::move(bytes),extent_header,0};
  }
  const bool packed=magic(bytes,packed_member_magic);
  if(packed?root.format<packed_slice_capability:root.format<3 || !magic(bytes,member_magic))
    fail(Code::corrupt,"unsupported indexed extent frame");
  const auto member=std::span<const std::byte>(bytes).subspan(extent_header);
  try {
    const auto maximum_method=available_group_method(root.format);
    std::optional<IndexedPackedSlice> slice;
    if(packed)slice=read_packed_slice(bytes);
    const auto reference=slice?NativeGroupReference{slice->base,slice->generation}:read_native_group_header(member,maximum_method);
    if(reference.base_id==object || (reference.base_id&ordinal_mask)>=root.next_extent ||
        index.find(native_group_reference_key(reference.base_id,object))!=object)
      fail(Code::corrupt,"indexed shared-base backreference is missing");
    IndexedSharedBaseRead local_base;
    auto& scratch=cached_base?*cached_base:local_base;
    const auto* anchor=retained?retained->find(reference.base_id,reference.base_generation):nullptr;
    if(!anchor) {
      if(scratch.object!=reference.base_id || scratch.generation!=reference.base_generation) {
        auto frame=fetch(source,reference.base_id,base_size,reference.base_generation);
        if(!frame)fail(Code::corrupt,"indexed shared-base generation mismatch");
        verify_base_frame(*frame);
        scratch={reference.base_id,reference.base_generation,NativeGroupBase(std::move(*frame),base_magic.size())};
      }
      anchor=&scratch;
    }
    if(!anchor->base)fail(Code::corrupt,"indexed shared-base frame missing");
    if(slice) {
      const auto anchor_payload=anchor->base->payload();
      if(anchor_payload.size()!=kNativeExtentBytes)fail(Code::corrupt,"indexed shared-base size differs");
      const auto data=checked_packed_payload(bytes,*slice,anchor_payload);
      validate_payload(inode,block,data);
      Bytes decoded(data.begin(),data.end());
      if(retained && anchor==&scratch)retained->remember(scratch);
      return {std::move(decoded),0,reference.base_id,0,true};
    }
    auto decoded=decode_native_group_member(member,*anchor->base,maximum_method);
    const auto reusable=compare_anchor && std::ranges::equal(decoded,anchor->base->payload())?anchor->generation:0;
    while(!decoded.empty() && decoded.back()==std::byte{})decoded.pop_back();
    validate_payload(inode,block,decoded);
    if(retained && anchor==&scratch)retained->remember(scratch);
    return {std::move(decoded),0,reference.base_id,reusable,false,native_group_tiled(reference.method)};
  }catch(const std::bad_alloc&){throw;}
  catch(const NativeFsError&){throw;}
  catch(const std::exception&){fail(Code::corrupt,"indexed shared-base proof failed");}
}
Bytes full_extent(DecodedIndexedExtent extent) {
  auto bytes=std::move(extent.data);
  bytes.erase(bytes.begin(),bytes.begin()+static_cast<std::ptrdiff_t>(extent.payload_offset));
  bytes.resize(kNativeExtentBytes);
  return bytes;
}
using RetiredGroupReferences=std::map<std::uint64_t,std::uint64_t>;

void retire_group_reference(std::uint64_t base,std::uint64_t member,IndexedVolumeRoot& root,
    RetiredGroupReferences& retired,std::vector<NativeNameEdit>& edits) {
  if(!root.shared_members)fail(Code::corrupt,"missing indexed shared member count");
  edits.push_back({native_group_reference_key(base,member),member,{}});
  ++retired[base];
  --root.shared_members;
}
void remove_unreferenced_bases(const NativeNameIndex& index,const RetiredGroupReferences& retired,
    IndexedVolumeRoot& root,std::vector<StoreMutation>& records) {
  for(const auto& [base,removed]:retired) {
    const auto count=index.group_reference_count(base);
    if(count<removed)fail(Code::corrupt,"indexed group reference count underflow");
    if(count==removed) {
      if(!root.shared_bases)fail(Code::corrupt,"missing indexed shared base count");
      records.push_back({base,true,{}});
      --root.shared_bases;
    }
  }
}

NativeFileInfo file_info(const IndexedInodeInput& inode,std::string path) {
  return {inode.inode,inode.parent,inode.size,inode.born,inode.modified,inode.directory,std::move(path)};
}
std::uint64_t reserved_extents(const IndexedInodeInput& inode) {
  return inode.allocation && *inode.allocation ? 1+(*inode.allocation-1)/kNativeExtentBytes : 0;
}
std::uint64_t inode_charge(const IndexedInodeInput& inode) {
  return inode.allocation?reserved_extents(inode):!inode.inline_data.empty();
}
} // namespace


namespace {

void subtract(std::uint64_t& count,std::uint64_t removed=1) {
  if(removed>count)fail(Code::corrupt,"indexed volume count underflow");
  count-=removed;
}

// Leave record slots for adjacent index pages, the checked inode and roots.
// This limits one retirement transaction, independent of total file size.
constexpr std::size_t retirement_quantum=kStoreMaxObjects/4;

IndexedPathBound child_path_bound(std::string_view name,IndexedPathBound suffix) {
  const auto units=indexed_utf16_length(name)+(suffix.units?1+suffix.units:0);
  const auto components=1+suffix.components;
  if(!units || units>kIndexedPathUnits || components>static_cast<int>(kIndexedPathDepth))
    fail(Code::corrupt,"indexed child exceeds directory shape bounds");
  return {static_cast<std::uint16_t>(units),static_cast<std::uint16_t>(components)};
}

std::optional<IndexedPathBound> child_path_bound(const IndexedInodeInput& inode) {
  if(inode.directory && !inode.shape)return {};
  return child_path_bound(inode.name,inode.shape?inode.shape->bound():IndexedPathBound{});
}

class IndexedVolumeTransaction final {
 public:
  IndexedVolumeTransaction(const PagedObjectStoreSnapshot& source,const IndexedVolumeRoot& expected,
      IndexedContentWatch watched)
      :source_(source),before_(expected),next_(expected),index_(source),watched_(watched) {
    if(decode_root(required(source_,namespace_object,root_size))!=before_ ||
        index_.kind()!=index_kind(before_) || index_.size()!=index_entries(before_))
      fail(Code::corrupt,"indexed transaction source changed");
    if(next_.sequence==(std::numeric_limits<std::uint64_t>::max)())
      fail(Code::limit,"indexed sequence exhausted");
    ++next_.sequence;
  }

  void apply(const NativeFileMutation& op) {
    const auto key=name_key(op.path);
    const auto write=op.operation==NativeFileOperation::write;
    const auto truncate=op.operation==NativeFileOperation::truncate;
    const auto rename=op.operation==NativeFileOperation::rename;
    if((!write && (op.offset || !op.data.empty())) || (!truncate && op.size) ||
        (!rename && (!op.target.empty() || op.replace)))
      fail(Code::invalid_operation,"unused indexed mutation fields");
    switch(op.operation) {
      case NativeFileOperation::create_file:
      case NativeFileOperation::create_directory:
        create(op.path,op.operation==NativeFileOperation::create_directory);
        return;
      case NativeFileOperation::write: write_file(resolve(key),op.offset,op.data);return;
      case NativeFileOperation::truncate: truncate_file(resolve(key),op.size);return;
      case NativeFileOperation::rename: move(resolve(key),op);return;
      case NativeFileOperation::remove: {
        const auto n=resolve(key);
        if(n.lifetime==IndexedLifetime::pending)
          fail(Code::invalid_operation,"indexed removal needs the pending inode's lifetime transaction");
        discard(n);
        return;
      }
    }
    fail(Code::invalid_operation,"unknown indexed mutation");
  }

  void apply(const NativeBasicMutation& op) {
    const auto n=metadata_inode(op.inode,op.created_ordinal);
    if(bool(op.value.attributes&0x10U)!=n.directory)
      fail(Code::invalid_operation,"indexed basic metadata type mismatch");
    if(n.basic!=op.value)edit(n.inode).basic=op.value;
  }

  void apply(const NativeSecurityMutation& op) {
    const auto n=metadata_inode(op.inode,op.created_ordinal);
    if(n.security!=op.value)edit(n.inode).security=op.value;
  }

  NativeInodeStorageMutationCommit write_inode(PagedObjectStoreWriter& writer,IndexedVolumeRoot& current,
      std::uint64_t id,std::uint64_t offset,std::span<const std::byte> bytes,NativeInodeWriteOptions options,
      const NativeAutomaticMetadata& effect,std::span<const NativeLz4ExtentCandidate> candidates) {
    const auto inode=data_inode(id);
    NativeInodeStorageMutationCommit result;
    result.offset=options.append?inode.size:offset;
    auto count=bytes.size();
    if(options.constrained) {
      const auto remaining=result.offset>=inode.size?0:inode.size-result.offset;
      count=static_cast<std::size_t>((std::min)(std::uint64_t(count),remaining));
    }
    write_file(inode,result.offset,bytes.first(count));
    if(count)apply_automatic(inode,effect);
    result.bytes=static_cast<std::uint32_t>(count);
    result.state=staged_allocation(inode);
    result.transaction=finish(writer,current,id,candidates);
    return result;
  }

  NativeInodeStorageMutationCommit truncate_inode(PagedObjectStoreWriter& writer,IndexedVolumeRoot& current,
      std::uint64_t id,std::uint64_t size,const NativeAutomaticMetadata& effect) {
    const auto inode=data_inode(id);
    truncate_file(inode,size);
    apply_automatic(inode,effect);
    NativeInodeStorageMutationCommit result;
    result.state=staged_allocation(inode);
    result.transaction=finish(writer,current);
    return result;
  }

  void set_delete(std::uint64_t id,bool requested) {
    require_lifetimes();
    const auto n=node(id);
    if(id==1 || indexed_detached(n.lifetime))
      fail(Code::invalid_operation,"indexed delete intent requires a linked non-root inode");
    const auto phase=requested?IndexedLifetime::pending:IndexedLifetime::linked;
    if(n.lifetime==phase)return;
    if(requested)require_empty_directory(n);
    edit(id).lifetime=phase;
  }

  void unlink(std::uint64_t id) {
    require_lifetimes();
    const auto n=node(id);
    if(n.lifetime==IndexedLifetime::detached)return;
    if(n.lifetime!=IndexedLifetime::pending)
      fail(Code::invalid_operation,"indexed unlink requires durable delete intent");
    require_empty_directory(n);
    detach(n,IndexedLifetime::detached);
  }

  void retire(std::uint64_t id) {
    require_lifetimes();
    const auto n=node(id);
    if(n.lifetime!=IndexedLifetime::detached)
      fail(Code::invalid_operation,"indexed retirement requires a detached inode");
    discard(n);
  }

  void finish_context(std::uint64_t id) {
    const auto n=node(id);
    if(!active_lifetime(n.lifetime))fail(Code::corrupt,"indexed lifetime record has no active owner");
    discard(n);
  }

  void reclaim(std::uint64_t id) {
    const auto n=node(id,true);
    if(n.lifetime!=IndexedLifetime::retiring)
      fail(Code::corrupt,"indexed retirement record has no retired owner");
    (void)edit(id);
    const auto parent=native_extent_key(id,0).parent;
    const auto page=index_.list(parent,{},retirement_quantum+1);
    for(std::size_t i=0;i<(std::min)(retirement_quantum,page.size());++i)
      clear_extent(load_extent(id,native_extent_block(page[i].key.name)));
    if(page.size()<=retirement_quantum)nodes_.at(id).value.reset();
  }

  void reserve(std::uint64_t id,std::uint64_t bytes) {
    const auto n=data_inode(id);
    if(bytes%kNativeAllocationUnit)
      fail(Code::invalid_operation,"indexed reservation is not allocation-unit aligned");
    if(bytes<n.size)truncate_file(n,bytes);
    if(n.allocation!=bytes)edit(id).allocation=bytes;
  }

  void overwrite(std::uint64_t id,const NativeOverwriteMetadata& effect) {
    const auto n=data_inode(id);
    if(n.lifetime!=IndexedLifetime::linked || !n.basic)
      fail(Code::invalid_operation,"indexed overwrite needs a live file with basic metadata");
    auto basic=*n.basic;
    basic.attributes=(effect.replace_attributes?effect.attributes:(basic.attributes|effect.attributes))|0x20U;
    basic.last_access_time=basic.last_write_time=basic.change_time=effect.utc_time;
    truncate_file(n,0);
    edit(id).basic=basic;
  }

  void cleanup(std::uint64_t id,bool deleting,bool initial,const NativeCleanupMetadata& effect) {
    const auto n=node(id);
    apply_automatic(n,effect);
    if(initial) {
      require_lifetimes();
      if(id==1)fail(Code::invalid_operation,"indexed root cannot have initial delete intent");
      if(n.directory && has_children(id)) {
        if(n.lifetime==IndexedLifetime::pending)
          fail(Code::corrupt,"indexed nonempty directory has durable delete intent");
        return;
      }
      if(!indexed_detached(n.lifetime))set_delete(id,true);
    }
    if(deleting)unlink(id);
  }

  void trim_reservation(std::uint64_t id) {
    const auto n=node(id);
    if(n.directory)return;
    if(!n.allocation)fail(Code::invalid_operation,"indexed Cleanup trim needs a tracked reservation");
    reserve(id,aligned_size(n.size));
  }

  std::uint64_t created_inode() const {
    if(created_.size()!=1)fail(Code::invalid_operation,"indexed creation must select exactly one inode");
    return created_.front();
  }

  NativeInodeAllocationInfo allocation(std::uint64_t id) const {
    return indexed_inode_allocation(node(id));
  }

  void automatic(std::uint64_t id,const NativeCleanupMetadata& effect) {
    apply_automatic(node(id),effect);
  }

  void enable_workload() noexcept {next_.workload.enabled=true;}

  NativeDirectoryBackfillStep prepare_directory_backfill(IndexedDirectoryBackfill& work) {
    using Step=NativeDirectoryBackfillStep;
    if(before_.directory_backfill_complete)return Step::idle;
    if(work.stack.empty())work.stack.push_back({1});
    auto& frame=work.stack.back();
    IndexedInodeInput directory;
    try {directory=node(frame.inode);}
    catch(const NativeFsError& error) {
      if(error.code()!=Code::not_found)throw;
      if(frame.inode==1)fail(Code::corrupt,"indexed backfill has no root directory");
      work.stack.pop_back();
      return Step::scanning;
    }
    if(indexed_detached(directory.lifetime) || (directory.shape && directory.inode!=1)) {
      work.stack.pop_back();
      return Step::scanning;
    }
    if(!directory.directory)fail(Code::corrupt,"indexed backfill inode is a regular file");
    if(directory.shape) {
      next_.directory_backfill_complete=true;
      return Step::published;
    }
    if(frame.revision!=directory.modified)frame={directory.inode,directory.modified};
    if(frame.ready) {
      edit(directory.inode).shape=frame.shape;
      next_.directory_shapes=true;
      next_.directory_backfill_complete=directory.inode==1;
      return Step::published;
    }
    constexpr std::size_t rows_per_opportunity=64;
    const auto rows=index_.list(directory.inode,frame.after,rows_per_opportunity);
    if(!rows.empty() && directory.lifetime!=IndexedLifetime::linked)
      fail(Code::corrupt,"indexed backfill found children of a pending directory");
    for(const auto& row:rows) {
      const auto child=linked_inode(row.key,row.value);
      const auto contribution=child_path_bound(child);
      if(!contribution) {
        if(work.stack.size()==kIndexedPathDepth+1)work.stack.clear(); // An ancestry may have moved.
        else work.stack.push_back({child.inode,child.modified});
        return Step::scanning;
      }
      frame.shape.replace({},contribution);
      frame.after=row.key.name;
    }
    frame.ready=rows.size()<rows_per_opportunity;
    return Step::scanning;
  }

  NativeFsCommit finish(PagedObjectStoreWriter& writer,IndexedVolumeRoot& current,
      std::uint64_t advised_inode=0,std::span<const NativeLz4ExtentCandidate> candidates={}) {
    NativeFsCommit result;
    result.created_inodes=std::move(created_);
    if(nodes_.empty() && next_.workload==before_.workload &&
        next_.directory_backfill_complete==before_.directory_backfill_complete) {
      result.storage=native_paged_info(healthy_checkpoint());result.sequence=before_.sequence;
      return result;
    }

    std::vector<StoreMutation> records;
    std::vector<NativeNameEdit> edits;
    RetiredGroupReferences retired;
    bool content_changed=false,needs_scan=false;
    if(next_.inline_files) {
      for(auto& [id,changed]:nodes_)
        if(changed.value && !changed.value->directory)prepare_inline_data(id,*changed.value);
    }
    for(const auto& [key,replacement]:names_) {
      const auto expected=index_.find(key);
      if(expected!=replacement)edits.push_back({key,expected,replacement});
    }
    for(auto& [key,extent]:extents_) {
      if(!extent.dirty)continue;
      const auto [id,block]=key;
      if(extent.original || !extent.payload.empty()) {
        content_changed=true;
        watch_selection([&](const auto& preparation){return preparation.watches_extent(id,block);});
      }
      auto& changed=nodes_.at(id);
      if(extent.base) {
        needs_scan=true; // The group's representative can change.
        retire_group_reference(extent.base,*extent.original,next_,retired,edits);
      }
      if(extent.payload.empty()) {
        if(!extent.original)continue;
        records.push_back({*extent.original,true,{}});
        edits.push_back({native_extent_key(id,block),extent.original,{}});
        subtract(next_.extents);
        ++changed.removed_extents;
      } else {
        const auto& n=*changed.value;
        auto object=extent.original;
        if(!object) {
          if(next_.next_extent==kNativeIndexedInodeLimit)fail(Code::limit,"indexed extent identity exhausted");
          object=kNativeIndexedDataObject|next_.next_extent++;
          edits.push_back({native_extent_key(id,block),{},object});
          ++next_.extents;
          ++changed.added_extents;
        }
        StoreMutation mutation{*object,false,encode_extent(n,{id,block,extent.payload})};
        if(id==advised_inode)
          apply_extent_candidate(mutation,unique_extent_candidate(candidates,block));
        records.push_back(std::move(mutation));
      }
    }
    for(const auto& [id,changed]:nodes_) {
      if(changed.value) {
        records.push_back({inode_domain|id,false,encode_indexed_inode(*changed.value,next_)});
        if(!changed.original_size)++next_.inodes;
      } else if(changed.original_size) {
        records.push_back({inode_domain|id,true,{}});
        subtract(next_.inodes);
      }
      update_charge(id,changed);
      const auto phase=changed.value?changed.value->lifetime:IndexedLifetime::linked;
      if(!changed.value || phase!=changed.original_lifetime) {
        content_changed=true;
        needs_scan=true;
        watch_selection([&](const auto& preparation){return preparation.watches_inode(id);});
      }
      if(phase!=changed.original_lifetime) {
        const auto original=marker_id(id,changed.original_lifetime);
        const auto current_marker=marker_id(id,phase);
        if(original && original!=current_marker)records.push_back({original,true,{}});
        if(current_marker)records.push_back({current_marker,false,Bytes{static_cast<std::byte>(phase)}});
        remove_lifetime(changed.original_lifetime);
        add_lifetime(phase);
      }
    }
    remove_unreferenced_bases(index_,retired,next_,records);
    validate_header_fields(next_);
    auto count=index_.size();
    for(const auto& edit:edits) {
      if(!edit.expected)++count;
      if(!edit.replacement)subtract(count);
    }
    if(count!=index_entries(next_) || next_.charged_extents<next_.extents)
      fail(Code::corrupt,"indexed transaction produced inconsistent counts");
    records.push_back({namespace_object,false,encode_root(next_)});
    if(records.size()>kStoreMaxObjects)fail(Code::limit,"indexed record transaction budget");
    std::size_t input=0;
    for(const auto& record:records) {
      if(record.bytes.size()>kStoreMaxTransactionBytes-input)
        fail(Code::limit,"indexed record transaction input budget");
      input+=record.bytes.size();
    }
    (void)healthy_checkpoint();
    try {
      result.storage=native_paged_info(edits.empty()?writer.commit(records):
          NativeNameIndex::commit(writer,edits,std::move(records)));
    }catch(const NativeFsError&){throw;}
    catch(const PagedStoreCapacityError& error) {
      if(writer.poisoned())fail(Code::storage_failure,"indexed publication failed after admission");
      throw NativeFsError(Code::limit,error.what(),error.append_shortfall_bytes());
    }catch(const std::bad_alloc&) {
      if(writer.poisoned())fail(Code::storage_failure,"indexed publication failed after admission");
      throw;
    }catch(const std::exception&){fail(Code::storage_failure,"indexed storage commit failed");}
    result.sequence=next_.sequence;
    current=next_;
    if(content_changed) {
      watched_.sequence=result.sequence;
      if(needs_scan)watched_.changes.require_scan(result.sequence);
      else for(const auto& [key,extent]:extents_) {
        if(extent.dirty && (extent.original || !extent.payload.empty()))
          watched_.changes.record({key.first,key.second},result.sequence);
      }
    }
    for(const auto& watch:watched_.preparations) {
      if(watch.changed)
        if(const auto preparation=watch.preparation.lock())preparation->invalidate();
    }
    return result;
  }

 private:
  template<class Matches>
  void watch_selection(Matches matches) noexcept {
    for(auto& watch:watched_.preparations) {
      if(!watch.changed)
        if(const auto preparation=watch.preparation.lock())watch.changed=matches(*preparation);
    }
  }

  struct InodeEdit {
    std::optional<IndexedInodeInput> value;
    std::optional<std::uint64_t> original_size,original_allocation;
    std::uint64_t original_reserved=0;
    IndexedLifetime original_lifetime=IndexedLifetime::linked;
    bool original_inline=false;
    std::uint64_t added_extents=0,removed_extents=0;
  };
  struct ExtentEdit {
    std::optional<std::uint64_t> original;
    Bytes payload;
    std::uint64_t base=0;
    bool dirty=false;
  };
  const PagedObjectStoreSnapshot& source_;
  IndexedVolumeRoot before_,next_;
  NativeNameIndex index_;
  IndexedContentWatch watched_;
  std::map<std::uint64_t,InodeEdit> nodes_;
  std::map<NativeNameKey,std::optional<std::uint64_t>> names_;
  std::map<std::pair<std::uint64_t,std::uint64_t>,ExtentEdit> extents_;
  std::vector<std::uint64_t> created_;
  std::size_t payload_bytes_=0;

  PagedStoreInfo healthy_checkpoint() const {
    const auto info=source_.info();
    if(info.degraded_roots || info.degraded_metadata)
      fail(Code::storage_failure,"indexed transaction encountered degraded metadata");
    if(info.degraded_payloads)
      fail(Code::replica_repair_pending,"indexed transaction needs verified payload repair");
    return info;
  }

  void require_lifetimes() const {
    if(next_.format<2)fail(Code::invalid_operation,"indexed lifetime capability is absent");
  }

  void update_charge(std::uint64_t id,const InodeEdit& n) {
    if(n.original_allocation || (n.value && n.value->allocation)) {
      const auto original=n.original_allocation?n.original_reserved:
          n.original_size?(n.original_inline?1:index_.extent_count(id)):0;
      subtract(next_.charged_extents,original);
      if(n.value && n.value->allocation)
        next_.charged_extents=add(next_.charged_extents,reserved_extents(*n.value));
      else if(n.value)
        fail(Code::corrupt,"indexed reservation was cleared on a retained inode");
    } else {
      subtract(next_.charged_extents,n.removed_extents+n.original_inline);
      next_.charged_extents=add(next_.charged_extents,n.added_extents+(n.value && !n.value->inline_data.empty()));
    }
  }

  void remove_lifetime(IndexedLifetime lifetime) {
    if(active_lifetime(lifetime))subtract(next_.active_lifetimes);
    if(indexed_detached(lifetime))subtract(next_.detached_inodes);
    if(lifetime==IndexedLifetime::retiring)subtract(next_.retiring_inodes);
  }

  void add_lifetime(IndexedLifetime lifetime) noexcept {
    if(active_lifetime(lifetime))++next_.active_lifetimes;
    if(indexed_detached(lifetime))++next_.detached_inodes;
    if(lifetime==IndexedLifetime::retiring)++next_.retiring_inodes;
  }

  IndexedInodeInput node(std::uint64_t id,bool allow_retiring=false) const {
    const auto staged=nodes_.find(id);
    if(staged!=nodes_.end() && !staged->second.value)
      fail(Code::not_found,"indexed inode was removed in this transaction");
    auto n=staged==nodes_.end()?load_inode(source_,before_,id):*staged->second.value;
    if(n.lifetime==IndexedLifetime::retiring && !allow_retiring)
      fail(Code::not_found,"indexed inode no longer accepts file operations");
    return n;
  }

  IndexedInodeInput data_inode(std::uint64_t id) const {
    auto n=node(id);
    if(n.directory)fail(Code::is_directory,"indexed inode data mutation needs a regular file");
    if(!indexed_detached(n.lifetime) && binding({n.parent,name_key(n.name)})!=id)
      fail(Code::corrupt,"indexed inode lost its live binding");
    return n;
  }

  void apply_automatic(const IndexedInodeInput& inode,const NativeAutomaticMetadata& effect) {
    if(const auto basic=updated_automatic_basic(inode.basic,inode.directory,effect))
      if(inode.basic!=*basic)edit(inode.inode).basic=*basic;
  }

  NativeInodeAllocationInfo staged_allocation(const IndexedInodeInput& before) const noexcept {
    const auto pending=nodes_.find(before.inode);
    return indexed_inode_allocation(pending==nodes_.end()?before:*pending->second.value);
  }

  IndexedInodeInput linked_inode(const NativeNameKey& key,std::uint64_t id) const {
    try {
      auto n=node(id);
      if(indexed_detached(n.lifetime) || n.parent!=key.parent || name_key(n.name)!=key.name)
        fail(Code::corrupt,"indexed transaction name and inode disagree");
      return n;
    }catch(const NativeFsError& error) {
      if(error.code()==Code::not_found)fail(Code::corrupt,"indexed transaction name has no inode");
      throw;
    }
  }

  IndexedInodeInput resolve(std::string_view key,
      NativeFsError::MissingPath last=NativeFsError::MissingPath::name) const {
    auto n=node(1);
    for(std::size_t begin=0;begin<key.size();) {
      if(!n.directory)fail(Code::not_directory,"indexed path ancestor is a file");
      const auto separator=key.find('/',begin);
      const auto end=separator==std::string_view::npos?key.size():separator;
      const NativeNameKey component{n.inode,std::string(key.substr(begin,end-begin))};
      const auto id=binding(component);
      if(!id)throw NativeFsError(separator==std::string_view::npos?last:NativeFsError::MissingPath::parent,
          "indexed path not found");
      n=linked_inode(component,*id);
      begin=end+1;
    }
    return n;
  }

  std::optional<std::uint64_t> binding(const NativeNameKey& key) const {
    const auto staged=names_.find(key);
    return staged==names_.end()?index_.find(key):staged->second;
  }

  NativeNameKey destination(std::string_view path,std::string& spelling) {
    const auto parsed=indexed_path_name(path);
    const auto& key=parsed.key;
    const auto slash=key.rfind('/');
    const auto start=slash==std::string::npos?0:slash+1;
    auto parent=resolve(start?std::string_view(key).substr(0,slash):std::string_view{},
        NativeFsError::MissingPath::parent);
    if(!parent.directory)fail(Code::not_directory,"indexed destination parent is a file");
    if(parent.lifetime==IndexedLifetime::pending)
      fail(Code::invalid_operation,"indexed destination directory has pending delete intent");
    const auto display_slash=path.rfind('/');
    spelling=path.substr(display_slash==std::string_view::npos?0:display_slash+1);
    if(parsed.extended)next_.name_profile=kIndexedOrdinalNames;
    return {parent.inode,key.substr(start)};
  }

  IndexedInodeInput& edit(std::uint64_t id) {
    auto it=nodes_.find(id);
    if(it==nodes_.end()) {
      if(nodes_.size()==kStoreMaxObjects)fail(Code::limit,"indexed inode transaction budget");
      auto n=load_inode(source_,before_,id);
      const auto original_size=n.size;
      const auto original_allocation=n.allocation;
      const auto reserved=reserved_extents(n);
      const auto lifetime=n.lifetime;
      const auto inline_file=!n.inline_data.empty();
      it=nodes_.emplace(id,InodeEdit{std::move(n),original_size,original_allocation,reserved,lifetime,inline_file}).first;
    }
    if(!it->second.value)fail(Code::not_found,"indexed inode was removed in this transaction");
    it->second.value->modified=next_.sequence;
    return *it->second.value;
  }

  IndexedInodeInput metadata_inode(std::uint64_t id,std::uint32_t ordinal) const {
    if(ordinal) {
      if(ordinal>created_.size())fail(Code::invalid_operation,"indexed creation selector out of range");
      id=created_[ordinal-1];
    }
    return node(id);
  }

  void create(std::string_view path,bool directory) {
    std::string spelling;
    auto key=destination(path,spelling);
    if(const auto id=binding(key)) {
      (void)linked_inode(key,*id);
      fail(Code::already_exists,"indexed destination exists");
    }
    if(next_.next_inode==kNativeIndexedInodeLimit || nodes_.size()==kStoreMaxObjects)
      fail(Code::limit,"indexed inode allocation budget");
    const auto id=next_.next_inode++;
    IndexedInodeInput n{id,key.parent,0,next_.sequence,next_.sequence,directory,std::move(spelling),{},{},{}};
    if(directory) {
      n.shape.emplace();
      next_.directory_shapes=true;
    }
    change_child(key.parent,nullptr,&n);
    nodes_.emplace(id,InodeEdit{std::move(n),{},{},0});
    names_[std::move(key)]=id;
    created_.push_back(id);
  }

  template<class Visitor>
  bool visit_children(std::uint64_t id,Visitor&& visitor) const {
    for(auto it=names_.lower_bound({id,{}});it!=names_.end() && it->first.parent==id;++it)
      if(it->second && !visitor(linked_inode(it->first,*it->second)))return false;
    std::string after;
    for(;;) {
      const auto page=index_.list(id,after,64);
      if(page.empty())return true;
      for(const auto& row:page)
        if(!names_.contains(row.key) && !visitor(linked_inode(row.key,row.value)))return false;
      after=page.back().key.name;
    }
  }

  void verify_descendant_paths(std::uint64_t id,std::size_t units,unsigned depth) {
    visit_children(id,[&](const IndexedInodeInput& child) {
      const auto length=units+1+indexed_utf16_length(child.name);
      validate_move_bounds(length,depth+1);
      if(child.shape)check_directory_move(*child.shape,length,depth+1);
      else if(child.directory)verify_descendant_paths(child.inode,length,depth+1);
      return true;
    });
  }

  void validate_move_bounds(std::size_t units,unsigned components) {
    if(units>kIndexedPathUnits || components>kIndexedPathDepth)
      fail(Code::invalid_path,"indexed rename exceeds descendant path bounds");
    if(units>240)next_.name_profile=kIndexedOrdinalNames;
  }

  void check_directory_move(const IndexedDirectoryShape& shape,std::size_t units,unsigned components) {
    const auto suffix=shape.bound();
    if(suffix.units)validate_move_bounds(units+1+suffix.units,components+suffix.components);
  }

  void change_child(std::uint64_t parent,const IndexedInodeInput* removed,const IndexedInodeInput* added) {
    auto* directory=&edit(parent);
    if(!directory->shape)return;
    auto before=removed?child_path_bound(*removed):std::nullopt;
    auto after=added?child_path_bound(*added):std::nullopt;
    if(removed && !before)fail(Code::corrupt,"indexed measured directory contains an unmeasured child");
    if(added && !after) {
      // Importing an older subtree invalidates summaries on its new ancestry.
      while(directory->shape) {
        directory->shape.reset();
        if(!directory->parent) {
          next_.directory_backfill_complete=false;
          return;
        }
        directory=&edit(directory->parent);
      }
      return;
    }
    while(directory->shape && before!=after) {
      const auto old_bound=directory->shape->bound();
      directory->shape->replace(before,after);
      const auto new_bound=directory->shape->bound();
      if(old_bound==new_bound || !directory->parent)return;
      before=child_path_bound(directory->name,old_bound);
      after=child_path_bound(directory->name,new_bound);
      directory=&edit(directory->parent);
    }
  }

  void move(const IndexedInodeInput& n,const NativeFileMutation& op) {
    if(n.lifetime==IndexedLifetime::pending)
      fail(Code::invalid_operation,"indexed rename source is delete-pending");
    std::string spelling;
    auto key=destination(op.target,spelling);
    for(auto id=key.parent;id;) {
      if(id==n.inode)fail(Code::invalid_operation,"indexed rename would create a cycle");
      id=node(id).parent;
    }
    if(const auto target=binding(key)) {
      if(*target==n.inode) {
        if(n.name==spelling)return;
      } else {
        if(!op.replace)fail(Code::already_exists,"indexed rename destination exists");
        const auto victim=linked_inode(key,*target);
        if(victim.lifetime==IndexedLifetime::pending)
          fail(Code::invalid_operation,"indexed replacement target is delete-pending");
        if(victim.directory!=n.directory)fail(Code::invalid_operation,"indexed rename type mismatch");
        discard(victim);
      }
    }
    if(n.directory) {
      const auto source_depth=std::count(op.path.begin(),op.path.end(),'/');
      const auto target_depth=std::count(op.target.begin(),op.target.end(),'/');
      const auto target_units=indexed_utf16_length(op.target);
      const auto target_components=static_cast<unsigned>(target_depth+1);
      if(n.shape)check_directory_move(*n.shape,target_units,target_components);
      else if(target_units>indexed_utf16_length(op.path) || target_depth>source_depth)
        verify_descendant_paths(n.inode,target_units,target_components);
    }
    names_[{n.parent,name_key(n.name)}]={};
    auto& entry=edit(n.inode);
    entry.parent=key.parent;entry.name=std::move(spelling);
    if(n.parent==key.parent)change_child(key.parent,&n,&entry);
    else {
      change_child(n.parent,&n,nullptr);
      change_child(key.parent,nullptr,&entry);
    }
    names_[std::move(key)]=n.inode;
  }

  bool has_children(std::uint64_t id) const {
    return !visit_children(id,[](const auto&){return false;});
  }

  void require_empty_directory(const IndexedInodeInput& n) const {
    if(n.directory && has_children(n.inode))fail(Code::not_empty,"indexed directory is not empty");
  }

  void remove_visible_name(const IndexedInodeInput& n) {
    if(n.parent) {
      names_[{n.parent,name_key(n.name)}]={};
      change_child(n.parent,&n,nullptr);
    }
  }

  void detach(const IndexedInodeInput& n,IndexedLifetime lifetime) {
    remove_visible_name(n);
    auto& changed=edit(n.inode);
    changed.parent=0;changed.name.clear();changed.lifetime=lifetime;
  }

  void discard(const IndexedInodeInput& n) {
    require_empty_directory(n);
    (void)edit(n.inode);
    if(!n.directory && next_.format>=2 && nodes_.at(n.inode).original_size &&
        index_.extent_count(n.inode)>retirement_quantum) {
      // A closed inode becomes inaccessible at this checkpoint. Keep its
      // original range until bounded retirement commits dispose of it.
      auto it=extents_.lower_bound({n.inode,0});
      while(it!=extents_.end() && it->first.first==n.inode) {
        payload_bytes_-=it->second.payload.capacity();
        it=extents_.erase(it);
      }
      const auto original=load_inode(source_,before_,n.inode);
      auto& retiring=edit(n.inode);
      retiring.size=original.size;retiring.allocation=original.allocation;
      detach(n,IndexedLifetime::retiring);
      return;
    }
    if(!n.directory)clear_extents(n.inode,0);
    nodes_.at(n.inode).value.reset();
    remove_visible_name(n);
  }

  void resize(ExtentEdit& extent,std::size_t size) {
    const auto rest=payload_bytes_-extent.payload.capacity();
    if(size>kStoreMaxTransactionBytes-rest)fail(Code::limit,"indexed staging payload budget");
    extent.payload.resize(size);
    if(extent.payload.capacity()>kStoreMaxTransactionBytes-rest)fail(Code::limit,"indexed staging payload budget");
    payload_bytes_=rest+extent.payload.capacity();
  }

  ExtentEdit& load_extent(std::uint64_t id,std::uint64_t block) {
    auto it=extents_.find({id,block});
    if(it!=extents_.end())return it->second;
    if(extents_.size()==kStoreMaxObjects)fail(Code::limit,"indexed extent transaction budget");
    ExtentEdit value;
    const auto& n=nodes_.at(id);
    if(n.original_size) {
      value.original=index_.find(native_extent_key(id,block));
      if(n.original_inline && block==0) {
        if(value.original)fail(Code::corrupt,"inline inode has an external extent");
        const auto& data=n.value->inline_data;
        resize(value,data.size());
        std::ranges::copy(data,value.payload.begin());
      }else if(value.original) {
        if(domain(*value.original)!=kNativeIndexedDataObject || !(*value.original&ordinal_mask) ||
            (*value.original&ordinal_mask)>=before_.next_extent)
          fail(Code::corrupt,"indexed data identity outside allocator");
        IndexedInodeInput original;original.inode=id;original.size=*n.original_size;
        auto extent=decode_extent(source_,index_,before_,*value.original,
            required(source_,*value.original,kNativeExtentBytes+extent_header),original,block);
        auto& bytes=extent.data;
        bytes.erase(bytes.begin(),bytes.begin()+static_cast<std::ptrdiff_t>(extent.payload_offset));
        if(bytes.capacity()>kStoreMaxTransactionBytes-payload_bytes_)
          fail(Code::limit,"indexed staging payload budget");
        payload_bytes_+=bytes.capacity();
        value.payload=std::move(bytes);
        value.base=extent.base;
      }
    }
    return extents_.emplace(std::pair{id,block},std::move(value)).first->second;
  }

  void clear_extent(ExtentEdit& extent) {
    extent.dirty=extent.dirty || extent.original.has_value() || !extent.payload.empty();
    payload_bytes_-=extent.payload.capacity();
    Bytes{}.swap(extent.payload);
  }

  void clear_extents(std::uint64_t id,std::uint64_t first) {
    if(!first && nodes_.at(id).original_inline)(void)load_extent(id,0);
    const auto start=native_extent_key(id,first?first-1:0);
    auto after=first?start.name:std::string{};
    if(nodes_.at(id).original_size)for(;;) {
      const auto page=index_.list(start.parent,after,64);
      if(page.empty())break;
      for(const auto& row:page)clear_extent(load_extent(id,native_extent_block(row.key.name)));
      after=page.back().key.name;
    }
    for(auto it=extents_.lower_bound({id,first});it!=extents_.end() && it->first.first==id;++it)
      clear_extent(it->second);
  }

  void trim(ExtentEdit& extent) {
    while(!extent.payload.empty() && extent.payload.back()==std::byte{})extent.payload.pop_back();
    if(extent.payload.empty())clear_extent(extent);
  }

  void prepare_inline_data(std::uint64_t id,IndexedInodeInput& inode) {
    if(inode.size>kIndexedInlineFileBytes) {
      if(!inode.inline_data.empty()) {
        // A sparse extension may leave the leading bytes untouched. Stage
        // them in the ordinary extent domain in this same publication.
        load_extent(id,0).dirty=true;
        inode.inline_data.clear();
      }
      return;
    }
    const auto found=extents_.find({id,0});
    if(found==extents_.end())return; // Metadata-only changes preserve the existing placement.
    auto& extent=found->second;
    if(extent.payload.size()>inode.size)fail(Code::corrupt,"indexed inline data exceeds file size");
    inode.inline_data=extent.payload;
    clear_extent(extent);
  }

  static std::uint64_t aligned_size(std::uint64_t size) {
    const auto remainder=size%kNativeAllocationUnit;
    const auto padding=remainder?kNativeAllocationUnit-remainder:0;
    if(padding>(std::numeric_limits<std::uint64_t>::max)()-size)
      fail(Code::limit,"indexed reservation alignment overflow");
    return size+padding;
  }

  void set_size(IndexedInodeInput& n,std::uint64_t size) {
    if(n.allocation && size>*n.allocation)n.allocation=aligned_size(size);
    n.size=size;
  }

  void write_file(const IndexedInodeInput& n,std::uint64_t offset,std::span<const std::byte> bytes) {
    if(n.directory)fail(Code::is_directory,"cannot write indexed directory");
    if(bytes.size()>(std::numeric_limits<std::uint64_t>::max)()-offset)
      fail(Code::limit,"indexed write range overflow");
    if(bytes.empty())return;
    auto& updated=edit(n.inode);
    for(std::size_t done=0;done<bytes.size();) {
      const auto position=offset+done;
      const auto within=static_cast<std::size_t>(position%kNativeExtentBytes);
      const auto count=(std::min)(bytes.size()-done,kNativeExtentBytes-within);
      const auto input=bytes.subspan(done,count);
      auto& extent=load_extent(n.inode,position/kNativeExtentBytes);
      const auto overlap=extent.payload.size()>within?(std::min)(count,extent.payload.size()-within):0;
      if((overlap && !std::ranges::equal(input.first(overlap),
              std::span<const std::byte>(extent.payload).subspan(within,overlap))) ||
          std::ranges::any_of(input.subspan(overlap),[](std::byte b){return b!=std::byte{};})) {
        if(extent.payload.size()<within+count)resize(extent,within+count);
        std::ranges::copy(input,extent.payload.begin()+static_cast<std::ptrdiff_t>(within));
        extent.dirty=true;
        trim(extent);
      }
      done+=count;
    }
    set_size(updated,(std::max)(n.size,offset+bytes.size()));
    if(next_.workload.enabled) {
      const auto room=(std::numeric_limits<std::uint64_t>::max)()-next_.workload.committed_input_bytes;
      next_.workload.committed_input_bytes+=(std::min)(room,static_cast<std::uint64_t>(bytes.size()));
    }
  }

  void truncate_file(const IndexedInodeInput& n,std::uint64_t size) {
    if(n.directory)fail(Code::is_directory,"cannot truncate indexed directory");
    if(size==n.size)return;
    auto& updated=edit(n.inode);
    if(size<n.size) {
      const auto block=size/kNativeExtentBytes;
      const auto tail=static_cast<std::size_t>(size%kNativeExtentBytes);
      clear_extents(n.inode,block+(tail?1U:0U));
      if(tail) {
        auto& extent=load_extent(n.inode,block);
        if(extent.payload.size()>tail) {
          resize(extent,tail);trim(extent);extent.dirty=true;
        }
      }
    }
    set_size(updated,size);
  }
};

} // namespace

IndexedPathBound IndexedDirectoryShape::bound() const noexcept {
  if(units.empty())return {};
  return {units.back().bound,components.back().bound};
}

void IndexedDirectoryShape::replace(std::optional<IndexedPathBound> before,std::optional<IndexedPathBound> after) {
  const auto replace_bucket=[](auto& axis,std::uint16_t removed,std::uint16_t added) {
    if(removed==added)return;
    if(removed) {
      const auto at=std::ranges::lower_bound(axis,removed,{},&Bucket::bound);
      if(at==axis.end() || at->bound!=removed)fail(Code::corrupt,"indexed directory shape is missing a child bucket");
      if(!--at->children)axis.erase(at);
    }
    if(added) {
      const auto at=std::ranges::lower_bound(axis,added,{},&Bucket::bound);
      if(at==axis.end() || at->bound!=added)axis.insert(at,{added,1});
      else if(at->children==ordinal_mask)fail(Code::limit,"indexed directory shape child count exhausted");
      else ++at->children;
    }
  };
  replace_bucket(units,before?before->units:0,after?after->units:0);
  replace_bucket(components,before?before->components:0,after?after->components:0);
}

IndexedVolumeRoot initialize_indexed_volume(PagedObjectStoreWriter& writer,NativeIndexedDataLayout layout) {
  const auto snapshot=writer.snapshot();
  if(writer.poisoned() || snapshot.info().generation!=2 || !snapshot.info().mirrored_payloads ||
      snapshot.info().objects)
    fail(Code::invalid_operation,"indexed initialization requires a new mirrored store");
  if(layout!=NativeIndexedDataLayout::extents && layout!=NativeIndexedDataLayout::inline_small_files)
    fail(Code::invalid_operation,"unknown indexed data layout");
  IndexedVolumeRoot root;
  root.identity=snapshot.identity();root.sequence=1;root.inodes=1;root.workload.enabled=true;
  root.format=2;root.directory_shapes=true;root.directory_backfill_complete=true;
  root.inline_files=layout==NativeIndexedDataLayout::inline_small_files;
  IndexedInodeInput directory;
  directory.inode=1;directory.born=1;directory.modified=1;directory.directory=true;
  directory.shape.emplace();
  std::vector<StoreMutation> initial;
  initial.push_back({namespace_object,false,encode_root(root)});
  initial.push_back({inode_domain|1,false,encode_indexed_inode(directory,root)});
  NativeNameIndex::initialize(writer,std::move(initial),NativeIndexKind::names_and_extents);
  return root;
}

void IndexedContentChanges::record(NativeExtentSelection extent,std::uint64_t sequence) noexcept {
  const auto retained=std::span(entries_).first(used_);
  const auto existing=std::ranges::find_if(retained,[&](const Change& change) {
    return change.extent.inode==extent.inode && change.extent.extent_index==extent.extent_index;
  });
  if(existing!=retained.end()) {
    existing->sequence=sequence;
    return;
  }
  if(used_<entries_.size()) {
    entries_[used_++]={extent,sequence};
    return;
  }
  const auto oldest=std::ranges::min_element(retained,{},&Change::sequence);
  lost_through_=(std::max)(lost_through_,oldest->sequence);
  *oldest={extent,sequence};
}

void IndexedContentChanges::require_scan(std::uint64_t sequence) noexcept {
  used_=0;
  lost_through_=sequence;
}

std::optional<std::size_t> IndexedContentChanges::after(std::uint64_t sequence,
    std::span<NativeExtentSelection> output) const noexcept {
  if(sequence<lost_through_)return {};
  std::size_t count=0;
  for(const auto& change:std::span(entries_).first(used_)) {
    if(change.sequence<=sequence)continue;
    if(count==output.size())return {};
    output[count++]=change.extent;
  }
  std::ranges::sort(output.first(count),{},[](NativeExtentSelection extent) {
    return std::pair{extent.inode,extent.extent_index};
  });
  return count;
}

NativeFsCommit commit_indexed_volume(PagedObjectStoreWriter& writer,const PagedObjectStoreSnapshot& source,
    IndexedVolumeRoot& current,const IndexedVolumeEdit& request,NativeInodeCommitInfo* published,
    NativeInodeAllocationInfo* allocation,IndexedContentWatch watched) {
  IndexedVolumeTransaction transaction(source,current,watched);
  auto id=request.inode;
  switch(request.action) {
    case NativeInodeAction::path_mutations:
      for(const auto& op:request.paths)transaction.apply(op);
      break;
    case NativeInodeAction::create:
      if(id || request.paths.size()!=1 ||
          (request.paths.front().operation!=NativeFileOperation::create_file &&
           request.paths.front().operation!=NativeFileOperation::create_directory))
        fail(Code::invalid_operation,"invalid indexed creation transaction");
      transaction.apply(request.paths.front());
      id=transaction.created_inode();
      if(request.reservation)transaction.reserve(id,*request.reservation);
      if(request.deleting)transaction.set_delete(id,true);
      break;
    case NativeInodeAction::overwrite:
      if(!request.overwrite)fail(Code::invalid_operation,"indexed overwrite metadata is missing");
      transaction.overwrite(id,*request.overwrite);
      if(request.reservation)transaction.reserve(id,*request.reservation);
      break;
    case NativeInodeAction::reserve:
      if(!request.reservation)fail(Code::invalid_operation,"indexed reservation is missing");
      transaction.reserve(id,*request.reservation);
      if(request.automatic)transaction.automatic(id,*request.automatic);
      break;
    case NativeInodeAction::set_delete:transaction.set_delete(id,true);break;
    case NativeInodeAction::clear_delete:transaction.set_delete(id,false);break;
    case NativeInodeAction::unlink:transaction.unlink(id);break;
    case NativeInodeAction::retire:transaction.retire(id);break;
    case NativeInodeAction::cleanup:
    case NativeInodeAction::cleanup_initial:
      if(!request.automatic)fail(Code::invalid_operation,"indexed Cleanup metadata is missing");
      transaction.cleanup(id,request.deleting,request.action==NativeInodeAction::cleanup_initial,
          *request.automatic);
      if(request.trim_allocation)transaction.trim_reservation(id);
      break;
    case NativeInodeAction::enable_workload:transaction.enable_workload();break;
    default:fail(Code::invalid_operation,"operation is unavailable for the indexed capability");
  }
  for(const auto& op:request.basic)transaction.apply(op);
  for(const auto& op:request.security)transaction.apply(op);
  NativeInodeAllocationInfo committed;
  if(published || allocation)committed=transaction.allocation(id);
  auto result=transaction.finish(writer,current);
  if(published)*published=committed.info;
  if(allocation)*allocation=committed;
  return result;
}

void advance_indexed_retirement(PagedObjectStoreWriter& writer,const PagedObjectStoreSnapshot& source,
    IndexedVolumeRoot& current,bool closed_session,IndexedContentWatch watched) {
  IndexedVolumeTransaction transaction(source,current,watched);
  const auto marker_domain=closed_session?lifetime_domain:retirement_domain;
  const auto page=source.list_page(marker_domain,1);
  if(page.empty() || domain(page.front().id)!=marker_domain)
    fail(Code::corrupt,"indexed root references missing retirement work");
  const auto id=page.front().id&ordinal_mask;
  if(closed_session)transaction.finish_context(id);
  else transaction.reclaim(id);
  (void)transaction.finish(writer,current);
}

NativeDirectoryBackfillStep advance_indexed_directory_backfill(PagedObjectStoreWriter& writer,
    const PagedObjectStoreSnapshot& source,IndexedVolumeRoot& current,IndexedDirectoryBackfill& work,
    IndexedContentWatch watched) {
  IndexedVolumeTransaction transaction(source,current,watched);
  const auto step=transaction.prepare_directory_backfill(work);
  if(step==NativeDirectoryBackfillStep::published) {
    (void)transaction.finish(writer,current);
    work.stack.pop_back();
  }
  return step;
}

namespace {
auto extent_coordinate(NativeExtentSelection selection) noexcept {
  return std::pair{selection.inode,selection.extent_index};
}
PagedStoreInfo check_group_storage(const PagedObjectStoreSnapshot& source) {
  const auto info=source.info();
  if(!info.mirrored_payloads || info.degraded_roots || info.degraded_metadata || info.degraded_payloads)
    fail(Code::storage_failure,"indexed shared-base work requires healthy mirrored storage");
  return info;
}
void check_group_root(const PagedObjectStoreSnapshot& source,const NativeNameIndex& index,
    const IndexedVolumeRoot& root) {
  if(decode_root(required(source,namespace_object,root_size))!=root ||
      index.kind()!=index_kind(root) || index.size()!=index_entries(root))
    fail(Code::corrupt,"indexed shared-base checkpoint differs");
}
}

IndexedSharedBasePreparation::IndexedSharedBasePreparation(const PagedObjectStoreSnapshot& source,
    IndexedVolumeRoot root,NativeExtentSelection base,std::span<const NativeExtentSelection> members,
    std::span<const NativeContentToken> expected,bool allow_subset,bool packed_only)
    :source_(source),original_(root),allow_subset_(allow_subset),packed_only_(packed_only) {
  if(members.size()>=kStoreMaxObjects)fail(Code::limit,"indexed shared-base selection budget");
  members_.reserve(members.size()+1);
  members_.push_back(Member{base});
  for(const auto selected:members)members_.push_back(Member{selected});
  const auto coordinate=[](const Member& member){return extent_coordinate(member.selected);};
  std::ranges::sort(members_,{},coordinate);
  const auto duplicates=std::ranges::unique(members_,{},coordinate);
  members_.erase(duplicates.begin(),duplicates.end());
  const auto base_position=std::ranges::lower_bound(members_,extent_coordinate(base),{},coordinate);
  base_index_=static_cast<std::size_t>(base_position-members_.begin());
  if(!expected.empty()) {
    if(expected.size()!=members_.size())fail(Code::invalid_operation,"incomplete indexed candidate bindings");
    for(const auto& binding:expected) {
      const auto key=extent_coordinate(binding.extent);
      const auto found=std::ranges::lower_bound(members_,key,{},coordinate);
      if(found==members_.end() || coordinate(*found)!=key || found->object || !binding.object || !binding.revision)
        fail(Code::invalid_operation,"invalid indexed candidate binding");
      found->object=binding.object;found->generation=binding.revision;
      found->reuse_bound_base=binding.exact_base;
    }
  }
  progress_.total_members=members_.size();
  if(members_.size()<2)progress_.phase=NativeSharedBasePhase::declined;
}

bool IndexedSharedBasePreparation::watches_extent(std::uint64_t inode,std::uint64_t block) const noexcept {
  if(invalidated_)return false;
  const auto key=std::pair{inode,block};
  const auto member=std::ranges::lower_bound(members_,key,{},[](const Member& item){return extent_coordinate(item.selected);});
  return member!=members_.end() && extent_coordinate(member->selected)==key && !member->skipped;
}
bool IndexedSharedBasePreparation::watches_inode(std::uint64_t inode) const noexcept {
  if(invalidated_)return false;
  auto member=std::ranges::lower_bound(members_,inode,{},[](const Member& item){return item.selected.inode;});
  for(;member!=members_.end() && member->selected.inode==inode;++member)
    if(!member->skipped)return true;
  return false;
}

std::vector<std::byte> IndexedSharedBasePreparation::read_member(Member& member,bool find_reusable_anchor) {
  const auto [id,block]=member.selected;
  const auto inode=load_inode(source_,original_,id);
  if(inode.directory || inode.lifetime!=IndexedLifetime::linked)
    fail(Code::invalid_operation,"indexed shared-base selection requires a linked file");
  const auto object=index_->find(native_extent_key(id,block));
  if(member.object && object!=member.object)fail(Code::invalid_operation,"indexed candidate object changed");
  if(!object)fail(Code::not_found,"indexed shared-base selection is a sparse extent");
  if((*object&ordinal_mask)>=original_.next_extent)fail(Code::corrupt,"invalid selected extent ordinal");
  const auto metadata=source_.stat(*object);
  if(!metadata)fail(Code::corrupt,"indexed selected extent record is missing");
  if(member.generation && metadata->generation!=member.generation)
    fail(Code::invalid_operation,"indexed candidate generation changed");
  const bool reuse_bound_base=find_reusable_anchor && member.reuse_bound_base;
  IndexedSharedBaseRead bound;
  auto extent=decode_extent(source_,*index_,original_,*object,
      required(source_,*object,kNativeExtentBytes+extent_header),inode,block,
      find_reusable_anchor && !reuse_bound_base,reuse_bound_base?&bound:nullptr);
  member.object=*object;
  member.generation=metadata->generation;
  member.previous_base=extent.base;
  member.payload_bytes=extent.payload().size();
  if(reuse_bound_base) {
    if(!bound.object || bound.object!=extent.base || !bound.generation || !bound.base)
      fail(Code::invalid_operation,"indexed candidate no longer refers to a shared base");
    reusable_anchor_=bound.object;
    reusable_generation_=bound.generation;
    return std::move(*bound.base).take_payload();
  }
  if(extent.reusable_anchor_generation) {
    reusable_anchor_=extent.base;
    reusable_generation_=extent.reusable_anchor_generation;
  }
  return full_extent(std::move(extent));
}

NativeSharedBaseProgress IndexedSharedBasePreparation::progress() const noexcept {
  auto result=progress_;
  result.reference_changed=reference_member_!=0;
  result.section_comparison=section_reference_.has_value() || prefixed_reference_.has_value() ||
      (search_ && search_->purpose==NativeSharedBaseSearchPurpose::section_comparison);
  result.owned_payload_bytes+=nominated_base_.size();
  if(section_reference_)result.owned_payload_bytes+=section_reference_->base.size()+section_reference_->frame_bytes;
  if(prefixed_reference_)result.owned_payload_bytes+=prefixed_reference_->frame_bytes;
  if(packed_reference_)result.owned_payload_bytes+=packed_reference_->base.capacity()+packed_reference_->frame_bytes;
  if(search_)result.owned_payload_bytes+=search_->bytes->base.size()+
      search_->bytes->members.size()*std::uint64_t(kNativeExtentBytes);
  return result;
}

std::size_t IndexedSharedBasePreparation::search_slot(std::size_t position) const noexcept {
  return position-search_->first-(base_index_>=search_->first && base_index_<position);
}

bool IndexedSharedBasePreparation::stage_search(std::size_t maximum_members,std::weak_ptr<const void>& owner_window,
    NativeSharedBaseSearchPurpose purpose) {
  if(packed_only_ || progress_.phase!=NativeSharedBasePhase::encoding_members || search_storage_unavailable_)return false;
  const auto first=progress_.completed_members;
  if(!search_) {
    const auto available=members_.size()-first-(base_index_>=first);
    const auto count=(std::min)(available,maximum_members);
    if(!count || !owner_window.expired())return false;
    if(purpose==NativeSharedBaseSearchPurpose::section_comparison &&
        (first || count!=available || (reusable_anchor_?!store_zstd_available():count<2)))return false;
    try {
      auto bytes=std::make_shared<NativeSharedBaseSearchWindow::Payload>();
      bytes->members.reserve(count);
      bytes->base=base_;
      bytes->whole_new_group=!first && count==available && !reusable_anchor_;
      owner_window=bytes;
      search_.emplace(SearchWindow{std::move(bytes),first,count,purpose});
    }catch(const std::bad_alloc&){
      search_storage_unavailable_=true;
      return false;
    }
  }
  auto& window=*search_;
  auto& staged=window.bytes->members;
  if(window.purpose!=purpose || window.first!=first || maximum_members<window.member_count || staged.size()==window.member_count)
    return false;
  auto next=first+staged.size();
  if(base_index_>=first && base_index_<=next)++next;
  (void)check_group_storage(source_);
  auto bytes=read_member(members_[next],false);
  (void)check_group_storage(source_);
  staged.push_back(std::move(bytes));
  return true;
}

NativeSharedBaseSearchWindow IndexedSharedBasePreparation::search_window() const noexcept {
  NativeSharedBaseSearchWindow window;
  if(!invalidated_ && search_ && search_->purpose==NativeSharedBaseSearchPurpose::advice &&
      search_->bytes->members.size()==search_->member_count)
    window.payload_=search_->bytes;
  return window;
}

bool IndexedSharedBasePreparation::can_change_reference() const noexcept {
  return !progress_.completed_members && !reference_member_ && search_ && search_->bytes->whole_new_group &&
      search_->bytes->members.size()==search_->member_count;
}

void IndexedSharedBasePreparation::change_reference(std::uint32_t member) {
  if(!member)return;
  auto selected=search_->bytes->members[member-1];
  nominated_base_=std::move(base_);
  base_=std::move(selected);
  reference_member_=member;
}

bool IndexedSharedBasePreparation::price_reference() {
  if(!can_change_reference() || search_->member_count<2)return false;
  const auto members=static_cast<std::uint32_t>(search_->member_count);
  const auto candidates=static_cast<std::uint32_t>(std::bit_width(members+1));
  auto& progress=pricing_;
  if(progress.candidate>=candidates)return false;
  const auto reference=anchor_match::reference_member_at(progress.candidate,candidates,members);
  const auto peer=progress.peer+(progress.peer>=reference);
  const auto staged=[&](auto ordinal)->const Bytes& {
    return ordinal?search_->bytes->members[ordinal-1]:search_->bytes->base;
  };
  progress.total+=price_native_group_xor(staged(peer),staged(reference));
  if(++progress.peer==members) {
    if(!progress.candidate || progress.total<progress.best) {
      progress.best=progress.total;
      progress.selected=reference;
    }
    progress.peer=0;
    progress.total=0;
    if(++progress.candidate==candidates)change_reference(progress.selected);
  }
  return true;
}

void IndexedSharedBasePreparation::admit_variants(bool packing,bool staged_reference,bool automatic) noexcept {
  packing_admitted_=packing;
  automatic_compression_=automatic;
  if(!packing) {
    packed_reference_.reset();
    if(packed_only_)progress_.phase=NativeSharedBasePhase::declined;
  }
  staged_reference_admitted_=staged_reference;
  if(!staged_reference && staged_reference_comparison_) {
    compare_sections_=false;
    compare_prefix_=false;
    staged_reference_comparison_=false;
    section_reference_.reset();
    prefixed_reference_.reset();
  }
}

bool IndexedSharedBasePreparation::stage_section_reference() {
  try {
    if(!section_reference_)section_reference_.emplace();
    auto& trial=*section_reference_;
    if(!trial.index) {
      if(trial.sampled<=search_->member_count) {
        const auto& sample=trial.sampled?search_->bytes->members[trial.sampled-1]:search_->bytes->base;
        trial.sections.sample(sample);
        ++trial.sampled;
        return true;
      }
      trial.base=trial.sections.finish();
      if(trial.base.empty() || trial.base==base_) {
        section_reference_.reset();
        return false;
      }
      trial.frames.reserve(members_.size());
      trial.index.emplace(trial.base);
      return true;
    }
    const auto position=trial.frames.size();
    if(position==members_.size())return false;

    const auto& bytes=position==base_index_?search_->bytes->base:search_->bytes->members[search_slot(position)];
    const auto& member=members_[position];
    auto frame=encode_native_group_member(bytes,trial.base,kNativeGroupBaseBit+1,1);
    if(auto copies=trial.index->encode(bytes,trial.base,kNativeGroupBaseBit+1,1,
        frame?frame->size():kNativeExtentBytes))frame=std::move(copies);
    if(!frame || root_size+base_size+trial.frame_bytes+extent_header+frame->size()>kStoreMaxTransactionBytes) {
      section_reference_.reset();
      return false;
    }
    if(!std::ranges::equal(decode_native_group_member(*frame,trial.base,NativeGroupMethod::anchor_copies),bytes)) {
      section_reference_.reset();
      return false;
    }
    auto& record=trial.frames.emplace_back(extent_header+frame->size());
    set_magic(record,member_magic);
    put(record,8,member.selected.inode);
    put(record,16,member.selected.extent_index);
    std::copy(frame->begin(),frame->end(),record.begin()+extent_header);
    trial.frame_bytes+=record.size();
    return true;
  }catch(const std::bad_alloc&) {
    // The independent representation remains available when optional work
    // cannot allocate its training or encoding scratch.
    section_reference_.reset();
    return false;
  }
}

bool IndexedSharedBasePreparation::stage_prefixed_reference() {
  if(!search_ || !store_zstd_available())return false;
  try {
    if(!prefixed_reference_)prefixed_reference_.emplace();
    auto& trial=*prefixed_reference_;
    const auto position=trial.frames.size();
    if(position==members_.size()) {
      if(!trial.frame_bytes)prefixed_reference_.reset();
      return false;
    }
    const auto& bytes=position==base_index_?search_->bytes->base:search_->bytes->members[search_slot(position)];
    const auto& member=members_[position];
    auto& frame=trial.frames.emplace_back();
    if(member.frame.size()<=extent_header+kNativeGroupMemberHeaderBytes)return true;
    const auto limit=member.frame.size()-extent_header;
    const auto encoded=encode_native_group_prefix(bytes,base_,kNativeGroupBaseBit+1,1,limit);
    if(!encoded)return true;
    if(!std::ranges::equal(decode_native_group_member(*encoded,base_,NativeGroupMethod::prefixed_zstd),bytes)) {
      prefixed_reference_.reset();
      return false;
    }
    frame.resize(extent_header+encoded->size());
    set_magic(frame,member_magic);
    put(frame,8,member.selected.inode);
    put(frame,16,member.selected.extent_index);
    std::copy(encoded->begin(),encoded->end(),frame.begin()+extent_header);
    trial.frame_bytes+=frame.size();
    return true;
  }catch(const std::exception&) {
    prefixed_reference_.reset();
    return false;
  }
}

bool IndexedSharedBasePreparation::stage_packed_reference() {
  if(!packed_reference_)return false;
  try {
    auto& trial=*packed_reference_;
    if(trial.sampled<members_.size()) {
      const auto position=trial.sampled;
      const auto& bytes=position==base_index_?search_->bytes->base:search_->bytes->members[search_slot(position)];
      const auto length=members_[position].payload_bytes;
      if(!position)trial.base.reserve(kNativeExtentBytes);
      trial.base.insert(trial.base.end(),bytes.begin(),bytes.begin()+static_cast<std::ptrdiff_t>(length));
      if(++trial.sampled==members_.size())trial.base.resize(kNativeExtentBytes);
      return true;
    }
    const auto position=trial.frames.size();
    if(position==members_.size())return false;
    const auto& member=members_[position];
    const auto& bytes=position==base_index_?search_->bytes->base:search_->bytes->members[search_slot(position)];
    if(root_size+base_size+trial.frame_bytes+packed_slice_size>kStoreMaxTransactionBytes) {
      packed_reference_.reset();
      return false;
    }
    const auto expected=std::span(bytes).first(member.payload_bytes);
    trial.frames.push_back(make_packed_slice(member.selected,trial.slice_offset,expected,trial.base));
    trial.frame_bytes+=packed_slice_size;
    trial.slice_offset+=expected.size();
    return true;
  }catch(const std::bad_alloc&) {
    packed_reference_.reset();
    return false;
  }
}

bool IndexedSharedBasePreparation::advance_packed_group() {
  using Phase=NativeSharedBasePhase;
  if(progress_.phase==Phase::reading_base) {
    index_.emplace(source_);
    check_group_root(source_,*index_,original_);
    packed_reference_.emplace().base.resize(kNativeExtentBytes);
    progress_.phase=Phase::encoding_members;
  }
  auto& trial=*packed_reference_;
  auto& member=members_[progress_.completed_members];
  const auto bytes=read_member(member,false);
  (void)check_group_storage(source_);
  if(member.previous_base || !member.payload_bytes || member.payload_bytes>trial.base.size()-trial.slice_offset) {
    packed_reference_.reset();
    progress_.phase=Phase::declined;
    return true;
  }
  const auto expected=std::span(bytes).first(member.payload_bytes);
  std::ranges::copy(expected,trial.base.begin()+static_cast<std::ptrdiff_t>(trial.slice_offset));
  trial.frames.push_back(make_packed_slice(member.selected,trial.slice_offset,expected,trial.base));
  trial.frame_bytes+=packed_slice_size;
  trial.slice_offset+=expected.size();
  if(++progress_.completed_members<members_.size())return false;
  progress_.phase=Phase::ready;
  return true;
}

bool IndexedSharedBasePreparation::advance(std::span<const NativeSharedBaseCopyRange> advice,
    const NativeSharedBaseSearchReply* reply) {
  using Phase=NativeSharedBasePhase;
  if(invalidated_)fail(Code::invalid_operation,"selected indexed shared-base content changed");
  if(progress_.phase==Phase::ready || progress_.phase==Phase::declined)return true;
  (void)check_group_storage(source_);
  if(packed_only_)return advance_packed_group();
  if(progress_.phase==Phase::reading_base) {
    index_.emplace(source_);
    check_group_root(source_,*index_,original_);
    base_=read_member(members_[base_index_],true);
    (void)check_group_storage(source_);
    progress_.owned_payload_bytes=base_.size();
    progress_.phase=Phase::encoding_members;
    return false;
  }

  if(progress_.completed_members==members_.size()) {
    if(stage_packed_reference())return false;
    if(compare_sections_) {
      if(stage_section_reference())return false;
      compare_sections_=false;
      if(section_reference_)section_reference_->index.reset();
    }
    if(compare_prefix_ && stage_prefixed_reference())return false;
    search_.reset();
    progress_.phase=Phase::ready;
    return true;
  }
  const auto position=progress_.completed_members;
  auto& member=members_[position];
  const auto is_base=position==base_index_;
  const bool same_window=reply && search_ && reply->window.payload_==search_->bytes;
  if(same_window && can_change_reference() && reply->reference_member<=search_->member_count &&
      reply->body_method==NativePackedGroupMethod::sparse_xor && reply->packed_bodies.size()==search_->member_count)
    change_reference(reply->reference_member);
  Bytes owned;
  std::span<const std::byte> bytes=reference_member_?nominated_base_:base_,packed_body{};
  auto body_method=NativePackedGroupMethod::xor_planes;
  NativePackedGroupCheck* packed_check=nullptr;
  if(!is_base) {
    const auto slot=search_?search_slot(position):0;
    if(search_ && slot<search_->bytes->members.size()) {
      bytes=search_->bytes->members[slot];
      if(same_window && reply->reference_member==reference_member_) {
        if(reply->copies.size()==search_->member_count)advice=reply->copies[slot];
        if(reply->packed_bodies.size()==search_->member_count) {
          packed_body=reply->packed_bodies[slot];
          body_method=reply->body_method;
          if(reply->packed_checks.size()==search_->member_count)packed_check=&reply->packed_checks[slot];
        }
      }
      else if(same_window && reply->packed_checks.size()==search_->member_count)
        reply->packed_checks[slot]=NativePackedGroupCheck::invalid;
    } else {
      owned=read_member(member,false);
      bytes=owned;
    }
  }
  (void)check_group_storage(source_);
  if(body_method==NativePackedGroupMethod::collective && !packed_body.empty()) {
    if(const auto body=parse_collective_group_body(packed_body)) {
      body_method=body->method;
      packed_body=body->bytes;
    } else {
      if(packed_check)*packed_check=NativePackedGroupCheck::invalid;
      packed_check=nullptr;
      packed_body={};
    }
  }
  if(!reusable_anchor_ || member.previous_base!=reusable_anchor_) {
    // The private placeholder binds no stored dependency. The final anchor
    // identity and generation come from the live transaction at finish.
    auto encoded=encode_native_group_member(bytes,base_,kNativeGroupBaseBit+1,1);
    std::uint32_t capability=3;
    bool selected_advice=false;
    if(!is_base && !advice.empty()) {
      if(auto candidate=encode_native_group_copies(bytes,base_,kNativeGroupBaseBit+1,1,advice,
          encoded?encoded->size():kNativeExtentBytes)) {
        encoded=std::move(candidate);
        capability=anchor_copy_capability;
        selected_advice=true;
      }
    }
    const auto xor_limit=encoded?encoded->size():kNativeExtentBytes;
    const bool sparse_advice=body_method==NativePackedGroupMethod::sparse_xor;
    const auto xor_body=body_method==NativePackedGroupMethod::xor_planes?packed_body:std::span<const std::byte>{};
    auto checked=sparse_advice?check_native_group_sparse_xor_body(packed_body,bytes,base_,kNativeGroupBaseBit+1,1,xor_limit):
        check_native_group_xor_body(xor_body,bytes,base_,kNativeGroupBaseBit+1,1,xor_limit,true);
    if(packed_check && (sparse_advice || body_method==NativePackedGroupMethod::xor_planes))*packed_check=checked.check;
    const bool checked_packed=checked.frame.has_value();
    auto packed=std::move(checked.frame);
    auto xor_method=checked.method;
    if(checked.check!=NativePackedGroupCheck::verified || (sparse_advice && !packed)) {
      packed=encode_native_group_xor(bytes,base_,kNativeGroupBaseBit+1,1,xor_limit);
      if(checked.check!=NativePackedGroupCheck::verified) {
        if(auto sparse=encode_native_group_sparse_xor(bytes,base_,kNativeGroupBaseBit+1,1,
            packed?packed->size():xor_limit)) {
          packed=std::move(sparse);
          xor_method=NativeGroupMethod::sparse_xor;
        }
      }
    }
    if(packed) {
      encoded=std::move(packed);
      capability=xor_method==NativeGroupMethod::sparse_xor?sparse_xor_capability:packed_xor_capability;
      selected_advice=checked_packed;
    }
    if(!is_base || reference_member_) {
      const auto delta_limit=encoded?encoded->size():kNativeExtentBytes;
      const auto delta_body=body_method==NativePackedGroupMethod::modular_delta?packed_body:std::span<const std::byte>{};
      auto checked_delta=check_native_group_delta_body(delta_body,bytes,base_,kNativeGroupBaseBit+1,1,delta_limit);
      if(packed_check && body_method==NativePackedGroupMethod::modular_delta)*packed_check=checked_delta.check;
      const bool advised_delta=checked_delta.frame.has_value();
      auto delta=std::move(checked_delta.frame);
      if(!delta)delta=encode_native_group_delta(bytes,base_,kNativeGroupBaseBit+1,1,delta_limit);
      if(delta) {
        encoded=std::move(delta);
        capability=modular_delta_capability;
        selected_advice=advised_delta;
      }
    }
    const auto limit=encoded?encoded->size():kNativeExtentBytes;
    if(limit>kNativeGroupCopyMinimumBytes) {
      if(!anchor_index_)anchor_index_.emplace(base_);
      if(auto shifted=anchor_index_->encode(bytes,base_,kNativeGroupBaseBit+1,1,limit)) {
        encoded=std::move(shifted);
        capability=anchor_copy_capability;
        selected_advice=false;
      }
    }
    if(!encoded) {
      if(allow_subset_) {
        member.skipped=true;
        return complete_member();
      }
      anchor_index_.reset();
      search_.reset();
      progress_.phase=Phase::declined;
      return true;
    }
    const auto packed_advice=selected_advice && (capability==packed_xor_capability || capability==modular_delta_capability ||
        (sparse_advice && capability==sparse_xor_capability));
    if(!packed_advice &&
        !std::ranges::equal(decode_native_group_member(*encoded,base_,available_group_method(capability)),bytes))
      fail(Code::corrupt,"indexed shared-base private CPU comparison failed");
    group_capability_=(std::max)(group_capability_,capability);
    if(selected_advice)++progress_.selected_advice_members;
    member.advised=selected_advice;
    const auto frame_bytes=extent_header+encoded->size();
    const auto committed_input=root_size+(reusable_anchor_?0:base_size)+progress_.owned_payload_bytes-base_.size();
    if(frame_bytes>kStoreMaxTransactionBytes-committed_input)
      fail(Code::limit,"indexed shared-base staging input budget");
    member.frame.resize(frame_bytes);
    set_magic(member.frame,member_magic);
    put(member.frame,8,member.selected.inode);
    put(member.frame,16,member.selected.extent_index);
    std::copy(encoded->begin(),encoded->end(),member.frame.begin()+extent_header);
    progress_.owned_payload_bytes+=frame_bytes;
  }
  return complete_member();
}

bool IndexedSharedBasePreparation::complete_member() {
  using Phase=NativeSharedBasePhase;
  ++progress_.completed_members;
  const bool complete_window=search_ && search_->bytes->members.size()==search_->member_count &&
      search_->first==0 && search_->member_count+1==members_.size();
  if(search_ && !complete_window && search_slot(progress_.completed_members)>=search_->bytes->members.size())
    search_.reset();
  if(progress_.completed_members!=members_.size())return false;

  anchor_index_.reset();
  if(complete_window && std::ranges::none_of(members_,&Member::skipped)) {
    const bool reserved=search_->purpose==NativeSharedBaseSearchPurpose::section_comparison;
    const auto header=extent_header+kNativeGroupMemberHeaderBytes;
    if(reserved || staged_reference_admitted_) {
      const auto minimum=base_.size()+members_.size()*header;
      compare_sections_=!reusable_anchor_ &&
          progress_.owned_payload_bytes>=minimum+source_.isolation_unit();
      // Automatic compression can try small improvements on the same base.
      // A few bytes at a bank boundary can release a whole protected pair.
      compare_prefix_=(automatic_compression_ || reusable_anchor_ || compare_sections_) && store_zstd_available() &&
          std::ranges::any_of(members_,[&](const auto& member){return member.frame.size()>header;});
      staged_reference_comparison_=!reserved && (compare_sections_ || compare_prefix_);
    }
    if(packing_admitted_ && !reusable_anchor_) {
      std::size_t bytes=0;
      for(const auto& member:members_) {
        if(!member.payload_bytes || member.payload_bytes>kNativeExtentBytes-bytes) {
          bytes=0;
          break;
        }
        bytes+=member.payload_bytes;
      }
      if(bytes)packed_reference_.emplace();
    }
    if(compare_sections_ || compare_prefix_ || packed_reference_)return false;
  }
  search_.reset();
  progress_.phase=Phase::ready;
  return true;
}

NativeSharedBaseCommit IndexedSharedBasePreparation::finish(PagedObjectStoreWriter& writer,
    const PagedObjectStoreSnapshot& source,IndexedVolumeRoot& current) {
  if(invalidated_ || progress_.phase!=NativeSharedBasePhase::ready)
    fail(Code::invalid_operation,"indexed shared-base preparation is stale or incomplete");
  const auto before=check_group_storage(source);
  const NativeNameIndex index(source);
  check_group_root(source,index,current);

  NativeSharedBaseCommit result;
  result.transaction.storage=native_paged_info(before);
  result.transaction.sequence=current.sequence;
  if(current.sequence==(std::numeric_limits<std::uint64_t>::max)() ||
      before.generation>(std::numeric_limits<std::uint64_t>::max)()-2)
    fail(Code::limit,"indexed shared-base generation exhausted");
  auto next=current;
  next.format=(std::max)(current.format,packed_only_?packed_slice_capability:group_capability_);
  ++next.sequence;
  auto anchor=reusable_anchor_,generation=reusable_generation_;
  const bool new_anchor=!anchor;
  if(new_anchor) {
    if(next.next_extent==kNativeIndexedInodeLimit)fail(Code::limit,"indexed shared-base identity exhausted");
    anchor=kNativeIndexedDataObject|next.next_extent++;
    if(source.stat(anchor))fail(Code::corrupt,"indexed shared-base identity already exists");
    generation=before.generation+1;
    ++next.shared_bases;
  } else {
    const auto metadata=source.stat(anchor);
    if(!metadata || metadata->generation!=generation)
      fail(Code::corrupt,"selected indexed shared base changed");
    const auto existing=required(source,anchor,base_size);
    verify_base_frame(existing);
    if(!std::ranges::equal(base_,std::span(existing).subspan(base_magic.size())))
      fail(Code::corrupt,"selected indexed shared base bytes differ");
  }

  std::vector<StoreMutation> records;
  std::vector<NativeNameEdit> edits;
  RetiredGroupReferences retired;
  auto* direct_pack=packed_only_?&packed_reference_.value():nullptr;
  for(std::size_t i=0;i<members_.size();++i) {
    auto& member=members_[i];
    if(member.skipped) {
      result.skipped_objects.push_back(member.object);
      continue;
    }
    ++result.members;
    const auto [id,block]=member.selected;
    const auto inode=load_inode(source,current,id);
    const auto metadata=source.stat(member.object);
    if(inode.directory || inode.lifetime!=IndexedLifetime::linked ||
        index.find(native_extent_key(id,block))!=member.object || !metadata || metadata->generation!=member.generation)
      fail(Code::invalid_operation,"selected indexed shared-base binding changed");
    auto& frame=direct_pack?direct_pack->frames[i]:member.frame;
    if(frame.empty())continue;
    if(direct_pack) {
      put(frame,24,anchor);
      put(frame,32,generation);
    }else bind_native_group_member(std::span(frame).subspan(extent_header),anchor,generation,
        available_group_method(next.format));
    records.push_back({member.object,false,std::move(frame)});
    if(member.previous_base)retire_group_reference(member.previous_base,member.object,next,retired,edits);
    edits.push_back({native_group_reference_key(anchor,member.object),{},member.object});
    ++next.shared_members;
  }
  (void)check_group_storage(source);
  std::ranges::sort(result.skipped_objects);
  if(records.empty())return result;
  if(new_anchor) {
    Bytes frame(base_size);
    set_magic(frame,base_magic);
    const auto& plaintext=direct_pack?direct_pack->base:base_;
    std::copy(plaintext.begin(),plaintext.end(),frame.begin()+static_cast<std::ptrdiff_t>(base_magic.size()));
    records.push_back({anchor,false,std::move(frame)});
  }
  remove_unreferenced_bases(index,retired,next,records);
  validate_header_fields(next);
  records.push_back({namespace_object,false,encode_root(next)});
  auto proposed=NativeNameIndex::prepare_commit(source,edits,std::move(records),current.format<3);
  const auto replace_records=[&](std::map<std::uint64_t,Bytes>& replacements) {
    std::vector<StoreMutation> variant;
    std::uint64_t input=0;
    variant.reserve(proposed.size());
    for(const auto& mutation:proposed) {
      const auto replacement=replacements.find(mutation.id);
      if(replacement==replacements.end())variant.push_back(mutation);
      else variant.push_back({mutation.id,false,std::move(replacement->second)});
      input+=variant.back().bytes.size();
    }
    if(input>kStoreMaxTransactionBytes)variant.clear();
    return variant;
  };
  std::vector<StoreMutation> section,prefixed,packed;
  auto section_root=next,prefixed_root=next,packed_root=next;
  const auto alternate_reference=[&](auto& trial,IndexedVolumeRoot& root,bool slices)->std::vector<StoreMutation> {
    if(!new_anchor || !trial || trial->frames.size()!=members_.size())return {};
    try {
      std::map<std::uint64_t,Bytes> replacements;
      for(std::size_t i=0;i<members_.size();++i) {
        auto& frame=trial->frames[i];
        if(slices) {
          put(frame,24,anchor);
          put(frame,32,generation);
        } else bind_native_group_member(std::span(frame).subspan(extent_header),anchor,generation,NativeGroupMethod::anchor_copies);
        replacements.emplace(members_[i].object,std::move(frame));
      }
      auto& base=replacements[anchor];
      base.resize(base_size);
      set_magic(base,base_magic);
      std::copy(trial->base.begin(),trial->base.end(),base.begin()+static_cast<std::ptrdiff_t>(base_magic.size()));
      root.format=(std::max)(current.format,slices?packed_slice_capability:anchor_copy_capability);
      replacements.emplace(namespace_object,encode_root(root));

      return replace_records(replacements);
    }catch(const std::bad_alloc&) {
      return {};
    }
  };
  section=alternate_reference(section_reference_,section_root,false);
  if(!direct_pack)packed=alternate_reference(packed_reference_,packed_root,true);
  std::size_t prefixed_members=0,replaced_advice=0;
  if(prefixed_reference_ && prefixed_reference_->frames.size()==members_.size()) {
    try {
      std::map<std::uint64_t,Bytes> replacements;
      for(std::size_t i=0;i<members_.size();++i) {
        auto& frame=prefixed_reference_->frames[i];
        if(frame.empty())continue;
        replaced_advice+=members_[i].advised;
        bind_native_group_member(std::span(frame).subspan(extent_header),anchor,generation,NativeGroupMethod::prefixed_zstd);
        replacements.emplace(members_[i].object,std::move(frame));
      }
      prefixed_members=replacements.size();
      prefixed_root.format=(std::max)(next.format,prefixed_zstd_capability);
      replacements.emplace(namespace_object,encode_root(prefixed_root));
      prefixed=replace_records(replacements);
    }catch(const std::bad_alloc&) {
      prefixed.clear();
    }
  }
  (void)check_group_storage(source);
  std::size_t selected=0;
  try {
    PagedCompactedSizes sizes;
    const std::array<std::span<const StoreMutation>,3> alternatives{section,prefixed,packed};
    const auto published=writer.commit_if_compacted_smaller(proposed,namespace_object,sizes,alternatives,selected);
    result.before_compacted_lower_bound=sizes.before;
    result.after_compacted_upper_bound=sizes.after_upper;
    if(!published)return result;
    result.transaction.storage=native_paged_info(*published);
  }catch(const PagedStoreCapacityError& error) {
    if(writer.poisoned())fail(Code::storage_failure,"indexed shared-base publication failed after admission");
    throw NativeFsError(Code::limit,error.what(),error.append_shortfall_bytes());
  }
  result.section_reference=selected==1;
  result.packed_reference=packed_only_ || selected==3;
  const bool replaced_reference=result.section_reference || result.packed_reference;
  result.prefixed_zstd_members=selected==2?prefixed_members:0;
  result.advised_members=replaced_reference?0:
      progress_.selected_advice_members-(selected==2?replaced_advice:0);
  if(result.packed_reference)current=packed_root;
  else if(result.section_reference)current=section_root;
  else if(result.prefixed_zstd_members)current=prefixed_root;
  else current=next;
  result.transaction.sequence=current.sequence;
  auto reference_index=base_index_;
  if(!replaced_reference && reference_member_) {
    const auto slot=static_cast<std::size_t>(reference_member_-1);
    reference_index=slot+(slot>=base_index_);
  }
  result.reference=members_[reference_index].selected;
  result.applied=true;
  return result;
}

NativeInodeStorageMutationCommit write_indexed_inode(PagedObjectStoreWriter& writer,
    const PagedObjectStoreSnapshot& source,IndexedVolumeRoot& current,std::uint64_t id,
    std::uint64_t offset,std::span<const std::byte> bytes,NativeInodeWriteOptions options,
    const NativeAutomaticMetadata& effect,std::span<const NativeLz4ExtentCandidate> candidates,
    IndexedContentWatch watched) {
  IndexedVolumeTransaction transaction(source,current,watched);
  return transaction.write_inode(writer,current,id,offset,bytes,options,effect,candidates);
}
NativeInodeStorageMutationCommit truncate_indexed_inode(PagedObjectStoreWriter& writer,
    const PagedObjectStoreSnapshot& source,IndexedVolumeRoot& current,std::uint64_t id,
    std::uint64_t size,const NativeAutomaticMetadata& effect,IndexedContentWatch watched) {
  IndexedVolumeTransaction transaction(source,current,watched);
  return transaction.truncate_inode(writer,current,id,size,effect);
}
NativeInodeAllocationInfo indexed_inode_allocation(const IndexedInodeInput& n) noexcept {
  return {{n.inode,n.size,n.modified,n.basic,n.directory,n.lifetime==IndexedLifetime::pending,
      indexed_detached(n.lifetime)},n.allocation};
}
std::optional<NativeBasicMetadata> updated_automatic_basic(const std::optional<NativeBasicMetadata>& previous,
    bool directory,const NativeAutomaticMetadata& effect) {
  const bool times=effect.set_access_time || effect.set_write_time || effect.set_change_time;
  const bool archive=effect.set_archive && !directory;
  if(!times && !archive)return {};
  if(!previous)fail(Code::invalid_operation,"automatic metadata needs existing basic metadata");
  auto value=*previous;
  if(archive)value.attributes|=0x20U;
  if(effect.set_access_time)value.last_access_time=effect.utc_time;
  if(effect.set_write_time)value.last_write_time=effect.utc_time;
  if(effect.set_change_time)value.change_time=effect.utc_time;
  return value;
}

bool indexed_volume_root(std::span<const std::byte> bytes) noexcept {return magic(bytes,root_magic);}
Bytes rebind_indexed_volume_root(IndexedVolumeRoot root,StoreIdentity identity) {
  root.identity=identity;
  validate_header_fields(root);
  return encode_root(root);
}
IndexedVolumeView::IndexedVolumeView(const PagedObjectStoreSnapshot& store,std::span<const std::byte> root)
    :IndexedVolumeView(store,decode_root(root)) {}
IndexedVolumeView::IndexedVolumeView(const PagedObjectStoreSnapshot& store,IndexedVolumeRoot root)
    :store_(store),root_(root),index_(store) {
  validate_header_fields(root_);
  if(root_.identity!=store_.identity() || index_.kind()!=index_kind(root_) ||
      !root_.inodes || root_.inodes>=root_.next_inode || root_.extents>=root_.next_extent ||
      root_.shared_bases>=root_.next_extent-root_.extents ||
      root_.charged_extents<root_.extents ||
      root_.charged_extents>(std::numeric_limits<std::size_t>::max)() ||
      index_.size()!=index_entries(root_))
    fail(Code::corrupt,"wrong indexed root capability or counts");
  const auto directory=referenced_inode(1);
  if(root_.directory_backfill_complete && !directory.shape)
    fail(Code::corrupt,"completed indexed backfill lacks root shape");
}
IndexedInodeInput IndexedVolumeView::inode(std::uint64_t id) const {
  auto input=load_inode(store_,root_,id);
  if(input.lifetime==IndexedLifetime::retiring)fail(Code::not_found,"indexed inode is retired");
  return input;
}
IndexedInodeInput IndexedVolumeView::referenced_inode(std::uint64_t id,bool allow_retiring) const {
  try{return allow_retiring?load_inode(store_,root_,id):inode(id);}
  catch(const NativeFsError& error) {
    if(error.code()==Code::not_found)fail(Code::corrupt,"indexed reference has no inode");
    throw;
  }
}
IndexedInodeInput IndexedVolumeView::resolve(std::string_view path_value,std::string* spelling) const {
  const auto parsed=indexed_path_name(path_value,true);
  const auto& key=parsed.key;
  auto current=inode(1);
  for(std::size_t start=0;start<key.size();) {
    if(!current.directory)fail(Code::not_directory,"indexed path ancestor is a file");
    const auto separator=key.find('/',start),end=separator==std::string::npos?key.size():separator;
    const auto component=key.substr(start,end-start);
    const auto child=index_.find({current.inode,component});
    if(!child)throw NativeFsError(separator==std::string::npos?NativeFsError::MissingPath::name:
        NativeFsError::MissingPath::parent,"indexed path not found");
    auto entry=referenced_inode(*child);
    if(indexed_detached(entry.lifetime) || entry.parent!=current.inode || name_key(entry.name)!=component)
      fail(Code::corrupt,"indexed name points to a different inode");
    if(spelling) {
      if(!spelling->empty())spelling->push_back('/');
      spelling->append(entry.name);
    }
    current=std::move(entry);
    start=end+1;
  }
  if(!root_.name_profile && parsed.utf16_units>240)
    fail(Code::corrupt,"indexed path exceeds the selected legacy name profile");
  return current;
}
std::string IndexedVolumeView::path(const IndexedInodeInput& input,bool include_spelling) const {
  if(indexed_detached(input.lifetime))return {};
  std::string result;
  if(include_spelling)result=input.name;
  const auto limit=root_.name_profile?kIndexedPathUnits:240;
  auto units=indexed_utf16_length(input.name);
  auto current=&input;
  IndexedInodeInput parent;
  for(unsigned depth=0;current->inode!=1;++depth) {
    if(depth==kIndexedPathDepth || units>limit ||
        index_.find({current->parent,name_key(current->name)})!=current->inode)
      fail(Code::corrupt,"indexed inode ancestry mismatch");
    parent=referenced_inode(current->parent);
    if(!parent.directory || parent.lifetime!=IndexedLifetime::linked)
      fail(Code::corrupt,"indexed inode has an unavailable parent");
    if(parent.inode!=1) {
      units+=indexed_utf16_length(parent.name)+1;
      if(include_spelling)result=parent.name+"/"+result;
    }
    current=&parent;
  }
  return result;
}
NativeInodeState IndexedVolumeView::state(std::string_view path_value) const {
  std::string spelling;
  const auto selected=resolve(path_value,&spelling);
  return {file_info(selected,std::move(spelling)),selected.lifetime==IndexedLifetime::pending,false};
}
NativeInodeState IndexedVolumeView::state_inode(std::uint64_t id) const {
  const auto selected=inode(id);
  return {file_info(selected,path(selected)),selected.lifetime==IndexedLifetime::pending,
      indexed_detached(selected.lifetime)};
}
NativeInodeAllocationInfo IndexedVolumeView::allocation_inode(std::uint64_t id,bool verify_ancestry) const {
  const auto selected=inode(id);
  if(verify_ancestry)(void)path(selected,false);
  return indexed_inode_allocation(selected);
}
std::optional<NativeDirectoryRecordPage> IndexedVolumeView::directory_records(std::uint64_t directory,std::string_view after,
    std::size_t maximum,const std::function<bool(const NativeInodeState&)>& admit) const {
  const auto parent=inode(directory);
  // Resolve the owned handle and its ancestry in the same view as the page.
  if(!admit({file_info(parent,path(parent)),parent.lifetime==IndexedLifetime::pending,indexed_detached(parent.lifetime)}))
    return {};
  const auto key=after.empty() || after=="." || after==".."?std::string{}:indexed_component_key(after);
  if(!parent.directory)fail(Code::not_directory,"indexed enumeration of a file");
  NativeDirectoryRecordPage result;
  const auto rows=index_.list(directory,key,maximum,&result.more);
  if(!rows.empty() && parent.lifetime!=IndexedLifetime::linked)
    fail(Code::corrupt,"indexed deleted directory retains visible entries");
  result.entries.reserve(rows.size());
  for(const auto& row:rows) {
    auto n=referenced_inode(row.value);
    if(indexed_detached(n.lifetime) || n.parent!=directory || name_key(n.name)!=row.key.name)
      fail(Code::corrupt,"indexed directory entry differs from inode");
    result.entries.push_back({std::move(n.name),indexed_inode_allocation(n)});
  }
  return result;
}
IndexedDirectoryPage IndexedVolumeView::list(std::uint64_t directory,std::string_view after,std::size_t maximum) const {
  const auto parent=inode(directory);
  if(!parent.directory)fail(Code::not_directory,"indexed enumeration of a file");
  auto prefix=path(parent);
  if(!prefix.empty())prefix.push_back('/');
  bool more=false;
  const auto records=index_.list(directory,after,maximum,&more);
  if(!records.empty() && parent.lifetime!=IndexedLifetime::linked)
    fail(Code::corrupt,"indexed deleted directory retains visible entries");
  if(!records.empty() && static_cast<std::size_t>(std::count(prefix.begin(),prefix.end(),'/'))>=kIndexedPathDepth)
    fail(Code::corrupt,"indexed directory exceeds native path depth");
  IndexedDirectoryPage result;
  result.entries.reserve(records.size());
  const auto prefix_units=indexed_utf16_length(prefix);
  const auto limit=root_.name_profile?kIndexedPathUnits:240;
  for(const auto& record:records) {
    const auto child=referenced_inode(record.value);
    if(child.parent!=directory || name_key(child.name)!=record.key.name ||
        prefix_units+indexed_utf16_length(child.name)>limit)
      fail(Code::corrupt,"indexed directory entry differs from inode");
    result.entries.push_back(file_info(child,prefix+child.name));
  }
  if(more)result.after=records.back().key.name;
  return result;
}
std::vector<NativeFileInfo> IndexedVolumeView::list_all(std::uint64_t directory) const {
  std::vector<NativeFileInfo> entries;
  std::optional<std::string> after;
  do {
    auto page=list(directory,after.value_or(std::string{}),128);
    entries.insert(entries.end(),std::make_move_iterator(page.entries.begin()),
        std::make_move_iterator(page.entries.end()));
    after=std::move(page.after);
  }while(after);
  return entries;
}
bool IndexedSharedBaseCache::bind(const PagedObjectStoreSnapshot& source) noexcept {
  const auto info=source.info();
  if(!info.mirrored_payloads || info.degraded_roots || info.degraded_metadata || info.degraded_payloads) {
    clear();
    return false;
  }
  const auto image=source.identity();
  if(checkpoint_!=info.generation || image_!=image) {
    clear();
    checkpoint_=info.generation;
    image_=image;
  }
  return true;
}

const IndexedSharedBaseRead* IndexedSharedBaseCache::find(std::uint64_t object,std::uint64_t generation) noexcept {
  const auto hit=std::ranges::find_if(frames_,[&](const auto& entry) {
    return entry.object==object && entry.generation==generation;
  });
  if(hit==frames_.end())return nullptr;
  std::rotate(hit,hit+1,frames_.end());
  return &frames_.back();
}

void IndexedSharedBaseCache::remember(IndexedSharedBaseRead& entry) noexcept {
  // Retained storage fits the existing memory envelope of one native read.
  // Cache allocation failure leaves the request-local verified frame usable.
  constexpr auto capacity=kNativeMaxReadBytes/(base_size+sizeof(IndexedSharedBaseRead));
  if(frames_.size()==capacity) {
    std::rotate(frames_.begin(),frames_.begin()+1,frames_.end());
    frames_.back()=std::move(entry);
  } else {
    try{frames_.push_back(std::move(entry));}
    catch(const std::bad_alloc&){clear();return;}
  }
  entry={};
}

void IndexedSharedBaseCache::clear() noexcept {
  std::vector<IndexedSharedBaseRead>{}.swap(frames_);
  image_={};
  checkpoint_=0;
}

Bytes IndexedVolumeView::read(const IndexedInodeInput& selected,std::uint64_t offset,std::uint32_t bytes,
    IndexedSharedBaseCache* retained,NativeLz4ReadRoute* route,NativeGroupReadRoute* groups) const {
  if(selected.lifetime==IndexedLifetime::retiring)fail(Code::not_found,"indexed inode is retired");
  if(selected.directory)fail(Code::is_directory,"cannot read indexed directory");
  if(bytes>kNativeMaxReadBytes)fail(Code::limit,"indexed read budget");
  if(!bytes || offset>=selected.size)return {};
  const auto count=(std::min)(std::uint64_t(bytes),selected.size-offset);
  Bytes output(static_cast<std::size_t>(count));
  if(!selected.inline_data.empty()) {
    if(offset<selected.inline_data.size()) {
      const auto data=std::span(selected.inline_data).subspan(static_cast<std::size_t>(offset));
      std::ranges::copy(data.first((std::min)(data.size(),output.size())),output.begin());
    }
    return output;
  }
  if(retained && !retained->bind(store_))retained=nullptr;
  const auto end=offset+count,first=offset/kNativeExtentBytes,last=(end-1)/kNativeExtentBytes;
  const auto start=native_extent_key(selected.inode,first?first-1:0);
  const auto range=index_.list(start.parent,first?std::string_view(start.name):std::string_view{},
      static_cast<std::size_t>(last-first+1));
  IndexedSharedBaseRead cached_base;
  for(const auto& entry:range) {
    const auto block=native_extent_block(entry.key.name);
    if(block>last)break;
    if((entry.value&ordinal_mask)>=root_.next_extent)
      fail(Code::corrupt,"indexed extent reference exceeds allocator");
    DecodedIndexedExtent extent;
    if(groups && groups->ready) {
      auto& pending=*groups->ready;
      auto* ready=pending.find(selected.inode,block,entry.value);
      if(ready && store_.matches_group_read(ready->record,ready->base,*pending.metadata_)) {
        validate_payload(selected,block,ready->bytes);
        extent.data=std::exchange(ready->bytes,Bytes{});
        extent.tiled_group=true;
        ++pending.consumed_;
      }
    }
    if(extent.data.empty()) {
      std::optional<Bytes> record;
      try {
        record=store_.read_matching(entry.value,extent_header+kNativeExtentBytes,std::nullopt,route);
      }catch(const std::bad_alloc&){throw;}
      catch(const std::exception&){fail(Code::storage_failure,"indexed object integrity or read failure");}
      if(!record)fail(Code::corrupt,"indexed object reference missing");
      extent=decode_extent(store_,index_,root_,entry.value,std::move(*record),
          selected,block,false,&cached_base,retained);
    }
    if(groups && extent.tiled_group)groups->tiled=true;
    const auto data=extent.payload();
    const auto begin=block*kNativeExtentBytes;
    const auto from=(std::max)(begin,offset),to=(std::min)(end,begin+data.size());
    if(from<to)std::copy_n(data.begin()+static_cast<std::ptrdiff_t>(from-begin),
        static_cast<std::size_t>(to-from),output.begin()+static_cast<std::ptrdiff_t>(from-offset));
  }
  if(retained)(void)retained->bind(store_);
  return output;
}
bool IndexedVolumeView::capture_native_lz4(const IndexedInodeInput& selected,std::uint64_t offset,
    std::size_t maximum_encoded,NativeLz4CapturedFrame& out) const {
  if(selected.directory || selected.lifetime!=IndexedLifetime::linked || offset>=selected.size)return false;
  const auto object=index_.find(native_extent_key(selected.inode,offset/kNativeExtentBytes));
  if(!object || (*object&ordinal_mask)>=root_.next_extent)return false;
  return store_.capture_native_lz4(*object,extent_header+kNativeExtentBytes,maximum_encoded,out);
}
std::vector<NativeLz4CapturedFrame> IndexedVolumeView::capture_native_lz4_range(
    const IndexedInodeInput& selected,std::uint64_t offset,std::uint32_t bytes,std::size_t maximum_encoded,
    std::size_t remaining_frame_bytes,const std::function<bool()>& interrupted) const {
  if(selected.directory || selected.lifetime!=IndexedLifetime::linked || offset>=selected.size ||
      offset%kNativeExtentBytes || !bytes || bytes>kNativeMaxReadBytes || !remaining_frame_bytes)return {};
  const auto end=offset+(std::min)(std::uint64_t(bytes),selected.size-offset);
  const auto first=offset/kNativeExtentBytes,last=(end-1)/kNativeExtentBytes;
  const auto start=native_extent_key(selected.inode,first?first-1:0);
  const auto range=index_.list(start.parent,first?std::string_view(start.name):std::string_view{},
      static_cast<std::size_t>(last-first+1));
  std::vector<NativeLz4CapturedFrame> frames;
  frames.reserve(range.size());
  for(const auto& entry:range) {
    if(interrupted && interrupted())return {};
    if(native_extent_block(entry.key.name)>last || !remaining_frame_bytes)break;
    if((entry.value&ordinal_mask)>=root_.next_extent)return {};
    NativeLz4CapturedFrame frame;
    if(store_.capture_native_lz4(entry.value,extent_header+kNativeExtentBytes,maximum_encoded,frame,
          remaining_frame_bytes)) {
      remaining_frame_bytes-=frame.key().decoded_bytes+frame.encoded().size();
      frames.push_back(std::move(frame));
    }
  }
  const auto info=store_.info();
  if(info.degraded_roots || info.degraded_metadata || info.degraded_payloads ||
      (interrupted && interrupted()))return {};
  return frames;
}
std::unique_ptr<NativeGroupReadBatch> IndexedVolumeView::capture_group_range(
    const IndexedInodeInput& selected,std::uint64_t offset,std::uint32_t bytes,
    std::size_t remaining,const std::function<bool()>& interrupted) const {
  if(root_.format<packed_xor_capability || !root_.shared_members || selected.directory ||
      selected.lifetime!=IndexedLifetime::linked || offset>=selected.size || offset%kNativeExtentBytes ||
      !bytes || bytes>kNativeMaxReadBytes)return {};
  constexpr auto frame_limit=extent_header+kNativeExtentBytes-1;
  // Unfunded or ineligible probes can hold a member and base temporarily.
  constexpr auto scratch=frame_limit+base_size;
  if(remaining<=scratch)return {};
  remaining-=scratch;
  const auto descriptor=NativeGroupReadyReads::descriptor_bytes();
  const auto end=offset+(std::min)(std::uint64_t{bytes},selected.size-offset);
  const auto first=offset/kNativeExtentBytes,last=(end-1)/kNativeExtentBytes;
  const auto capacity=static_cast<std::size_t>((std::min)(last-first+1,
      std::uint64_t{remaining/(descriptor+kNativeExtentBytes)}));
  if(!capacity)return {};
  remaining-=capacity*descriptor;
  (void)check_group_storage(store_);
  const auto start=native_extent_key(selected.inode,first?first-1:0);
  const auto range=index_.list(start.parent,first?std::string_view(start.name):std::string_view{},
      static_cast<std::size_t>(last-first+1));
  auto batch=std::unique_ptr<NativeGroupReadBatch>(new NativeGroupReadBatch);
  batch->inode_=selected.inode;
  batch->members_.reserve(capacity);
  batch->bases_.reserve(capacity);
  for(const auto& entry:range) {
    if(interrupted && interrupted())return {};
    const auto block=native_extent_block(entry.key.name);
    if(block>last || batch->members_.size()==capacity || remaining<=kNativeExtentBytes)break;
    if((entry.value&ordinal_mask)>=root_.next_extent)return {};
    PagedGroupReadFrame member;
    if(!store_.capture_group_read_record(entry.value,frame_limit,std::nullopt,member) ||
        !magic(member.bytes,member_magic))continue;
    if(member.bytes.size()<=extent_header || get(member.bytes,8)!=selected.inode ||
        get(member.bytes,16)!=block)return {};
    const auto frame=std::span<const std::byte>(member.bytes).subspan(extent_header);
    const auto reference=read_native_group_header(frame,available_group_method(root_.format));
    if(!native_group_tiled(reference.method))continue;
    if(reference.base_id==entry.value || (reference.base_id&ordinal_mask)>=root_.next_extent ||
        index_.find(native_group_reference_key(reference.base_id,entry.value))!=entry.value)return {};
    const auto parameters=native_group_tile_parameters(frame,reference.method);
    const auto offsets=native_group_tile_offsets(frame.subspan(kNativeGroupMemberHeaderBytes),reference.method,parameters);
    const auto existing=std::ranges::find_if(batch->bases_,[&](const auto& base) {
      return base.record.object==reference.base_id && base.record.generation==reference.base_generation;
    });
    const auto base_index=static_cast<std::size_t>(existing-batch->bases_.begin());
    const auto cost=member.bytes.size()+kNativeExtentBytes+(existing==batch->bases_.end()?base_size:0);
    if(cost>remaining)continue;
    if(existing==batch->bases_.end()) {
      PagedGroupReadFrame base;
      if(!store_.capture_group_read_record(reference.base_id,base_size,reference.base_generation,base))return {};
      verify_base_frame(base.bytes);
      batch->bases_.push_back({base.record,NativeGroupBase(std::move(base.bytes),base_magic.size())});
    }
    if(batch->bases_[base_index].bytes.payload_digest()!=reference.base_digest)return {};
    const auto available=selected.size-extent_begin(selected,block);
    const auto maximum=static_cast<std::uint32_t>((std::min)(available,std::uint64_t{kNativeExtentBytes}));
    member.bytes.erase(member.bytes.begin(),member.bytes.begin()+extent_header);
    batch->members_.push_back({member.record,block,maximum,base_index,reference.decoded_digest,
        reference.method,parameters,offsets,std::move(member.bytes)});
    remaining-=cost;
  }
  if(batch->members_.empty() || (interrupted && interrupted()))return {};
  (void)check_group_storage(store_);
  return batch;
}
std::optional<IndexedVolumeView::CheckedContent> IndexedVolumeView::locate_content(
    NativeExtentSelection extent,IndexedContentLookup lookup) const {
  (void)check_group_storage(store_);
  std::optional<std::uint64_t> object;
  if(lookup==IndexedContentLookup::first || lookup==IndexedContentLookup::after) {
    const auto start=lookup==IndexedContentLookup::first?std::nullopt:
        std::optional(native_extent_key(extent.inode,extent.extent_index));
    if(const auto entry=index_.next_extent(start)) {
      extent={entry->key.parent-kNativeIndexedInodeLimit,native_extent_block(entry->key.name)};
      object=entry->value;
    }
  } else object=index_.find(native_extent_key(extent.inode,extent.extent_index));
  (void)check_group_storage(store_);
  if(!object)return {};
  auto inode=referenced_inode(extent.inode,true);
  NativeContentToken token{extent};
  if(inode.lifetime==IndexedLifetime::linked) {
    if(inode.directory || !inode.size || extent.extent_index>(inode.size-1)/kNativeExtentBytes ||
        (*object&ordinal_mask)>=root_.next_extent)
      fail(Code::corrupt,"invalid indexed content selection");
    const auto metadata=store_.stat(*object);
    if(!metadata)fail(Code::corrupt,"indexed content record is missing");
    token.object=*object;
    token.revision=token.content_revision=metadata->generation;
    token.stored_payload_bytes=metadata->stored_payload_bytes;
  }
  (void)check_group_storage(store_);
  return CheckedContent{token,std::move(inode)};
}
std::optional<NativeContentToken> IndexedVolumeView::content_at(NativeExtentSelection extent) const {
  if(const auto checked=locate_content(extent,IndexedContentLookup::exact))return checked->token;
  return {};
}
std::optional<IndexedContentSample> IndexedVolumeView::sample_content(NativeExtentSelection extent,
    IndexedContentLookup lookup,const std::function<bool(const NativeContentToken&)>& read_payload) const {
  const auto checked=locate_content(extent,lookup);
  if(!checked)return {};
  IndexedContentSample sample{checked->token,{}};
  // Coordinate, inode, and revision come from this same checked snapshot.
  if(sample.token.object && (!read_payload || read_payload(sample.token)))
    sample.payload=materialize_content(sample.token,checked->inode,lookup==IndexedContentLookup::published_reference);
  return sample;
}
Bytes IndexedVolumeView::materialize_content(NativeContentToken& token,const IndexedInodeInput& inode,
    bool published_reference) const {
  auto extent=decode_extent(store_,index_,root_,token.object,
      required(store_,token.object,extent_header+kNativeExtentBytes),
      inode,token.extent.extent_index,!published_reference);
  (void)check_group_storage(store_);
  token.grouped=extent.base!=0;
  token.exact_base=false;
  token.packed_slice=extent.packed_slice;
  token.standalone_bytes=token.grouped?0:static_cast<std::uint32_t>(extent.payload().size());
  if(!token.grouped)return full_extent(std::move(extent));
  // A member whose bytes equal the base still supplies its existing feature.
  // Otherwise sample the first checked reverse reference once per shared base.
  if(!published_reference && !extent.reusable_anchor_generation) {
    const auto witnesses=index_.list(native_group_reference_key(extent.base,token.object).parent,{},1);
    (void)check_group_storage(store_);
    if(witnesses.empty())fail(Code::corrupt,"shared base lost its indexed references");
    if(witnesses.front().value!=token.object)
      return token.packed_slice?full_extent(std::move(extent)):Bytes{};
  }
  token.exact_base=true;
  // Match the member's logical layout, then fetch its protected base for
  // encoding. The assembled base can place shared sections at other offsets.
  return full_extent(std::move(extent));
}
std::uint64_t IndexedVolumeView::verify_namespace(const IndexedInodeInput& directory,std::size_t path_units,
    unsigned components,const IndexedReopenAdmission& admission) const {
  IndexedDirectoryShape expected;
  std::uint64_t reachable=1;
  const auto limit=root_.name_profile?kIndexedPathUnits:240;
  index_.visit_directory(directory.inode,[&](std::string_view key,std::uint64_t inode) {
    if(directory.lifetime!=IndexedLifetime::linked || components==kIndexedPathDepth)
      fail(Code::corrupt,"indexed directory contains unreachable children");
    const auto child=referenced_inode(inode);
    if(indexed_detached(child.lifetime) || child.parent!=directory.inode || name_key(child.name)!=key)
      fail(Code::corrupt,"indexed namespace child differs from its name binding");
    const auto units=path_units+(components?1:0)+indexed_utf16_length(child.name);
    if(units>limit)fail(Code::corrupt,"indexed namespace path exceeds its profile");
    if(directory.shape) {
      const auto bound=child_path_bound(child);
      if(!bound)fail(Code::corrupt,"indexed directory shape has an unmeasured child");
      expected.replace({},bound);
    }
    if(admission)admission(indexed_inode_allocation(child),child.security);
    reachable=add(reachable,child.directory?verify_namespace(child,units,components+1,admission):1);
  });
  if(directory.shape && expected!=*directory.shape)
    fail(Code::corrupt,"indexed directory shape differs from its children");
  return reachable;
}

void IndexedVolumeView::verify_inventory(bool published,const IndexedReopenAdmission& admission) const {
  // Related lookups share the bounded scratch cache for this audit. The
  // verified object walk still checks both copies directly.
  const auto query=store_.scoped_query();
  const auto names=index_.scoped_audit();
  auto remaining_name_pages=index_.verify_reachable_pages();
  const auto root_inode=referenced_inode(1);
  if(admission)admission(indexed_inode_allocation(root_inode),root_inode.security);
  if(verify_namespace(root_inode,0,0,admission)!=root_.inodes-root_.detached_inodes)
    fail(Code::corrupt,"indexed namespace has unreachable inodes");
  std::uint64_t inodes=0,extents=0,charged=0,lifetimes=0,detached=0,retiring=0;
  std::uint64_t bases=0,members=0;
  bool found_root=false;
  IndexedSharedBaseCache checked_bases;
  std::optional<IndexedInodeInput> extent_owner;
  store_.visit_verified_objects([&](std::uint64_t id,Bytes bytes) {
    if(id==namespace_object) {
      if(!published || bytes!=encode_root(root_))
        fail(Code::corrupt,"unexpected indexed root publication");
      found_root=true;
    } else if(domain(id)==inode_domain) {
      const auto entry=decode_inode_with_lifetime(store_,root_,id&ordinal_mask,bytes);
      ++inodes;charged=add(charged,inode_charge(entry));
      if(active_lifetime(entry.lifetime))++lifetimes;
      if(indexed_detached(entry.lifetime))++detached;
      if(entry.lifetime==IndexedLifetime::retiring)++retiring;
      if(entry.directory && entry.lifetime!=IndexedLifetime::linked &&
          !index_.list(entry.inode,{},1).empty())
        fail(Code::corrupt,"indexed deleted directory retains children");
    } else if(domain(id)==lifetime_domain || domain(id)==retirement_domain) {
      const auto entry=referenced_inode(id&ordinal_mask,true);
      if(root_.format<2 || marker_id(entry.inode,entry.lifetime)!=id)
        fail(Code::corrupt,"unreferenced indexed lifetime marker");
    } else if(domain(id)==kNativeIndexedDataObject) {
      if(!(id&ordinal_mask) || (id&ordinal_mask)>=root_.next_extent)
        fail(Code::corrupt,"indexed extent allocator mismatch");
      if(bytes.size()>extent_header+kNativeExtentBytes)
        fail(Code::storage_failure,"indexed extent exceeds its frame bound");
      if(magic(bytes,base_magic)) {
        verify_base_frame(bytes);
        if(root_.format<3 || !index_.group_reference_count(id))
          fail(Code::corrupt,"unreferenced indexed shared base");
        ++bases;
        return;
      }
      if(bytes.size()<=extent_header)fail(Code::corrupt,"truncated indexed extent");
      const auto inode=get(bytes,8);
      if(!extent_owner || extent_owner->inode!=inode)extent_owner=referenced_inode(inode,true);
      const auto& owner=*extent_owner;
      const auto block=get(bytes,16);
      const auto decoded=decode_extent(store_,index_,root_,id,std::move(bytes),owner,block,
          false,nullptr,&checked_bases);
      if(decoded.base)++members;
      if(index_.find(native_extent_key(owner.inode,block))!=id)
        fail(Code::corrupt,"unreferenced indexed extent");
      ++extents;
      if(!owner.allocation)charged=add(charged,1);
    } else if(domain(id)==kNativeNameIndexObject)index_.account_verified_page(id,remaining_name_pages);
    else fail(Code::corrupt,"foreign indexed object domain");
  });
  if(remaining_name_pages)fail(Code::corrupt,"missing name index pages");
  if(found_root!=published || inodes!=root_.inodes || extents!=root_.extents || charged!=root_.charged_extents ||
      lifetimes!=root_.active_lifetimes || detached!=root_.detached_inodes || retiring!=root_.retiring_inodes ||
      bases!=root_.shared_bases || members!=root_.shared_members)
    fail(Code::corrupt,"indexed volume inventory mismatch");
}
void IndexedVolumeView::verify_all(const IndexedReopenAdmission& admission) const {verify_inventory(true,admission);}

void IndexedVolumeView::verify_equivalent(const IndexedVolumeView& target) const {
  auto rebound=root_;
  rebound.identity=target.root_.identity;
  if(rebound!=target.root_)fail(Code::invalid_operation,"indexed handoff changes the native root");
  std::uint64_t after=0;
  for(;;) {
    const auto left=store_.list_page(after,64);
    const auto right=target.store_.list_page(after,64);
    if(left.size()!=right.size())fail(Code::invalid_operation,"indexed handoff changes the inventory");
    if(left.empty())return;
    for(std::size_t i=0;i<left.size();++i) {
      const auto& a=left[i];const auto& b=right[i];
      if(a.id!=b.id || a.generation!=b.generation || a.logical_bytes!=b.logical_bytes)
        fail(Code::invalid_operation,"indexed handoff changes object identity or length");
      if(a.id!=namespace_object && store_.read(a.id)!=target.store_.read(b.id))
        fail(Code::invalid_operation,"indexed handoff changes logical object bytes");
    }
    after=left.back().id;
  }
}

IndexedVolumeBuilder::IndexedVolumeBuilder(PagedObjectStoreWriter output,std::uint64_t sequence,
    std::uint64_t next_inode,NativeWorkloadInfo workload,std::uint32_t format):writer_(std::move(output)) {
  root_.identity=writer_->snapshot().identity();
  root_.sequence=sequence;root_.next_inode=next_inode;root_.workload=workload;root_.format=format;
  validate_header_fields(root_);
  NativeNameIndex::initialize(*writer_,{},NativeIndexKind::names_and_extents);
}
void IndexedVolumeBuilder::append(std::span<const IndexedInodeInput> inodes,std::span<const IndexedExtentInput> extents) {
  if(!writer_)fail(Code::invalid_operation,"indexed volume is already published");
  if(inodes.empty() && extents.empty())fail(Code::invalid_operation,"empty indexed import batch");
  if(inodes.size()>=kStoreMaxObjects || extents.size()>=kStoreMaxObjects-inodes.size())
    fail(Code::limit,"indexed import batch budget");
  const auto snapshot=writer_->snapshot();
  auto next=root_;
  std::map<std::uint64_t,const IndexedInodeInput*> incoming;
  std::vector<NativeNameEdit> edits;
  std::vector<StoreMutation> records;
  std::size_t input_bytes=0;
  const auto append_record=[&](std::uint64_t id,Bytes bytes) {
    if(bytes.size()>kStoreMaxTransactionBytes-input_bytes)fail(Code::limit,"indexed import input budget");
    input_bytes+=bytes.size();
    records.push_back({id,false,std::move(bytes)});
  };
  for(const auto& inode:inodes) {
    if(inode.lifetime!=IndexedLifetime::linked)
      fail(Code::invalid_operation,"indexed import requires a resolved inode");
    auto frame=encode_indexed_inode(inode,next);
    const auto id=inode_domain|inode.inode;
    if(!incoming.emplace(inode.inode,&inode).second || snapshot.stat(id))
      fail(Code::already_exists,"indexed import repeats an inode");
    if(inode.inode!=1)edits.push_back({{inode.parent,name_key(inode.name)},{},inode.inode});
    append_record(id,std::move(frame));
    ++next.inodes;next.charged_extents=add(next.charged_extents,inode_charge(inode));
  }
  for(const auto& extent:extents) {
    const auto staged=incoming.find(extent.inode);
    const auto inode=staged==incoming.end()?load_inode(snapshot,next,extent.inode):*staged->second;
    if(next.next_extent==kNativeIndexedInodeLimit)fail(Code::limit,"indexed extent identity exhausted");
    const auto id=kNativeIndexedDataObject|next.next_extent++;
    append_record(id,encode_extent(inode,extent));
    edits.push_back({native_extent_key(inode.inode,extent.block),{},id});
    ++next.extents;
    if(!inode.allocation)next.charged_extents=add(next.charged_extents,1);
  }
  if(edits.empty())writer_->commit(records);
  else NativeNameIndex::commit(*writer_,edits,std::move(records));
  root_=next;
}
NativeFilesystemSnapshot IndexedVolumeBuilder::finish() {
  if(!writer_)fail(Code::invalid_operation,"indexed volume is already published");
  const auto staged=writer_->snapshot();
  IndexedVolumeView(staged,root_).verify_inventory(false);
  const auto info=staged.info();
  if(info.degraded_roots || info.degraded_metadata || info.degraded_payloads)
    fail(Code::storage_failure,"indexed import has degraded copies");
  const StoreMutation root{namespace_object,false,encode_root(root_)};
  writer_->commit({&root,1});
  auto published=writer_->snapshot();
  writer_.reset();
  return NativeFilesystemSnapshot::open(std::move(published));
}
} // namespace flitzfs::detail
