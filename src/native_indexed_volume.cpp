#include "native_indexed_volume.hpp"
#include "native_object_storage.hpp"
#include "flitzfs/catalog.hpp"
#include <algorithm>
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
constexpr std::uint64_t namespace_object=1,inode_domain=1ULL<<63;
constexpr std::uint64_t ordinal_mask=kNativeIndexedInodeLimit-1;
constexpr std::size_t root_size=96,extent_header=24;
constexpr std::size_t maximum_inode_bytes=104+100+kNativeMaxSecurityBytes;

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
std::optional<Bytes> fetch(const PagedObjectStoreSnapshot& store,std::uint64_t id,std::size_t budget) {
  try{return store.read_if_present(id,budget);}
  catch(const std::bad_alloc&){throw;}
  catch(const std::exception&){fail(Code::storage_failure,"indexed object integrity or read failure");}
}
Bytes required(const PagedObjectStoreSnapshot& store,std::uint64_t id,std::size_t budget) {
  auto bytes=fetch(store,id,budget);
  if(!bytes)fail(Code::corrupt,"indexed object reference missing");
  return std::move(*bytes);
}
std::string name_key(std::string_view path,bool root=false) {
  try{return canonical_catalog_path(path,root);}
  catch(const std::bad_alloc&){throw;}
  catch(const std::exception&){fail(Code::invalid_path,"invalid indexed native path");}
}
void validate_header_fields(const IndexedVolumeRoot& root) {
  if(root.identity==StoreIdentity{} || !root.sequence || root.next_inode<2 ||
      root.next_inode>kNativeIndexedInodeLimit || !root.next_extent ||
      root.next_extent>kNativeIndexedInodeLimit || (!root.workload.enabled && root.workload.committed_input_bytes))
    fail(Code::corrupt,"invalid indexed root identity or allocator");
}
IndexedVolumeRoot decode_root(std::span<const std::byte> bytes) {
  if(bytes.size()!=root_size || !magic(bytes,root_magic) || get(bytes,8,4)!=1 ||
      get(bytes,12,4)>1 || get(bytes,88,4)!=kNativeExtentBytes || get(bytes,92,4))
    fail(Code::corrupt,"unsupported indexed native volume root");
  IndexedVolumeRoot root;
  std::copy_n(bytes.begin()+72,16,root.identity.begin());
  root.sequence=get(bytes,16);root.next_inode=get(bytes,24);root.next_extent=get(bytes,32);
  root.inodes=get(bytes,40);root.extents=get(bytes,48);root.charged_extents=get(bytes,56);
  root.workload={get(bytes,12,4)==1,get(bytes,64)};
  return root;
}
Bytes encode_root(const IndexedVolumeRoot& root) {
  Bytes bytes(root_size);
  set_magic(bytes,root_magic);put(bytes,8,1,4);put(bytes,12,root.workload.enabled,4);
  put(bytes,16,root.sequence);put(bytes,24,root.next_inode);put(bytes,32,root.next_extent);
  put(bytes,40,root.inodes);put(bytes,48,root.extents);put(bytes,56,root.charged_extents);
  put(bytes,64,root.workload.committed_input_bytes);
  std::copy(root.identity.begin(),root.identity.end(),bytes.begin()+72);
  put(bytes,88,kNativeExtentBytes,4);
  return bytes;
}
IndexedInodeInput load_inode(const PagedObjectStoreSnapshot& store,const IndexedVolumeRoot& root,std::uint64_t id) {
  if(!id || id>=root.next_inode)fail(Code::not_found,"indexed inode is outside the namespace");
  const auto bytes=fetch(store,inode_domain|id,maximum_inode_bytes);
  if(!bytes)fail(Code::not_found,"indexed inode does not exist");
  return decode_indexed_inode(*bytes,id,root);
}
std::uint64_t extent_begin(const IndexedInodeInput& inode,std::uint64_t block) {
  if(inode.directory || !inode.size || block>(inode.size-1)/kNativeExtentBytes)
    fail(Code::corrupt,"indexed extent outside its file");
  return block*kNativeExtentBytes;
}
void validate_payload(const IndexedInodeInput& inode,std::uint64_t block,std::span<const std::byte> payload) {
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
void verify_extent(const Bytes& bytes,const IndexedInodeInput& inode,std::uint64_t block) {
  if(bytes.size()<=extent_header || !magic(bytes,extent_magic) || get(bytes,8)!=inode.inode || get(bytes,16)!=block)
    fail(Code::corrupt,"indexed extent owner or logical offset mismatch");
  validate_payload(inode,block,std::span<const std::byte>(bytes).subspan(extent_header));
}
NativeFileInfo file_info(const IndexedInodeInput& inode,std::string path) {
  return {inode.inode,inode.parent,inode.size,inode.born,inode.modified,inode.directory,std::move(path)};
}
std::uint64_t reserved_extents(const IndexedInodeInput& inode) {
  return inode.allocation && *inode.allocation ? 1+(*inode.allocation-1)/kNativeExtentBytes : 0;
}
} // namespace


