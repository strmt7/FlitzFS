#include "native_indexed_volume.hpp"
#include "flitzfs/catalog.hpp"
#include <algorithm>
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
