#include "flitzfs/native_filesystem.hpp"
#include "native_object_storage.hpp"
#include "native_indexed_volume.hpp"
#include "native_lz4_capture.hpp"
#include "native_lz4_decode_wire.hpp"
#include "store_read_scope.hpp"
#include "indexed_names.hpp"
#include "native_name_index.hpp"
#include "flitzfs/catalog.hpp"
#include "flitzfs/directory_order.hpp"
#include "record_digest.hpp"
#include "protected_stripe_image.hpp"
#include "native_group_codec.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <new>
#include <iterator>
#include <type_traits>
#include <set>
#include <utility>

namespace flitzfs {
bool native_cleanup_metadata_valid(const NativeCleanupMetadata& value) noexcept {
  const bool time=value.set_access_time || value.set_write_time || value.set_change_time;
  return value.utc_time<=static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) &&
      (time || value.utc_time==0);
}
bool native_overwrite_metadata_valid(const NativeOverwriteMetadata& value) noexcept {
  return value.utc_time<=static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) &&
      !(value.attributes&~(kNativeBasicAttributeMask&~0x10U));
}
namespace {
using Bytes=std::vector<std::byte>;
using Code=NativeFsErrorCode;
constexpr std::uint64_t kNamespaceObject=1;
constexpr std::uint64_t kInodeObjectBit=1ULL<<63;
constexpr std::uint64_t kDirectoryObjectBit=1ULL<<62;
constexpr std::size_t kMetadataCap=kNativeExtentBytes,kDirtyCap=kNativeMaxDirtyExtents;
constexpr std::size_t kBasicNodeHeader=88,kSecurityNodeHeader=96,kAllocationNodeHeader=104;
static_assert(64+kNativeMaxNodes*(kBasicNodeHeader+100)+16*kNativeMaxExtents<=kMetadataCap);
static_assert(64+kNativeMaxNodes*(kSecurityNodeHeader+100)+16*kNativeMaxExtents<=kMetadataCap);
static_assert(64+kNativeMaxNodes*(kAllocationNodeHeader+100)+16*kNativeMaxExtents<=kMetadataCap);
static_assert(kDirtyCap*kNativeExtentBytes+kMetadataCap<=kStoreMaxTransactionBytes);
static_assert(24*kNativeMaxNodes+112*(kNativeMaxNodes-1)<=kMetadataCap);
static_assert(kNativeMaxExtents*kNativeExtentBytes+kMetadataCap<=kStoreMaxLogicalBytes);
constexpr std::array<std::byte,8> kMagic{std::byte{'F'},std::byte{'L'},std::byte{'T'},std::byte{'X'},std::byte{'N'},std::byte{'S'},std::byte{'0'},std::byte{'1'}};
constexpr std::array<std::byte,8> kInodeRootMagic{std::byte{'F'},std::byte{'L'},std::byte{'T'},std::byte{'X'},std::byte{'P'},std::byte{'N'},std::byte{'0'},std::byte{'1'}};
constexpr std::array<std::byte,8> kDirectoryMagic{std::byte{'F'},std::byte{'L'},std::byte{'T'},std::byte{'X'},std::byte{'D'},std::byte{'N'},std::byte{'0'},std::byte{'1'}};
constexpr std::size_t kDirectoryHeaderBytes=24,kDirectoryEntryHeaderBytes=12;
constexpr auto kLifecycleMagic=[] {auto magic=kMagic;magic[7]=std::byte{'2'};return magic;}();
constexpr auto kBasicMagic=[] {auto magic=kMagic;magic[7]=std::byte{'3'};return magic;}();
constexpr auto kSecurityMagic=[] {auto magic=kMagic;magic[7]=std::byte{'4'};return magic;}();
constexpr auto kAllocationMagic=[] {auto magic=kMagic;magic[7]=std::byte{'5'};return magic;}();
constexpr auto kWorkloadMagic=[] {auto magic=kMagic;magic[7]=std::byte{'6'};return magic;}();
constexpr auto kSharedMagic=[] {auto magic=kMagic;magic[7]=std::byte{'7'};return magic;}();
const std::array<std::byte,8>& namespace_magic(std::uint32_t format) {
  return format==1?kMagic:format==2?kLifecycleMagic:format==3?kBasicMagic:format==4?kSecurityMagic:format==5?kAllocationMagic:format==6?kWorkloadMagic:kSharedMagic;
}
std::size_t node_header(std::uint32_t format) {return format>=5?kAllocationNodeHeader:format==4?kSecurityNodeHeader:format==3?kBasicNodeHeader:64;}
[[noreturn]] void fail(Code code,const char* reason){throw NativeFsError(code,reason);}
detail::IndexedVolumeView verify_indexed_root(const PagedObjectStoreSnapshot& snapshot,
    const detail::IndexedVolumeRoot& expected) {
  const auto bytes=snapshot.read(kNamespaceObject,kMetadataCap);
  const detail::IndexedVolumeView view(snapshot,bytes);
  if(view.root()!=expected)fail(Code::corrupt,"indexed writer root differs from backing");
  return view;
}
bool zeros(std::span<const std::byte> bytes){return std::all_of(bytes.begin(),bytes.end(),[](auto b){return b==std::byte{0};});}
std::uint64_t integer(std::span<const std::byte> bytes,std::size_t at,unsigned width) {
  if(at>bytes.size() || width>bytes.size()-at || width>8)fail(Code::corrupt,"native metadata field bounds");
  std::uint64_t value=0;for(unsigned i=0;i<width;++i)value|=std::uint64_t(std::to_integer<unsigned char>(bytes[at+i]))<<(8*i);
  return value;
}
void put(std::span<std::byte> bytes,std::size_t at,std::uint64_t value,unsigned width) {
  for(unsigned i=0;i<width;++i)bytes[at+i]=static_cast<std::byte>(value>>(8*i));
}
struct Node {
  std::uint64_t inode=0,parent=0,size=0,born=0,modified=0;
  bool directory=false,delete_pending=false,unlinked=false;
  std::optional<NativeBasicMetadata> basic;
  std::optional<NativeSecurityDescriptor> security;
  std::optional<std::uint64_t> allocation;
  std::string name,key;
  std::map<std::uint64_t,std::uint64_t> extents; // Logical block -> object ID.
  bool operator==(const Node&) const = default;
};
struct Model {
  StoreIdentity identity{};
  std::uint64_t sequence=1,next_inode=2,committed_input_bytes=0;
  std::uint32_t format=1;
  bool inode_records=false;
  bool indexed_names=false;
  bool shared_bases=false;
  std::map<std::uint64_t,detail::NativeGroupReference> members;
  std::set<std::uint64_t> bases;
  std::map<std::uint64_t,Node> nodes;
  std::map<std::pair<std::uint64_t,std::string>,std::uint64_t> names;
};
static_assert(std::is_nothrow_move_assignable_v<Model>);
static_assert(std::is_trivially_copyable_v<NativeWorkloadInfo>);
static_assert(sizeof(NativeWorkloadInfo)<=16);
static_assert(std::is_nothrow_move_assignable_v<NativeFsCommit>);
static_assert(std::is_nothrow_move_constructible_v<NativeInodeWriteCommit>);
static_assert(std::is_nothrow_move_constructible_v<NativeFsCommit>);
static_assert(std::is_nothrow_move_constructible_v<NativeInodeMutationCommit>);
static_assert(std::is_nothrow_move_constructible_v<NativeInodeCreateCommit>);
static_assert(std::is_nothrow_copy_assignable_v<NativeInodeCommitInfo>);
static_assert(sizeof(NativeInodeCommitInfo)<=80);
static_assert(std::is_nothrow_copy_assignable_v<NativeInodeAllocationInfo>);
static_assert(std::is_nothrow_move_constructible_v<NativeInodeAllocationCommit>);
static_assert(std::is_nothrow_move_constructible_v<NativeInodeStorageMutationCommit>);
static_assert(sizeof(NativeInodeAllocationInfo)<=96);
static_assert(sizeof(std::optional<std::uint64_t>)<=16);
NativeInodeCommitInfo commit_info(const Node& n) {
  return {n.inode,n.size,n.modified,n.basic,n.directory,n.delete_pending,n.unlinked};
}
std::uint64_t aligned_allocation(std::uint64_t bytes) {
  return (bytes+kNativeAllocationUnit-1)/kNativeAllocationUnit*kNativeAllocationUnit;
}
NativeAllocationUsage allocation_usage(const Model& model) {
  NativeAllocationUsage result;
  for(const auto& [id,n]:model.nodes) {
    (void)id;result.live_extents+=n.extents.size();
    result.charged_extents+=n.allocation?
        static_cast<std::size_t>((*n.allocation+kNativeExtentBytes-1)/kNativeExtentBytes):n.extents.size();
  }
  return result;
}
std::string canonical(std::string_view path,bool root=false) {
  try{return canonical_catalog_path(path,root);}
  catch(const std::bad_alloc&){throw;}
  catch(const std::exception&){fail(Code::invalid_path,"invalid native virtual path");}
}
std::string component(std::string_view name) {
  if(name.empty() || name.size()>100 || name.find('/')!=std::string_view::npos)fail(Code::invalid_path,"invalid native name component");
  return canonical(name);
}
const Node& node(const Model& model,std::uint64_t id) {
  const auto it=model.nodes.find(id);if(it==model.nodes.end())fail(Code::corrupt,"missing native inode");
  return it->second;
}
const Node& selected_inode(const Model& model,std::uint64_t id) {
  const auto it=model.nodes.find(id);
  if(!id || it==model.nodes.end())fail(Code::not_found,"native inode not found");
  return it->second;
}
Node& node(Model& model,std::uint64_t id) {
  const auto it=model.nodes.find(id);if(it==model.nodes.end())fail(Code::corrupt,"missing native inode");
  return it->second;
}
const Node* child(const Model& model,std::uint64_t parent,std::string_view key) {
  const auto found=model.names.find({parent,std::string(key)});
  return found==model.names.end()?nullptr:&node(model,found->second);
}
bool has_children(const Model& model,std::uint64_t parent) {
  const auto found=model.names.lower_bound({parent,{}});
  return found!=model.names.end() && found->first.first==parent;
}
void rebuild_names(Model& model) {
  model.names.clear();
  for(const auto& [id,n]:model.nodes)if(id!=1 && !n.unlinked)
    if(!model.names.emplace(std::pair{n.parent,n.key},id).second)
      fail(Code::corrupt,"duplicate native name index entry");
}
std::uint64_t resolve(const Model& model,std::string_view path,
    NativeFsError::MissingPath last=NativeFsError::MissingPath::name) {
  const auto key=canonical(path,true);
  std::uint64_t current=1;
  for(std::size_t start=0;start<key.size();) {
    const auto& parent=node(model,current);if(!parent.directory)fail(Code::not_directory,"native parent is a file");
    const auto slash=key.find('/',start),end=slash==std::string::npos?key.size():slash;
    const auto* found=child(model,current,std::string_view(key).substr(start,end-start));
    if(!found)throw NativeFsError(slash==std::string::npos?last:NativeFsError::MissingPath::parent,
        "native path not found");
    current=found->inode;start=end+1;
  }
  return current;
}
std::pair<std::uint64_t,std::string> destination(const Model& model,std::string_view path) {
  (void)canonical(path);
  const auto slash=path.rfind('/');
  const auto parent=resolve(model,slash==std::string_view::npos?std::string_view{}:path.substr(0,slash),
      NativeFsError::MissingPath::parent);
  if(!node(model,parent).directory)fail(Code::not_directory,"native parent is a file");
  if(node(model,parent).delete_pending)fail(Code::invalid_operation,"native destination parent is delete-pending");
  return {parent,std::string(slash==std::string_view::npos?path:path.substr(slash+1))};
}
std::string display_path(const Model& model,const Node& n) {
  if(n.unlinked)return {};
  std::string path=n.name;auto parent=n.parent;unsigned depth=0;
  while(parent) {
    if(++depth>16)fail(Code::limit,"native parent cycle/depth");
    const auto& p=node(model,parent);
    if(p.inode!=1)path=p.name+"/"+path;
    parent=p.parent;
  }
  return path;
}
NativeFileInfo file_info(const Model& model,const Node& n) {
  return {n.inode,n.parent,n.size,n.born,n.modified,n.directory,display_path(model,n)};
}
std::size_t extent_count(const Model& model) {
  std::size_t count=0;for(const auto& [id,n]:model.nodes){(void)id;count+=n.extents.size();}return count;
}
void validate_basic(const NativeBasicMetadata& value,bool directory) {
  constexpr auto maximum=static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
  if(value.creation_time>maximum || value.last_access_time>maximum ||
      value.last_write_time>maximum || value.change_time>maximum ||
      (value.attributes&~kNativeBasicAttributeMask) ||
      bool(value.attributes&0x10U)!=directory || (directory && (value.attributes&0x100U)))
    fail(Code::invalid_operation,"invalid canonical native Windows basic metadata");
}
void validate_structure(const Model& model,bool pending=false) {
  if(model.nodes.empty() || model.nodes.size()>kNativeMaxNodes || model.sequence==0)fail(Code::limit,"native namespace budget");
  if(model.format<1 || model.format>7 || (model.format<6 && model.committed_input_bytes))
    fail(Code::corrupt,"native namespace capability/accounting");
  if(model.inode_records && (model.format!=7 || model.next_inode>kInodeObjectBit))
    fail(Code::corrupt,"inode-record capability/allocator");
  if(model.indexed_names && (!model.inode_records || model.next_inode>kDirectoryObjectBit))
    fail(Code::corrupt,"directory-index capability/allocator");
  if(model.shared_bases && !model.indexed_names)fail(Code::corrupt,"shared-base namespace capability");
  if(!model.shared_bases && (!model.members.empty() || !model.bases.empty()))fail(Code::corrupt,"legacy shared dependency cache");
  const auto& root=node(model,1);
  if(root.parent || !root.directory || !root.name.empty() || root.size || !root.extents.empty() ||
      root.born!=1 || root.delete_pending || root.unlinked)
    fail(Code::corrupt,"native root invariant");
  std::set<std::pair<std::uint64_t,std::string>> names;
  std::set<std::uint64_t> objects;
  std::uint64_t largest=0;
  for(const auto& [id,n]:model.nodes) {
    if(model.inode_records && id>=kInodeObjectBit)fail(Code::limit,"inode-record identity exhausted");
    if(model.indexed_names && id>=kDirectoryObjectBit)fail(Code::limit,"directory-index identity exhausted");
    if(id!=n.inode || !id || n.born==0 || n.born>n.modified || n.modified>model.sequence ||
        n.size>kNativeMaxFileBytes || (n.directory && (n.size || !n.extents.empty())) ||
        (n.delete_pending && n.unlinked) || (model.format==1 && (n.delete_pending || n.unlinked)))
      fail(Code::corrupt,"native inode invariant");
    if(n.allocation && (model.format<5 || n.directory ||
        *n.allocation%kNativeAllocationUnit || *n.allocation<n.size || *n.allocation>kNativeAllocationCapacity))
      fail(Code::corrupt,"native allocation reservation invariant");
    if(n.basic) {
      if(model.format<3)fail(Code::corrupt,"basic metadata without namespace capability");
      validate_basic(*n.basic,n.directory);
    }
    if(n.security && (model.format<4 || !native_security_descriptor_framed(*n.security)))
      fail(Code::corrupt,"security descriptor framing/capability");
    largest=(std::max)(largest,id);
    if(id!=1) {
      if(n.unlinked) {
        if(n.parent || !n.name.empty() || !n.key.empty())fail(Code::corrupt,"detached native inode has a name/parent");
      } else {
        if(!n.parent || n.parent==id || !node(model,n.parent).directory ||
            node(model,n.parent).unlinked || node(model,n.parent).delete_pending ||
            component(n.name)!=n.key || !names.emplace(n.parent,n.key).second)
          fail(Code::corrupt,"native parent/name collision");
        (void)canonical(display_path(model,n));
        const auto indexed=model.names.find({n.parent,n.key});
        if(indexed==model.names.end() || indexed->second!=id)fail(Code::corrupt,"native name index differs from inode");
      }
    }
    for(const auto& [block,object]:n.extents) {
      if(block>=kNativeMaxFileBytes/kNativeExtentBytes || block*kNativeExtentBytes>=n.size ||
          (!pending && (object<2 || object>kStoreMaxObjects)) ||
          (object && ((model.format<7 && !objects.insert(object).second) || object<2 || object>kStoreMaxObjects)))
        fail(Code::corrupt,"native extent ownership/bounds");
    }
  }
  if(model.next_inode<=largest || model.next_inode<2)fail(Code::corrupt,"native inode allocator");
  if(names.size()!=model.names.size())fail(Code::corrupt,"foreign native name index entry");
  if(!pending && extent_count(model)>kNativeMaxExtents)fail(Code::limit,"native live extent budget");
  if(allocation_usage(model).charged_extents>kNativeMaxExtents)
    fail(Code::limit,"native reserved/live extent budget");
}
Bytes object_bytes(const detail::NativeObjectSnapshot& objects,std::uint64_t id,
    std::size_t maximum_decoded_bytes=kStoreMaxObjectBytes) {
  try{
    if(const auto* paged=objects.paged_if())return paged->read(id,maximum_decoded_bytes);
    return objects.read(id);
  }
  catch(const std::bad_alloc&){throw;}
  catch(const std::exception&){fail(Code::storage_failure,"native backing object verification failed");}
}
detail::NativeGroupReference group_reference(std::span<const std::byte> bytes) {
  try{return detail::parse_native_group_member(bytes);}
  catch(const std::bad_alloc&){throw;}
  catch(const std::exception&){fail(Code::corrupt,"invalid native shared-base frame");}
}
bool group_base_id(std::uint64_t id){return id>detail::kNativeGroupBaseBit && id<kDirectoryObjectBit;}
Bytes extent_bytes(const detail::NativeObjectSnapshot& objects,const Model& model,std::uint64_t id) {
  auto bytes=object_bytes(objects,id);const auto found=model.members.find(id);
  if(found==model.members.end()) {
    if(bytes.size()!=kNativeExtentBytes)fail(Code::corrupt,"native plain extent size");return bytes;
  }
  if(!model.shared_bases || group_reference(bytes)!=found->second)fail(Code::corrupt,"shared dependency cache differs");
  const auto& ref=found->second;const auto base=objects.stat(ref.base_id);
  if(!base || base->generation!=ref.base_generation || base->logical_bytes!=kNativeExtentBytes)
    fail(Code::corrupt,"shared base missing/generation differs");
  const auto data=object_bytes(objects,ref.base_id);
  try{return detail::decode_native_group_member(bytes,data);}
  catch(const std::bad_alloc&){throw;}
  catch(const std::exception&){fail(Code::corrupt,"shared extent CPU verification failed");}
}
void refresh_group_bases(Model& model) {
  model.bases.clear();for(const auto& [id,ref]:model.members){(void)id;model.bases.insert(ref.base_id);}
}
template<class Read> void validate_data(const Model& model,Read read) {
  for(const auto& [id,n]:model.nodes)for(const auto& [block,object]:n.extents) {
    const auto bytes=read(id,block,object);
    if(bytes.size()!=kNativeExtentBytes || zeros(bytes))fail(Code::corrupt,"native noncanonical extent bytes");
    const auto begin=block*kNativeExtentBytes;
    if(n.size-begin<kNativeExtentBytes &&
        !zeros(std::span<const std::byte>(bytes).subspan(static_cast<std::size_t>(n.size-begin))))
      fail(Code::corrupt,"native bytes beyond EOF");
  }
}
std::size_t inode_frame_size(const Node& n, std::uint32_t format) noexcept {
  return node_header(format) + n.name.size() + 16*n.extents.size() +
      (n.security ? n.security->size() : 0);
}
std::size_t checked_namespace_size(const Model& model) {
  validate_structure(model);
  std::size_t size = 64;
  for (const auto& [id,n] : model.nodes) {
    (void)id;
    const auto inode_bytes = inode_frame_size(n, model.format);
    if (inode_bytes > kMetadataCap - size) fail(Code::limit, "native metadata budget");
    size += inode_bytes;
  }
  return size;
}
void encode_namespace_header(std::span<std::byte> bytes, const Model& model) noexcept {
  const auto& magic=namespace_magic(model.format);
  std::copy(magic.begin(),magic.end(),bytes.begin());
  put(bytes,8,model.format,4);put(bytes,12,model.nodes.size(),4);put(bytes,16,model.sequence,8);
  put(bytes,24,model.next_inode,8);put(bytes,32,kNativeExtentBytes,4);put(bytes,36,extent_count(model),4);
  std::copy(model.identity.begin(),model.identity.end(),bytes.begin()+40);
  if(model.format>=6)put(bytes,56,model.committed_input_bytes,8);
}
void encode_inode(std::span<std::byte> bytes, const Node& n, std::uint32_t format) {
  put(bytes,0,n.inode,8);put(bytes,8,n.parent,8);put(bytes,16,n.directory?1:2,4);
  const auto flags=(n.delete_pending?1U:0U)|(n.unlinked?2U:0U)|(n.basic?4U:0U)|
      (n.security?8U:0U)|(n.allocation?16U:0U);
  put(bytes,20,flags,4);
  put(bytes,24,n.size,8);put(bytes,32,n.born,8);put(bytes,40,n.modified,8);
  put(bytes,48,n.name.size(),2);put(bytes,50,n.extents.size(),2);
  if (n.basic) {
    put(bytes,52,n.basic->attributes,4);put(bytes,56,n.basic->creation_time,8);
    put(bytes,64,n.basic->last_access_time,8);put(bytes,72,n.basic->last_write_time,8);
    put(bytes,80,n.basic->change_time,8);
  }
  if (format>=4) put(bytes,88,n.security?n.security->size():0,4);
  if (format>=5) put(bytes,96,n.allocation.value_or(0),8);
  auto cursor = node_header(format);
  for (const auto c : n.name) bytes[cursor++]=static_cast<std::byte>(static_cast<unsigned char>(c));
  if (n.security) {
    std::copy(n.security->begin(),n.security->end(),bytes.begin()+static_cast<std::ptrdiff_t>(cursor));
    cursor += n.security->size();
  }
  for (const auto& [block,object] : n.extents) {
    put(bytes,cursor,block,8);
    put(bytes,cursor+8,object,8);
    cursor += 16;
  }
}
Bytes inode_frame(const Node& n, std::uint32_t format) {
  Bytes frame(inode_frame_size(n,format));
  encode_inode(frame,n,format);
  return frame;
}
Bytes encode(const Model& model) {
  Bytes bytes(checked_namespace_size(model));
  encode_namespace_header(bytes, model);
  std::size_t cursor = 64;
  for (const auto& [id,n] : model.nodes) {
    (void)id;
    const auto size = inode_frame_size(n, model.format);
    encode_inode(std::span<std::byte>(bytes).subspan(cursor,size), n, model.format);
    cursor += size;
  }
  return bytes;
}
std::size_t inventory_limit(const Model& model) {
  return model.shared_bases?4*kStoreMaxObjects:model.indexed_names?3*kStoreMaxObjects:model.inode_records?2*kStoreMaxObjects:kStoreMaxObjects;
}
using InodeRootBytes=std::array<std::byte,64>;
InodeRootBytes inode_root_bytes(const Model& model) noexcept {
  InodeRootBytes bytes{};
  encode_namespace_header(bytes,model);
  std::copy(kInodeRootMagic.begin(),kInodeRootMagic.end(),bytes.begin());
  put(bytes,8,model.shared_bases?3:model.indexed_names?2:1,4);
  return bytes;
}
Bytes namespace_record(const Model& model) {
  if (!model.inode_records) return encode(model);
  (void)checked_namespace_size(model);
  const auto bytes=inode_root_bytes(model);
  return Bytes(bytes.begin(),bytes.end());
}
Bytes directory_bytes(const Model& model,std::uint64_t directory) {
  Bytes bytes(kDirectoryHeaderBytes);std::copy(kDirectoryMagic.begin(),kDirectoryMagic.end(),bytes.begin());
  put(bytes,8,1,4);put(bytes,16,directory,8);std::uint32_t count=0;
  for(auto it=model.names.lower_bound({directory,{}});it!=model.names.end() && it->first.first==directory;++it) {
    const auto& key=it->first.second;const auto at=bytes.size();
    bytes.resize(at+kDirectoryEntryHeaderBytes+key.size());
    put(bytes,at,it->second,8);put(bytes,at+8,key.size(),2);
    for(std::size_t i=0;i<key.size();++i)
      bytes[at+kDirectoryEntryHeaderBytes+i]=static_cast<std::byte>(static_cast<unsigned char>(key[i]));
    ++count;
  }
  if(bytes.size()>kMetadataCap)fail(Code::limit,"native directory index budget");
  put(bytes,12,count,4);return bytes;
}
bool directory_matches(std::span<const std::byte> bytes,const Model& model,std::uint64_t directory) {
  if(bytes.size()<kDirectoryHeaderBytes || bytes.size()>kMetadataCap ||
      !std::equal(kDirectoryMagic.begin(),kDirectoryMagic.end(),bytes.begin()) ||
      integer(bytes,8,4)!=1 || integer(bytes,16,8)!=directory)
    return false;
  std::size_t at=kDirectoryHeaderBytes,count=0;
  for(auto it=model.names.lower_bound({directory,{}});it!=model.names.end() && it->first.first==directory;++it) {
    const auto& key=it->first.second;
    const auto size=kDirectoryEntryHeaderBytes+key.size();
    if(bytes.size()-at<size || integer(bytes,at,8)!=it->second ||
        integer(bytes,at+8,2)!=key.size() || integer(bytes,at+10,2)!=0)
      return false;
    const std::string_view stored(reinterpret_cast<const char*>(bytes.data()+at+kDirectoryEntryHeaderBytes),key.size());
    if(stored!=key)return false;
    at+=size;++count;
  }
  return at==bytes.size() && integer(bytes,12,4)==count;
}
void verify_directory(const detail::NativeObjectSnapshot& objects,const Model& model,const Node& directory) {
  if(!directory.directory)fail(Code::not_directory,"native enumeration of file");
  if(model.indexed_names &&
      !directory_matches(object_bytes(objects,kDirectoryObjectBit|directory.inode,kMetadataCap),model,directory.inode))
    fail(Code::corrupt,"native directory index changed since admission");
}
bool inode_matches(const detail::NativeObjectSnapshot& objects,const Model& model,const Node& n) {
  const auto expected=inode_frame(n,model.format);
  return object_bytes(objects,kInodeObjectBit|n.inode,expected.size())==expected;
}
void verify_inode(const detail::NativeObjectSnapshot& objects,const Model& model,const Node& n) {
  if(model.inode_records && !inode_matches(objects,model,n))
    fail(Code::corrupt,"native inode changed since admission");
}
const Node& resolve_snapshot(const detail::NativeObjectSnapshot& objects,const Model& model,
    std::string_view path,bool verify_inodes=false) {
  if(!model.indexed_names && !(verify_inodes && model.inode_records))return node(model,resolve(model,path));
  const auto key=canonical(path,true);
  const auto* current=&node(model,1);
  for(std::size_t start=0;start<key.size();) {
    const auto slash=key.find('/',start),end=slash==std::string::npos?key.size():slash;
    const auto component=std::string_view(key).substr(start,end-start);
    if(verify_inodes)verify_inode(objects,model,*current);
    verify_directory(objects,model,*current);
    const auto* found=child(model,current->inode,component);
    if(!found)throw NativeFsError(slash==std::string::npos?NativeFsError::MissingPath::name:
        NativeFsError::MissingPath::parent,"native indexed path not found");
    current=found;start=end+1;
  }
  if(verify_inodes)verify_inode(objects,model,*current);
  return *current;
}
void namespace_mutations(std::vector<StoreMutation>& output,const Model& model,const Model* previous=nullptr) {
  if (model.inode_records && previous && !previous->inode_records)
    fail(Code::invalid_operation,"inode layout conversion requires a new image");
  output.push_back({kNamespaceObject,false,namespace_record(model)});
  if (!model.inode_records) return;
  if (previous) (void)checked_namespace_size(*previous);
  for (const auto& [id,n] : model.nodes) {
    if (previous && previous->format==model.format) {
      const auto old = previous->nodes.find(id);
      if (old!=previous->nodes.end() && n==old->second) continue;
    }
    output.push_back({kInodeObjectBit|id,false,inode_frame(n,model.format)});
  }
  if (previous) for (const auto& [id,n] : previous->nodes) {
    (void)n;
    if (!model.nodes.contains(id)) output.push_back({kInodeObjectBit|id,true,{}});
  }
  if(model.indexed_names) {
    if(previous && !previous->indexed_names)fail(Code::invalid_operation,"directory index conversion requires a new image");
    for(const auto& [id,n]:model.nodes)if(n.directory) {
      auto bytes=directory_bytes(model,id);const auto old_node=previous?previous->nodes.find(id):model.nodes.end();
      if(!previous || old_node==previous->nodes.end() || bytes!=directory_bytes(*previous,id))
        output.push_back({kDirectoryObjectBit|id,false,std::move(bytes)});
    }
    if(previous)for(const auto& [id,n]:previous->nodes)if(n.directory && !model.nodes.contains(id))
      output.push_back({kDirectoryObjectBit|id,true,{}});
  }
}
bool namespace_matches(const detail::NativeObjectSnapshot& objects,const Model& model) {
  if(!model.inode_records)return object_bytes(objects,kNamespaceObject)==encode(model);
  if(object_bytes(objects,kNamespaceObject)!=namespace_record(model))return false;
  for(const auto& entry:model.nodes)
    if(!inode_matches(objects,model,entry.second))return false;
  if(model.indexed_names)for(const auto& [id,n]:model.nodes)
    if(n.directory && !directory_matches(object_bytes(objects,kDirectoryObjectBit|id),model,id))return false;
  return true;
}
Node decode_inode(std::span<const std::byte> bytes, std::uint32_t format,
    std::size_t& cursor, std::uint64_t previous,std::uint32_t name_profile=0) {
  const auto header=node_header(format);
  if(cursor>bytes.size() || bytes.size()-cursor<header)fail(Code::corrupt,"truncated native inode");
  const auto h=bytes.subspan(cursor,header);cursor+=header;
  Node n;n.inode=integer(h,0,8);n.parent=integer(h,8,8);const auto kind=integer(h,16,4);
  const auto flags=integer(h,20,4);
  n.directory=kind==1;n.delete_pending=(flags&1)!=0;n.unlinked=(flags&2)!=0;
  n.size=integer(h,24,8);n.born=integer(h,32,8);n.modified=integer(h,40,8);
  const auto name_bytes=integer(h,48,2),extents=integer(h,50,2);
  const auto maximum_name=name_profile==detail::kIndexedOrdinalNames?detail::kIndexedNameBytes:100;
  if((kind!=1 && kind!=2) || n.inode<=previous || name_bytes>maximum_name || extents>kNativeMaxExtents ||
      (flags&~std::uint64_t{31}) || (format==1 && flags) || (flags&3)==3 ||
      (format<3 && (flags&4)) || (format<4 && (flags&8)) || (format<5 && (flags&16)) ||
      (format>=5 && !(flags&16) && integer(h,96,8)) ||
      (!(flags&4) && !zeros(h.subspan(52,(std::min)(header,std::size_t{88})-52))) ||
      (format>=4 && (integer(h,92,4) || bool(flags&8)!=(integer(h,88,4)!=0))) ||
      name_bytes>bytes.size()-cursor)fail(Code::corrupt,"invalid native inode fields");
  if(flags&16)n.allocation=integer(h,96,8);
  if(flags&4) {
    n.basic=NativeBasicMetadata{integer(h,56,8),integer(h,64,8),integer(h,72,8),integer(h,80,8),
        static_cast<std::uint32_t>(integer(h,52,4))};
  }
  n.name.assign(reinterpret_cast<const char*>(bytes.data()+cursor),static_cast<std::size_t>(name_bytes));
  cursor+=static_cast<std::size_t>(name_bytes);
  if(flags&8) {
    const auto length=integer(h,88,4);
    if(length>kNativeMaxSecurityBytes || length>bytes.size()-cursor ||
        !native_security_descriptor_framed(bytes.subspan(cursor,static_cast<std::size_t>(length))))
      fail(Code::corrupt,"invalid native stored security descriptor");
    n.security=NativeSecurityDescriptor(bytes.begin()+static_cast<std::ptrdiff_t>(cursor),
        bytes.begin()+static_cast<std::ptrdiff_t>(cursor+length));cursor+=static_cast<std::size_t>(length);
  }
  if(n.inode!=1 && !n.unlinked) {
    try {n.key=name_profile==detail::kIndexedOrdinalNames?detail::indexed_component_key(n.name):component(n.name);}
    catch(const NativeFsError&){fail(Code::corrupt,"invalid native stored component");}
  }
  std::uint64_t prior_block=0;
  for(std::uint64_t e=0;e<extents;++e) {
    if(bytes.size()-cursor<16)fail(Code::corrupt,"truncated native extent");
    const auto block=integer(bytes,cursor,8),object=integer(bytes,cursor+8,8);cursor+=16;
    if(e && block<=prior_block)fail(Code::corrupt,"native extent ordering");
    n.extents.emplace(block,object);prior_block=block;
  }
  return n;
}
Model decode(const detail::NativeObjectSnapshot& objects,std::span<const std::byte> header) {
  if (header.size()<64 || header.size()>kMetadataCap || integer(header,32,4)!=kNativeExtentBytes)
    fail(Code::corrupt,"unsupported native namespace framing");
  const bool inode_records = std::equal(kInodeRootMagic.begin(),kInodeRootMagic.end(),header.begin());
  const auto wire_version = integer(header,8,4);
  const auto count = integer(header,12,4);
  const auto declared_extents = integer(header,36,4);
  const bool indexed_names = inode_records && wire_version>=2;
  const bool shared_bases = inode_records && wire_version==3;
  const auto format = inode_records ? 7U : static_cast<std::uint32_t>(wire_version);
  if (inode_records) {
    if (!objects.paged() || header.size()!=64 || wire_version<1 || wire_version>3 ||
        !count || count>kNativeMaxNodes || declared_extents>kNativeMaxExtents)
      fail(Code::corrupt,"unsupported inode-record root");
    if (shared_bases && !objects.mirrored())
      fail(Code::corrupt,"shared bases require protected payloads");
  } else {
    if (wire_version<1 || wire_version>7 || (format<6 && !zeros(header.subspan(56,8))) ||
        !std::equal(namespace_magic(format).begin(),namespace_magic(format).end(),header.begin()))
      fail(Code::corrupt,"unsupported native namespace capability/magic");
    if (!count || count>kNativeMaxNodes || declared_extents>kNativeMaxExtents)
      fail(Code::limit,"native metadata counts");
  }
  if (format>=2 && !objects.info().replicated_checkpoint)
    fail(Code::corrupt,"native lifetime capability requires replicated current checkpoints");

  Model model;
  model.format=format;
  model.inode_records=inode_records;
  model.indexed_names=indexed_names;
  model.shared_bases=shared_bases;
  if (format>=6) model.committed_input_bytes=integer(header,56,8);
  model.sequence=integer(header,16,8);
  model.next_inode=integer(header,24,8);
  std::copy_n(header.begin()+40,16,model.identity.begin());
  if (model.identity!=objects.identity() || model.sequence>objects.info().generation)
    fail(Code::corrupt,"native identity/sequence binding");
  std::uint64_t previous=0;
  const auto insert = [&](Node n) {
    previous=n.inode;
    model.nodes.emplace(previous,std::move(n));
  };
  std::vector<StoreObjectInfo> catalog;
  if (inode_records) {
    std::size_t observed=0, metadata_bytes=header.size();
    catalog=objects.list(inventory_limit(model));
    for (const auto& record : catalog) {
      if (!(record.id&kInodeObjectBit)) continue;
      if (++observed>count) fail(Code::corrupt,"extra inode metadata record");
      const auto frame=object_bytes(objects,record.id);
      if (frame.size()>kMetadataCap-metadata_bytes)
        fail(Code::corrupt,"inode record aggregate size");
      metadata_bytes+=frame.size();
      std::size_t cursor=0;
      auto inode=decode_inode(frame,format,cursor,previous);
      if (inode.inode!=(record.id^kInodeObjectBit) || cursor!=frame.size())
        fail(Code::corrupt,"inode record identity/size binding");
      insert(std::move(inode));
    }
    if (observed!=count) fail(Code::corrupt,"missing inode metadata record");
  } else {
    std::size_t cursor=64;
    for (std::uint64_t i=0;i<count;++i) insert(decode_inode(header,format,cursor,previous));
    if (cursor!=header.size()) fail(Code::corrupt,"native metadata trailing/count mismatch");
  }
  if (extent_count(model)!=declared_extents)
    fail(Code::corrupt,"native metadata trailing/count mismatch");
  rebuild_names(model);
  try{validate_structure(model);}
  catch(const NativeFsError&){fail(Code::corrupt,"invalid native namespace graph");}
  std::set<std::uint64_t> expected{kNamespaceObject};
  for(const auto& [id,n]:model.nodes){if(inode_records)expected.insert(kInodeObjectBit|id);
    if(indexed_names && n.directory) {
      expected.insert(kDirectoryObjectBit|id);
      if(!directory_matches(object_bytes(objects,kDirectoryObjectBit|id),model,id))
        fail(Code::corrupt,"persisted native directory index differs from inode graph");
    }
    for(const auto& [block,object]:n.extents){(void)block;expected.insert(object);}}
  if (!inode_records) catalog=objects.list(inventory_limit(model));
  if(shared_bases) {
    for(const auto& object:catalog)if(object.id>=2 && object.id<=kStoreMaxObjects && expected.contains(object.id) && object.logical_bytes!=kNativeExtentBytes) {
      const auto ref=group_reference(object_bytes(objects,object.id));model.members.emplace(object.id,ref);model.bases.insert(ref.base_id);
    }
    expected.insert(model.bases.begin(),model.bases.end());
  }
  if(catalog.size()!=expected.size())fail(Code::corrupt,"foreign or dangling native objects");
  for(const auto& object:catalog)if(!expected.contains(object.id) ||
      (object.id!=kNamespaceObject && !(inode_records && (object.id&kInodeObjectBit)) &&
       !(indexed_names && (object.id&kDirectoryObjectBit)) &&
       !(shared_bases && model.members.contains(object.id)) && object.logical_bytes!=kNativeExtentBytes))
    fail(Code::corrupt,"native object extent mismatch");
  validate_data(model,[&](auto,auto,std::uint64_t object){return extent_bytes(objects,model,object);});
  return model;
}
Model decode(const detail::NativeObjectSnapshot& objects) {
  return decode(objects,object_bytes(objects,kNamespaceObject));
}
struct DirtyExtent {
  Bytes bytes;
  std::uint64_t original_object=0;
  bool modified=false,compare_original=false;
};
using Dirty=std::map<std::pair<std::uint64_t,std::uint64_t>,DirtyExtent>;
void admit_lz4_extent_candidates(std::span<const NativeLz4ExtentCandidate> candidates) {
  if(candidates.size()>kDirtyCap)fail(Code::limit,"native advisory extent count");
  std::uint64_t total=0;
  for(const auto& candidate:candidates) {
    if(candidate.payload.size()>kStoreMaxObjectBytes ||
        candidate.payload.size()>kStoreMaxTransactionBytes-total)
      fail(Code::limit,"native advisory byte budget");
    total+=candidate.payload.size();
  }
}
struct Stage {
  Model model;const Model& original;const detail::NativeObjectSnapshot& base;Dirty dirty;
  bool changed=false;std::vector<std::uint64_t> created;
  Stage(const Model& previous,const detail::NativeObjectSnapshot& objects):model(previous),original(previous),base(objects) {
    if(model.sequence==(std::numeric_limits<std::uint64_t>::max)())fail(Code::limit,"native sequence exhausted");
    ++model.sequence;created.reserve(16);
  }
  DirtyExtent& block(Node& n,std::uint64_t index) {
    const auto key=std::pair{n.inode,index};
    if(auto found=dirty.find(key);found!=dirty.end())return found->second;
    if(dirty.size()>=kDirtyCap)fail(Code::limit,"native staging extent budget");
    const auto existing=n.extents.find(index);
    auto bytes=existing==n.extents.end()?Bytes(kNativeExtentBytes):extent_bytes(base,original,existing->second);
    if(bytes.size()!=kNativeExtentBytes)fail(Code::corrupt,"native source extent size");
    std::uint64_t previous_object=0;
    if(const auto inode=original.nodes.find(n.inode);inode!=original.nodes.end())
      if(const auto extent=inode->second.extents.find(index);extent!=inode->second.extents.end())previous_object=extent->second;
    // A prior zeroing/shrink may have removed this reference in the same batch.
    const bool compare_original=previous_object && (existing==n.extents.end() || existing->second!=previous_object);
    return dirty.emplace(key,DirtyExtent{std::move(bytes),previous_object,false,compare_original}).first->second;
  }
  static void changing(DirtyExtent& extent) {
    extent.compare_original=extent.compare_original || extent.modified;
    extent.modified=true;
  }
  void unchanged(const Node& n,std::uint64_t index,const DirtyExtent& extent) {
    // Preserve earlier changes in this batch; discard only a read-only buffer.
    if(!extent.modified)dirty.erase({n.inode,index});
  }
  void finish_block(Node& n,std::uint64_t index) {
    const auto key=std::pair{n.inode,index};auto& bytes=dirty.at(key).bytes;
    if(zeros(bytes)){n.extents.erase(index);dirty.erase(key);}
    // A shared object is immutable for every other inode/block. Detach before
    // ID allocation, including partial writes and shrink-tail zeroing. Exact
    // reversions are independently compared before reattaching the original.
    else if(model.format>=7)n.extents[index]=0;
    else n.extents.try_emplace(index,0);
  }
  void coalesce_equal_extents() {
    // One verified source object at a time, plus one representative. Digests
    // propose candidates only; exact CPU byte comparison authorizes sharing.
    // The maps are bounded by the existing live-object/extent limits. No
    // encoded stream, GPU runtime, external dictionary or filename is a base.
    struct Representative {std::uint64_t object;std::uint32_t stored;};
    std::vector<Representative> groups;
    std::map<detail::Digest,std::vector<std::size_t>> representatives;
    std::map<std::uint64_t,std::size_t> resolved;
    std::map<std::uint64_t,std::uint32_t> costs;
    for(const auto& info:base.list(inventory_limit(model)))costs.emplace(info.id,info.stored_payload_bytes);
    std::size_t comparisons=0;
    for(const auto& [inode,n]:model.nodes)for(const auto& [index,object]:n.extents) {
      (void)inode;(void)index;
      if(resolved.contains(object))continue;
      const auto cost=costs.find(object);
      if(cost==costs.end())fail(Code::corrupt,"coalescing object missing from inventory");
      const auto bytes=extent_bytes(base,original,object);
      if(bytes.size()!=kNativeExtentBytes || zeros(bytes))
        fail(Code::corrupt,"invalid coalescing source extent");
      auto& candidates=representatives[detail::record_digest(bytes)];
      auto selected=groups.size();
      for(const auto candidate:candidates) {
        // Also bound adversarial hash-bucket work; no writes have occurred.
        if(comparisons>=kNativeMaxExtents)fail(Code::limit,"coalescing comparison budget");
        ++comparisons;
        if(extent_bytes(base,original,groups.at(candidate).object)==bytes){selected=candidate;break;}
      }
      if(selected==groups.size()) {
        groups.push_back({object,cost->second});candidates.push_back(selected);
      }else {
        auto& group=groups.at(selected);
        if(cost->second<group.stored || (cost->second==group.stored && object<group.object))
          group={object,cost->second};
      }
      resolved.emplace(object,selected);
    }
    // Final representatives may appear late in inode order and use another
    // CPU-readable codec. Redirect only after choosing the smallest existing
    // payload, with ID as a deterministic tie-breaker. No new encoding is needed.
    for(auto& [inode,n]:model.nodes)for(auto& [index,object]:n.extents) {
      (void)inode;(void)index;
      const auto selected=groups.at(resolved.at(object)).object;
      if(object!=selected){object=selected;changed=true;}
    }
    // Leave every legacy byte unchanged when there is no new sharing. The
    // namespace and removal of redundant live IDs publish in one replicated
    // transaction. Old snapshots retain their immutable record references.
    if(changed)model.format=7;
  }
  void discard(Node& n) {
    if(n.directory && has_children(model,n.inode))fail(Code::not_empty,"native directory not empty");
    for(auto it=dirty.begin();it!=dirty.end();)it=it->first.first==n.inode?dirty.erase(it):std::next(it);
    const auto parent=n.parent,id=n.inode;
    if(parent)node(model,parent).modified=model.sequence;
    if(!n.unlinked)model.names.erase({parent,n.key});
    model.nodes.erase(id);changed=true;
  }
  void set_delete(std::uint64_t id,bool requested) {
    auto& n=node(model,selected_inode(model,id).inode);
    if(id==1 || n.unlinked)fail(Code::invalid_operation,"cannot change delete intent on root/detached inode");
    if(n.delete_pending==requested)return;
    if(requested && n.directory && has_children(model,id))fail(Code::not_empty,"delete-pending directory is not empty");
    model.format=(std::max)(model.format,2U);n.delete_pending=requested;n.modified=model.sequence;changed=true;
  }
  void unlink(std::uint64_t id) {
    auto& n=node(model,selected_inode(model,id).inode);
    if(n.unlinked)return; // Repeated cleanup never discards retained payloads.
    if(id==1 || !n.delete_pending)fail(Code::invalid_operation,"native unlink needs persistent delete intent");
    if(n.directory && has_children(model,id))fail(Code::not_empty,"native unlink directory is not empty");
    const auto parent=n.parent;
    model.names.erase({parent,n.key});
    n.parent=0;n.name.clear();n.key.clear();n.delete_pending=false;n.unlinked=true;
    n.modified=model.sequence;node(model,parent).modified=model.sequence;model.format=(std::max)(model.format,2U);changed=true;
  }
  void retire(std::uint64_t id) {
    auto& n=node(model,selected_inode(model,id).inode);
    if(id==1 || !n.unlinked)fail(Code::invalid_operation,"native retirement needs detached inode");
    discard(n);
  }
  void finish_session() {
    std::vector<std::uint64_t> ids;ids.reserve(model.nodes.size());
    for(const auto& [id,n]:model.nodes)if(n.delete_pending || n.unlinked)ids.push_back(id);
    for(const auto id:ids)discard(node(model,id));
  }
  void set_basic(const NativeBasicMutation& op) {
    std::uint64_t id=op.inode;
    if(!id) {
      if(!op.created_ordinal || op.created_ordinal>created.size())
        fail(Code::invalid_operation,"basic metadata creation selector missing");
      id=created[op.created_ordinal-1];
    }
    auto& n=node(model,selected_inode(model,id).inode);
    validate_basic(op.value,n.directory);
    if(n.basic && *n.basic==op.value)return;
    n.basic=op.value;n.modified=model.sequence;model.format=(std::max)(model.format,3U);changed=true;
  }
  void set_security(const NativeSecurityMutation& op) {
    std::uint64_t id=op.inode;
    if(!id) {
      if(!op.created_ordinal || op.created_ordinal>created.size())
        fail(Code::invalid_operation,"security creation selector missing");
      id=created[op.created_ordinal-1];
    }
    auto& n=node(model,selected_inode(model,id).inode);
    if(n.security && *n.security==op.value)return;
    // Enforce the SAME whole-namespace cap before copying a replacement.
    std::size_t size=64+op.value.size();
    for(const auto& [key,item]:model.nodes)
      size+=node_header((std::max)(model.format,4U))+item.name.size()+16*item.extents.size()+
          (key!=id && item.security?item.security->size():0);
    if(size>kMetadataCap)fail(Code::limit,"native aggregate security metadata budget");
    n.security=op.value;n.modified=model.sequence;model.format=(std::max)(model.format,4U);changed=true;
  }
  void automatic_metadata(std::uint64_t id,const NativeAutomaticMetadata& effect) {
    auto& n=node(model,selected_inode(model,id).inode);
    if(const auto value=detail::updated_automatic_basic(n.basic,n.directory,effect))
      set_basic(NativeBasicMutation{id,0,*value});
  }
  void overwrite_inode(std::uint64_t id,const NativeOverwriteMetadata& effect) {
    auto& n=node(model,selected_inode(model,id).inode);
    if(n.directory)fail(Code::is_directory,"cannot overwrite native directory");
    if(n.delete_pending || n.unlinked)
      fail(Code::invalid_operation,"cannot overwrite pending/detached native inode");
    if(!n.basic)fail(Code::invalid_operation,"overwrite needs existing basic metadata");
    auto value=*n.basic;
    value.attributes=(effect.replace_attributes?effect.attributes:(value.attributes|effect.attributes))|0x20U;
    value.last_access_time=value.last_write_time=value.change_time=effect.utc_time;
    NativeFileMutation op;op.operation=NativeFileOperation::truncate;op.size=0;
    apply(op,id);
    set_basic(NativeBasicMutation{id,0,value});
    // An explicit overwrite is a durable event, including an already empty
    // file and identical sampled fields. Keep the same THREE barriers.
    n.modified=model.sequence;changed=true;
  }
  void reserve(std::uint64_t id,std::uint64_t bytes) {
    auto& n=node(model,selected_inode(model,id).inode);
    if(n.directory)fail(Code::is_directory,"cannot reserve a native directory");
    if(bytes%kNativeAllocationUnit || bytes>kNativeAllocationCapacity)
      fail(Code::invalid_operation,"invalid aligned native reservation");
    if(bytes<n.size) {
      NativeFileMutation op;op.operation=NativeFileOperation::truncate;op.size=bytes;apply(op,id);
    }
    if(n.allocation && *n.allocation==bytes)return;
    n.allocation=bytes;n.modified=model.sequence;
    model.format=(std::max)(model.format,5U);changed=true;
  }
  void trim_reservation(std::uint64_t id) {
    const auto& n=selected_inode(model,id);
    if(n.directory)return; // Directories have no allocation field or payload.
    if(!n.allocation)fail(Code::invalid_operation,"Cleanup trim requires a tracked reservation");
    reserve(id,aligned_allocation(n.size));
  }
  void cleanup_initial(std::uint64_t id,bool deleting,const NativeCleanupMetadata& effect) {
    auto& n=node(model,selected_inode(model,id).inode);
    if(id==1)fail(Code::invalid_operation,"initial delete-on-close is forbidden on root");
    bool eligible=true;
    if(n.directory && has_children(model,id))eligible=false;
    if(!eligible && n.delete_pending)
      fail(Code::corrupt,"nonempty directory has impossible durable delete intent");
    automatic_metadata(id,effect);
    if(!n.unlinked && eligible) {
      set_delete(id,true);
      if(deleting)unlink(id);
    }
  }
  void cleanup(std::uint64_t id,bool deleting,const NativeCleanupMetadata& effect) {
    automatic_metadata(id,effect);
    if(deleting)unlink(id);
  }
  void apply(const NativeFileMutation& op,std::uint64_t inode=0) {
    const bool write=op.operation==NativeFileOperation::write;
    const bool trunc=op.operation==NativeFileOperation::truncate;
    const bool rename=op.operation==NativeFileOperation::rename;
    if(inode) {
      if((!write && !trunc) || !op.path.empty())fail(Code::invalid_operation,"invalid inode mutation selector");
      (void)selected_inode(model,inode);
    } else (void)canonical(op.path);
    if((!write && (op.offset || !op.data.empty())) || (!trunc && op.size) ||
        (!rename && (!op.target.empty() || op.replace)))fail(Code::invalid_operation,"unused native mutation fields");
    if(op.operation==NativeFileOperation::create_file || op.operation==NativeFileOperation::create_directory) {
      const auto [parent,name]=destination(model,op.path);const auto key=component(name);
      if(child(model,parent,key))fail(Code::already_exists,"native destination exists");
      if(model.nodes.size()>=kNativeMaxNodes || model.next_inode==(std::numeric_limits<std::uint64_t>::max)())
        fail(Code::limit,"native inode budget");
      Node n;n.inode=model.next_inode++;n.parent=parent;n.name=name;n.key=key;
      n.directory=op.operation==NativeFileOperation::create_directory;n.born=n.modified=model.sequence;
      created.push_back(n.inode);model.names.emplace(std::pair{parent,key},n.inode);
      model.nodes.emplace(n.inode,std::move(n));
      node(model,parent).modified=model.sequence;changed=true;return;
    }
    const auto id=inode?inode:resolve(model,op.path);auto& n=node(model,id);
    if(id==1)fail(Code::invalid_operation,"native root mutation forbidden");
    if(n.delete_pending && !write && !trunc)
      fail(Code::invalid_operation,"delete-pending inode must use lifetime operations");
    if(write) {
      if(n.directory)fail(Code::is_directory,"cannot write native directory");
      if(op.offset>kNativeMaxFileBytes || op.data.size()>kNativeMaxFileBytes-op.offset)fail(Code::limit,"native write bounds");
      if(op.data.empty())return;
      if(n.allocation && (op.offset>kNativeAllocationCapacity || op.data.size()>kNativeAllocationCapacity-op.offset))
        fail(Code::limit,"native write exceeds reserved-capacity domain");
      std::size_t done=0;
      while(done<op.data.size()) {
        const auto position=op.offset+done,index=position/kNativeExtentBytes;
        const auto within=static_cast<std::size_t>(position%kNativeExtentBytes);
        const auto count=(std::min)(op.data.size()-done,std::size_t(kNativeExtentBytes)-within);
        auto& extent=block(n,index);
        const auto first=op.data.begin()+static_cast<std::ptrdiff_t>(done);
        const auto target=extent.bytes.begin()+static_cast<std::ptrdiff_t>(within);
        if(std::equal(first,first+static_cast<std::ptrdiff_t>(count),target))unchanged(n,index,extent);
        else {
          changing(extent);std::copy_n(first,count,target);finish_block(n,index);
        }
        done+=count;
      }
      n.size=(std::max)(n.size,op.offset+op.data.size());
      if(n.allocation)*n.allocation=(std::max)(*n.allocation,aligned_allocation(n.size));
      // Count accepted logical input in this private transaction, including
      // identical/zero data. Rejected or unselected stages grant no workload.
      // Saturation stops further accounting instead of failing user writes.
      if(model.format>=6) {
        const auto room=(std::numeric_limits<std::uint64_t>::max)()-model.committed_input_bytes;
        model.committed_input_bytes+=(std::min)(room,static_cast<std::uint64_t>(op.data.size()));
      }
      n.modified=model.sequence;changed=true;return;
    }
    if(trunc) {
      if(n.directory)fail(Code::is_directory,"cannot truncate native directory");
      if(op.size>kNativeMaxFileBytes)fail(Code::limit,"native truncate bounds");
      if(n.allocation && op.size>kNativeAllocationCapacity)
        fail(Code::limit,"native EOF exceeds reserved-capacity domain");
      if(op.size==n.size)return;
      const auto blocks=(op.size+kNativeExtentBytes-1)/kNativeExtentBytes;
      for(auto it=n.extents.lower_bound(blocks);it!=n.extents.end();) {
        dirty.erase({n.inode,it->first});it=n.extents.erase(it);
      }
      if(op.size<n.size && op.size%kNativeExtentBytes && n.extents.contains(op.size/kNativeExtentBytes)) {
        const auto index=op.size/kNativeExtentBytes;auto& extent=block(n,index);
        const auto tail=std::span<const std::byte>(extent.bytes).subspan(static_cast<std::size_t>(op.size%kNativeExtentBytes));
        if(zeros(tail))unchanged(n,index,extent);
        else {
          changing(extent);
          std::fill(extent.bytes.begin()+static_cast<std::ptrdiff_t>(op.size%kNativeExtentBytes),extent.bytes.end(),std::byte{0});
          finish_block(n,index);
        }
      }
      n.size=op.size;
      if(n.allocation)*n.allocation=(std::max)(*n.allocation,aligned_allocation(n.size));
      n.modified=model.sequence;changed=true;return;
    }
    if(rename) {
      const auto [parent,name]=destination(model,op.target);const auto key=component(name);
      for(auto p=parent;p;p=node(model,p).parent)if(p==id)fail(Code::invalid_operation,"native rename cycle");
      const auto* target=child(model,parent,key);
      if(target && target->inode==id) {
        if(n.name==name)return;
      } else if(target) {
        if(!op.replace)fail(Code::already_exists,"native rename destination exists");
        if(target->delete_pending)fail(Code::invalid_operation,"native rename target is delete-pending");
        if(target->directory!=n.directory)fail(Code::invalid_operation,"native rename type mismatch");
        discard(node(model,target->inode));
      }
      const auto old_parent=n.parent;
      model.names.erase({old_parent,n.key});model.names.emplace(std::pair{parent,key},id);
      n.parent=parent;n.name=name;n.key=key;n.modified=model.sequence;
      node(model,old_parent).modified=node(model,parent).modified=model.sequence;changed=true;return;
    }
    if(op.operation==NativeFileOperation::remove){discard(n);return;}
    fail(Code::invalid_operation,"unknown native operation");
  }
  std::vector<StoreMutation> mutations(std::uint64_t candidate_inode,
      std::span<const NativeLz4ExtentCandidate> candidates) {
    if(extent_count(model)>kNativeMaxExtents)fail(Code::limit,"native live extent budget");
    // Unchanged records were fully admitted on initialize/resume or a prior
    // acknowledgement. Verify and validate every touched record privately;
    // public snapshot/open still scrub ALL reachable data before exposure.
    for(const auto& [key,extent]:dirty) {
      const auto& bytes=extent.bytes;
      const auto& n=node(model,key.first);const auto index=key.second;
      if(!n.extents.contains(index) || bytes.size()!=kNativeExtentBytes || zeros(bytes))
        fail(Code::corrupt,"native dirty extent invariant");
      const auto begin=index*kNativeExtentBytes;
      if(n.size-begin<kNativeExtentBytes &&
          !zeros(std::span<const std::byte>(bytes).subspan(static_cast<std::size_t>(n.size-begin))))
        fail(Code::corrupt,"native dirty bytes beyond EOF");
    }
    // Reuse only the SAME inode/block's immutable original record, after an
    // independent verified read and exact byte comparison. Do this BEFORE ID
    // allocation: a removed original ID must not be reassigned elsewhere first.
    // Single changed writes need no second read; only possible reversions do.
    for(auto it=dirty.begin();it!=dirty.end();) {
      const auto& extent=it->second;
      if(extent.compare_original && extent.original_object &&
          extent.bytes==extent_bytes(base,original,extent.original_object)) {
        node(model,it->first.first).extents.at(it->first.second)=extent.original_object;
        it=dirty.erase(it);
      } else ++it;
    }
    std::set<std::uint64_t> used;
    for(const auto& [id,n]:model.nodes){(void)id;for(const auto& [index,object]:n.extents){(void)index;if(object)used.insert(object);}}
    for(auto& [id,n]:model.nodes)for(auto& [index,object]:n.extents)if(!object) {
      if(!dirty.contains({id,index}))fail(Code::corrupt,"native pending extent has no private data");
      std::uint64_t candidate=2;while(used.contains(candidate))++candidate;
      if(candidate>kStoreMaxObjects)fail(Code::limit,"native extent object allocator");
      object=candidate;used.insert(object);
    }
    for(const auto& [id,n]:model.nodes)for(const auto& [index,object]:n.extents)
      if(dirty.contains({id,index}))model.members.erase(object);
    for(auto it=model.members.begin();it!=model.members.end();)it=!used.contains(it->first)?model.members.erase(it):std::next(it);
    refresh_group_bases(model);validate_structure(model);
    std::vector<StoreMutation> output;output.reserve(kStoreMaxObjects);
    namespace_mutations(output,model,&original);
    for(const auto& [id,n]:model.nodes)for(const auto& [index,object]:n.extents) {
      const auto found=dirty.find({id,index});
      if(found!=dirty.end()) {
        StoreMutation mutation{object,false,found->second.bytes};
        std::span<const std::byte> selected;unsigned matches=0;
        if(id==candidate_inode)for(const auto& candidate:candidates)
          if(candidate.extent_index==index){++matches;selected=candidate.payload;}
        // Reused/zero records never reach this branch. Ambiguous advice is
        // discarded; complete CPU source comparison remains in the encoder.
        if(matches==1 && !selected.empty() && selected.size()<found->second.bytes.size())
          mutation.lz4_candidate.assign(selected.begin(),selected.end());
        output.push_back(std::move(mutation));
      }
    }
    for(const auto& previous:base.list(inventory_limit(model)))if(previous.id!=kNamespaceObject &&
        !(model.inode_records && (previous.id&kInodeObjectBit)) &&
        !(model.indexed_names && (previous.id&kDirectoryObjectBit)) &&
        !(model.shared_bases && model.bases.contains(previous.id)) && !used.contains(previous.id))
      output.push_back({previous.id,true,{}});
    if(output.size()>inventory_limit(model))fail(Code::limit,"native object mutation budget");
    return output;
  }
};
}

