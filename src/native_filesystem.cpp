#include "flitzfs/native_filesystem.hpp"
#include "flitzfs/catalog.hpp"
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
constexpr std::size_t kMetadataCap=kNativeExtentBytes,kDirtyCap=kNativeMaxDirtyExtents;
constexpr std::size_t kBasicNodeHeader=88,kSecurityNodeHeader=96,kAllocationNodeHeader=104;
static_assert(64+kNativeMaxNodes*(kBasicNodeHeader+100)+16*kNativeMaxExtents<=kMetadataCap);
static_assert(64+kNativeMaxNodes*(kSecurityNodeHeader+100)+16*kNativeMaxExtents<=kMetadataCap);
static_assert(64+kNativeMaxNodes*(kAllocationNodeHeader+100)+16*kNativeMaxExtents<=kMetadataCap);
static_assert(kDirtyCap*kNativeExtentBytes+kMetadataCap<=kStoreMaxTransactionBytes);
static_assert(kNativeMaxExtents*kNativeExtentBytes+kMetadataCap<=kStoreMaxLogicalBytes);
constexpr std::array<std::byte,8> kMagic{std::byte{'F'},std::byte{'L'},std::byte{'T'},std::byte{'X'},std::byte{'N'},std::byte{'S'},std::byte{'0'},std::byte{'1'}};
constexpr auto kLifecycleMagic=[] {auto magic=kMagic;magic[7]=std::byte{'2'};return magic;}();
constexpr auto kBasicMagic=[] {auto magic=kMagic;magic[7]=std::byte{'3'};return magic;}();
constexpr auto kSecurityMagic=[] {auto magic=kMagic;magic[7]=std::byte{'4'};return magic;}();
constexpr auto kAllocationMagic=[] {auto magic=kMagic;magic[7]=std::byte{'5'};return magic;}();
constexpr auto kWorkloadMagic=[] {auto magic=kMagic;magic[7]=std::byte{'6'};return magic;}();
const std::array<std::byte,8>& namespace_magic(std::uint32_t format) {
  return format==1?kMagic:format==2?kLifecycleMagic:format==3?kBasicMagic:format==4?kSecurityMagic:format==5?kAllocationMagic:kWorkloadMagic;
}
std::size_t node_header(std::uint32_t format) {return format>=5?kAllocationNodeHeader:format==4?kSecurityNodeHeader:format==3?kBasicNodeHeader:64;}
[[noreturn]] void fail(Code code,const char* reason){throw NativeFsError(code,reason);}
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
};
struct Model {
  StoreIdentity identity{};
  std::uint64_t sequence=1,next_inode=2,committed_input_bytes=0;
  std::uint32_t format=1;
  std::map<std::uint64_t,Node> nodes;
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
  for(const auto& [id,n]:model.nodes)if(id!=1 && n.parent==parent && n.key==key)return &n;
  return nullptr;
}
std::uint64_t resolve(const Model& model,std::string_view path) {
  const auto key=canonical(path,true);
  std::uint64_t current=1;
  for(std::size_t start=0;start<key.size();) {
    const auto& parent=node(model,current);if(!parent.directory)fail(Code::not_directory,"native parent is a file");
    const auto slash=key.find('/',start),end=slash==std::string::npos?key.size():slash;
    const auto* found=child(model,current,std::string_view(key).substr(start,end-start));
    if(!found)fail(Code::not_found,"native path not found");
    current=found->inode;start=end+1;
  }
  return current;
}
std::pair<std::uint64_t,std::string> destination(const Model& model,std::string_view path) {
  (void)canonical(path);
  const auto slash=path.rfind('/');
  const auto parent=resolve(model,slash==std::string_view::npos?std::string_view{}:path.substr(0,slash));
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
  if(model.format<1 || model.format>6 || (model.format<6 && model.committed_input_bytes))
    fail(Code::corrupt,"native namespace capability/accounting");
  const auto& root=node(model,1);
  if(root.parent || !root.directory || !root.name.empty() || root.size || !root.extents.empty() ||
      root.born!=1 || root.delete_pending || root.unlinked)
    fail(Code::corrupt,"native root invariant");
  std::set<std::pair<std::uint64_t,std::string>> names;
  std::set<std::uint64_t> objects;
  std::uint64_t largest=0;
  for(const auto& [id,n]:model.nodes) {
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
      }
    }
    for(const auto& [block,object]:n.extents) {
      if(block>=kNativeMaxFileBytes/kNativeExtentBytes || block*kNativeExtentBytes>=n.size ||
          (!pending && (object<2 || object>kStoreMaxObjects)) ||
          (object && (!objects.insert(object).second || object<2 || object>kStoreMaxObjects)))
        fail(Code::corrupt,"native extent ownership/bounds");
    }
  }
  if(model.next_inode<=largest || model.next_inode<2)fail(Code::corrupt,"native inode allocator");
  if(!pending && extent_count(model)>kNativeMaxExtents)fail(Code::limit,"native live extent budget");
  if(allocation_usage(model).charged_extents>kNativeMaxExtents)
    fail(Code::limit,"native reserved/live extent budget");
}
Bytes object_bytes(const ObjectStoreSnapshot& objects,std::uint64_t id) {
  try{return objects.read(id);}
  catch(const std::bad_alloc&){throw;}
  catch(const std::exception&){fail(Code::storage_failure,"native backing object verification failed");}
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
Bytes encode(const Model& model) {
  validate_structure(model);
  std::size_t size=64;
  for(const auto& [id,n]:model.nodes){(void)id;size+=node_header(model.format)+n.name.size()+16*n.extents.size()+(n.security?n.security->size():0);}
  if(size>kMetadataCap)fail(Code::limit,"native metadata budget");
  Bytes bytes(size);const auto& magic=namespace_magic(model.format);
  std::copy(magic.begin(),magic.end(),bytes.begin());
  put(bytes,8,model.format,4);put(bytes,12,model.nodes.size(),4);put(bytes,16,model.sequence,8);
  put(bytes,24,model.next_inode,8);put(bytes,32,kNativeExtentBytes,4);put(bytes,36,extent_count(model),4);
  std::copy(model.identity.begin(),model.identity.end(),bytes.begin()+40);
  if(model.format>=6)put(bytes,56,model.committed_input_bytes,8);
  std::size_t cursor=64;
  for(const auto& [id,n]:model.nodes) {
    put(bytes,cursor,id,8);put(bytes,cursor+8,n.parent,8);put(bytes,cursor+16,n.directory?1:2,4);
    put(bytes,cursor+20,(n.delete_pending?1U:0U)|(n.unlinked?2U:0U)|(n.basic?4U:0U)|(n.security?8U:0U)|(n.allocation?16U:0U),4);
    put(bytes,cursor+24,n.size,8);put(bytes,cursor+32,n.born,8);put(bytes,cursor+40,n.modified,8);
    put(bytes,cursor+48,n.name.size(),2);put(bytes,cursor+50,n.extents.size(),2);
    if(n.basic) {
      put(bytes,cursor+52,n.basic->attributes,4);put(bytes,cursor+56,n.basic->creation_time,8);
      put(bytes,cursor+64,n.basic->last_access_time,8);put(bytes,cursor+72,n.basic->last_write_time,8);
      put(bytes,cursor+80,n.basic->change_time,8);
    }
    if(model.format>=4)put(bytes,cursor+88,n.security?n.security->size():0,4);
    if(model.format>=5)put(bytes,cursor+96,n.allocation.value_or(0),8);
    cursor+=node_header(model.format);
    for(char c:n.name)bytes[cursor++]=static_cast<std::byte>(static_cast<unsigned char>(c));
    if(n.security) {
      std::copy(n.security->begin(),n.security->end(),bytes.begin()+static_cast<std::ptrdiff_t>(cursor));
      cursor+=n.security->size();
    }
    for(const auto& [block,object]:n.extents){put(bytes,cursor,block,8);put(bytes,cursor+8,object,8);cursor+=16;}
  }
  return bytes;
}
Model decode(const ObjectStoreSnapshot& objects) {
  const auto bytes=object_bytes(objects,kNamespaceObject);
  const auto all=std::span<const std::byte>(bytes);
  if(bytes.size()<64 || bytes.size()>kMetadataCap ||
      integer(all,32,4)!=kNativeExtentBytes)
    fail(Code::corrupt,"unsupported native namespace framing");
  const auto format=integer(all,8,4);
  if(format<1 || format>6 || (format<6 && !zeros(all.subspan(56,8))) ||
      !std::equal(namespace_magic(static_cast<std::uint32_t>(format)).begin(),
                  namespace_magic(static_cast<std::uint32_t>(format)).end(),bytes.begin()))
    fail(Code::corrupt,"unsupported native namespace capability/magic");
  const auto count=integer(all,12,4),declared_extents=integer(all,36,4);
  if(!count || count>kNativeMaxNodes || declared_extents>kNativeMaxExtents)fail(Code::limit,"native metadata counts");
  if(format>=2 && !objects.info().replicated_checkpoint)
    fail(Code::corrupt,"native lifetime capability requires replicated current checkpoints");
  Model model;model.format=static_cast<std::uint32_t>(format);
  if(format>=6)model.committed_input_bytes=integer(all,56,8);
  model.sequence=integer(all,16,8);model.next_inode=integer(all,24,8);
  std::copy_n(bytes.begin()+40,16,model.identity.begin());
  if(model.identity!=objects.identity() || model.sequence>objects.info().generation)fail(Code::corrupt,"native identity/sequence binding");
  std::size_t cursor=64;std::uint64_t previous=0;
  for(std::uint64_t i=0;i<count;++i) {
    const auto header=node_header(model.format);
    if(cursor>bytes.size() || bytes.size()-cursor<header)fail(Code::corrupt,"truncated native inode");
    const auto h=all.subspan(cursor,header);cursor+=header;
    Node n;n.inode=integer(h,0,8);n.parent=integer(h,8,8);const auto kind=integer(h,16,4);
    const auto flags=integer(h,20,4);
    n.directory=kind==1;n.delete_pending=(flags&1)!=0;n.unlinked=(flags&2)!=0;
    n.size=integer(h,24,8);n.born=integer(h,32,8);n.modified=integer(h,40,8);
    const auto name_bytes=integer(h,48,2),extents=integer(h,50,2);
    if((kind!=1 && kind!=2) || n.inode<=previous || name_bytes>100 || extents>kNativeMaxExtents ||
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
          !native_security_descriptor_framed(all.subspan(cursor,static_cast<std::size_t>(length))))
        fail(Code::corrupt,"invalid native stored security descriptor");
      n.security=NativeSecurityDescriptor(bytes.begin()+static_cast<std::ptrdiff_t>(cursor),
          bytes.begin()+static_cast<std::ptrdiff_t>(cursor+length));cursor+=static_cast<std::size_t>(length);
    }
    try{n.key=n.inode==1 || n.unlinked?"":component(n.name);}
    catch(const NativeFsError&){fail(Code::corrupt,"invalid native stored component");}
    std::uint64_t prior_block=0;
    for(std::uint64_t e=0;e<extents;++e) {
      if(bytes.size()-cursor<16)fail(Code::corrupt,"truncated native extent");
      const auto block=integer(all,cursor,8),object=integer(all,cursor+8,8);cursor+=16;
      if(e && block<=prior_block)fail(Code::corrupt,"native extent ordering");
      n.extents.emplace(block,object);prior_block=block;
    }
    previous=n.inode;model.nodes.emplace(n.inode,std::move(n));
  }
  if(cursor!=bytes.size() || extent_count(model)!=declared_extents)fail(Code::corrupt,"native metadata trailing/count mismatch");
  try{validate_structure(model);}
  catch(const NativeFsError&){fail(Code::corrupt,"invalid native namespace graph");}
  std::set<std::uint64_t> expected{kNamespaceObject};
  for(const auto& [id,n]:model.nodes){(void)id;for(const auto& [block,object]:n.extents){(void)block;expected.insert(object);}}
  const auto catalog=objects.list();
  if(catalog.size()!=expected.size())fail(Code::corrupt,"foreign or dangling native objects");
  for(const auto& object:catalog)if(!expected.contains(object.id) ||
      (object.id!=kNamespaceObject && object.logical_bytes!=kNativeExtentBytes))
    fail(Code::corrupt,"native object extent mismatch");
  validate_data(model,[&](auto,auto,std::uint64_t object){return object_bytes(objects,object);});
  return model;
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
  Model model;const Model& original;const ObjectStoreSnapshot& base;Dirty dirty;
  bool changed=false;std::vector<std::uint64_t> created;
  Stage(const Model& previous,const ObjectStoreSnapshot& objects):model(previous),original(previous),base(objects) {
    if(model.sequence==(std::numeric_limits<std::uint64_t>::max)())fail(Code::limit,"native sequence exhausted");
    ++model.sequence;created.reserve(16);
  }
  DirtyExtent& block(Node& n,std::uint64_t index) {
    const auto key=std::pair{n.inode,index};
    if(auto found=dirty.find(key);found!=dirty.end())return found->second;
    if(dirty.size()>=kDirtyCap)fail(Code::limit,"native staging extent budget");
    const auto existing=n.extents.find(index);
    auto bytes=existing==n.extents.end()?Bytes(kNativeExtentBytes):object_bytes(base,existing->second);
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
    else n.extents.try_emplace(index,0);
  }
  void discard(Node& n) {
    if(n.directory)for(const auto& [id,other]:model.nodes)if(id!=n.inode && other.parent==n.inode)fail(Code::not_empty,"native directory not empty");
    for(auto it=dirty.begin();it!=dirty.end();)it=it->first.first==n.inode?dirty.erase(it):std::next(it);
    const auto parent=n.parent,id=n.inode;
    if(parent)node(model,parent).modified=model.sequence;
    model.nodes.erase(id);changed=true;
  }
  void set_delete(std::uint64_t id,bool requested) {
    auto& n=node(model,selected_inode(model,id).inode);
    if(id==1 || n.unlinked)fail(Code::invalid_operation,"cannot change delete intent on root/detached inode");
    if(n.delete_pending==requested)return;
    if(requested && n.directory)
      for(const auto& [other_id,other]:model.nodes)
        if(other_id!=id && other.parent==id)fail(Code::not_empty,"delete-pending directory is not empty");
    model.format=(std::max)(model.format,2U);n.delete_pending=requested;n.modified=model.sequence;changed=true;
  }
  void unlink(std::uint64_t id) {
    auto& n=node(model,selected_inode(model,id).inode);
    if(n.unlinked)return; // Repeated cleanup never discards retained payloads.
    if(id==1 || !n.delete_pending)fail(Code::invalid_operation,"native unlink needs persistent delete intent");
    if(n.directory)for(const auto& [other_id,other]:model.nodes)
      if(other_id!=id && other.parent==id)fail(Code::not_empty,"native unlink directory is not empty");
    const auto parent=n.parent;
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
    const bool times=effect.set_access_time || effect.set_write_time || effect.set_change_time;
    const bool archive=effect.set_archive && !n.directory;
    if(times || archive) {
      if(!n.basic)fail(Code::invalid_operation,"automatic metadata needs existing basic metadata");
      auto value=*n.basic;
      if(archive)value.attributes|=0x20U;
      if(effect.set_access_time)value.last_access_time=effect.utc_time;
      if(effect.set_write_time)value.last_write_time=effect.utc_time;
      if(effect.set_change_time)value.change_time=effect.utc_time;
      set_basic(NativeBasicMutation{id,0,value});
    }
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
    if(n.directory)for(const auto& [other_id,other]:model.nodes)
      if(other_id!=id && other.parent==id){eligible=false;break;}
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
      created.push_back(n.inode);model.nodes.emplace(n.inode,std::move(n));
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
          extent.bytes==object_bytes(base,extent.original_object)) {
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
    validate_structure(model);
    std::vector<StoreMutation> output;output.reserve(kStoreMaxObjects);
    output.push_back({kNamespaceObject,false,encode(model)});
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
    for(const auto& previous:base.list())if(previous.id!=kNamespaceObject && !used.contains(previous.id))output.push_back({previous.id,true,{}});
    if(output.size()>kStoreMaxObjects)fail(Code::limit,"native object mutation budget");
    return output;
  }
};
}