namespace {

void decrement(std::uint64_t& count) {
  if(!count)fail(Code::corrupt,"indexed volume count underflow");
  --count;
}

class IndexedVolumeTransaction final {
 public:
  IndexedVolumeTransaction(const PagedObjectStoreSnapshot& source,const IndexedVolumeRoot& expected)
      :source_(source),before_(expected),next_(expected),index_(source) {
    if(decode_root(required(source_,namespace_object,root_size))!=before_ ||
        index_.kind()!=NativeIndexKind::names_and_extents ||
        index_.size()!=add(before_.inodes-1,before_.extents))
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
      case NativeFileOperation::remove: discard(resolve(key));return;
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

  void enable_workload() noexcept {next_.workload.enabled=true;}

  NativeFsCommit finish(PagedObjectStoreWriter& writer,IndexedVolumeRoot& current) {
    NativeFsCommit result;
    result.created_inodes=std::move(created_);
    if(nodes_.empty() && next_.workload==before_.workload) {
      result.storage=native_paged_info(healthy_checkpoint());result.sequence=before_.sequence;
      return result;
    }

    std::vector<StoreMutation> records;
    std::vector<NativeNameEdit> edits;
    for(const auto& [key,replacement]:names_) {
      const auto expected=index_.find(key);
      if(expected!=replacement)edits.push_back({key,expected,replacement});
    }
    for(auto& [key,extent]:extents_) {
      if(!extent.dirty)continue;
      const auto [id,block]=key;
      const auto& changed=nodes_.at(id);
      if(extent.payload.empty()) {
        if(!extent.original)continue;
        records.push_back({*extent.original,true,{}});
        edits.push_back({native_extent_key(id,block),extent.original,{}});
        decrement(next_.extents);
        if(!changed.original_allocation)decrement(next_.charged_extents);
      } else {
        const auto& n=*changed.value;
        auto object=extent.original;
        if(!object) {
          if(next_.next_extent==kNativeIndexedInodeLimit)fail(Code::limit,"indexed extent identity exhausted");
          object=kNativeIndexedDataObject|next_.next_extent++;
          edits.push_back({native_extent_key(id,block),{},object});
          ++next_.extents;
          if(!n.allocation)next_.charged_extents=add(next_.charged_extents,1);
        }
        records.push_back({*object,false,encode_extent(n,{id,block,extent.payload})});
      }
    }
    for(const auto& [id,changed]:nodes_) {
      if(changed.value) {
        records.push_back({inode_domain|id,false,encode_indexed_inode(*changed.value,next_)});
        if(!changed.original_size)++next_.inodes;
        const auto reserved=reserved_extents(*changed.value);
        if(reserved<changed.original_reserved)fail(Code::corrupt,"indexed reservation shrank");
        next_.charged_extents=add(next_.charged_extents,reserved-changed.original_reserved);
      } else if(changed.original_size) {
        records.push_back({inode_domain|id,true,{}});
        decrement(next_.inodes);
        if(next_.charged_extents<changed.original_reserved)
          fail(Code::corrupt,"indexed reservation underflow");
        next_.charged_extents-=changed.original_reserved;
      }
    }
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
      fail(Code::limit,error.what());
    }catch(const std::bad_alloc&) {
      if(writer.poisoned())fail(Code::storage_failure,"indexed publication failed after admission");
      throw;
    }catch(const std::exception&){fail(Code::storage_failure,"indexed storage commit failed");}
    result.sequence=next_.sequence;
    current=next_;
    return result;
  }