namespace detail {
namespace {
void validate_indexed_inode(const Node& n,const IndexedVolumeRoot& root) {
  if(!n.inode || n.inode>=root.next_inode || !n.born || n.born>n.modified ||
      n.modified>root.sequence || !n.extents.empty() ||
      (n.delete_pending && n.unlinked) || (root.format==1 && (n.delete_pending || n.unlinked)) ||
      (n.directory && (n.allocation || n.size)) ||
      (n.allocation && (*n.allocation<n.size || *n.allocation%kNativeAllocationUnit)))
    fail(Code::corrupt,"invalid indexed inode state");
  if(n.inode==1) {
    if(!n.directory || n.parent || !n.name.empty() || n.born!=1 || n.delete_pending || n.unlinked)
      fail(Code::corrupt,"invalid indexed root inode");
  } else if(n.unlinked) {
    if(n.parent || !n.name.empty())fail(Code::corrupt,"invalid indexed detached inode name");
  } else {
    if(!n.parent || n.parent>=root.next_inode || n.parent==n.inode)
      fail(Code::corrupt,"invalid indexed inode parent");
    if(root.name_profile)(void)indexed_component_key(n.name);
    else (void)component(n.name);
  }
  if(n.basic)validate_basic(*n.basic,n.directory);
  if(n.security && (n.security->size()>kNativeMaxSecurityBytes || !native_security_descriptor_framed(*n.security)))
    fail(Code::corrupt,"invalid indexed inode security");
}
void append_directory_shape(Bytes& bytes,const IndexedDirectoryShape& shape) {
  const auto start=bytes.size();
  bytes.resize(start+kIndexedShapeHeaderBytes+kIndexedShapeBucketBytes*(shape.units.size()+shape.components.size()));
  put(bytes,start,shape.units.size(),2);put(bytes,start+2,shape.components.size(),2);
  auto at=start+kIndexedShapeHeaderBytes;
  for(const auto* axis:{&shape.units,&shape.components})for(const auto& bucket:*axis) {
    put(bytes,at,bucket.bound,2);put(bytes,at+2,bucket.children,8);
    at+=kIndexedShapeBucketBytes;
  }
}

IndexedDirectoryShape decode_directory_shape(std::span<const std::byte> bytes,const IndexedVolumeRoot& root) {
  if(bytes.size()<kIndexedShapeHeaderBytes)fail(Code::corrupt,"truncated indexed directory shape");
  const auto units=integer(bytes,0,2),components=integer(bytes,2,2);
  if(units>kIndexedPathUnits || components>kIndexedPathDepth ||
      bytes.size()!=kIndexedShapeHeaderBytes+(units+components)*kIndexedShapeBucketBytes)
    fail(Code::corrupt,"invalid indexed directory shape length");
  IndexedDirectoryShape result;
  auto at=kIndexedShapeHeaderBytes;
  const auto read_axis=[&](auto& axis,std::uint64_t count,std::size_t maximum) {
    axis.reserve(static_cast<std::size_t>(count));
    std::uint64_t total=0,previous=0;
    for(std::uint64_t i=0;i<count;++i,at+=kIndexedShapeBucketBytes) {
      const auto bound=integer(bytes,at,2),children=integer(bytes,at+2,8);
      if(bound<=previous || bound>maximum || !children || children>=root.next_inode-total)
        fail(Code::corrupt,"invalid indexed directory shape bucket");
      axis.push_back({static_cast<std::uint16_t>(bound),children});
      total+=children;previous=bound;
    }
    return total;
  };
  const auto total_units=read_axis(result.units,units,kIndexedPathUnits);
  if(total_units!=read_axis(result.components,components,kIndexedPathDepth))
    fail(Code::corrupt,"indexed directory shape totals differ");
  return result;
}
}
Bytes encode_indexed_inode(const IndexedInodeInput& input,const IndexedVolumeRoot& root) {
  Node n;
  n.inode=input.inode;n.parent=input.parent;n.size=input.size;n.born=input.born;n.modified=input.modified;
  n.directory=input.directory;n.name=input.name;n.basic=input.basic;n.security=input.security;n.allocation=input.allocation;
  n.delete_pending=input.lifetime==IndexedLifetime::pending;n.unlinked=indexed_detached(input.lifetime);
  validate_indexed_inode(n,root);
  auto bytes=inode_frame(n,7);
  if(input.shape) {
    if(!root.directory_shapes || !n.directory || (input.lifetime!=IndexedLifetime::linked && input.shape->bound().components))
      fail(Code::corrupt,"indexed directory shape outside linked directory");
    append_directory_shape(bytes,*input.shape);
  }
  return bytes;
}
IndexedInodeInput decode_indexed_inode(std::span<const std::byte> bytes,std::uint64_t inode,
    const IndexedVolumeRoot& root) {
  if(bytes.size()<node_header(7) || integer(bytes,50,2))
    fail(Code::corrupt,"indexed inode has inline extents");
  std::size_t cursor=0;
  auto node=decode_inode(bytes,7,cursor,0,root.name_profile);
  if(node.inode!=inode)fail(Code::corrupt,"indexed inode identity mismatch");
  try { validate_indexed_inode(node,root); }
  catch(const NativeFsError&) { fail(Code::corrupt,"invalid indexed inode fields"); }
  std::optional<IndexedDirectoryShape> shape;
  if(cursor!=bytes.size()) {
    if(!root.directory_shapes || !node.directory)fail(Code::corrupt,"unexpected indexed inode extension");
    shape=decode_directory_shape(bytes.subspan(cursor),root);
    if((node.delete_pending || node.unlinked) && shape->bound().components)
      fail(Code::corrupt,"indexed deleted directory shape has children");
  }
  return {node.inode,node.parent,node.size,node.born,node.modified,node.directory,std::move(node.name),
      node.basic,std::move(node.security),node.allocation,
      node.delete_pending?IndexedLifetime::pending:
          node.unlinked?IndexedLifetime::detached:IndexedLifetime::linked,std::move(shape)};
}
}