struct NativeFilesystemSnapshot::Impl {
  ObjectStoreSnapshot objects;Model model;
  explicit Impl(ObjectStoreSnapshot snapshot):objects(std::move(snapshot)),model(decode(objects)){}
};
struct NativeFilesystemWriter::Impl {
  ObjectStoreWriter objects;Model model;bool poisoned=false;
  Impl(ObjectStoreWriter writer,Model state):objects(std::move(writer)),model(std::move(state)){}
};
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
  return NativeFilesystemSnapshot(std::make_unique<Impl>(std::move(objects)));
}
StoreIdentity NativeFilesystemSnapshot::identity() const noexcept{return impl_?impl_->model.identity:StoreIdentity{};}
StoreCommitInfo NativeFilesystemSnapshot::storage_info() const noexcept{return impl_?impl_->objects.info():StoreCommitInfo{};}
std::uint64_t NativeFilesystemSnapshot::sequence() const noexcept{return impl_?impl_->model.sequence:0;}
NativeWorkloadInfo NativeFilesystemSnapshot::workload_info() const noexcept {
  if(!impl_||impl_->model.format<6)return {};
  return {true,impl_->model.committed_input_bytes};
}
NativeFileInfo NativeFilesystemSnapshot::stat(std::string_view path) const {
  if(!impl_)fail(Code::invalid_operation,"moved-from native snapshot");
  return file_info(impl_->model,node(impl_->model,resolve(impl_->model,path)));
}
namespace {
std::vector<NativeFileInfo> list_node(const Model& model,const Node& directory) {
  if(!directory.directory)fail(Code::not_directory,"native enumeration of file");
  std::vector<NativeFileInfo> result;
  std::vector<const Node*> children;
  for(const auto& [inode,n]:model.nodes)if(inode!=1 && n.parent==directory.inode)children.push_back(&n);
  std::sort(children.begin(),children.end(),[](auto a,auto b){return a->key<b->key;});
  result.reserve(children.size());for(const auto* n:children)result.push_back(file_info(model,*n));
  return result;
}
Bytes read_node(const ObjectStoreSnapshot& objects,const Node& n,std::uint64_t offset,std::uint32_t bytes) {
  if(n.directory)fail(Code::is_directory,"cannot read native directory");
  if(offset>=n.size || !bytes)return {};
  const auto count=(std::min)(std::uint64_t(bytes),n.size-offset),end=offset+count;
  Bytes output(static_cast<std::size_t>(count));
  for(auto it=n.extents.lower_bound(offset/kNativeExtentBytes);it!=n.extents.end();++it) {
    const auto begin=it->first*kNativeExtentBytes;if(begin>=end)break;
    const auto data=object_bytes(objects,it->second);
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
  return file_info(impl_->model,selected_inode(impl_->model,inode));
}
NativeInodeState NativeFilesystemSnapshot::inode_state(std::uint64_t inode) const {
  if(!impl_)fail(Code::invalid_operation,"moved-from native snapshot");
  const auto& n=selected_inode(impl_->model,inode);
  return {file_info(impl_->model,n),n.delete_pending,n.unlinked};
}
std::optional<NativeBasicMetadata> NativeFilesystemSnapshot::basic_metadata(std::uint64_t inode) const {
  if(!impl_)fail(Code::invalid_operation,"moved-from native snapshot");
  return selected_inode(impl_->model,inode).basic;
}
std::optional<NativeSecurityDescriptor> NativeFilesystemSnapshot::security_descriptor(std::uint64_t inode) const {
  if(!impl_)fail(Code::invalid_operation,"moved-from native snapshot");
  return selected_inode(impl_->model,inode).security;
}
NativeInodeAllocationInfo NativeFilesystemSnapshot::allocation_info(std::uint64_t inode) const {
  if(!impl_)fail(Code::invalid_operation,"moved-from native snapshot");
  const auto& n=selected_inode(impl_->model,inode);return {commit_info(n),n.allocation};
}
NativeAllocationUsage NativeFilesystemSnapshot::allocation_usage() const {
  if(!impl_)fail(Code::invalid_operation,"moved-from native snapshot");
  return flitzfs::allocation_usage(impl_->model);
}
bool NativeFilesystemSnapshot::needs_session_recovery() const noexcept {
  if(!impl_)return false;
  for(const auto& [id,n]:impl_->model.nodes){(void)id;if(n.delete_pending || n.unlinked)return true;}
  return false;
}
std::vector<NativeFileInfo> NativeFilesystemSnapshot::list(std::string_view directory) const {
  if(!impl_)fail(Code::invalid_operation,"moved-from native snapshot");
  return list_node(impl_->model,node(impl_->model,resolve(impl_->model,directory)));
}
std::vector<NativeFileInfo> NativeFilesystemSnapshot::list_inode(std::uint64_t directory) const {
  if(!impl_)fail(Code::invalid_operation,"moved-from native snapshot");
  return list_node(impl_->model,selected_inode(impl_->model,directory));
}
Bytes NativeFilesystemSnapshot::read(std::string_view path,std::uint64_t offset,std::uint32_t bytes) const {
  if(!impl_)fail(Code::invalid_operation,"moved-from native snapshot");
  if(bytes>kNativeMaxReadBytes)fail(Code::limit,"native read request budget");
  return read_node(impl_->objects,node(impl_->model,resolve(impl_->model,path)),offset,bytes);
}
Bytes NativeFilesystemSnapshot::read_inode(std::uint64_t inode,std::uint64_t offset,std::uint32_t bytes) const {
  if(!impl_)fail(Code::invalid_operation,"moved-from native snapshot");
  if(bytes>kNativeMaxReadBytes)fail(Code::limit,"native read request budget");
  return read_node(impl_->objects,selected_inode(impl_->model,inode),offset,bytes);
}
void NativeFilesystemSnapshot::verify_all() const {
  if(!impl_)fail(Code::invalid_operation,"moved-from native snapshot");
  impl_->objects.verify_all();(void)decode(impl_->objects);
}
NativeFilesystemWriter NativeFilesystemWriter::initialize(ObjectStoreWriter objects) {
  auto snapshot=objects.snapshot();
  if(snapshot.info().generation!=0 || !snapshot.list().empty())fail(Code::invalid_operation,"native initialization requires new empty store");
  Model model;model.identity=snapshot.identity();
  Node root;root.inode=1;root.directory=true;root.born=root.modified=1;
  model.nodes.emplace(1,std::move(root));
  auto p=std::make_unique<Impl>(std::move(objects),model);
  const StoreMutation metadata{kNamespaceObject,false,encode(model)};
  p->objects.commit_replicated({&metadata,1});
  return NativeFilesystemWriter(std::move(p));
}
NativeFsCommit NativeFilesystemWriter::enable_workload_accounting() {
  return transact({},InodeAction::enable_workload,0);
}
NativeFilesystemWriter NativeFilesystemWriter::resume(ObjectStoreWriter objects) {
  return resume_impl(std::move(objects),false);
}
NativeFilesystemWriter NativeFilesystemWriter::resume_closed_session(ObjectStoreWriter objects) {
  return resume_impl(std::move(objects),true);
}
NativeFilesystemWriter NativeFilesystemWriter::resume_impl(ObjectStoreWriter objects,bool closed_session) {
  if(objects.poisoned())fail(Code::invalid_operation,"native admission requires a healthy recovered writer");
  try {
    auto snapshot=objects.snapshot();auto model=decode(snapshot);bool needs=false;
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

const ObjectStoreSnapshot& NativeFilesystemSnapshot::objects_for_volume_head() const {
  if(!impl_)fail(Code::invalid_operation,"moved-from volume image");return impl_->objects;
}
void NativeFilesystemSnapshot::verify_compacted_equivalent_for_volume_head(
    const NativeFilesystemSnapshot& target) const {
  if(!impl_||!target.impl_)fail(Code::invalid_operation,"moved-from handoff image");
  verify_all();target.verify_all();
  if(needs_session_recovery()||target.needs_session_recovery())
    fail(Code::invalid_operation,"handoff requires retired resolved file contexts");
  auto expected=decode(impl_->objects);expected.identity=target.identity();
  if(encode(expected)!=encode(decode(target.impl_->objects)))
    fail(Code::invalid_operation,"compacted namespace differs");
  const auto left=impl_->objects.list(),right=target.impl_->objects.list();
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
NativeFilesystemWriter NativeFilesystemWriter::copy_image(const NativeFilesystemSnapshot& source,
    std::shared_ptr<StoreByteIo> target,StoreIdentity id,StoreCompressionOptions compression,
    StoreCheckpointOptions checkpoints,bool reencode) {
  if(!source.impl_)fail(Code::invalid_operation,"moved-from native compaction source");
  if(source.needs_session_recovery())
    fail(Code::invalid_operation,"native compaction needs completed file-context recovery");
  if(id==source.identity() || id==StoreIdentity{} || !target)
    fail(Code::invalid_operation,"native compaction requires a new target identity");
  try {
    auto model=decode(source.impl_->objects);model.identity=id;
    const auto metadata=encode(model);
    return resume(ObjectStoreWriter::compact_replacing(source.impl_->objects,
        std::move(target),id,kNamespaceObject,metadata,compression,checkpoints,reencode));
  }catch(const std::bad_alloc&){fail(Code::limit,"native compaction allocation failure");}
  catch(const NativeFsError&){throw;}
  catch(const std::exception&){fail(Code::storage_failure,"native compaction failed; retain source and partial target");}
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

bool NativeFilesystemWriter::matches_recompression(const Recompression& candidate) const noexcept {
  return !poisoned() && candidate.matches(impl_->objects);
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
  if(operations.size()>16 || basic.size()>16 || security.size()>16 ||
      operations.size()+basic.size()+security.size()==0 ||
      operations.size()+basic.size()+security.size()>16)fail(Code::limit,"native combined operation count");
  for(const auto& op:security) {
    if((!op.inode)==(!op.created_ordinal) || op.created_ordinal>16 ||
        !native_security_descriptor_framed(op.value))
      fail(Code::invalid_operation,"invalid native security descriptor/selector");
  }
  for(const auto& op:basic) {
    if((!op.inode)==(!op.created_ordinal) || op.created_ordinal>16)
      fail(Code::invalid_operation,"basic metadata requires exactly one valid selector");
    // Reject request sentinels/unsupported bits before backing I/O. Node-kind
    // consistency is checked against the staged selected inode.
    validate_basic(op.value,bool(op.value.attributes&0x10U));
  }
  std::uint64_t input=0;
  for(const auto& op:operations) {
    if(static_cast<int>(op.operation)<0 || static_cast<int>(op.operation)>static_cast<int>(NativeFileOperation::remove))
      fail(Code::invalid_operation,"unknown native operation");
    if(op.data.size()>kStoreMaxObjectBytes)fail(Code::limit,"native write size budget");
    input+=op.data.size();if(input>kStoreMaxTransactionBytes)fail(Code::limit,"native input budget");
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
  if(reservation && (directory || *reservation%kNativeAllocationUnit || *reservation>kNativeAllocationCapacity))
    fail(Code::invalid_operation,"invalid native creation reservation");
  validate_basic(value,directory);
  if(!native_security_descriptor_framed(descriptor))
    fail(Code::invalid_operation,"invalid native creation security descriptor");
  try {
    (void)canonical(path); // Bound/validate BEFORE copying caller-controlled bytes.
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
    try{result.storage=impl_->objects.commit_replicated(changes);}
    catch(const std::exception&){
      if(impl_->objects.poisoned())impl_->poisoned=true;
      fail(Code::storage_failure,"native transaction storage commit failed");
    }
    impl_->model=std::move(stage.model);
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
      (reservation && (*reservation%kNativeAllocationUnit || *reservation>kNativeAllocationCapacity)))
    fail(Code::invalid_operation,"invalid canonical native overwrite metadata/reservation");
  const auto& n=selected_inode(impl_->model,inode);
  if(n.directory)fail(Code::is_directory,"cannot overwrite native directory");
  if(n.delete_pending || n.unlinked || !n.basic)
    fail(Code::invalid_operation,"overwrite needs a live regular inode with basic metadata");
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
  if(bytes%kNativeAllocationUnit || bytes>kNativeAllocationCapacity || !native_cleanup_metadata_valid(effect))
    fail(Code::invalid_operation,"invalid native reservation input");
  NativeInodeAllocationCommit result;
  result.transaction=transact({},InodeAction::reserve,inode,{},effect.any()?&effect:nullptr,
      false,nullptr,{},nullptr,&bytes,&result.state);
  return result;
}
NativeInodeAllocationInfo NativeFilesystemWriter::inspect_allocation(std::uint64_t inode) const {
  auto objects=checked_namespace();(void)objects;
  const auto& n=selected_inode(impl_->model,inode);return {commit_info(n),n.allocation};
}
NativeAllocationUsage NativeFilesystemWriter::inspect_allocation_usage() const {
  auto objects=checked_namespace();(void)objects;return allocation_usage(impl_->model);
}
NativeFsCommit NativeFilesystemWriter::retire_inode(std::uint64_t inode) {
  return transact({},InodeAction::retire,inode);
}
ObjectStoreSnapshot NativeFilesystemWriter::checked_namespace() const {
  if(poisoned())fail(Code::storage_failure,"native writer is terminal");
  try {
    auto snapshot=impl_->objects.snapshot_for_staging();
    if(object_bytes(snapshot,kNamespaceObject)!=encode(impl_->model))
      fail(Code::corrupt,"native cached namespace differs from backing record");
    return snapshot;
  }catch(const std::bad_alloc&){fail(Code::limit,"native namespace query allocation failure");}
  catch(const NativeFsError& error) {
    if(error.code()==Code::corrupt || error.code()==Code::storage_failure)impl_->poisoned=true;
    throw;
  }catch(const std::exception&){impl_->poisoned=true;fail(Code::storage_failure,"native namespace query failed");}
}
std::optional<NativeBasicMetadata> NativeFilesystemWriter::inspect_basic_metadata(std::uint64_t inode) const {
  auto objects=checked_namespace();(void)objects;
  return selected_inode(impl_->model,inode).basic;
}
std::optional<NativeSecurityDescriptor> NativeFilesystemWriter::inspect_security_descriptor(std::uint64_t inode) const {
  auto objects=checked_namespace();(void)objects;
  return selected_inode(impl_->model,inode).security;
}
NativeInodeState NativeFilesystemWriter::inspect_path(std::string_view path) const {
  auto objects=checked_namespace();(void)objects;
  const auto& n=node(impl_->model,resolve(impl_->model,path));
  return {file_info(impl_->model,n),n.delete_pending,n.unlinked};
}
NativeInodeState NativeFilesystemWriter::inspect_inode(std::uint64_t inode) const {
  auto objects=checked_namespace();(void)objects;
  const auto& n=selected_inode(impl_->model,inode);
  return {file_info(impl_->model,n),n.delete_pending,n.unlinked};
}
std::vector<NativeFileInfo> NativeFilesystemWriter::inspect_directory(std::uint64_t inode) const {
  auto objects=checked_namespace();(void)objects;
  return list_node(impl_->model,selected_inode(impl_->model,inode));
}
Bytes NativeFilesystemWriter::read_inode(std::uint64_t inode,std::uint64_t offset,std::uint32_t bytes) const {
  if(poisoned())fail(Code::storage_failure,"native writer is terminal");
  if(bytes>kNativeMaxReadBytes)fail(Code::limit,"native read request budget");
  try {
    auto objects=checked_namespace();
    return read_node(objects,selected_inode(impl_->model,inode),offset,bytes);
  }catch(const std::bad_alloc&){fail(Code::limit,"native range read allocation failure");}
  catch(const NativeFsError& error) {
    if(error.code()==Code::corrupt || error.code()==Code::storage_failure)impl_->poisoned=true;
    throw;
  }catch(const std::exception&){impl_->poisoned=true;fail(Code::storage_failure,"native range read failed");}
}
NativeFsCommit NativeFilesystemWriter::flush() {
  if(poisoned())fail(Code::storage_failure,"native writer is terminal");
  bool completed=false;
  try {
    (void)checked_namespace();
    NativeFsCommit result;result.sequence=impl_->model.sequence;
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
  try{return NativeFilesystemSnapshot::open(impl_->objects.snapshot());}
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
  if(poisoned()||impl_->model.format<6)return {};
  return {true,impl_->model.committed_input_bytes};
}
bool NativeFilesystemWriter::poisoned() const noexcept{return !impl_ || impl_->poisoned || impl_->objects.poisoned();}
}