 private:
  struct InodeEdit {
    std::optional<IndexedInodeInput> value;
    std::optional<std::uint64_t> original_size,original_allocation;
    std::uint64_t original_reserved=0;
  };
  struct ExtentEdit {
    std::optional<std::uint64_t> original;
    Bytes payload;
    bool dirty=false;
  };
  const PagedObjectStoreSnapshot& source_;
  IndexedVolumeRoot before_,next_;
  NativeNameIndex index_;
  std::map<std::uint64_t,InodeEdit> nodes_;
  std::map<NativeNameKey,std::optional<std::uint64_t>> names_;
  std::map<std::pair<std::uint64_t,std::uint64_t>,ExtentEdit> extents_;
  std::vector<std::uint64_t> created_;
  std::size_t payload_bytes_=0;

  PagedStoreInfo healthy_checkpoint() const {
    const auto info=source_.info();
    if(info.degraded_roots || info.degraded_metadata || info.degraded_payloads)
      fail(Code::storage_failure,"indexed transaction encountered degraded storage");
    return info;
  }

  IndexedInodeInput node(std::uint64_t id) const {
    const auto staged=nodes_.find(id);
    if(staged==nodes_.end())return load_inode(source_,before_,id);
    if(!staged->second.value)fail(Code::not_found,"indexed inode was removed in this transaction");
    return *staged->second.value;
  }

  IndexedInodeInput linked_inode(const NativeNameKey& key,std::uint64_t id) const {
    try {
      auto n=node(id);
      if(n.parent!=key.parent || name_key(n.name)!=key.name)
        fail(Code::corrupt,"indexed transaction name and inode disagree");
      return n;
    }catch(const NativeFsError& error) {
      if(error.code()==Code::not_found)fail(Code::corrupt,"indexed transaction name has no inode");
      throw;
    }
  }

  IndexedInodeInput resolve(std::string_view key) const {
    auto n=node(1);
    for(std::size_t begin=0;begin<key.size();) {
      if(!n.directory)fail(Code::not_directory,"indexed path ancestor is a file");
      const auto separator=key.find('/',begin);
      const auto end=separator==std::string_view::npos?key.size():separator;
      n=child({n.inode,std::string(key.substr(begin,end-begin))});
      begin=end+1;
    }
    return n;
  }

  std::optional<std::uint64_t> binding(const NativeNameKey& key) const {
    const auto staged=names_.find(key);
    return staged==names_.end()?index_.find(key):staged->second;
  }

  IndexedInodeInput child(const NativeNameKey& key) const {
    const auto id=binding(key);
    if(!id)fail(Code::not_found,"indexed path not found");
    return linked_inode(key,*id);
  }

  NativeNameKey destination(std::string_view path,std::string& spelling) const {
    const auto key=name_key(path);
    const auto slash=key.rfind('/');
    const auto start=slash==std::string::npos?0:slash+1;
    auto parent=resolve(start?std::string_view(key).substr(0,slash):std::string_view{});
    if(!parent.directory)fail(Code::not_directory,"indexed destination parent is a file");
    spelling=path.substr(start);
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
      it=nodes_.emplace(id,InodeEdit{std::move(n),original_size,original_allocation,reserved}).first;
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
    (void)edit(key.parent);
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

  void verify_descendant_paths(std::uint64_t id,std::size_t bytes,unsigned depth) const {
    visit_children(id,[&](const IndexedInodeInput& child) {
      const auto length=bytes+1+child.name.size();
      if(length>240 || depth==16)fail(Code::invalid_path,"indexed rename exceeds descendant path bounds");
      if(child.directory)verify_descendant_paths(child.inode,length,depth+1);
      return true;
    });
  }

  void move(const IndexedInodeInput& n,const NativeFileMutation& op) {
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
        if(victim.directory!=n.directory)fail(Code::invalid_operation,"indexed rename type mismatch");
        discard(victim);
      }
    }
    if(n.directory) {
      const auto source_depth=std::count(op.path.begin(),op.path.end(),'/');
      const auto target_depth=std::count(op.target.begin(),op.target.end(),'/');
      if(op.target.size()>op.path.size() || target_depth>source_depth)
        verify_descendant_paths(n.inode,op.target.size(),static_cast<unsigned>(target_depth+1));
    }
    names_[{n.parent,name_key(n.name)}]={};
    auto& entry=edit(n.inode);
    entry.parent=key.parent;entry.name=std::move(spelling);
    (void)edit(n.parent);
    (void)edit(key.parent);
    names_[std::move(key)]=n.inode;
  }