struct NativeFilesystemSnapshot::Impl {
  detail::NativeObjectSnapshot objects;Model model;
  std::optional<detail::IndexedVolumeView> indexed;
  std::shared_ptr<detail::ProtectedStripeImage> sealed;
  std::uint64_t sealed_limit=0;
  std::shared_ptr<const void> enumeration_owner=std::make_shared<const unsigned char>(0);
  explicit Impl(detail::NativeObjectSnapshot snapshot):objects(std::move(snapshot)) {
    const auto root=object_bytes(objects,kNamespaceObject,kMetadataCap);
    if(detail::indexed_volume_root(root)) {
      const auto* paged=objects.paged_if();
      if(!paged)fail(Code::corrupt,"indexed volume requires protected paged storage");
      indexed.emplace(*paged,root);
    }else model=decode(objects,root);
  }
};
namespace {
struct SharedBaseOwner {
  std::array<std::uint64_t,kStoreMaxObjects+1> revisions{};
  std::array<std::uint64_t,kStoreMaxObjects+1> content_revisions{};
  std::weak_ptr<const void> active;
  std::weak_ptr<const void> search_window;
  struct SearchLease {
    std::weak_ptr<const void> window;
    std::size_t extents;
  };
  std::vector<detail::IndexedPreparationWatch> indexed_work;
  std::vector<SearchLease> batch_windows;
  bool busy() const noexcept {
    return !active.expired() || std::ranges::any_of(indexed_work,[](const auto& watch) {
      return !watch.preparation.expired();
    });
  }
};
}
struct NativeFilesystemWriter::Impl {
  detail::NativeObjectWriter objects;Model model;bool poisoned=false;
  // An allocation-free copy of the canonical root of this committed Model.
  InodeRootBytes inode_root{};
  std::optional<detail::IndexedVolumeRoot> indexed;
  detail::IndexedDirectoryBackfill directory_backfill;
  detail::IndexedSharedBaseCache shared_base_reads;
  std::uint64_t indexed_content_sequence=1;
  detail::IndexedContentChanges indexed_changes;
  std::shared_ptr<SharedBaseOwner> shared_owner=std::make_shared<SharedBaseOwner>();
  void remember(std::span<const StoreMutation> changes,std::uint64_t generation,bool content_changed=true) noexcept {
    for(const auto& change:changes)if(change.id>=2 && change.id<=kStoreMaxObjects) {
      shared_owner->revisions[change.id]=generation;
      if(content_changed)shared_owner->content_revisions[change.id]=generation;
    }
  }
  Impl(detail::NativeObjectWriter writer,Model state):objects(std::move(writer)),model(std::move(state)) {
    if(model.inode_records)inode_root=inode_root_bytes(model);
  }
  explicit Impl(detail::NativeObjectWriter writer):objects(std::move(writer)) {}
  detail::IndexedVolumeView checked_indexed_view(const PagedObjectStoreSnapshot& snapshot) const {
    return verify_indexed_root(snapshot,*indexed);
  }
  template<class Read>
  auto inspect_indexed(Read&& read,detail::PagedReadMetadata* metadata=nullptr) {
    const auto snapshot=objects.snapshot_for_indexed_read();
    const auto query=snapshot.paged_if()->scoped_query(metadata);
    const auto view=checked_indexed_view(*snapshot.paged_if());
    return std::forward<Read>(read)(view);
  }
  template<class Read>
  auto read_indexed(Read&& read,detail::PagedReadMetadata* metadata=nullptr) {
    try {
      return inspect_indexed(std::forward<Read>(read),metadata);
    }catch(const std::bad_alloc&){
      shared_base_reads.clear();
      fail(Code::limit,"indexed namespace query allocation failure");
    }catch(const NativeFsError& error) {
      if(error.code()==Code::corrupt || error.code()==Code::storage_failure) {
        shared_base_reads.clear();
        poisoned=true;
      }
      throw;
    }catch(const std::exception&){
      shared_base_reads.clear();
      poisoned=true;
      fail(Code::storage_failure,"indexed namespace query failed");
    }
  }
  template<class Commit>
  auto mutate_indexed(Commit&& commit) {
    try {
      const auto source=objects.snapshot_for_indexed_read();
      auto& watched=shared_owner->indexed_work;
      for(auto& watch:watched)watch.changed=false;
      return std::forward<Commit>(commit)(*objects.paged_if(),*source.paged_if(),*indexed,
          detail::IndexedContentWatch{indexed_content_sequence,watched,indexed_changes});
    }catch(const std::bad_alloc&){fail(Code::limit,"indexed transaction allocation failure");}
    catch(const NativeFsError& error) {
      if(error.code()==Code::corrupt || error.code()==Code::storage_failure)poisoned=true;
      throw;
    }catch(const std::exception&){poisoned=true;fail(Code::storage_failure,"indexed transaction failed");}
  }
  void accept(Model&& state) noexcept {
    model=std::move(state);
    if(model.inode_records)inode_root=inode_root_bytes(model);
  }
  template<class Read>
  auto read_checked(bool complete,Read&& read) {
    if(indexed)fail(Code::invalid_operation,"operation requires an indexed namespace implementation");
    try {
      const bool scoped=!complete && model.inode_records;
      auto snapshot=objects.snapshot_for_staging(scoped);
      if(!scoped) {
        if(!namespace_matches(snapshot,model))
          fail(Code::corrupt,"native cached namespace differs from backing record");
      } else {
        const auto root=object_bytes(snapshot,kNamespaceObject,inode_root.size());
        if(!std::ranges::equal(root,inode_root))
          fail(Code::corrupt,"native cached root differs from backing record");
      }
      return std::forward<Read>(read)(std::move(snapshot));
    }catch(const std::bad_alloc&){fail(Code::limit,"native namespace query allocation failure");}
    catch(const NativeFsError& error) {
      if(error.code()==Code::corrupt || error.code()==Code::storage_failure)poisoned=true;
      throw;
    }catch(const std::exception&){poisoned=true;fail(Code::storage_failure,"native namespace query failed");}
  }
  const Node& checked_inode(const detail::NativeObjectSnapshot& snapshot,std::uint64_t inode) const {
    const auto& n=selected_inode(model,inode);
    verify_inode(snapshot,model,n);
    return n;
  }
  template<class Read>
  auto read_checked_inode(std::uint64_t inode,Read&& read) {
    return read_checked(false,[&](const auto& snapshot) {
      return std::forward<Read>(read)(snapshot,checked_inode(snapshot,inode));
    });
  }
};
struct NativeFilesystemWriter::SharedBaseWork::Impl {
  struct Binding {NativeExtentSelection selection;std::uint64_t object=0;};
  std::weak_ptr<SharedBaseOwner> owner;
  std::shared_ptr<const void> reservation;
  std::weak_ptr<const void> batch_window;
  bool batched=false;
  std::optional<detail::NativeObjectSnapshot> source;
  std::shared_ptr<detail::IndexedSharedBasePreparation> indexed;
  std::vector<Binding> bindings;
  std::vector<std::uint64_t> ids;
  std::map<std::uint64_t,std::uint64_t> revisions;
  Model decode_model;
  Bytes base;
  std::vector<StoreMutation> frames;
  std::map<std::uint64_t,detail::NativeGroupReference> references;
  std::uint64_t base_object=0;
  NativeSharedBaseProgress state{NativeSharedBasePhase::reading_base};
  void retire(NativeSharedBasePhase phase) noexcept {
    if(state.phase==NativeSharedBasePhase::completed || state.phase==NativeSharedBasePhase::cancelled ||
        state.phase==NativeSharedBasePhase::failed)return;
    state.phase=phase;state.owned_payload_bytes=0;
    indexed.reset();source.reset();reservation.reset();
    Bytes{}.swap(base);frames.clear();references.clear();decode_model.members.clear();
    bindings.clear();ids.clear();revisions.clear();
  }
};
NativeFilesystemWriter::SharedBaseWork::SharedBaseWork(std::unique_ptr<Impl> p) noexcept:impl_(std::move(p)){}
NativeFilesystemWriter::SharedBaseWork::SharedBaseWork(SharedBaseWork&&) noexcept=default;
NativeFilesystemWriter::SharedBaseWork& NativeFilesystemWriter::SharedBaseWork::operator=(SharedBaseWork&&) noexcept=default;
NativeFilesystemWriter::SharedBaseWork::~SharedBaseWork()=default;
NativeSharedBaseProgress NativeFilesystemWriter::SharedBaseWork::progress() const noexcept {
  if(!impl_)return {};auto state=impl_->state;
  if((impl_->owner.expired() || (impl_->indexed && impl_->indexed->invalidated())) &&
      (state.phase==NativeSharedBasePhase::reading_base ||
      state.phase==NativeSharedBasePhase::encoding_members || state.phase==NativeSharedBasePhase::ready))
    state.phase=NativeSharedBasePhase::failed;
  return state;
}
NativeSharedBaseSearchWindow NativeFilesystemWriter::SharedBaseWork::search_window() const noexcept {
  if(!impl_ || !impl_->indexed || impl_->owner.expired())return {};
  return impl_->indexed->search_window();
}
void NativeFilesystemWriter::SharedBaseWork::cancel() noexcept {
  if(!impl_)return;
  impl_->retire(progress().phase==NativeSharedBasePhase::failed ?
      NativeSharedBasePhase::failed : NativeSharedBasePhase::cancelled);
}
void validate_native_lz4_extent_candidates(std::span<const NativeLz4ExtentCandidate> candidates) {
  admit_lz4_extent_candidates(candidates);
}
NativeFilesystemSnapshot::NativeFilesystemSnapshot(std::unique_ptr<Impl> p) noexcept:impl_(std::move(p)){}
NativeFilesystemSnapshot::NativeFilesystemSnapshot(NativeFilesystemSnapshot&&) noexcept=default;
NativeFilesystemSnapshot& NativeFilesystemSnapshot::operator=(NativeFilesystemSnapshot&&) noexcept=default;
NativeFilesystemSnapshot::~NativeFilesystemSnapshot()=default;
NativeFilesystemWriter::NativeFilesystemWriter(std::unique_ptr<Impl> p) noexcept:impl_(std::move(p)){}
NativeFilesystemWriter::NativeFilesystemWriter(NativeFilesystemWriter&&) noexcept=default;
NativeFilesystemWriter& NativeFilesystemWriter::operator=(NativeFilesystemWriter&&) noexcept=default;
NativeFilesystemWriter::~NativeFilesystemWriter()=default;
NativeFilesystemSnapshot NativeFilesystemSnapshot::open(ObjectStoreSnapshot objects) {
  return open_storage(std::move(objects));
}
NativeFilesystemSnapshot NativeFilesystemSnapshot::open(PagedObjectStoreSnapshot objects) {
  return open_storage(std::move(objects));
}
NativeFilesystemSnapshot NativeFilesystemSnapshot::open_image(std::shared_ptr<StoreByteIo> image,NativeImageStorage storage) {
  if(storage.kind==NativeImageKind::sealed_indexed) {
    auto backing=detail::ProtectedStripeImage::open(std::move(image),storage.maximum_physical_bytes);
    auto view=open(PagedObjectStoreSnapshot::open_sealed(backing));
    if(!view.impl_->indexed)fail(Code::corrupt,"sealed image requires an indexed namespace");
    view.impl_->sealed=std::move(backing);
    view.impl_->sealed_limit=storage.maximum_physical_bytes;
    return view;
  }
  if(storage.kind==NativeImageKind::flat && !storage.maximum_physical_bytes)
    return open(ObjectStoreSnapshot::open(std::move(image)));
  if((storage.kind==NativeImageKind::paged || storage.kind==NativeImageKind::indexed_paged) &&
      storage.maximum_physical_bytes) {
    auto view=open(PagedObjectStoreSnapshot::open(std::move(image),{storage.maximum_physical_bytes}));
    if(view.image_storage().kind!=storage.kind)fail(Code::corrupt,"native image capability does not match descriptor");
    return view;
  }
  fail(Code::invalid_operation,"invalid explicit image storage descriptor");
}
NativeImageStorage NativeFilesystemSnapshot::image_storage() const noexcept {
  if(impl_ && impl_->sealed)return {NativeImageKind::sealed_indexed,impl_->sealed_limit};
  if(impl_)if(const auto* paged=impl_->objects.paged_if())
    return {impl_->indexed?NativeImageKind::indexed_paged:NativeImageKind::paged,paged->limits().maximum_physical_bytes};
  return {};
}
NativeFilesystemSnapshot NativeFilesystemSnapshot::open_storage(detail::NativeObjectSnapshot objects) {
  return NativeFilesystemSnapshot(std::make_unique<Impl>(std::move(objects)));
}
StoreIdentity NativeFilesystemSnapshot::identity() const noexcept{return impl_?impl_->objects.identity():StoreIdentity{};}
StoreCommitInfo NativeFilesystemSnapshot::storage_info() const noexcept {
  if(!impl_)return {};
  auto result=impl_->objects.info();
  if(impl_->sealed) {
    result.physical_end=impl_->sealed->occupied();
    result.degraded_checkpoint=result.degraded_checkpoint || impl_->sealed->degraded();
  }
  return result;
}
std::uint64_t NativeFilesystemSnapshot::sequence() const noexcept {
  if(!impl_)return 0;
  return impl_->indexed?impl_->indexed->root().sequence:impl_->model.sequence;
}
NativeWorkloadInfo NativeFilesystemSnapshot::workload_info() const noexcept {
  if(impl_ && impl_->indexed)return impl_->indexed->root().workload;
  if(!impl_||impl_->model.format<6)return {};
  return {true,impl_->model.committed_input_bytes};
}
NativeSharedBaseInfo NativeFilesystemSnapshot::shared_base_info() const noexcept {
  if(impl_ && impl_->indexed) {
    const auto& root=impl_->indexed->root();
    return {root.format>=3,static_cast<std::size_t>(root.shared_members),static_cast<std::size_t>(root.shared_bases)};
  }
  return impl_?NativeSharedBaseInfo{impl_->model.shared_bases,impl_->model.members.size(),impl_->model.bases.size()}:NativeSharedBaseInfo{};
}
std::optional<NativeContentToken> NativeFilesystemWriter::maintenance_next(NativeExtentSelection after,bool first) const {
  if(!maintenance_supported())fail(Code::invalid_operation,"content scan requires healthy shared-base storage");
  if(impl_->indexed)return impl_->read_indexed([&](const auto& view){return view.next_content(after,first);});
  for(auto it=first?impl_->model.nodes.begin():impl_->model.nodes.lower_bound(after.inode);it!=impl_->model.nodes.end();++it) {
    const auto& n=it->second;if(n.directory || n.unlinked || n.delete_pending)continue;
    auto extent=first || n.inode!=after.inode?n.extents.begin():n.extents.upper_bound(after.extent_index);
    if(extent!=n.extents.end())return NativeContentToken{{n.inode,extent->first},extent->second,
        impl_->shared_owner->revisions[extent->second],impl_->shared_owner->content_revisions[extent->second],
        impl_->model.members.contains(extent->second)};
  }
  return {};
}
std::optional<NativeContentToken> NativeFilesystemWriter::maintenance_token(NativeExtentSelection extent) {
  if(!maintenance_indexed())fail(Code::invalid_operation,"content lookup requires indexed storage");
  return impl_->read_indexed([&](const auto& view){return view.content_at(extent);});
}
bool NativeFilesystemWriter::maintenance_matches(const NativeContentToken& token) const noexcept {
  if(poisoned() || impl_->indexed || token.object<2 || token.object>kStoreMaxObjects ||
      impl_->shared_owner->revisions[token.object]!=token.revision)return false;
  const auto found=impl_->model.nodes.find(token.extent.inode);
  if(found==impl_->model.nodes.end() || found->second.directory || found->second.unlinked || found->second.delete_pending)return false;
  const auto extent=found->second.extents.find(token.extent.extent_index);
  return extent!=found->second.extents.end() && extent->second==token.object;
}
std::vector<std::byte> NativeFilesystemWriter::maintenance_read(NativeContentToken& token,bool published_reference) {
  if(maintenance_indexed())return impl_->read_indexed([&](const auto& view){return view.read_content(token,published_reference);});
  if(!maintenance_matches(token))fail(Code::invalid_operation,"stale content sample");
  try{auto source=checked_namespace();return extent_bytes(source,impl_->model,token.object);}
  catch(const std::bad_alloc&){fail(Code::limit,"content sample allocation failure");}
  catch(const NativeFsError& error){if(error.code()==Code::corrupt || error.code()==Code::storage_failure)impl_->poisoned=true;throw;}
  catch(const std::exception&){impl_->poisoned=true;fail(Code::storage_failure,"content sample read failed");}
}
std::uint64_t NativeFilesystemWriter::maintenance_content_sequence() const noexcept {
  if(poisoned())return 0;
  return impl_->indexed?impl_->indexed_content_sequence:impl_->model.sequence;
}
std::optional<std::size_t> NativeFilesystemWriter::maintenance_changes_since(std::uint64_t sequence,
    std::span<NativeExtentSelection> output) const noexcept {
  if(!maintenance_indexed() || !sequence || sequence>impl_->indexed_content_sequence)return {};
  return impl_->indexed_changes.after(sequence,output);
}
bool NativeFilesystemWriter::maintenance_indexed() const noexcept{return !poisoned() && impl_->indexed.has_value();}
bool NativeFilesystemWriter::maintenance_has_candidates() const noexcept {
  return !poisoned() && (!impl_->indexed || impl_->indexed->extents>=2);
}
std::uint64_t NativeFilesystemWriter::maintenance_sequence() const noexcept {
  if(poisoned())return 0;
  return impl_->indexed?impl_->indexed->sequence:impl_->model.sequence;
}
StoreIdentity NativeFilesystemWriter::maintenance_identity() const noexcept {
  if(poisoned())return {};
  return impl_->indexed?impl_->indexed->identity:impl_->model.identity;
}
std::uint64_t NativeFilesystemWriter::maintenance_physical_end() const noexcept{return poisoned()?0:impl_->objects.physical_end_for_staging();}
bool NativeFilesystemWriter::maintenance_busy() const noexcept{return !poisoned() && impl_->shared_owner->busy();}
bool NativeFilesystemWriter::maintenance_supported() const noexcept{return !poisoned() && (impl_->indexed || impl_->model.shared_bases);}
NativeFileInfo NativeFilesystemSnapshot::stat(std::string_view path) const {
  if(!impl_)fail(Code::invalid_operation,"moved-from native snapshot");
  if(impl_->indexed)return impl_->indexed->state(path).info;
  return file_info(impl_->model,resolve_snapshot(impl_->objects,impl_->model,path));
}
namespace {
void verify_path_ancestors(const detail::NativeObjectSnapshot& objects,const Model& model,const Node& n) {
  if(!model.inode_records || n.unlinked)return;
  unsigned depth=0;
  for(auto parent=n.parent;parent;) {
    if(++depth>16)fail(Code::corrupt,"native inode ancestry changed");
    const auto& ancestor=node(model,parent);
    verify_inode(objects,model,ancestor);
    parent=ancestor.parent;
  }
}
std::vector<NativeFileInfo> list_node(const Model& model,const Node& directory,
    const detail::NativeObjectSnapshot* scoped=nullptr) {
  if(!directory.directory)fail(Code::not_directory,"native enumeration of file");
  std::vector<NativeFileInfo> result;
  for(auto it=model.names.lower_bound({directory.inode,{}});it!=model.names.end() && it->first.first==directory.inode;++it) {
    const auto& entry=node(model,it->second);
    if(scoped)verify_inode(*scoped,model,entry);
    result.push_back(file_info(model,entry));
  }
  return result;
}
Bytes read_node(const detail::NativeObjectSnapshot& objects,const Model& model,const Node& n,std::uint64_t offset,std::uint32_t bytes) {
  if(n.directory)fail(Code::is_directory,"cannot read native directory");
  if(offset>=n.size || !bytes)return {};
  const auto count=(std::min)(std::uint64_t(bytes),n.size-offset),end=offset+count;
  Bytes output(static_cast<std::size_t>(count));
  for(auto it=n.extents.lower_bound(offset/kNativeExtentBytes);it!=n.extents.end();++it) {
    const auto begin=it->first*kNativeExtentBytes;if(begin>=end)break;
    const auto data=extent_bytes(objects,model,it->second);
    if(data.size()!=kNativeExtentBytes)fail(Code::corrupt,"native read extent size");
    const auto first=(std::max)(begin,offset),last=(std::min)(begin+kNativeExtentBytes,end);
    std::copy_n(data.begin()+static_cast<std::ptrdiff_t>(first-begin),static_cast<std::size_t>(last-first),
                output.begin()+static_cast<std::ptrdiff_t>(first-offset));
  }
  return output;
}
}
NativeFileInfo NativeFilesystemSnapshot::stat_inode(std::uint64_t inode) const {
  if(!impl_)fail(Code::invalid_operation,"moved-from native snapshot");
  if(impl_->indexed)return impl_->indexed->state_inode(inode).info;
  return file_info(impl_->model,selected_inode(impl_->model,inode));
}
NativeInodeState NativeFilesystemSnapshot::inode_state(std::uint64_t inode) const {
  if(!impl_)fail(Code::invalid_operation,"moved-from native snapshot");
  if(impl_->indexed)return impl_->indexed->state_inode(inode);
  const auto& n=selected_inode(impl_->model,inode);
  return {file_info(impl_->model,n),n.delete_pending,n.unlinked};
}
std::optional<NativeBasicMetadata> NativeFilesystemSnapshot::basic_metadata(std::uint64_t inode) const {
  if(!impl_)fail(Code::invalid_operation,"moved-from native snapshot");
  if(impl_->indexed)return impl_->indexed->inode(inode).basic;
  return selected_inode(impl_->model,inode).basic;
}
std::optional<NativeSecurityDescriptor> NativeFilesystemSnapshot::security_descriptor(std::uint64_t inode) const {
  if(!impl_)fail(Code::invalid_operation,"moved-from native snapshot");
  if(impl_->indexed)return impl_->indexed->inode(inode).security;
  return selected_inode(impl_->model,inode).security;
}
NativeInodeAllocationInfo NativeFilesystemSnapshot::allocation_info(std::uint64_t inode) const {
  if(!impl_)fail(Code::invalid_operation,"moved-from native snapshot");
  if(impl_->indexed)return detail::indexed_inode_allocation(impl_->indexed->inode(inode));
  const auto& n=selected_inode(impl_->model,inode);return {commit_info(n),n.allocation};
}
NativeAllocationUsage NativeFilesystemSnapshot::allocation_usage() const {
  if(!impl_)fail(Code::invalid_operation,"moved-from native snapshot");
  if(impl_->indexed) {
    const auto& root=impl_->indexed->root();
    return {static_cast<std::size_t>(root.charged_extents),static_cast<std::size_t>(root.extents)};
  }
  return flitzfs::allocation_usage(impl_->model);
}
bool NativeFilesystemSnapshot::needs_session_recovery() const noexcept {
  if(!impl_)return false;
  if(impl_->indexed)return impl_->indexed->root().active_lifetimes!=0;
  for(const auto& [id,n]:impl_->model.nodes){(void)id;if(n.delete_pending || n.unlinked)return true;}
  return false;
}
std::vector<NativeFileInfo> NativeFilesystemSnapshot::list(std::string_view directory) const {
  if(!impl_)fail(Code::invalid_operation,"moved-from native snapshot");
  if(impl_->indexed)return list_inode(impl_->indexed->resolve(directory).inode);
  return list_inode(resolve_snapshot(impl_->objects,impl_->model,directory).inode);
}
std::vector<NativeFileInfo> NativeFilesystemSnapshot::list_inode(std::uint64_t directory) const {
  if(!impl_)fail(Code::invalid_operation,"moved-from native snapshot");
  if(impl_->indexed)return impl_->indexed->list_all(directory);
  const auto& entry=selected_inode(impl_->model,directory);
  if(impl_->model.indexed_names)verify_directory(impl_->objects,impl_->model,entry);
  return list_node(impl_->model,entry);
}
NativeDirectoryPage NativeFilesystemSnapshot::list_inode_page(std::uint64_t directory,
    const NativeDirectoryCursor& after,std::size_t maximum) const {
  if(!impl_)fail(Code::invalid_operation,"moved-from native snapshot");
  if(!maximum || maximum>1024)fail(Code::limit,"directory page budget");
  if(after.initialized_ && (after.directory_!=directory || after.owner_.lock()!=impl_->enumeration_owner))
    fail(Code::invalid_operation,"directory cursor belongs to a different snapshot/directory");
  if(impl_->indexed) {
    auto page=impl_->indexed->list(directory,after.after_,maximum);
    NativeDirectoryPage result{std::move(page.entries),{}};
    if(page.after) {
      NativeDirectoryCursor next;next.owner_=impl_->enumeration_owner;next.directory_=directory;
      next.after_=std::move(*page.after);next.initialized_=true;result.next=std::move(next);
    }
    return result;
  }
  verify_directory(impl_->objects,impl_->model,selected_inode(impl_->model,directory));
  NativeDirectoryPage result;result.entries.reserve(maximum);
  auto it=after.initialized_?impl_->model.names.upper_bound({directory,after.after_}):
      impl_->model.names.lower_bound({directory,{}});
  auto last=it;
  for(;it!=impl_->model.names.end() && it->first.first==directory && result.entries.size()<maximum;++it) {
    result.entries.push_back(file_info(impl_->model,node(impl_->model,it->second)));
    last=it;
  }
  if(it!=impl_->model.names.end() && it->first.first==directory) {
    NativeDirectoryCursor next;next.owner_=impl_->enumeration_owner;next.directory_=directory;
    next.after_=last->first.second;next.initialized_=true;result.next=std::move(next);
  }
  return result;
}
Bytes NativeFilesystemSnapshot::read(std::string_view path,std::uint64_t offset,std::uint32_t bytes) const {
  if(!impl_)fail(Code::invalid_operation,"moved-from native snapshot");
  if(bytes>kNativeMaxReadBytes)fail(Code::limit,"native read request budget");
  if(impl_->indexed) {
    const auto query=impl_->objects.paged_if()->scoped_query();
    return impl_->indexed->read(impl_->indexed->resolve(path),offset,bytes);
  }
  return read_node(impl_->objects,impl_->model,resolve_snapshot(impl_->objects,impl_->model,path),offset,bytes);
}
Bytes NativeFilesystemSnapshot::read_inode(std::uint64_t inode,std::uint64_t offset,std::uint32_t bytes) const {
  if(!impl_)fail(Code::invalid_operation,"moved-from native snapshot");
  if(bytes>kNativeMaxReadBytes)fail(Code::limit,"native read request budget");
  if(impl_->indexed) {
    const auto query=impl_->objects.paged_if()->scoped_query();
    return impl_->indexed->read(impl_->indexed->inode(inode),offset,bytes);
  }
  return read_node(impl_->objects,impl_->model,selected_inode(impl_->model,inode),offset,bytes);
}
void NativeFilesystemSnapshot::verify_all() const {verify_all({});}
void NativeFilesystemSnapshot::verify_all(const detail::IndexedReopenAdmission& admission) const {
  if(!impl_)fail(Code::invalid_operation,"moved-from native snapshot");
  if(impl_->sealed)impl_->sealed->verify_all();
  if(impl_->indexed){impl_->indexed->verify_all(admission);return;}
  if(admission)fail(Code::invalid_operation,"indexed startup metadata requires an indexed volume");
  impl_->objects.verify_all();(void)decode(impl_->objects);
}
void NativeFilesystemSnapshot::recheck_verified_indexed_mirrors(const NativeFilesystemSnapshot& audited) const {
  if(!matches_indexed_checkpoint(audited))
    fail(Code::invalid_operation,"indexed reopen checkpoint differs from its native audit");
  impl_->objects.paged_if()->recheck_verified_mirrors(*audited.impl_->objects.paged_if());
}
detail::FoldedCheckpoint NativeFilesystemSnapshot::build_folded_workspace(std::shared_ptr<StoreByteIo> target,
    StoreIdentity id,StoreCompressionOptions compression) const {
  if(!impl_ || !impl_->indexed || impl_->sealed || needs_session_recovery())
    fail(Code::invalid_operation,"folded workspace requires a closed mirrored indexed source");
  verify_all();
  const auto root=detail::rebind_indexed_volume_root(impl_->indexed->root(),id);
  return detail::NativeObjectWriter::folded_workspace(*impl_->objects.paged_if(),std::move(target),id,root,
      compression,detail::kIndexedVolumeIdentityOffset);
}

NativeFilesystemSnapshot NativeFilesystemSnapshot::create_sealed_indexed_copy(
    std::shared_ptr<StoreByteIo> workspace,std::shared_ptr<StoreByteIo> target,StoreIdentity id,
    std::uint64_t maximum,StoreCompressionOptions options) const {
  if(!impl_ || !impl_->indexed || impl_->sealed || needs_session_recovery() ||
      !workspace || !target || workspace==target || id==StoreIdentity{} || id==identity() ||
      workspace->extent() || target->extent() || !valid_store_io_contract(target->contract()))
    fail(Code::invalid_operation,"sealed copy requires a closed indexed image and two new backings");
  const auto* original=impl_->objects.paged_if();
  if(!original->info().mirrored_payloads)fail(Code::invalid_operation,"sealed copy requires a mirrored source");
  const auto root=detail::rebind_indexed_volume_root(impl_->indexed->root(),id);
  {
    const auto packed=detail::NativeObjectWriter::sealed_workspace(*original,workspace,id,root,options,
        detail::kIndexedVolumeIdentityOffset);
    packed.snapshot().verify_all();
  }
  (void)detail::ProtectedStripeImage::seal(std::move(workspace),target,id,maximum);
  auto result=open_image(std::move(target),{NativeImageKind::sealed_indexed,maximum});
  result.verify_all();
  return result;
}
NativeFilesystemSnapshot NativeFilesystemSnapshot::create_indexed_copy(std::shared_ptr<StoreByteIo> target,
    StoreIdentity identity,PagedStoreLimits limits,StoreCompressionOptions compression) const {
  if(!impl_ || impl_->indexed || !target || identity==this->identity() || identity==StoreIdentity{})
    fail(Code::invalid_operation,"indexed export requires a legacy source and new target identity");
  try {
    impl_->objects.verify_all();
    const auto model=decode(impl_->objects);
    for(const auto& entry:model.nodes)if(entry.second.delete_pending || entry.second.unlinked)
      fail(Code::invalid_operation,"indexed export requires retired file contexts");
    detail::IndexedVolumeBuilder builder(PagedObjectStoreWriter::create_mirrored(std::move(target),identity,limits,compression),
        model.sequence,model.next_inode,{model.format>=6,model.committed_input_bytes},2);
    constexpr std::size_t inode_batch=64,extent_batch=16;
    std::vector<detail::IndexedInodeInput> inodes;inodes.reserve(inode_batch);
    for(const auto& [id,n]:model.nodes) {
      inodes.push_back({id,n.parent,n.size,n.born,n.modified,n.directory,n.name,n.basic,n.security,n.allocation});
      if(inodes.size()==inode_batch){builder.append(inodes);inodes.clear();}
    }
    if(!inodes.empty())builder.append(inodes);
    std::vector<detail::IndexedExtentInput> extents;extents.reserve(extent_batch);
    std::vector<Bytes> data;data.reserve(extent_batch);
    for(const auto& [id,n]:model.nodes)for(const auto& [block,object]:n.extents) {
      auto bytes=extent_bytes(impl_->objects,model,object);
      const auto length=(std::min)(std::uint64_t(kNativeExtentBytes),n.size-block*kNativeExtentBytes);
      bytes.resize(static_cast<std::size_t>(length));
      while(!bytes.empty() && bytes.back()==std::byte{0})bytes.pop_back();
      if(bytes.empty())continue;
      data.push_back(std::move(bytes));extents.push_back({id,block,data.back()});
      if(extents.size()==extent_batch){builder.append({},extents);extents.clear();data.clear();}
    }
    if(!extents.empty())builder.append({},extents);
    return builder.finish();
  }catch(const std::bad_alloc&){fail(Code::limit,"indexed export allocation failure");}
  catch(const NativeFsError&){throw;}
  catch(const std::exception&){fail(Code::storage_failure,"indexed export failed; source remains intact");}
}
NativeFilesystemWriter NativeFilesystemWriter::initialize(ObjectStoreWriter objects) {
  return initialize_storage(std::move(objects));
}
NativeFilesystemWriter NativeFilesystemWriter::initialize(PagedObjectStoreWriter objects) {
  return initialize_storage(std::move(objects));
}
NativeFilesystemWriter NativeFilesystemWriter::initialize_inode_records(PagedObjectStoreWriter objects) {
  return initialize_storage(std::move(objects),true);
}
NativeFilesystemWriter NativeFilesystemWriter::initialize_indexed_names(PagedObjectStoreWriter objects) {
  return initialize_storage(std::move(objects),true,true);
}
NativeFilesystemWriter NativeFilesystemWriter::initialize_indexed(PagedObjectStoreWriter objects) {
  try {
    auto p=std::make_unique<Impl>(detail::NativeObjectWriter(std::move(objects)));
    p->indexed=detail::initialize_indexed_volume(*p->objects.paged_if());
    return NativeFilesystemWriter(std::move(p));
  }catch(const std::bad_alloc&){fail(Code::limit,"indexed writer initialization allocation failure");}
  catch(const NativeFsError&){throw;}
  catch(const std::exception&){fail(Code::storage_failure,"indexed writer initialization failed");}
}
NativeImageKind NativeFilesystemWriter::image_kind() const {
  const auto& state=checked_impl();
  if(state.indexed)return NativeImageKind::indexed_paged;
  return state.objects.paged()?NativeImageKind::paged:NativeImageKind::flat;
}
NativeFilesystemWriter NativeFilesystemWriter::initialize_shared_bases(PagedObjectStoreWriter objects) {
  return initialize_storage(std::move(objects),true,true,true);
}
NativeFilesystemWriter NativeFilesystemWriter::initialize_storage(detail::NativeObjectWriter objects,bool inode_records,bool indexed_names,bool shared_bases) {
  auto snapshot=objects.snapshot();
  if(objects.poisoned() || snapshot.info().generation!=(objects.paged()?2U:0U) || !snapshot.list().empty())
    fail(Code::invalid_operation,"native initialization requires new empty store");
  if(inode_records && !objects.paged())fail(Code::invalid_operation,"inode records require paged storage");
  if(shared_bases && !snapshot.mirrored())fail(Code::invalid_operation,"shared bases require mirrored paged storage");
  Model model;model.identity=snapshot.identity();model.inode_records=inode_records;model.indexed_names=indexed_names;
  model.shared_bases=shared_bases;
  if(inode_records)model.format=7;
  Node root;root.inode=1;root.directory=true;root.born=root.modified=1;
  model.nodes.emplace(1,std::move(root));
  auto p=std::make_unique<Impl>(std::move(objects),model);
  std::vector<StoreMutation> metadata;namespace_mutations(metadata,model);
  p->objects.commit_replicated(metadata,model.indexed_names,model.shared_bases);
  return NativeFilesystemWriter(std::move(p));
}
NativeFsCommit NativeFilesystemWriter::enable_workload_accounting() {
  return transact({},InodeAction::enable_workload,0);
}
NativeFilesystemWriter::SharedBaseWork NativeFilesystemWriter::prepare_shared_base(
    NativeExtentSelection base_selection,std::span<const NativeExtentSelection> members) {
  return prepare_shared_base(base_selection,members,{});
}
NativeFilesystemWriter::SharedBaseWork NativeFilesystemWriter::prepare_shared_base(
    NativeExtentSelection base_selection,std::span<const NativeExtentSelection> members,
    std::span<const NativeContentToken> expected,bool batch,bool allow_subset,SharedBaseMode mode) {
  if(poisoned() || (!impl_->indexed && !impl_->model.shared_bases) || !impl_->objects.paged_if())
    fail(Code::invalid_operation,"shared-base work requires its explicit namespace");
  if(members.size()>kNativeMaxExtents)fail(Code::limit,"shared-base selection budget");
  auto& owner=*impl_->shared_owner;
  const bool packed_only=mode==SharedBaseMode::pack;
  if(!owner.active.expired() || (!batch && owner.busy()) ||
      ((batch || packed_only) && (!impl_->indexed || expected.empty())))
    fail(Code::invalid_operation,"incompatible shared-base work reservation");
  try {
    auto p=std::make_unique<SharedBaseWork::Impl>();p->owner=impl_->shared_owner;p->batched=batch;
    p->source.emplace(impl_->objects.snapshot_for_staging(impl_->indexed.has_value()));
    if(impl_->indexed) {
      p->indexed=std::make_shared<detail::IndexedSharedBasePreparation>(
          *p->source->paged_if(),*impl_->indexed,base_selection,members,expected,allow_subset,packed_only);
      p->state=p->indexed->progress();
    } else {
      p->decode_model.shared_bases=true;
      const auto bind=[&](NativeExtentSelection selection) {
        const auto& n=selected_inode(impl_->model,selection.inode);
        if(n.directory)fail(Code::is_directory,"shared-base selection is a directory");
        const auto found=n.extents.find(selection.extent_index);
        if(found==n.extents.end())fail(Code::not_found,"shared-base selection is a sparse/missing extent");
        p->bindings.push_back({selection,found->second});return found->second;
      };
      p->base_object=bind(base_selection);std::set<std::uint64_t> ids{p->base_object};
      for(const auto selected:members)ids.insert(bind(selected));
      p->ids.assign(ids.begin(),ids.end());p->state.total_members=ids.size();
      for(const auto id:ids) {
        p->revisions.emplace(id,impl_->shared_owner->revisions[id]);
        const auto found=impl_->model.members.find(id);
        if(found!=impl_->model.members.end())p->decode_model.members.emplace(*found);
      }
      p->frames.reserve(ids.size());
    }
    if(p->state.total_members<2)p->retire(NativeSharedBasePhase::declined);
    else {
      p->reservation=std::make_shared<const unsigned char>(0);
      if(p->indexed) {
        std::erase_if(owner.indexed_work,[](const auto& watch){return watch.preparation.expired();});
        owner.indexed_work.push_back({p->indexed});
      }
      if(!batch)owner.active=p->reservation;
    }
    return SharedBaseWork(std::move(p));
  }catch(const std::bad_alloc&){fail(Code::limit,"shared-base preparation allocation failure");}
}
void NativeFilesystemWriter::check_shared_base(SharedBaseWork& work) const {
  if(poisoned() || !work.impl_)fail(Code::invalid_operation,"invalid shared-base work/source");
  auto& p=*work.impl_;const auto owner=p.owner.lock();
  if(!owner || owner!=impl_->shared_owner)fail(Code::invalid_operation,"shared-base owner expired/differs");
  if(p.state.phase==NativeSharedBasePhase::declined)return;
  if((p.state.phase!=NativeSharedBasePhase::reading_base && p.state.phase!=NativeSharedBasePhase::encoding_members &&
      p.state.phase!=NativeSharedBasePhase::ready) || !p.reservation ||
      (!p.batched && owner->active.lock()!=p.reservation))
    fail(Code::invalid_operation,"shared-base work is terminal");
  if(p.indexed) {
    if(p.indexed->invalidated())fail(Code::invalid_operation,"selected indexed shared-base content changed");
    return;
  }
  for(const auto& [id,revision]:p.revisions)if(owner->revisions[id]!=revision)
    fail(Code::invalid_operation,"selected shared-base content changed");
  for(const auto& binding:p.bindings) {
    const auto found=impl_->model.nodes.find(binding.selection.inode);
    if(found==impl_->model.nodes.end())fail(Code::invalid_operation,"selected shared-base inode retired");
    const auto extent=found->second.extents.find(binding.selection.extent_index);
    if(extent==found->second.extents.end() || extent->second!=binding.object)
      fail(Code::invalid_operation,"selected shared-base extent changed");
  }
}
bool NativeFilesystemWriter::stage_shared_base_search(SharedBaseWork& work,std::size_t maximum_members) {
  return stage_shared_base_search(work,maximum_members,NativeSharedBaseSearchPurpose::advice);
}
bool NativeFilesystemWriter::stage_shared_base_search(SharedBaseWork& work,std::size_t maximum_members,
    NativeSharedBaseSearchPurpose purpose) {
  return stage_shared_base_search(work,maximum_members,0,purpose);
}
bool NativeFilesystemWriter::stage_shared_base_search(SharedBaseWork& work,std::size_t maximum_members,
    std::size_t retained_extents,NativeSharedBaseSearchPurpose purpose) {
  return maximum_members && shared_base_step(work,{},nullptr,maximum_members,retained_extents,false,purpose);
}
bool NativeFilesystemWriter::advance_shared_base(SharedBaseWork& work,std::span<const NativeSharedBaseCopyRange> advice,
    const NativeSharedBaseSearchReply* reply) {
  return shared_base_step(work,advice,reply,0);
}
bool NativeFilesystemWriter::price_shared_base_reference(SharedBaseWork& work) {
  return shared_base_step(work,{},nullptr,0,0,true);
}
void NativeFilesystemWriter::admit_shared_base_variants(SharedBaseWork& work,bool packing,bool staged_reference) noexcept {
  if(auto* pending=work.impl_.get();pending && pending->indexed) {
    pending->indexed->admit_variants(packing,staged_reference);
    pending->state=pending->indexed->progress();
  }
}
bool NativeFilesystemWriter::shared_base_step(SharedBaseWork& work,std::span<const NativeSharedBaseCopyRange> advice,
    const NativeSharedBaseSearchReply* reply,std::size_t search_members,std::size_t retained_extents,bool price_reference,
    NativeSharedBaseSearchPurpose purpose) {
  auto* p=work.impl_.get();
  try {
    check_shared_base(work);
    if(price_reference) {
      if(!p->indexed || !p->indexed->price_reference())return false;
      p->state=p->indexed->progress();
      return true;
    }
    if(search_members) {
      if(!p->indexed)return false;
      auto& owner=*impl_->shared_owner;
      if(p->batched && !owner.search_window.expired())return false;
      auto& window=p->batched?p->batch_window:owner.search_window;
      const bool new_lease=p->batched && window.expired();
      if(new_lease || !p->batched)
        std::erase_if(owner.batch_windows,[](const auto& lease){return lease.window.expired();});
      if(!p->batched && !owner.batch_windows.empty())return false;
      if(new_lease) {
        auto remaining=retained_extents;
        for(const auto& lease:owner.batch_windows) {
          if(lease.extents>remaining)return false;
          remaining-=lease.extents;
        }
        if(search_members>=remaining)return false;
        owner.batch_windows.reserve(owner.batch_windows.size()+1);
      }
      if(p->batched && purpose!=NativeSharedBaseSearchPurpose::advice)return false;
      const auto staged=p->indexed->stage_search(search_members,window,purpose);
      if(new_lease && !window.expired())owner.batch_windows.push_back({window,search_members+1});
      if(staged)p->state=p->indexed->progress();
      return staged;
    }
    if(p->state.phase==NativeSharedBasePhase::ready || p->state.phase==NativeSharedBasePhase::declined)return true;
    if(p->indexed) {
      const auto done=p->indexed->advance(advice,reply);
      p->state=p->indexed->progress();
      if(p->state.phase==NativeSharedBasePhase::declined)p->retire(NativeSharedBasePhase::declined);
      return done;
    }
    if(p->state.phase==NativeSharedBasePhase::reading_base) {
      p->base=extent_bytes(*p->source,p->decode_model,p->base_object);
      p->state.owned_payload_bytes=p->base.size();p->state.phase=NativeSharedBasePhase::encoding_members;return false;
    }
    const auto id=p->ids.at(p->state.completed_members);
    const auto bytes=id==p->base_object?p->base:extent_bytes(*p->source,p->decode_model,id);
    // Binding fields are private placeholders until publication against the
    // current root. Plaintext hashes and patches do not depend on those fields.
    auto encoded=detail::encode_native_group_member(bytes,p->base,detail::kNativeGroupBaseBit+1,1);
    if(!encoded || p->state.owned_payload_bytes+encoded->size()+64>kStoreMaxTransactionBytes+2*kMetadataCap) {
      p->retire(NativeSharedBasePhase::declined);return true;
    }
    if(detail::decode_native_group_member(*encoded,p->base)!=bytes)fail(Code::corrupt,"shared-base private CPU comparison");
    p->references.emplace(id,group_reference(*encoded));p->state.owned_payload_bytes+=encoded->size();
    p->frames.push_back({id,false,std::move(*encoded)});++p->state.completed_members;
    if(p->state.completed_members==p->state.total_members)p->state.phase=NativeSharedBasePhase::ready;
    return p->state.phase==NativeSharedBasePhase::ready;
  }catch(const std::bad_alloc&){
    if(p)p->retire(NativeSharedBasePhase::failed);fail(Code::limit,"shared-base step allocation failure");
  }catch(const NativeFsError& error){
    if(impl_ && (error.code()==Code::corrupt || error.code()==Code::storage_failure))impl_->poisoned=true;
    if(p)p->retire(NativeSharedBasePhase::failed);throw;
  }catch(const std::exception&){
    if(impl_)impl_->poisoned=true;if(p)p->retire(NativeSharedBasePhase::failed);
    fail(Code::storage_failure,"shared-base preparation read failed; recover independently");
  }
}
NativeSharedBaseCommit NativeFilesystemWriter::finish_shared_base(SharedBaseWork& work) {
  auto* p=work.impl_.get();bool committing=false;
  try {
    check_shared_base(work);NativeSharedBaseCommit result;result.members=p->state.total_members;
    if(p->state.phase!=NativeSharedBasePhase::ready && p->state.phase!=NativeSharedBasePhase::declined)
      fail(Code::invalid_operation,"shared-base preparation is incomplete");
    if(p->state.phase==NativeSharedBasePhase::declined) {
      result.transaction.storage=impl_->objects.snapshot_for_staging().info();
      result.transaction.sequence=maintenance_sequence();
      p->retire(NativeSharedBasePhase::completed);return result;
    }
    if(p->indexed) {
      result=impl_->mutate_indexed([&](auto& writer,const auto& source,auto& root,auto) {
        return p->indexed->finish(writer,source,root);
      });
      if(result.section_reference || result.packed_reference) {
        p->state.selected_advice_members=0;
        p->state.reference_changed=false;
      }
      p->retire(NativeSharedBasePhase::completed);
      return result;
    }
    auto source=checked_namespace();const auto* paged=source.paged_if();
    if(!paged || !source.mirrored())fail(Code::invalid_operation,"shared-base dependencies require protected backing");
    result.transaction.storage=source.info();result.transaction.sequence=impl_->model.sequence;
    if(impl_->model.sequence==(std::numeric_limits<std::uint64_t>::max)() ||
        source.info().generation>(std::numeric_limits<std::uint64_t>::max)()-2)
      fail(Code::limit,"shared-base generation exhausted");
    std::uint64_t anchor=detail::kNativeGroupBaseBit+1;
    while(impl_->model.bases.contains(anchor))++anchor;
    if(!group_base_id(anchor))fail(Code::limit,"shared-base identity exhausted");
    const auto generation=source.info().generation+1;auto model=impl_->model;++model.sequence;
    auto changes=std::move(p->frames);changes.reserve(changes.size()+model.bases.size()+2);
    for(auto& change:changes) {
      auto ref=p->references.at(change.id);ref.base_id=anchor;ref.base_generation=generation;
      detail::bind_native_group_member(change.bytes,anchor,generation);
      model.members[change.id]=ref;
    }
    refresh_group_bases(model);changes.push_back({anchor,false,std::move(p->base)});
    for(const auto old:impl_->model.bases)if(!model.bases.contains(old))changes.push_back({old,true,{}});
    changes.push_back({kNamespaceObject,false,namespace_record(model)});
    // Compare complete compacted layouts of the CURRENT root. An unrelated
    // foreground transaction is preserved, not overwritten by a saved Model.
    auto before=source.list(inventory_limit(model));auto after=before;
    for(auto& object:before)if(object.id==kNamespaceObject)object.stored_payload_bytes=0;
    for(const auto& change:changes) {
      const auto found=std::lower_bound(after.begin(),after.end(),change.id,[](const auto& object,auto id){return object.id<id;});
      if(change.erase){if(found==after.end() || found->id!=change.id)fail(Code::corrupt,"shared-base garbage reference missing");after.erase(found);}
      else {
        StoreObjectInfo object{change.id,generation,static_cast<std::uint32_t>(change.bytes.size()),static_cast<std::uint32_t>(change.bytes.size())};
        if(found!=after.end() && found->id==change.id)*found=object;else after.insert(found,object);
      }
    }
    result.before_compacted_lower_bound=paged->rebuilt_size(before);
    result.after_compacted_upper_bound=paged->rebuilt_size(after);
    if(result.after_compacted_upper_bound>=result.before_compacted_lower_bound) {
      p->retire(NativeSharedBasePhase::completed);return result;
    }
    check_shared_base(work);
    if(impl_->objects.generation_for_staging()!=source.info().generation)
      fail(Code::invalid_operation,"shared-base publication root changed");
    result.transaction.sequence=model.sequence;committing=true;
    result.transaction.storage=impl_->objects.commit_replicated(changes,true,true);
    impl_->remember(changes,result.transaction.storage.generation,false);impl_->accept(std::move(model));
    result.applied=true;p->retire(NativeSharedBasePhase::completed);return result;
  }catch(const std::bad_alloc&){
    if(committing && impl_ && impl_->objects.poisoned())impl_->poisoned=true;
    if(p)p->retire(NativeSharedBasePhase::failed);fail(Code::limit,"shared-base publication allocation failure");
  }catch(const NativeFsError& error){
    if(impl_ && (error.code()==Code::corrupt || (error.code()==Code::storage_failure && (!committing || impl_->objects.poisoned()))))
      impl_->poisoned=true;
    if(p)p->retire(NativeSharedBasePhase::failed);throw;
  }catch(const std::exception&){
    if(impl_ && (!committing || impl_->objects.poisoned()))impl_->poisoned=true;
    if(p)p->retire(NativeSharedBasePhase::failed);fail(Code::storage_failure,"shared-base transaction failed; recover independently");
  }
}
NativeSharedBaseCommit NativeFilesystemWriter::compress_shared_base(NativeExtentSelection base,
    std::span<const NativeExtentSelection> members) {
  auto work=prepare_shared_base(base,members);while(!advance_shared_base(work)){}
  return finish_shared_base(work);
}