  void discard(const IndexedInodeInput& n) {
    if(n.directory && !visit_children(n.inode,[](const auto&){return false;}))
      fail(Code::not_empty,"indexed directory is not empty");
    (void)edit(n.inode);
    if(!n.directory)clear_extents(n.inode,0);
    nodes_.at(n.inode).value.reset();
    names_[{n.parent,name_key(n.name)}]={};
    (void)edit(n.parent);
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
      if(value.original) {
        if(domain(*value.original)!=kNativeIndexedDataObject || !(*value.original&ordinal_mask) ||
            (*value.original&ordinal_mask)>=before_.next_extent)
          fail(Code::corrupt,"indexed data identity outside allocator");
        auto bytes=required(source_,*value.original,kNativeExtentBytes+extent_header);
        IndexedInodeInput original;original.inode=id;original.size=*n.original_size;
        verify_extent(bytes,original,block);
        bytes.erase(bytes.begin(),bytes.begin()+extent_header);
        if(bytes.capacity()>kStoreMaxTransactionBytes-payload_bytes_)
          fail(Code::limit,"indexed staging payload budget");
        payload_bytes_+=bytes.capacity();
        value.payload=std::move(bytes);
      }
    }
    return extents_.emplace(std::pair{id,block},std::move(value)).first->second;
  }

  void clear_extent(ExtentEdit& extent) {
    extent.dirty=extent.dirty || extent.original.has_value();
    payload_bytes_-=extent.payload.capacity();
    Bytes{}.swap(extent.payload);
  }

  void clear_extents(std::uint64_t id,std::uint64_t first) {
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

  void set_size(IndexedInodeInput& n,std::uint64_t size) {
    if(n.allocation && size>*n.allocation) {
      const auto remainder=size%kNativeAllocationUnit;
      const auto padding=remainder?kNativeAllocationUnit-remainder:0;
      if(padding>(std::numeric_limits<std::uint64_t>::max)()-size)
        fail(Code::limit,"indexed reservation alignment overflow");
      n.allocation=size+padding;
    }
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

IndexedVolumeRoot initialize_indexed_volume(PagedObjectStoreWriter& writer) {
  const auto snapshot=writer.snapshot();
  if(writer.poisoned() || snapshot.info().generation!=2 || !snapshot.info().mirrored_payloads ||
      snapshot.info().objects)
    fail(Code::invalid_operation,"indexed initialization requires a new mirrored store");
  IndexedVolumeRoot root;
  root.identity=snapshot.identity();root.sequence=1;root.inodes=1;root.workload.enabled=true;
  IndexedInodeInput directory;
  directory.inode=1;directory.born=1;directory.modified=1;directory.directory=true;
  std::vector<StoreMutation> initial;
  initial.push_back({namespace_object,false,encode_root(root)});
  initial.push_back({inode_domain|1,false,encode_indexed_inode(directory,root)});
  NativeNameIndex::initialize(writer,std::move(initial),NativeIndexKind::names_and_extents);
  return root;
}

NativeFsCommit commit_indexed_volume(PagedObjectStoreWriter& writer,const PagedObjectStoreSnapshot& source,
    IndexedVolumeRoot& current,std::span<const NativeFileMutation> operations,
    std::span<const NativeBasicMutation> basic,std::span<const NativeSecurityMutation> security,bool enable_workload) {
  IndexedVolumeTransaction transaction(source,current);
  if(enable_workload)transaction.enable_workload();
  for(const auto& op:operations)transaction.apply(op);
  for(const auto& op:basic)transaction.apply(op);
  for(const auto& op:security)transaction.apply(op);
  return transaction.finish(writer,current);
}

bool indexed_volume_root(std::span<const std::byte> bytes) noexcept {return magic(bytes,root_magic);}
IndexedVolumeView::IndexedVolumeView(const PagedObjectStoreSnapshot& store,std::span<const std::byte> root)
    :IndexedVolumeView(store,decode_root(root)) {}
IndexedVolumeView::IndexedVolumeView(const PagedObjectStoreSnapshot& store,IndexedVolumeRoot root)
    :store_(store),root_(root),index_(store) {
  validate_header_fields(root_);
  if(root_.identity!=store_.identity() || index_.kind()!=NativeIndexKind::names_and_extents ||
      !root_.inodes || root_.inodes>=root_.next_inode || root_.extents>=root_.next_extent ||
      root_.charged_extents<root_.extents ||
      root_.charged_extents>(std::numeric_limits<std::size_t>::max)() ||
      index_.size()!=add(root_.inodes-1,root_.extents))
    fail(Code::corrupt,"wrong indexed root capability or counts");
  (void)referenced_inode(1);
}
IndexedInodeInput IndexedVolumeView::inode(std::uint64_t id) const {
  return load_inode(store_,root_,id);
}
IndexedInodeInput IndexedVolumeView::referenced_inode(std::uint64_t id) const {
  try{return inode(id);}
  catch(const NativeFsError& error) {
    if(error.code()==Code::not_found)fail(Code::corrupt,"indexed reference has no inode");
    throw;
  }
}
IndexedInodeInput IndexedVolumeView::resolve(std::string_view path_value,std::string* spelling) const {
  const auto key=name_key(path_value,true);
  auto current=inode(1);
  for(std::size_t start=0;start<key.size();) {
    if(!current.directory)fail(Code::not_directory,"indexed path ancestor is a file");
    const auto separator=key.find('/',start),end=separator==std::string::npos?key.size():separator;
    const auto component=key.substr(start,end-start);
    const auto child=index_.find({current.inode,component});
    if(!child)fail(Code::not_found,"indexed path not found");
    auto entry=referenced_inode(*child);
    if(entry.parent!=current.inode || name_key(entry.name)!=component)
      fail(Code::corrupt,"indexed name points to a different inode");
    if(spelling) {
      if(!spelling->empty())spelling->push_back('/');
      spelling->append(entry.name);
    }
    current=std::move(entry);
    start=end+1;
  }
  return current;
}
std::string IndexedVolumeView::path(const IndexedInodeInput& input) const {
  std::string result=input.name;
  auto current=input;
  for(unsigned depth=0;current.inode!=1;++depth) {
    if(depth==16 || result.size()>240 || index_.find({current.parent,name_key(current.name)})!=current.inode)
      fail(Code::corrupt,"indexed inode ancestry mismatch");
    current=referenced_inode(current.parent);
    if(!current.directory)fail(Code::corrupt,"indexed inode parent is a file");
    if(current.inode!=1)result=current.name+"/"+result;
  }
  return result;
}
NativeFileInfo IndexedVolumeView::stat(std::string_view path_value) const {
  std::string spelling;
  const auto selected=resolve(path_value,&spelling);
  return file_info(selected,std::move(spelling));
}
NativeFileInfo IndexedVolumeView::stat_inode(std::uint64_t id) const {
  const auto selected=inode(id);
  return file_info(selected,path(selected));
}
IndexedDirectoryPage IndexedVolumeView::list(std::uint64_t directory,std::string_view after,std::size_t maximum) const {
  const auto parent=inode(directory);
  if(!parent.directory)fail(Code::not_directory,"indexed enumeration of a file");
  auto prefix=path(parent);
  if(!prefix.empty())prefix.push_back('/');
  const auto records=index_.list(directory,after,maximum);
  if(!records.empty() && std::count(prefix.begin(),prefix.end(),'/')>=16)
    fail(Code::corrupt,"indexed directory exceeds native path depth");
  IndexedDirectoryPage result;
  result.entries.reserve(records.size());
  for(const auto& record:records) {
    const auto child=referenced_inode(record.value);
    if(child.parent!=directory || name_key(child.name)!=record.key.name || prefix.size()+child.name.size()>240)
      fail(Code::corrupt,"indexed directory entry differs from inode");
    result.entries.push_back(file_info(child,prefix+child.name));
  }
  if(records.size()==maximum && !index_.list(directory,records.back().key.name,1).empty())
    result.after=records.back().key.name;
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
Bytes IndexedVolumeView::read(const IndexedInodeInput& selected,std::uint64_t offset,std::uint32_t bytes) const {
  if(selected.directory)fail(Code::is_directory,"cannot read indexed directory");
  if(bytes>kNativeMaxReadBytes)fail(Code::limit,"indexed read budget");
  if(!bytes || offset>=selected.size)return {};
  const auto count=(std::min)(std::uint64_t(bytes),selected.size-offset);
  const auto end=offset+count,first=offset/kNativeExtentBytes,last=(end-1)/kNativeExtentBytes;
  const auto start=native_extent_key(selected.inode,first?first-1:0);
  const auto range=index_.list(start.parent,first?std::string_view(start.name):std::string_view{},
      static_cast<std::size_t>(last-first+1));
  Bytes output(static_cast<std::size_t>(count));
  for(const auto& entry:range) {
    const auto block=native_extent_block(entry.key.name);
    if(block>last)break;
    if((entry.value&ordinal_mask)>=root_.next_extent)
      fail(Code::corrupt,"indexed extent reference exceeds allocator");
    const auto data=required(store_,entry.value,extent_header+kNativeExtentBytes);
    verify_extent(data,selected,block);
    const auto begin=block*kNativeExtentBytes;
    const auto from=(std::max)(begin,offset),to=(std::min)(end,begin+(data.size()-extent_header));
    if(from<to)std::copy_n(data.begin()+static_cast<std::ptrdiff_t>(extent_header+from-begin),
        static_cast<std::size_t>(to-from),output.begin()+static_cast<std::ptrdiff_t>(from-offset));
  }
  return output;
}
void IndexedVolumeView::verify_inventory(bool published) const {
  store_.verify_all();
  index_.verify_all();
  std::uint64_t cursor=0,inodes=0,extents=0,charged=0;
  bool found_root=false;
  for(;;) {
    const auto records=store_.list_page(cursor,64);
    if(records.empty())break;
    for(const auto& record:records) {
      if(record.id==namespace_object) {
        if(!published || required(store_,record.id,root_size)!=encode_root(root_))
          fail(Code::corrupt,"unexpected indexed root publication");
        found_root=true;
      } else if(domain(record.id)==inode_domain) {
        const auto entry=inode(record.id&ordinal_mask);
        (void)path(entry);
        ++inodes;charged=add(charged,reserved_extents(entry));
      } else if(domain(record.id)==kNativeIndexedDataObject) {
        if(!(record.id&ordinal_mask) || (record.id&ordinal_mask)>=root_.next_extent)
          fail(Code::corrupt,"indexed extent allocator mismatch");
        const auto bytes=required(store_,record.id,extent_header+kNativeExtentBytes);
        if(bytes.size()<=extent_header)fail(Code::corrupt,"truncated indexed extent");
        const auto owner=referenced_inode(get(bytes,8));
        const auto block=get(bytes,16);
        verify_extent(bytes,owner,block);
        if(index_.find(native_extent_key(owner.inode,block))!=record.id)
          fail(Code::corrupt,"unreferenced indexed extent");
        ++extents;
        if(!owner.allocation)charged=add(charged,1);
      } else if(domain(record.id)!=kNativeNameIndexObject)fail(Code::corrupt,"foreign indexed object domain");
    }
    cursor=records.back().id;
  }
  if(found_root!=published || inodes!=root_.inodes || extents!=root_.extents || charged!=root_.charged_extents)
    fail(Code::corrupt,"indexed volume inventory mismatch");
}
void IndexedVolumeView::verify_all() const {verify_inventory(true);}

IndexedVolumeBuilder::IndexedVolumeBuilder(PagedObjectStoreWriter output,std::uint64_t sequence,
    std::uint64_t next_inode,NativeWorkloadInfo workload):writer_(std::move(output)) {
  root_.identity=writer_->snapshot().identity();
  root_.sequence=sequence;root_.next_inode=next_inode;root_.workload=workload;
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
    auto frame=encode_indexed_inode(inode,next);
    const auto id=inode_domain|inode.inode;
    if(!incoming.emplace(inode.inode,&inode).second || snapshot.stat(id))
      fail(Code::already_exists,"indexed import repeats an inode");
    if(inode.inode!=1)edits.push_back({{inode.parent,name_key(inode.name)},{},inode.inode});
    append_record(id,std::move(frame));
    ++next.inodes;next.charged_extents=add(next.charged_extents,reserved_extents(inode));
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