NativeFsCommit NativeFilesystemWriter::coalesce_equal_extents() {
  return transact({},InodeAction::coalesce_extents,0);
}
NativeFilesystemWriter NativeFilesystemWriter::resume(ObjectStoreWriter objects) {
  return resume_impl(std::move(objects),false);
}
NativeFilesystemWriter NativeFilesystemWriter::resume(PagedObjectStoreWriter objects) {
  return resume_impl(std::move(objects),false);
}
NativeFilesystemWriter NativeFilesystemWriter::resume_closed_session(ObjectStoreWriter objects) {
  return resume_impl(std::move(objects),true);
}
NativeFilesystemWriter NativeFilesystemWriter::resume_closed_session(PagedObjectStoreWriter objects) {
  return resume_impl(std::move(objects),true);
}
NativeFilesystemWriter NativeFilesystemWriter::lease_verified_indexed_reopen(
    const NativeFilesystemSnapshot& source,StoreCompressionOptions foreground) {
  if(!source.impl_ || !source.impl_->indexed || source.needs_session_recovery() ||
      source.storage_info().degraded_checkpoint)
    fail(Code::invalid_operation,"verified indexed reopen requires resolved and healthy source");
  const auto* paged=source.impl_->objects.paged_if();
  auto objects=PagedObjectStoreWriter::resume_impl(source.backing_for_volume_head(),source.identity(),
      source.storage_info().generation,paged->limits(),foreground,false,paged);
  auto owner=std::make_unique<Impl>(std::move(objects));
  owner->indexed=source.impl_->indexed->root();
  return NativeFilesystemWriter(std::move(owner));
}
NativeFilesystemWriter NativeFilesystemWriter::resume_impl(detail::NativeObjectWriter objects,bool closed_session) {
  if(objects.poisoned())fail(Code::invalid_operation,"native admission requires a healthy recovered writer");
  try {
    auto snapshot=objects.snapshot();
    const auto root=object_bytes(snapshot,kNamespaceObject,kMetadataCap);
    if(detail::indexed_volume_root(root)) {
      const auto* paged=snapshot.paged_if();
      if(!paged)fail(Code::corrupt,"indexed admission requires mirrored paged storage");
      const detail::IndexedVolumeView view(*paged,root);
      view.verify_all();
      const auto info=paged->info();
      if(info.degraded_roots || info.degraded_metadata || info.degraded_payloads)
        fail(Code::storage_failure,"indexed writable admission requires healthy copies");
      if(view.root().active_lifetimes && !closed_session)
        fail(Code::invalid_operation,"indexed admission needs explicit retired-file-context recovery");
      auto p=std::make_unique<Impl>(std::move(objects));
      p->indexed=view.root();
      while(p->indexed->active_lifetimes) {
        p->mutate_indexed([](auto& writer,const auto& source,auto& current,auto watched) {
          detail::advance_indexed_retirement(writer,source,current,true,watched);
        });
      }
      return NativeFilesystemWriter(std::move(p));
    }
    auto model=decode(snapshot,root);bool needs=false;
    for(const auto& [id,n]:model.nodes){(void)id;needs=needs || n.delete_pending || n.unlinked;}
    if(needs && !closed_session)
      fail(Code::invalid_operation,"native admission needs explicit retired-file-context recovery");
    if(!snapshot.info().replicated_checkpoint) {
      const StoreMutation metadata{kNamespaceObject,false,encode(model)};
      objects.commit_replicated({&metadata,1});
    }
    NativeFilesystemWriter result(std::make_unique<Impl>(std::move(objects),std::move(model)));
    if(needs)result.transact({},InodeAction::finish_session,0);
    return result;
  }catch(const std::bad_alloc&){fail(Code::limit,"native admission allocation failure");}
  catch(const NativeFsError&){throw;}
  catch(const std::exception&){fail(Code::storage_failure,"native writable admission failed; recover independently");}
}

std::shared_ptr<StoreByteIo> NativeFilesystemSnapshot::backing_for_volume_head() const {
  if(!impl_)fail(Code::invalid_operation,"moved-from volume image");
  if(const auto* paged=impl_->objects.paged_if())return paged->backing_for_volume_head();
  return impl_->objects.flat().backing_for_volume_head();
}
bool NativeFilesystemSnapshot::matches_indexed_checkpoint(const NativeFilesystemSnapshot& other) const noexcept {
  return impl_ && other.impl_ && impl_->indexed && other.impl_->indexed &&
      impl_->indexed->root()==other.impl_->indexed->root() && image_storage()==other.image_storage() &&
      impl_->objects.paged_if()->same_checkpoint(*other.impl_->objects.paged_if());
}
std::optional<NativeContentToken> NativeFilesystemSnapshot::indexed_extent_token(NativeExtentSelection extent) const {
  if(!impl_ || !impl_->indexed)fail(Code::invalid_operation,"content binding requires an indexed snapshot");
  return impl_->indexed->content_at(extent);
}
bool NativeFilesystemSnapshot::has_degraded_indexed_payloads() const noexcept {
  if(!impl_ || !impl_->indexed)return false;
  const auto info=impl_->objects.paged_if()->info();
  return info.mirrored_payloads && info.degraded_payloads && !info.degraded_metadata;
}
void NativeFilesystemSnapshot::verify_compacted_equivalent_for_volume_head(
    const NativeFilesystemSnapshot& target,bool closed_indexed) const {
  if(!impl_||!target.impl_)fail(Code::invalid_operation,"moved-from handoff image");
  verify_all();target.verify_all();
  if(!(closed_indexed && impl_->indexed && target.impl_->indexed) &&
      (needs_session_recovery() || target.needs_session_recovery()))
    fail(Code::invalid_operation,"handoff requires retired resolved file contexts");
  if(impl_->indexed || target.impl_->indexed) {
    if(!impl_->indexed || !target.impl_->indexed || image_storage()!=target.image_storage())
      fail(Code::invalid_operation,"indexed handoff requires the same layout and capacity");
    impl_->indexed->verify_equivalent(*target.impl_->indexed);
    return;
  }
  auto expected=decode(impl_->objects);expected.identity=target.identity();const auto actual=decode(target.impl_->objects);
  if(image_storage()!=target.image_storage() || impl_->objects.mirrored()!=target.impl_->objects.mirrored() ||
      expected.inode_records!=actual.inode_records || expected.indexed_names!=actual.indexed_names || expected.shared_bases!=actual.shared_bases ||
      encode(expected)!=encode(actual))
    fail(Code::invalid_operation,"compacted namespace differs");
  const auto left=impl_->objects.list(inventory_limit(expected)),right=target.impl_->objects.list(inventory_limit(actual));
  if(left.size()!=right.size())fail(Code::invalid_operation,"compacted object inventory differs");
  for(std::size_t i=0;i<left.size();++i) {
    if(left[i].id!=right[i].id || left[i].logical_bytes!=right[i].logical_bytes)
      fail(Code::invalid_operation,"compacted object identity/size differs");
    if(left[i].id==kNamespaceObject)continue;
    if(left[i].generation!=right[i].generation || impl_->objects.read(left[i].id)!=target.impl_->objects.read(right[i].id))
      fail(Code::invalid_operation,"compacted payload differs");
  }
}
NativeFilesystemWriter NativeFilesystemWriter::compact(const NativeFilesystemSnapshot& source,
    std::shared_ptr<StoreByteIo> target,StoreIdentity id,StoreCompressionOptions compression) {
  return compact(source,std::move(target),id,compression,{});
}
NativeFilesystemWriter NativeFilesystemWriter::compact(const NativeFilesystemSnapshot& source,
    std::shared_ptr<StoreByteIo> target,StoreIdentity id,StoreCompressionOptions compression,
    StoreCheckpointOptions checkpoints) {
  return copy_image(source,std::move(target),id,compression,checkpoints,false);
}
NativeFilesystemWriter NativeFilesystemWriter::recompress(const NativeFilesystemSnapshot& source,
    std::shared_ptr<StoreByteIo> target,StoreIdentity id,StoreCompressionOptions compression,
    StoreCheckpointOptions checkpoints) {
  return copy_image(source,std::move(target),id,compression,checkpoints,true);
}
PagedObjectStoreWriter NativeFilesystemWriter::copy_indexed_image(const NativeFilesystemSnapshot& source,
    std::shared_ptr<StoreByteIo> target,StoreIdentity id,StoreCompressionOptions compression,bool reencode) {
  source.verify_all();
  const auto root=detail::rebind_indexed_volume_root(source.impl_->indexed->root(),id);
  if(source.impl_->sealed)
    return detail::NativeObjectWriter::recover_sealed(*source.impl_->objects.paged_if(),std::move(target),
        id,{source.impl_->sealed_limit},root,compression,reencode,detail::kIndexedVolumeIdentityOffset);
  return detail::NativeObjectWriter::rebuild(*source.impl_->objects.paged_if(),std::move(target),
      id,root,compression,reencode,detail::kIndexedVolumeIdentityOffset);
}
NativeFilesystemWriter NativeFilesystemWriter::copy_image(const NativeFilesystemSnapshot& source,
    std::shared_ptr<StoreByteIo> target,StoreIdentity id,StoreCompressionOptions compression,
    StoreCheckpointOptions checkpoints,bool reencode) {
  if(!source.impl_)fail(Code::invalid_operation,"moved-from native compaction source");
  if(source.needs_session_recovery())
    fail(Code::invalid_operation,"native compaction needs completed file-context recovery");
  if(id==source.identity() || id==StoreIdentity{} || !target)
    fail(Code::invalid_operation,"native compaction requires a new target identity");
  try {
    if(source.impl_->indexed) {
      if(checkpoints.compact_metadata)
        fail(Code::invalid_operation,"indexed isolation derives from its backing contract");
      return resume(copy_indexed_image(source,std::move(target),id,compression,reencode));
    }
    auto model=decode(source.impl_->objects);model.identity=id;
    if(const auto* paged=source.impl_->objects.paged_if()) {
      if(checkpoints.compact_metadata)
        fail(Code::invalid_operation,"paged rebuild isolation already derives from the backend contract");
      const auto root=namespace_record(model);
      return resume(detail::NativeObjectWriter::rebuild(*paged,std::move(target),id,root,compression,reencode));
    }
    const auto metadata=encode(model);
    return resume(ObjectStoreWriter::compact_replacing(source.impl_->objects.flat(),
        std::move(target),id,kNamespaceObject,metadata,compression,checkpoints,reencode));
  }catch(const std::bad_alloc&){fail(Code::limit,"native compaction allocation failure");}
  catch(const NativeFsError&){throw;}
  catch(const std::exception& error){
    // Retain bounded backend context rather than hiding the recovery/admission
    // reason. This changes diagnostics only, never retries or ownership.
    std::string message="native compaction failed; retain source and partial target: ";
    const auto* detail=error.what();std::size_t length=0;
    if(detail)while(length<192 && detail[length])++length;
    if(length)message.append(detail,length);
    fail(Code::storage_failure,message.c_str());
  }
}

NativeFilesystemWriter::Recompression NativeFilesystemWriter::prepare_recompression(
    std::shared_ptr<StoreByteIo> target,StoreIdentity id,StoreCompressionOptions compression,
    StoreCheckpointOptions checkpoints) const {
  (void)checked_namespace();
  for(const auto& [inode,entry]:impl_->model.nodes) {
    (void)inode;
    if(entry.delete_pending || entry.unlinked)
      fail(Code::invalid_operation,"staged recompression needs resolved file contexts");
  }
  try {
    auto model=impl_->model;model.identity=id;
    const auto metadata=encode(model);
    return impl_->objects.prepare_recompression(std::move(target),id,kNamespaceObject,
        metadata,compression,checkpoints);
  }catch(const std::bad_alloc&){fail(Code::limit,"staged recompression allocation failure");}
  catch(const NativeFsError&){throw;}
  catch(const std::exception&){fail(Code::storage_failure,"staged recompression preparation failed");}
}

PagedObjectStoreWriter::Rewrite NativeFilesystemWriter::prepare_folded_recompression(
    std::shared_ptr<StoreByteIo> target,StoreIdentity id,StoreCompressionOptions compression,
    std::function<void(std::uint64_t)> admission) const {
  if(poisoned() || !impl_->indexed || impl_->indexed->active_lifetimes || !admission)
    fail(Code::invalid_operation,"folded rewrite requires a healthy indexed writer and resolved file contexts");
  const auto root=detail::rebind_indexed_volume_root(*impl_->indexed,id);
  return impl_->objects.prepare_folded_rebuild(std::move(target),id,root,compression,
      detail::kIndexedVolumeIdentityOffset,std::move(admission));
}

bool NativeFilesystemWriter::advance_folded_recompression(PagedObjectStoreWriter::Rewrite& candidate,
    detail::FoldedCheckpoint& ready) const {
  if(poisoned() || !impl_->indexed)fail(Code::invalid_operation,"invalid folded rewrite source");
  const auto& source=*impl_->objects.paged_if();
  if(!candidate.advance(source))return false;
  ready=candidate.release_folded_workspace(source);
  return true;
}

void NativeFilesystemWriter::verify_folded_recompression_source(const PagedObjectStoreWriter::Rewrite& candidate) const {
  if(poisoned() || !impl_->indexed)fail(Code::invalid_operation,"invalid folded rewrite source");
  candidate.verify_folded_source(*impl_->objects.paged_if());
}

PagedObjectStoreWriter::Rewrite NativeFilesystemWriter::prepare_paged_recompression(
    std::shared_ptr<StoreByteIo> target,StoreIdentity id,StoreCompressionOptions compression,bool reencode,
    PagedRewriteMode mode) const {
  return prepare_paged_recompression_impl(std::move(target),id,compression,reencode,mode,{});
}
PagedObjectStoreWriter::Rewrite NativeFilesystemWriter::prepare_paged_recompression_impl(
    std::shared_ptr<StoreByteIo> target,StoreIdentity id,StoreCompressionOptions compression,bool reencode,
    PagedRewriteMode mode,std::optional<std::uint64_t> cold_after_generation,
    std::function<bool(std::uint64_t)> reserve_writes) const {
  if(poisoned() || !impl_->objects.paged_if())
    fail(Code::invalid_operation,"staged paged rebuild needs a healthy paged writer");
  if(impl_->indexed) {
    if(impl_->indexed->active_lifetimes)
      fail(Code::invalid_operation,"staged indexed rebuild needs resolved file contexts");
    try {
      const auto root=detail::rebind_indexed_volume_root(*impl_->indexed,id);
      std::optional<PagedObjectStoreWriter::RebuildColdEncoding> cold;
      if(cold_after_generation) {
        cold.emplace(PagedObjectStoreWriter::RebuildColdEncoding{*cold_after_generation,
            detail::kNativeIndexedDataObject+1,2*detail::kNativeIndexedDataObject,std::move(reserve_writes)});
      }
      return impl_->objects.prepare_paged_rebuild(std::move(target),id,root,compression,reencode,
          detail::kIndexedVolumeIdentityOffset,mode,std::move(cold));
    }catch(const std::bad_alloc&){fail(Code::limit,"staged indexed preparation allocation failure");}
    catch(const NativeFsError&){throw;}
    catch(const std::exception&){fail(Code::storage_failure,"staged indexed preparation failed");}
  }
  if(mode!=PagedRewriteMode::exact_checkpoint)fail(Code::invalid_operation,"foreground reconciliation needs indexed storage");
  for(const auto& [inode,entry]:impl_->model.nodes) {
    (void)inode;if(entry.delete_pending || entry.unlinked)
      fail(Code::invalid_operation,"staged paged rebuild needs resolved file contexts");
  }
  try {
    auto model=impl_->model;model.identity=id;
    const auto root=namespace_record(model);
    return impl_->objects.prepare_paged_rebuild(std::move(target),id,root,compression,reencode);
  }catch(const std::bad_alloc&){fail(Code::limit,"staged paged preparation allocation failure");}
  catch(const NativeFsError&){throw;}
  catch(const std::exception&){fail(Code::storage_failure,"staged paged preparation failed");}
}
bool NativeFilesystemWriter::advance_paged_recompression(PagedObjectStoreWriter::Rewrite& candidate) const {
  if(poisoned() || !impl_->objects.paged_if() || !candidate.replaces_native_root())
    fail(Code::invalid_operation,"invalid native paged candidate/source");
  try{return candidate.advance(*impl_->objects.paged_if());}
  catch(const std::bad_alloc&){fail(Code::limit,"staged paged allocation failure");}
  catch(const std::exception&){fail(Code::storage_failure,"staged paged candidate failed; source selection unchanged");}
}
bool NativeFilesystemWriter::paged_recompression_ready(const PagedObjectStoreWriter::Rewrite& candidate) const noexcept {
  return !poisoned() && impl_->objects.paged_if() && candidate.ready_for(*impl_->objects.paged_if());
}

NativeFilesystemWriter NativeFilesystemWriter::finish_paged_recompression(PagedObjectStoreWriter::Rewrite& candidate) const {
  if(poisoned() || !impl_->objects.paged_if() || !candidate.replaces_native_root())
    fail(Code::invalid_operation,"invalid native paged candidate/source");
  try {
    if(impl_->indexed) {
      auto root=*impl_->indexed;
      root.identity=candidate.identity();
      auto objects=candidate.finish(*impl_->objects.paged_if());
      auto state=std::make_unique<Impl>(detail::NativeObjectWriter(std::move(objects)));
      state->indexed=root;
      return NativeFilesystemWriter(std::move(state));
    }
    // Each target object already passed independent pre/post-publication CPU
    // comparison. Preserve the exact acknowledged graph without a hidden full
    // backing scan at handoff. This Model copy retains today's bounded frontend.
    auto model=impl_->model;model.identity=candidate.identity();
    auto objects=candidate.finish(*impl_->objects.paged_if());
    return NativeFilesystemWriter(std::make_unique<Impl>(std::move(objects),std::move(model)));
  }catch(const std::bad_alloc&){fail(Code::limit,"staged paged finish allocation failure");}
  catch(const std::exception&){fail(Code::storage_failure,"staged paged finish failed; source selection unchanged");}
}

bool NativeFilesystemWriter::matches_recompression(const Recompression& candidate) const noexcept {
  return !poisoned() && impl_->objects.flat_if() && candidate.matches(*impl_->objects.flat_if());
}

bool NativeFilesystemWriter::indexed_payload_repair_pending() const noexcept {
  return !poisoned() && impl_->indexed && impl_->objects.payload_repair_pending();
}
std::uint64_t NativeFilesystemWriter::indexed_capacity_repair_generation() const noexcept {
  return !poisoned() && impl_->indexed?impl_->objects.capacity_repair_generation():0;
}
bool NativeFilesystemWriter::indexed_payload_scrub_eligible() const noexcept {
  return !poisoned() && impl_->indexed && (impl_->indexed->inodes>1 || impl_->indexed->extents);
}
PagedPayloadScrubResult NativeFilesystemWriter::scrub_indexed_payload(const PagedPayloadScrubBudget& budget) {
  if(!indexed_payload_scrub_eligible())return {};
  return impl_->objects.scrub_payload(budget);
}
bool NativeFilesystemWriter::restore_indexed_payload_scrub(const PagedPayloadScrubPosition& position) noexcept {
  return indexed_payload_scrub_eligible() && impl_->objects.restore_payload_scrub(position);
}
PagedPayloadRepairProgress NativeFilesystemWriter::repair_indexed_payload() {
  if(!indexed_payload_repair_pending())return PagedPayloadRepairProgress::idle;
  try{return impl_->objects.repair_payload();}
  catch(const std::bad_alloc&){fail(Code::limit,"indexed repair allocation failure");}
  catch(const std::exception&){fail(Code::storage_failure,"indexed replica repair failed");}
}
std::uint64_t NativeFilesystemWriter::indexed_obsolete_bytes() const noexcept {
  if(poisoned() || !impl_->indexed)return 0;
  const auto& store=*impl_->objects.paged_if();
  // The verified aggregate survives reopen. Allow the replacement namespace
  // body at its full decoded size, even though the current body is counted too.
  const auto retained=store.compaction_savings_lower_bound(detail::kIndexedVolumeRootMaximumBytes);
  return (std::max)(store.obsolete_bytes(),retained);
}

bool NativeFilesystemWriter::matches_paged_recompression(
    const PagedObjectStoreWriter::Rewrite& candidate) const noexcept {
  return !poisoned() && impl_->objects.paged_if() && candidate.matches(*impl_->objects.paged_if());
}

std::uint64_t NativeFilesystemWriter::recompression_generation() const noexcept {
  return poisoned()?0:impl_->objects.generation_for_staging();
}

std::optional<StoreRecompressionInput> NativeFilesystemWriter::next_recompression_input(
    const Recompression& candidate) {
  try{return candidate.next_input();}
  catch(const std::bad_alloc&){fail(Code::limit,"recompression input allocation failure");}
  catch(const std::exception&){fail(Code::storage_failure,"recompression input verification failed");}
}

std::optional<StoreRecompressionFeedback> NativeFilesystemWriter::recompression_feedback(
    const Recompression& candidate) noexcept {
  return candidate.feedback();
}

bool NativeFilesystemWriter::advance_recompression(Recompression& candidate,
    std::uint64_t object_id,std::span<const StoreMatchProposal> matches) {
  try{return candidate.advance(object_id,matches);}
  catch(const std::bad_alloc&){fail(Code::limit,"staged recompression allocation failure");}
  catch(const std::exception&){fail(Code::storage_failure,"staged recompression copy failed");}
}

void NativeFilesystemWriter::finish_recompression(Recompression& candidate) {
  try{(void)resume(candidate.finish());}
  catch(const std::bad_alloc&){fail(Code::limit,"staged recompression allocation failure");}
  catch(const NativeFsError&){throw;}
  catch(const std::exception&){fail(Code::storage_failure,"staged recompression verification failed");}
}

NativeFsCommit NativeFilesystemWriter::apply(std::span<const NativeFileMutation> operations) {
  return apply_with_basic(operations,{});
}
NativeFsCommit NativeFilesystemWriter::apply_with_basic(std::span<const NativeFileMutation> operations,
    std::span<const NativeBasicMutation> basic) {
  return apply_with_metadata(operations,basic,{});
}
NativeFsCommit NativeFilesystemWriter::apply_with_metadata(std::span<const NativeFileMutation> operations,
    std::span<const NativeBasicMutation> basic,std::span<const NativeSecurityMutation> security) {
  if(poisoned())fail(Code::storage_failure,"native writer is terminal");
  const auto maximum=impl_->indexed?kStoreMaxObjects:std::size_t{16};
  if(operations.size()>maximum || basic.size()>maximum || security.size()>maximum ||
      operations.size()+basic.size()+security.size()==0 ||
      operations.size()+basic.size()+security.size()>maximum)fail(Code::limit,"native combined operation count");
  for(const auto& op:security) {
    if((!op.inode)==(!op.created_ordinal) || op.created_ordinal>maximum ||
        !native_security_descriptor_framed(op.value))
      fail(Code::invalid_operation,"invalid native security descriptor/selector");
  }
  for(const auto& op:basic) {
    if((!op.inode)==(!op.created_ordinal) || op.created_ordinal>maximum)
      fail(Code::invalid_operation,"basic metadata requires exactly one valid selector");
    // Reject request sentinels/unsupported bits before backing I/O. Node-kind
    // consistency is checked against the staged selected inode.
    validate_basic(op.value,bool(op.value.attributes&0x10U));
  }
  std::uint64_t input=0;
  for(const auto& op:operations) {
    if(static_cast<int>(op.operation)<0 || static_cast<int>(op.operation)>static_cast<int>(NativeFileOperation::remove))
      fail(Code::invalid_operation,"unknown native operation");
    if(!impl_->indexed && op.data.size()>kStoreMaxObjectBytes)fail(Code::limit,"native write size budget");
    if(op.data.size()>kStoreMaxTransactionBytes-input)fail(Code::limit,"native input budget");
    input+=op.data.size();
  }
  return transact(operations,InodeAction::path_mutations,0,basic,nullptr,false,nullptr,security);
}
NativeFsCommit NativeFilesystemWriter::set_security_descriptor(std::uint64_t inode,std::span<const std::byte> value) {
  if(poisoned())fail(Code::storage_failure,"native writer is terminal");
  if(!native_security_descriptor_framed(value))
    fail(Code::invalid_operation,"invalid native security descriptor framing");
  const NativeSecurityMutation op{inode,0,NativeSecurityDescriptor(value.begin(),value.end())};
  return apply_with_metadata({},{},{&op,1});
}
NativeFsCommit NativeFilesystemWriter::set_basic_metadata(std::uint64_t inode,const NativeBasicMetadata& value) {
  const NativeBasicMutation op{inode,0,value};
  return apply_with_basic({},{&op,1});
}
NativeInodeCreateCommit NativeFilesystemWriter::create_inode_result(std::string_view path,
    bool directory,const NativeBasicMetadata& value,std::span<const std::byte> descriptor,
    bool delete_pending) {
  auto stored=create_inode_storage_result(path,directory,value,descriptor,{},delete_pending);
  NativeInodeCreateCommit result;result.info=stored.state.info;
  result.transaction=std::move(stored.transaction);return result;
}
NativeInodeAllocationCommit NativeFilesystemWriter::create_inode_storage_result(std::string_view path,
    bool directory,const NativeBasicMetadata& value,std::span<const std::byte> descriptor,
    std::optional<std::uint64_t> reservation,bool delete_pending) {
  if(poisoned())fail(Code::storage_failure,"native writer is terminal");
  if(reservation && (directory || *reservation%kNativeAllocationUnit ||
      (!impl_->indexed && *reservation>kNativeAllocationCapacity)))
    fail(Code::invalid_operation,"invalid native creation reservation");
  validate_basic(value,directory);
  if(!native_security_descriptor_framed(descriptor))
    fail(Code::invalid_operation,"invalid native creation security descriptor");
  try {
    // Bound the path before copying the caller's bytes into a mutation.
    if(impl_->indexed)(void)detail::indexed_path_name(path);
    else (void)canonical(path);
    NativeFileMutation op;op.path=std::string(path);
    op.operation=directory?NativeFileOperation::create_directory:NativeFileOperation::create_file;
    const NativeBasicMutation basic{0,1,value};
    const NativeSecurityMutation security{0,1,NativeSecurityDescriptor(descriptor.begin(),descriptor.end())};
    NativeInodeAllocationCommit result;
    result.transaction=transact({&op,1},InodeAction::create,0,{&basic,1},
        nullptr,delete_pending,nullptr,{&security,1},nullptr,
        reservation?&*reservation:nullptr,&result.state);
    return result;
  }catch(const std::bad_alloc&){fail(Code::limit,"native creation allocation failure");}
}
NativeFsCommit NativeFilesystemWriter::transact(std::span<const NativeFileMutation> operations,
    InodeAction action,std::uint64_t inode,std::span<const NativeBasicMutation> basic,
    const NativeAutomaticMetadata* automatic,bool delete_requested,NativeInodeCommitInfo* published,
    std::span<const NativeSecurityMutation> security,const NativeOverwriteMetadata* overwrite,
    const std::uint64_t* reservation,NativeInodeAllocationInfo* allocation,bool trim_allocation,
    std::span<const NativeLz4ExtentCandidate> lz4_candidates) {
  if(poisoned())fail(Code::storage_failure,"native writer is terminal");
  if(impl_->indexed) {
    const detail::IndexedVolumeEdit request{action,inode,operations,basic,security,automatic,overwrite,
        reservation,delete_requested,trim_allocation};
    return impl_->mutate_indexed([&](auto& writer,const auto& source,auto& root,auto watched) {
      return detail::commit_indexed_volume(writer,source,root,request,published,allocation,watched);
    });
  }
  admit_lz4_extent_candidates(lz4_candidates);
  if(!lz4_candidates.empty() && action!=InodeAction::mutate)
    fail(Code::invalid_operation,"native advice requires a selected inode mutation");
  if(automatic && !native_cleanup_metadata_valid(*automatic))
    fail(Code::invalid_operation,"invalid automatic metadata time/unused clock");
  bool committing=false;
  try {
    auto snapshot=checked_namespace();
    Stage stage(impl_->model,snapshot);
    switch(action) {
      case InodeAction::path_mutations:for(const auto& op:operations)stage.apply(op);break;
      case InodeAction::create:
        if(inode || operations.size()!=1 ||
            (operations.front().operation!=NativeFileOperation::create_file &&
             operations.front().operation!=NativeFileOperation::create_directory))
          fail(Code::invalid_operation,"invalid native creation transaction");
        stage.apply(operations.front());
        if(stage.created.size()!=1)fail(Code::corrupt,"native creation selector invariant");
        if(reservation)stage.reserve(stage.created.front(),*reservation);
        if(delete_requested)stage.set_delete(stage.created.front(),true);
        break;
      case InodeAction::overwrite:
        if(!inode || !overwrite || !operations.empty() || !basic.empty() ||
            !security.empty() || automatic || delete_requested ||
            !native_overwrite_metadata_valid(*overwrite))
          fail(Code::invalid_operation,"invalid native overwrite transaction");
        stage.overwrite_inode(inode,*overwrite);
        if(reservation)stage.reserve(inode,*reservation);
        break;
      case InodeAction::mutate:
        if(!inode || operations.size()!=1)fail(Code::invalid_operation,"invalid inode transaction");
        stage.apply(operations.front(),inode);break;
      case InodeAction::reserve:
        if(!reservation || !operations.empty() || !basic.empty() || !security.empty() || overwrite || delete_requested)
          fail(Code::invalid_operation,"invalid native reservation transaction");
        stage.reserve(inode,*reservation);break;
      case InodeAction::set_delete:stage.set_delete(inode,true);break;
      case InodeAction::clear_delete:stage.set_delete(inode,false);break;
      case InodeAction::unlink:stage.unlink(inode);break;
      case InodeAction::retire:stage.retire(inode);break;
      case InodeAction::finish_session:stage.finish_session();break;
      case InodeAction::enable_workload:
        if(stage.model.format<6){stage.model.format=6;stage.changed=true;}
        break;
      case InodeAction::coalesce_extents:stage.coalesce_equal_extents();break;
      case InodeAction::cleanup_initial:
        if(!automatic)fail(Code::invalid_operation,"invalid initial-delete Cleanup metadata");
        stage.cleanup_initial(inode,delete_requested,*automatic);break;
      case InodeAction::cleanup:
        if(!automatic)
          fail(Code::invalid_operation,"invalid automatic Cleanup metadata");
        stage.cleanup(inode,delete_requested,*automatic);break;
    }
    if(trim_allocation) {
      if(action!=InodeAction::cleanup && action!=InodeAction::cleanup_initial)
        fail(Code::invalid_operation,"trim outside native Cleanup");
      stage.trim_reservation(inode);
    }
    // Mutating stable-inode data and trusted effects share the same Stage,
    // verification and single replicated publication. Cleanup applies its
    // own effects before unlink; never apply either effect twice.
    if((action==InodeAction::mutate || action==InodeAction::reserve) && automatic)
      stage.automatic_metadata(inode,*automatic);
    for(const auto& op:basic)stage.set_basic(op);
    for(const auto& op:security)stage.set_security(op);
    validate_structure(stage.model,true);
    // Capture only bounded scalar/optional facts from the fully validated
    // staged model. Never query backing state or build a path after commit.
    NativeInodeCommitInfo info;NativeInodeAllocationInfo allocation_info;
    if(published || allocation) {
      const auto selected=action==InodeAction::create?stage.created.front():inode;
      const auto& n=selected_inode(stage.model,selected);
      info=commit_info(n);
      if(allocation)allocation_info={info,n.allocation};
    }
    NativeFsCommit result;result.created_inodes=std::move(stage.created);
    if(!stage.changed) {
      result.storage=snapshot.info();result.sequence=impl_->model.sequence;
      if(published)*published=info;
      if(allocation)*allocation=allocation_info;
      return result;
    }
    auto changes=stage.mutations(inode,lz4_candidates);result.sequence=stage.model.sequence;
    committing=true;
    try{result.storage=impl_->objects.commit_replicated(changes,stage.model.indexed_names,stage.model.shared_bases);}
    catch(const std::exception&){
      if(impl_->objects.poisoned())impl_->poisoned=true;
      fail(Code::storage_failure,"native transaction storage commit failed");
    }
    impl_->remember(changes,result.storage.generation);
    impl_->accept(std::move(stage.model));
    if(published)*published=info;
    if(allocation)*allocation=allocation_info;
    return result;
  }catch(const std::bad_alloc&){fail(Code::limit,"native allocation failure");}
  catch(const NativeFsError& e){
    if(e.code()==Code::corrupt || (e.code()==Code::storage_failure && (!committing || impl_->objects.poisoned())))impl_->poisoned=true;
    throw;
  }catch(const std::exception&){impl_->poisoned=true;fail(Code::storage_failure,"native backing read failed");}
}
NativeInodeWriteCommit NativeFilesystemWriter::write_inode(std::uint64_t inode,
    std::uint64_t offset,std::span<const std::byte> bytes,NativeInodeWriteOptions options) {
  return write_inode_with_metadata(inode,offset,bytes,options,{});
}
NativeInodeWriteCommit NativeFilesystemWriter::write_inode_with_metadata(std::uint64_t inode,
    std::uint64_t offset,std::span<const std::byte> bytes,NativeInodeWriteOptions options,
    const NativeAutomaticMetadata& effect) {
  NativeInodeWriteCommit result;
  auto committed=write_inode_result(inode,offset,bytes,options,effect);
  result.inode=committed.info.inode;result.offset=committed.offset;result.bytes=committed.bytes;
  result.transaction=std::move(committed.transaction);
  return result;
}
NativeInodeMutationCommit NativeFilesystemWriter::write_inode_result(std::uint64_t inode,
    std::uint64_t offset,std::span<const std::byte> bytes,NativeInodeWriteOptions options,
    const NativeAutomaticMetadata& effect) {
  auto stored=write_inode_storage_result(inode,offset,bytes,options,effect);
  NativeInodeMutationCommit result;result.info=stored.state.info;result.offset=stored.offset;result.bytes=stored.bytes;
  result.transaction=std::move(stored.transaction);return result;
}
NativeInodeStorageMutationCommit NativeFilesystemWriter::write_inode_storage_result(std::uint64_t inode,
    std::uint64_t offset,std::span<const std::byte> bytes,NativeInodeWriteOptions options,
    const NativeAutomaticMetadata& effect) {
  return write_inode_storage_with_lz4(inode,offset,bytes,{},options,effect);
}
NativeInodeStorageMutationCommit NativeFilesystemWriter::write_inode_storage_with_lz4(std::uint64_t inode,
    std::uint64_t offset,std::span<const std::byte> bytes,
    std::span<const NativeLz4ExtentCandidate> candidates,NativeInodeWriteOptions options,
    const NativeAutomaticMetadata& effect) {
  if(poisoned())fail(Code::storage_failure,"native writer is terminal");
  if(!native_cleanup_metadata_valid(effect))
    fail(Code::invalid_operation,"invalid automatic write time/unused clock");
  if(impl_->indexed) {
    const auto maximum=std::size_t(kStoreMaxTransactionBytes);
    if(bytes.size()>maximum)fail(Code::limit,"indexed inode input budget");
    admit_lz4_extent_candidates(candidates);
    return impl_->mutate_indexed([&](auto& writer,const auto& source,auto& root,auto watched) {
      return detail::write_indexed_inode(writer,source,root,inode,offset,bytes,options,effect,candidates,watched);
    });
  }
  if(bytes.size()>kStoreMaxObjectBytes)fail(Code::limit,"native write size budget");
  try {
    const auto& n=selected_inode(impl_->model,inode);
    if(n.directory)fail(Code::is_directory,"cannot write native directory");
    const auto selected=options.append?n.size:offset;
    auto count=bytes.size();
    if(options.constrained)
      count=selected>=n.size?0:(std::min)(count,static_cast<std::size_t>(
          (std::min)(n.size-selected,std::uint64_t(kStoreMaxObjectBytes))));
    if((!options.constrained || count) &&
        (selected>kNativeMaxFileBytes || count>kNativeMaxFileBytes-selected))
      fail(Code::limit,"native write bounds");
    NativeFileMutation op;op.operation=NativeFileOperation::write;
    // The private selector addresses this live inode directly, including a
    // detached inode. No visible/hidden surrogate pathname is manufactured.
    // No-op constrained ranges may carry any UINT64 offset; never forward
    // that sentinel through ordinary write arithmetic or create a sparse gap.
    op.offset=count?selected:(std::min)(selected,n.size);
    if(count)op.data.assign(bytes.begin(),bytes.begin()+static_cast<std::ptrdiff_t>(count));
    NativeInodeStorageMutationCommit result;result.offset=selected;
    result.bytes=static_cast<std::uint32_t>(count);
    result.transaction=transact({&op,1},InodeAction::mutate,inode,{},
        count && effect.any()?&effect:nullptr,false,nullptr,{},nullptr,nullptr,&result.state,false,candidates);
    return result;
  }catch(const std::bad_alloc&){fail(Code::limit,"native inode write allocation failure");}
}
NativeFsCommit NativeFilesystemWriter::truncate_inode(std::uint64_t inode,std::uint64_t bytes) {
  return truncate_inode_with_metadata(inode,bytes,{});
}
NativeFsCommit NativeFilesystemWriter::truncate_inode_with_metadata(std::uint64_t inode,
    std::uint64_t bytes,const NativeAutomaticMetadata& effect) {
  auto result=truncate_inode_result(inode,bytes,effect);
  return std::move(result.transaction);
}
NativeInodeMutationCommit NativeFilesystemWriter::truncate_inode_result(std::uint64_t inode,
    std::uint64_t bytes,const NativeAutomaticMetadata& effect) {
  auto stored=truncate_inode_storage_result(inode,bytes,effect);
  NativeInodeMutationCommit result;result.info=stored.state.info;
  result.transaction=std::move(stored.transaction);return result;
}
NativeInodeStorageMutationCommit NativeFilesystemWriter::truncate_inode_storage_result(std::uint64_t inode,
    std::uint64_t bytes,const NativeAutomaticMetadata& effect) {
  if(poisoned())fail(Code::storage_failure,"native writer is terminal");
  if(!native_cleanup_metadata_valid(effect))
    fail(Code::invalid_operation,"invalid automatic truncate time/unused clock");
  if(impl_->indexed)return impl_->mutate_indexed([&](auto& writer,const auto& source,auto& root,auto watched) {
    return detail::truncate_indexed_inode(writer,source,root,inode,bytes,effect,watched);
  });
  if(bytes>kNativeMaxFileBytes)fail(Code::limit,"native truncate bounds");
  try {
    const auto& n=selected_inode(impl_->model,inode);
    if(n.directory)fail(Code::is_directory,"cannot truncate native directory");
    NativeFileMutation op;op.operation=NativeFileOperation::truncate;
    op.size=bytes;
    NativeInodeStorageMutationCommit result;
    result.transaction=transact({&op,1},InodeAction::mutate,inode,{},
        effect.any()?&effect:nullptr,false,nullptr,{},nullptr,nullptr,&result.state);
    return result;
  }catch(const std::bad_alloc&){fail(Code::limit,"native inode truncate allocation failure");}
}
NativeInodeMutationCommit NativeFilesystemWriter::overwrite_inode_result(std::uint64_t inode,
    const NativeOverwriteMetadata& effect) {
  auto stored=overwrite_inode_storage_result(inode,effect);
  NativeInodeMutationCommit result;result.info=stored.state.info;
  result.transaction=std::move(stored.transaction);return result;
}
NativeInodeAllocationCommit NativeFilesystemWriter::overwrite_inode_storage_result(std::uint64_t inode,
    const NativeOverwriteMetadata& effect,std::optional<std::uint64_t> reservation) {
  if(poisoned())fail(Code::storage_failure,"native writer is terminal");
  if(!native_overwrite_metadata_valid(effect) ||
      (reservation && (*reservation%kNativeAllocationUnit ||
          (!impl_->indexed && *reservation>kNativeAllocationCapacity))))
    fail(Code::invalid_operation,"invalid canonical native overwrite metadata/reservation");
  if(!impl_->indexed) {
    const auto& n=selected_inode(impl_->model,inode);
    if(n.directory)fail(Code::is_directory,"cannot overwrite native directory");
    if(n.delete_pending || n.unlinked || !n.basic)
      fail(Code::invalid_operation,"overwrite needs a live regular inode with basic metadata");
  }
  NativeInodeAllocationCommit result;
  result.transaction=transact({},InodeAction::overwrite,inode,{},nullptr,false,nullptr,{},&effect,
      reservation?&*reservation:nullptr,&result.state);
  return result;
}
NativeFsCommit NativeFilesystemWriter::set_delete_pending(std::uint64_t inode,bool pending) {
  return transact({},pending?InodeAction::set_delete:InodeAction::clear_delete,inode);
}
NativeFsCommit NativeFilesystemWriter::unlink_inode(std::uint64_t inode) {
  return transact({},InodeAction::unlink,inode);
}
NativeFsCommit NativeFilesystemWriter::cleanup_inode(std::uint64_t inode,bool delete_file,
    const NativeCleanupMetadata& effect) {
  if(poisoned())fail(Code::storage_failure,"native writer is terminal");
  if(!native_cleanup_metadata_valid(effect))
    fail(Code::invalid_operation,"invalid automatic Cleanup time/unused clock");
  return transact({},InodeAction::cleanup,inode,{},&effect,delete_file);
}
NativeInodeMutationCommit NativeFilesystemWriter::cleanup_initial_delete_result(std::uint64_t inode,
    bool delete_file,const NativeCleanupMetadata& effect) {
  if(poisoned())fail(Code::storage_failure,"native writer is terminal");
  if(inode==1 || !native_cleanup_metadata_valid(effect))
    fail(Code::invalid_operation,"invalid initial-delete Cleanup input");
  NativeInodeMutationCommit result;
  result.transaction=transact({},InodeAction::cleanup_initial,inode,{},&effect,delete_file,&result.info);
  return result;
}
NativeInodeAllocationCommit NativeFilesystemWriter::cleanup_inode_storage_result(std::uint64_t inode,
    bool delete_file,bool initial_delete,const NativeCleanupEvent& event) {
  if(poisoned())fail(Code::storage_failure,"native writer is terminal");
  if((initial_delete && inode==1) || !native_cleanup_metadata_valid(event.metadata))
    fail(Code::invalid_operation,"invalid native Cleanup event");
  NativeInodeAllocationCommit result;
  result.transaction=transact({},initial_delete?InodeAction::cleanup_initial:InodeAction::cleanup,
      inode,{},&event.metadata,delete_file,nullptr,{},nullptr,nullptr,&result.state,event.trim_allocation);
  return result;
}
NativeInodeAllocationCommit NativeFilesystemWriter::reserve_inode_result(std::uint64_t inode,
    std::uint64_t bytes,const NativeAutomaticMetadata& effect) {
  if(poisoned())fail(Code::storage_failure,"native writer is terminal");
  if(bytes%kNativeAllocationUnit || (!impl_->indexed && bytes>kNativeAllocationCapacity) ||
      !native_cleanup_metadata_valid(effect))
    fail(Code::invalid_operation,"invalid native reservation input");
  NativeInodeAllocationCommit result;
  result.transaction=transact({},InodeAction::reserve,inode,{},effect.any()?&effect:nullptr,
      false,nullptr,{},nullptr,&bytes,&result.state);
  return result;
}
NativeInodeAllocationInfo NativeFilesystemWriter::inspect_allocation(std::uint64_t inode) const {
  return inspect_allocation(inode,false);
}
NativeInodeAllocationInfo NativeFilesystemWriter::inspect_allocation(std::uint64_t inode,bool verify_ancestry) const {
  auto& state=checked_impl();
  if(state.indexed)return state.read_indexed([&](const auto& view) {
    return view.allocation_inode(inode,verify_ancestry);
  });
  return state.read_checked_inode(inode,[&](const auto& snapshot,const Node& n) {
    if(verify_ancestry)verify_path_ancestors(snapshot,state.model,n);
    return NativeInodeAllocationInfo{commit_info(n),n.allocation};
  });
}
NativeAllocationUsage NativeFilesystemWriter::inspect_allocation_usage() const {
  auto& state=checked_impl();
  if(state.indexed)return state.read_indexed([](const auto& view) {
    return NativeAllocationUsage{static_cast<std::size_t>(view.root().charged_extents),
        static_cast<std::size_t>(view.root().extents)};
  });
  auto objects=checked_namespace();(void)objects;return allocation_usage(impl_->model);
}
NativeFsCommit NativeFilesystemWriter::retire_inode(std::uint64_t inode) {
  return transact({},InodeAction::retire,inode);
}
bool NativeFilesystemWriter::retirement_pending() const noexcept {
  return !poisoned() && impl_->indexed && impl_->indexed->retiring_inodes!=0;
}
bool NativeFilesystemWriter::reclaim_retired_inode() {
  auto& state=checked_impl();
  if(!state.indexed || !state.indexed->retiring_inodes)return false;
  state.mutate_indexed([](auto& writer,const auto& source,auto& root,auto watched) {
    detail::advance_indexed_retirement(writer,source,root,false,watched);
  });
  return true;
}
bool NativeFilesystemWriter::directory_backfill_pending() const noexcept {
  return !poisoned() && impl_->indexed && !impl_->indexed->directory_backfill_complete &&
      impl_->directory_backfill.capacity_sequence!=impl_->indexed->sequence;
}
NativeDirectoryBackfillStep NativeFilesystemWriter::advance_directory_backfill() {
  using Step=NativeDirectoryBackfillStep;
  auto& state=checked_impl();
  if(!directory_backfill_pending())return Step::idle;
  auto& work=state.directory_backfill;
  try {
    return state.mutate_indexed([&](auto& writer,const auto& source,auto& root,auto watched) {
      return detail::advance_indexed_directory_backfill(writer,source,root,work,watched);
    });
  }catch(const NativeFsError& error) {
    work.stack.clear();
    if(error.code()==Code::limit && error.append_shortfall_bytes()) {
      work.capacity_sequence=state.indexed->sequence;
      return Step::capacity;
    }
    throw;
  }
}
void NativeFilesystemWriter::cancel_directory_backfill() noexcept {
  if(impl_)impl_->directory_backfill={};
}
std::uint64_t NativeFilesystemWriter::indexed_append_floor() const noexcept {
  if(poisoned() || !impl_->indexed)return 0;
  return impl_->objects.paged_append_floor_for_staging();
}
bool NativeFilesystemWriter::supports_file_contexts() const noexcept {
  return !poisoned() && (!impl_->indexed || impl_->indexed->format>=2);
}
NativeFilesystemWriter::Impl& NativeFilesystemWriter::checked_impl() const {
  if(poisoned())fail(Code::storage_failure,"native writer is terminal");
  return *impl_;
}
detail::NativeObjectSnapshot NativeFilesystemWriter::checked_namespace() const {
  return checked_impl().read_checked(true,[](detail::NativeObjectSnapshot snapshot){return snapshot;});
}
std::optional<NativeBasicMetadata> NativeFilesystemWriter::inspect_basic_metadata(std::uint64_t inode) const {
  auto& state=checked_impl();
  if(state.indexed)return state.read_indexed([&](const auto& view){return view.inode(inode).basic;});
  return state.read_checked_inode(inode,[](const auto&,const Node& n){return n.basic;});
}
std::optional<NativeSecurityDescriptor> NativeFilesystemWriter::inspect_security_descriptor(std::uint64_t inode) const {
  auto& state=checked_impl();
  if(state.indexed)return state.read_indexed([&](const auto& view){return view.inode(inode).security;});
  return state.read_checked_inode(inode,[](const auto&,const Node& n){return n.security;});
}
NativeInodeState NativeFilesystemWriter::inspect_path(std::string_view path) const {
  auto& state=checked_impl();
  if(state.indexed)return state.read_indexed([&](const auto& view) {
    return view.state(path);
  });
  return state.read_checked(false,[&](const auto& snapshot) {
    const auto& n=resolve_snapshot(snapshot,state.model,path,true);
    return NativeInodeState{file_info(state.model,n),n.delete_pending,n.unlinked};
  });
}
NativePathLookup NativeFilesystemWriter::inspect_path_lookup(std::string_view path) const {
  auto& state=checked_impl();
  if(state.indexed)return state.read_indexed([&](const auto& view) {
    auto n=view.resolve(path);
    return NativePathLookup{detail::indexed_inode_allocation(n),std::move(n.security)};
  });
  return state.read_checked(false,[&](const auto& snapshot) {
    const auto& n=resolve_snapshot(snapshot,state.model,path,true);
    return NativePathLookup{{commit_info(n),n.allocation},n.security};
  });
}
NativeInodeState NativeFilesystemWriter::inspect_inode(std::uint64_t inode) const {
  auto& state=checked_impl();
  if(state.indexed)return state.read_indexed([&](const auto& view) {
    return view.state_inode(inode);
  });
  return state.read_checked_inode(inode,[&](const auto& snapshot,const Node& n) {
    verify_path_ancestors(snapshot,state.model,n);
    return NativeInodeState{file_info(state.model,n),n.delete_pending,n.unlinked};
  });
}
std::vector<NativeFileInfo> NativeFilesystemWriter::inspect_directory(std::uint64_t inode) const {
  auto& state=checked_impl();
  if(state.indexed)return state.read_indexed([&](const auto& view){return view.list_all(inode);});
  return state.read_checked_inode(inode,[&](const auto& snapshot,const Node& n) {
    verify_directory(snapshot,state.model,n);
    verify_path_ancestors(snapshot,state.model,n);
    return list_node(state.model,n,state.model.inode_records?&snapshot:nullptr);
  });
}
std::optional<NativeDirectoryRecordPage> NativeFilesystemWriter::inspect_directory_page(std::uint64_t inode,
    std::string_view after,std::size_t maximum,const std::function<bool(const NativeInodeState&)>& admit) const {
  auto& state=checked_impl();
  const auto check=[&](const NativeInodeState& directory) {
    if(!admit(directory))return false;
    if(!maximum || maximum>1024)fail(Code::limit,"directory page budget");
    return true;
  };
  if(state.indexed)return state.read_indexed([&](const auto& view) {
    return view.directory_records(inode,after,maximum,check);
  });
  return state.read_checked_inode(inode,[&](const auto& snapshot,const Node& directory)->std::optional<NativeDirectoryRecordPage> {
    verify_path_ancestors(snapshot,state.model,directory);
    if(!check({file_info(state.model,directory),directory.delete_pending,directory.unlinked}))return {};
    if(after.size()>100 || std::ranges::any_of(after,[](char c) {
        return c<0x20 || c>0x7e || c=='/' || c=='\\';
      }))fail(Code::invalid_path,"invalid native directory marker");
    // Both special entries sort before every real name.
    std::string key=after=="." || after==".."?std::string{}:std::string(after);
    std::ranges::transform(key,key.begin(),directory_ascii_lower);
    verify_directory(snapshot,state.model,directory);
    NativeDirectoryRecordPage result;
    result.entries.reserve(maximum);
    auto it=state.model.names.upper_bound({inode,key});
    for(;it!=state.model.names.end() && it->first.first==inode && result.entries.size()<maximum;++it) {
      const auto& entry=node(state.model,it->second);
      verify_inode(snapshot,state.model,entry);
      result.entries.push_back({entry.name,{commit_info(entry),entry.allocation}});
    }
    result.more=it!=state.model.names.end() && it->first.first==inode;
    return result;
  });
}
Bytes NativeFilesystemWriter::read_inode(std::uint64_t inode,std::uint64_t offset,std::uint32_t bytes,
    detail::NativeLz4ReadRoute* route) const {
  auto& state=checked_impl();
  if(bytes>kNativeMaxReadBytes)fail(Code::limit,"native read request budget");
  if(state.indexed) {
    auto* ready=dynamic_cast<detail::NativeLz4ReadyReads*>(route);
    return state.read_indexed([&](const auto& view) {
      return view.read(view.inode(inode),offset,bytes,&state.shared_base_reads,route);
    },ready?ready->query_metadata():nullptr);
  }
  return state.read_checked_inode(inode,[&](const auto& snapshot,const Node& n) {
    return read_node(snapshot,state.model,n,offset,bytes);
  });
}
bool NativeFilesystemWriter::supports_leased_lz4_capture() const noexcept {
  if(poisoned() || !impl_->indexed)return false;
  const auto* image=impl_->objects.paged_if();
  return image && image->concurrent_reads_supported();
}
std::unique_ptr<detail::NativeLz4CaptureLease> NativeFilesystemWriter::lease_native_lz4_capture(
    std::uint64_t inode,std::uint64_t offset,std::size_t maximum_encoded) const {
  if(!supports_leased_lz4_capture() || !maximum_encoded || !store_lz4_available())return {};
  auto snapshot=impl_->objects.paged_if()->snapshot_for_capture();
  if(!snapshot)return {};
  return std::unique_ptr<detail::NativeLz4CaptureLease>(new detail::NativeLz4CaptureLease(
      std::move(*snapshot),*impl_->indexed,inode,offset,maximum_encoded));
}
std::unique_ptr<detail::NativeWriteCaptureLease> NativeFilesystemWriter::lease_committed_write_capture() const {
  if(!supports_leased_lz4_capture())return {};
  auto snapshot=impl_->objects.paged_if()->snapshot_for_capture();
  if(!snapshot)return {};
  return std::unique_ptr<detail::NativeWriteCaptureLease>(new detail::NativeWriteCaptureLease(
      std::move(*snapshot),*impl_->indexed));
}
bool detail::NativeLz4CaptureLease::capture(NativeLz4CapturedFrame& out) const {
  out={};
  try {
    const std::function<bool()> no_interrupt;
    const StoreCaptureReadScope capture(no_interrupt);
    const auto query=snapshot_.scoped_query();
    const auto view=verify_indexed_root(snapshot_,root_);
    return view.capture_native_lz4(view.inode(inode_),offset_,maximum_encoded_,out);
  }catch(const std::exception&){return false;}
}
std::vector<detail::NativeCapturedWrite> detail::NativeWriteCaptureLease::read(
    std::span<const NativeCommittedWriteRange> sources,const std::function<bool(std::size_t)>& admit,
    const std::function<bool()>& interrupted) const {
  if(sources.empty())return {};
  try {
    const StoreCaptureReadScope capture(interrupted);
    if(capture.interrupted())return {};
    const auto query=snapshot_.scoped_query();
    const auto view=verify_indexed_root(snapshot_,root_);
    const auto selected_inode=[&](std::uint64_t id)->std::optional<IndexedInodeInput> {
      try{return view.inode(id);}
      catch(const NativeFsError& error) {
        if(error.code()!=NativeFsErrorCode::not_found)throw;
        return {};
      }
    };
    std::vector<NativeCapturedWrite> result;
    result.reserve(sources.size());
    for(std::size_t index=0;index<sources.size();++index) {
      const auto& source=sources[index];
      if(capture.interrupted() || !source.sequence || !source.inode || !source.revision || !source.bytes ||
          source.bytes>kStoreMaxTransactionBytes || source.bytes%kNativeExtentBytes ||
          source.offset%kNativeExtentBytes || source.offset>UINT64_MAX-source.bytes)return {};
      const auto inode=selected_inode(source.inode);
      if(!inode || inode->directory || inode->lifetime!=IndexedLifetime::linked ||
          inode->modified!=source.revision || source.offset+source.bytes>inode->size) {
        if(result.empty())return {};
        continue;
      }
      // Budget only a revision the selected backing can still supply.
      if(!admit(index)) {
        if(result.empty())return {};
        continue;
      }
      NativeCapturedWrite item{source.sequence,{}};
      item.bytes.reserve(source.bytes);
      for(std::uint32_t at=0;at<source.bytes;at+=kNativeExtentBytes) {
        if(capture.interrupted())return {};
        const auto block=view.read(*inode,source.offset+at,kNativeExtentBytes);
        if(block.size()!=kNativeExtentBytes)return {};
        item.bytes.insert(item.bytes.end(),block.begin(),block.end());
      }
      result.push_back(std::move(item));
    }
    if(!capture.interrupted()) {
      std::reverse(result.begin(),result.end());
      return result;
    }
  }catch(const std::exception&) {}
  return {};
}
std::vector<detail::NativeLz4CapturedFrame> detail::NativeLz4CaptureLease::capture_range(
    std::uint32_t bytes,std::size_t maximum_frame_bytes,const std::function<bool()>& interrupted,
    std::shared_ptr<PagedReadMetadata>* metadata) const {
  if(metadata)metadata->reset();
  if(!bytes || bytes>kNativeMaxReadBytes || offset_%kNativeExtentBytes || !maximum_frame_bytes)return {};
  try {
    const StoreCaptureReadScope capture(interrupted);
    if(capture.interrupted())return {};
    const auto query=snapshot_.scoped_query();
    const auto view=verify_indexed_root(snapshot_,root_);
    auto frames=view.capture_native_lz4_range(view.inode(inode_),offset_,bytes,maximum_encoded_,
        maximum_frame_bytes,interrupted);
    if(metadata && !frames.empty())*metadata=snapshot_.take_query_metadata();
    return frames;
  }catch(const std::exception&){return {};}
}
bool NativeFilesystemWriter::capture_native_lz4(std::uint64_t inode,std::uint64_t offset,
    std::size_t maximum_encoded,detail::NativeLz4CapturedFrame& out) const {
  out={};
  if(poisoned() || !impl_->indexed || !maximum_encoded || !store_lz4_available())return false;
  // A refused speculative query leaves the ordinary demand path to apply
  // its recovery and fencing rules using the current checkpoint.
  try {
    return impl_->inspect_indexed([&](const auto& view) {
      return view.capture_native_lz4(view.inode(inode),offset,maximum_encoded,out);
    });
  }catch(const std::exception&){return false;}
}
NativeFsCommit NativeFilesystemWriter::flush() {
  if(poisoned())fail(Code::storage_failure,"native writer is terminal");
  bool completed=false;
  try {
    NativeFsCommit result;
    if(impl_->indexed) {
      result.sequence=impl_->indexed->sequence;
      const auto verify=[&](const PagedObjectStoreSnapshot& snapshot) {
        const auto query=snapshot.scoped_query();
        (void)impl_->checked_indexed_view(snapshot);
      };
      try {
        result.storage=detail::native_paged_info(impl_->objects.paged_if()->flush_checkpoint_verified(verify));
      }catch(...) {
        impl_->shared_base_reads.clear();
        throw;
      }
      return result;
    }
    (void)checked_namespace();
    result.sequence=impl_->model.sequence;
    result.storage=impl_->objects.flush_checkpoint();completed=true;
    (void)checked_namespace();
    return result;
  }catch(const std::bad_alloc&) {
    if(completed || impl_->objects.poisoned()) {
      impl_->poisoned=true;fail(Code::storage_failure,"native flush completion could not be verified");
    }
    fail(Code::limit,"native flush preflight allocation failure");
  }catch(const NativeFsError& error) {
    if(completed || impl_->objects.poisoned() || error.code()==Code::corrupt ||
        error.code()==Code::storage_failure) {
      impl_->poisoned=true;fail(Code::storage_failure,"native flush state verification failed");
    }
    throw;
  }catch(const std::exception&) {
    impl_->poisoned=true;fail(Code::storage_failure,"native backend flush failed; recover independently");
  }catch(...) {
    impl_->poisoned=true;fail(Code::storage_failure,"unknown native backend flush failure");
  }
}
NativeFilesystemSnapshot NativeFilesystemWriter::snapshot() const {
  if(!impl_)fail(Code::invalid_operation,"moved-from native writer");
  try{return NativeFilesystemSnapshot::open_storage(impl_->objects.snapshot());}
  catch(const std::bad_alloc&){fail(Code::limit,"native snapshot allocation failure");}
  catch(const NativeFsError& e){
    if(e.code()==Code::corrupt || e.code()==Code::storage_failure)impl_->poisoned=true;
    throw;
  }
  catch(const std::exception&){
    impl_->poisoned=true;fail(Code::storage_failure,"native snapshot backing verification failed");
  }
}
NativeWorkloadInfo NativeFilesystemWriter::workload_info() const noexcept {
  if(poisoned())return {};
  if(impl_->indexed)return impl_->indexed->workload;
  if(impl_->model.format<6)return {};
  return {true,impl_->model.committed_input_bytes};
}
bool NativeFilesystemWriter::poisoned() const noexcept{return !impl_ || impl_->poisoned || impl_->objects.poisoned();}
}
