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
namespace {
using Bytes=std::vector<std::byte>;
using Code=NativeFsErrorCode;
constexpr std::uint64_t kNamespaceObject=1;
constexpr std::size_t kMetadataCap=kNativeExtentBytes,kDirtyCap=kNativeMaxDirtyExtents;
static_assert(64+kNativeMaxNodes*(64+100)+16*kNativeMaxExtents<=kMetadataCap);
static_assert(kDirtyCap*kNativeExtentBytes+kMetadataCap<=kStoreMaxTransactionBytes);
static_assert(kNativeMaxExtents*kNativeExtentBytes+kMetadataCap<=kStoreMaxLogicalBytes);
constexpr std::array<std::byte,8> kMagic{std::byte{'F'},std::byte{'L'},std::byte{'T'},std::byte{'X'},std::byte{'N'},std::byte{'S'},std::byte{'0'},std::byte{'1'}};
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
  bool directory=false;
  std::string name,key;
  std::map<std::uint64_t,std::uint64_t> extents; // Logical block -> object ID.
};
struct Model {
  StoreIdentity identity{};
  std::uint64_t sequence=1,next_inode=2;
  std::map<std::uint64_t,Node> nodes;
};
static_assert(std::is_nothrow_move_assignable_v<Model>);
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
  return {parent,std::string(slash==std::string_view::npos?path:path.substr(slash+1))};
}
std::string display_path(const Model& model,const Node& n) {
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
void validate_structure(const Model& model,bool pending=false) {
  if(model.nodes.empty() || model.nodes.size()>kNativeMaxNodes || model.sequence==0)fail(Code::limit,"native namespace budget");
  const auto& root=node(model,1);
  if(root.parent || !root.directory || !root.name.empty() || root.size || !root.extents.empty() || root.born!=1)
    fail(Code::corrupt,"native root invariant");
  std::set<std::pair<std::uint64_t,std::string>> names;
  std::set<std::uint64_t> objects;
  std::uint64_t largest=0;
  for(const auto& [id,n]:model.nodes) {
    if(id!=n.inode || !id || n.born==0 || n.born>n.modified || n.modified>model.sequence ||
        n.size>kNativeMaxFileBytes || (n.directory && (n.size || !n.extents.empty())))
      fail(Code::corrupt,"native inode invariant");
    largest=(std::max)(largest,id);
    if(id!=1) {
      if(!n.parent || n.parent==id || !node(model,n.parent).directory || component(n.name)!=n.key ||
          !names.emplace(n.parent,n.key).second)fail(Code::corrupt,"native parent/name collision");
      const auto path=display_path(model,n);
      (void)canonical(path);
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
  for(const auto& [id,n]:model.nodes){(void)id;size+=64+n.name.size()+16*n.extents.size();}
  if(size>kMetadataCap)fail(Code::limit,"native metadata budget");
  Bytes bytes(size);std::copy(kMagic.begin(),kMagic.end(),bytes.begin());
  put(bytes,8,1,4);put(bytes,12,model.nodes.size(),4);put(bytes,16,model.sequence,8);
  put(bytes,24,model.next_inode,8);put(bytes,32,kNativeExtentBytes,4);put(bytes,36,extent_count(model),4);
  std::copy(model.identity.begin(),model.identity.end(),bytes.begin()+40);
  std::size_t cursor=64;
  for(const auto& [id,n]:model.nodes) {
    put(bytes,cursor,id,8);put(bytes,cursor+8,n.parent,8);put(bytes,cursor+16,n.directory?1:2,4);
    put(bytes,cursor+24,n.size,8);put(bytes,cursor+32,n.born,8);put(bytes,cursor+40,n.modified,8);
    put(bytes,cursor+48,n.name.size(),2);put(bytes,cursor+50,n.extents.size(),2);cursor+=64;
    for(char c:n.name)bytes[cursor++]=static_cast<std::byte>(static_cast<unsigned char>(c));
    for(const auto& [block,object]:n.extents){put(bytes,cursor,block,8);put(bytes,cursor+8,object,8);cursor+=16;}
  }
  return bytes;
}
Model decode(const ObjectStoreSnapshot& objects) {
  const auto bytes=object_bytes(objects,kNamespaceObject);
  const auto all=std::span<const std::byte>(bytes);
  if(bytes.size()<64 || bytes.size()>kMetadataCap || !std::equal(kMagic.begin(),kMagic.end(),bytes.begin()) ||
      integer(all,8,4)!=1 || integer(all,32,4)!=kNativeExtentBytes || !zeros(all.subspan(56,8)))
    fail(Code::corrupt,"unsupported native namespace framing");
  const auto count=integer(all,12,4),declared_extents=integer(all,36,4);
  if(!count || count>kNativeMaxNodes || declared_extents>kNativeMaxExtents)fail(Code::limit,"native metadata counts");
  Model model;model.sequence=integer(all,16,8);model.next_inode=integer(all,24,8);
  std::copy_n(bytes.begin()+40,16,model.identity.begin());
  if(model.identity!=objects.identity() || model.sequence>objects.info().generation)fail(Code::corrupt,"native identity/sequence binding");
  std::size_t cursor=64;std::uint64_t previous=0;
  for(std::uint64_t i=0;i<count;++i) {
    if(cursor>bytes.size() || bytes.size()-cursor<64)fail(Code::corrupt,"truncated native inode");
    const auto h=all.subspan(cursor,64);cursor+=64;
    Node n;n.inode=integer(h,0,8);n.parent=integer(h,8,8);const auto kind=integer(h,16,4);
    n.directory=kind==1;n.size=integer(h,24,8);n.born=integer(h,32,8);n.modified=integer(h,40,8);
    const auto name_bytes=integer(h,48,2),extents=integer(h,50,2);
    if((kind!=1 && kind!=2) || n.inode<=previous || name_bytes>100 || extents>kNativeMaxExtents ||
        !zeros(h.subspan(20,4)) || !zeros(h.subspan(52,12)) ||
        name_bytes>bytes.size()-cursor)fail(Code::corrupt,"invalid native inode fields");
    n.name.assign(reinterpret_cast<const char*>(bytes.data()+cursor),static_cast<std::size_t>(name_bytes));
    cursor+=static_cast<std::size_t>(name_bytes);
    try{n.key=n.inode==1?"":component(n.name);}
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
using Dirty=std::map<std::pair<std::uint64_t,std::uint64_t>,Bytes>;
struct Stage {
  Model model;const ObjectStoreSnapshot& base;Dirty dirty;
  bool changed=false;std::vector<std::uint64_t> created;
  Stage(const Model& previous,const ObjectStoreSnapshot& objects):model(previous),base(objects) {
    if(model.sequence==(std::numeric_limits<std::uint64_t>::max)())fail(Code::limit,"native sequence exhausted");
    ++model.sequence;created.reserve(16);
  }
  Bytes& block(Node& n,std::uint64_t index) {
    const auto key=std::pair{n.inode,index};
    if(auto found=dirty.find(key);found!=dirty.end())return found->second;
    if(dirty.size()>=kDirtyCap)fail(Code::limit,"native staging extent budget");
    const auto existing=n.extents.find(index);
    auto bytes=existing==n.extents.end()?Bytes(kNativeExtentBytes):object_bytes(base,existing->second);
    if(bytes.size()!=kNativeExtentBytes)fail(Code::corrupt,"native source extent size");
    return dirty.emplace(key,std::move(bytes)).first->second;
  }
  void finish_block(Node& n,std::uint64_t index) {
    const auto key=std::pair{n.inode,index};auto& bytes=dirty.at(key);
    if(zeros(bytes)){n.extents.erase(index);dirty.erase(key);}
    else n.extents.try_emplace(index,0);
  }
  void discard(Node& n) {
    if(n.directory)for(const auto& [id,other]:model.nodes)if(id!=n.inode && other.parent==n.inode)fail(Code::not_empty,"native directory not empty");
    for(auto it=dirty.begin();it!=dirty.end();)it=it->first.first==n.inode?dirty.erase(it):std::next(it);
    const auto parent=n.parent,id=n.inode;
    node(model,parent).modified=model.sequence;model.nodes.erase(id);changed=true;
  }
  void apply(const NativeFileMutation& op) {
    (void)canonical(op.path);
    const bool write=op.operation==NativeFileOperation::write;
    const bool trunc=op.operation==NativeFileOperation::truncate;
    const bool rename=op.operation==NativeFileOperation::rename;
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
    const auto id=resolve(model,op.path);auto& n=node(model,id);
    if(id==1)fail(Code::invalid_operation,"native root mutation forbidden");
    if(write) {
      if(n.directory)fail(Code::is_directory,"cannot write native directory");
      if(op.offset>kNativeMaxFileBytes || op.data.size()>kNativeMaxFileBytes-op.offset)fail(Code::limit,"native write bounds");
      if(op.data.empty())return;
      std::size_t done=0;
      while(done<op.data.size()) {
        const auto position=op.offset+done,index=position/kNativeExtentBytes;
        const auto within=static_cast<std::size_t>(position%kNativeExtentBytes);
        const auto count=(std::min)(op.data.size()-done,std::size_t(kNativeExtentBytes)-within);
        auto& bytes=block(n,index);std::copy_n(op.data.begin()+static_cast<std::ptrdiff_t>(done),count,bytes.begin()+static_cast<std::ptrdiff_t>(within));
        finish_block(n,index);done+=count;
      }
      n.size=(std::max)(n.size,op.offset+op.data.size());n.modified=model.sequence;changed=true;return;
    }
    if(trunc) {
      if(n.directory)fail(Code::is_directory,"cannot truncate native directory");
      if(op.size>kNativeMaxFileBytes)fail(Code::limit,"native truncate bounds");
      if(op.size==n.size)return;
      const auto blocks=(op.size+kNativeExtentBytes-1)/kNativeExtentBytes;
      for(auto it=n.extents.lower_bound(blocks);it!=n.extents.end();) {
        dirty.erase({n.inode,it->first});it=n.extents.erase(it);
      }
      if(op.size<n.size && op.size%kNativeExtentBytes && n.extents.contains(op.size/kNativeExtentBytes)) {
        const auto index=op.size/kNativeExtentBytes;auto& bytes=block(n,index);
        std::fill(bytes.begin()+static_cast<std::ptrdiff_t>(op.size%kNativeExtentBytes),bytes.end(),std::byte{0});
        finish_block(n,index);
      }
      n.size=op.size;n.modified=model.sequence;changed=true;return;
    }
    if(rename) {
      const auto [parent,name]=destination(model,op.target);const auto key=component(name);
      for(auto p=parent;p;p=node(model,p).parent)if(p==id)fail(Code::invalid_operation,"native rename cycle");
      const auto* target=child(model,parent,key);
      if(target && target->inode==id) {
        if(n.name==name)return;
      } else if(target) {
        if(!op.replace)fail(Code::already_exists,"native rename destination exists");
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
  std::vector<StoreMutation> mutations() {
    if(extent_count(model)>kNativeMaxExtents)fail(Code::limit,"native live extent budget");
    std::set<std::uint64_t> used;
    for(const auto& [id,n]:model.nodes){(void)id;for(const auto& [index,object]:n.extents){(void)index;if(object)used.insert(object);}}
    for(auto& [id,n]:model.nodes)for(auto& [index,object]:n.extents)if(!object) {
      if(!dirty.contains({id,index}))fail(Code::corrupt,"native pending extent has no private data");
      std::uint64_t candidate=2;while(used.contains(candidate))++candidate;
      if(candidate>kStoreMaxObjects)fail(Code::limit,"native extent object allocator");
      object=candidate;used.insert(object);
    }
    validate_structure(model);
    // Unchanged records were fully admitted on initialize/resume or a prior
    // acknowledgement. Verify and validate every touched record privately;
    // public snapshot/open still scrub ALL reachable data before exposure.
    for(const auto& [key,bytes]:dirty) {
      const auto& n=node(model,key.first);const auto index=key.second;
      if(!n.extents.contains(index) || bytes.size()!=kNativeExtentBytes || zeros(bytes))
        fail(Code::corrupt,"native dirty extent invariant");
      const auto begin=index*kNativeExtentBytes;
      if(n.size-begin<kNativeExtentBytes &&
          !zeros(std::span<const std::byte>(bytes).subspan(static_cast<std::size_t>(n.size-begin))))
        fail(Code::corrupt,"native dirty bytes beyond EOF");
    }
    std::vector<StoreMutation> output;output.reserve(kStoreMaxObjects);
    output.push_back({kNamespaceObject,false,encode(model)});
    for(const auto& [id,n]:model.nodes)for(const auto& [index,object]:n.extents) {
      const auto found=dirty.find({id,index});if(found!=dirty.end())output.push_back({object,false,found->second});
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
NativeFileInfo NativeFilesystemSnapshot::stat(std::string_view path) const {
  if(!impl_)fail(Code::invalid_operation,"moved-from native snapshot");
  return file_info(impl_->model,node(impl_->model,resolve(impl_->model,path)));
}
std::vector<NativeFileInfo> NativeFilesystemSnapshot::list(std::string_view directory) const {
  if(!impl_)fail(Code::invalid_operation,"moved-from native snapshot");
  const auto id=resolve(impl_->model,directory);
  if(!node(impl_->model,id).directory)fail(Code::not_directory,"native enumeration of file");
  std::vector<NativeFileInfo> result;
  std::vector<const Node*> children;
  for(const auto& [inode,n]:impl_->model.nodes)if(inode!=1 && n.parent==id)children.push_back(&n);
  std::sort(children.begin(),children.end(),[](auto a,auto b){return a->key<b->key;});
  result.reserve(children.size());for(const auto* n:children)result.push_back(file_info(impl_->model,*n));
  return result;
}
Bytes NativeFilesystemSnapshot::read(std::string_view path,std::uint64_t offset,std::uint32_t bytes) const {
  if(!impl_)fail(Code::invalid_operation,"moved-from native snapshot");
  if(bytes>kNativeMaxReadBytes)fail(Code::limit,"native read request budget");
  const auto& n=node(impl_->model,resolve(impl_->model,path));
  if(n.directory)fail(Code::is_directory,"cannot read native directory");
  if(offset>=n.size || !bytes)return {};
  const auto count=(std::min)(std::uint64_t(bytes),n.size-offset),end=offset+count;
  Bytes output(static_cast<std::size_t>(count));
  for(auto it=n.extents.lower_bound(offset/kNativeExtentBytes);it!=n.extents.end();++it) {
    const auto begin=it->first*kNativeExtentBytes;if(begin>=end)break;
    const auto data=object_bytes(impl_->objects,it->second);
    if(data.size()!=kNativeExtentBytes)fail(Code::corrupt,"native read extent size");
    const auto first=(std::max)(begin,offset),last=(std::min)(begin+kNativeExtentBytes,end);
    std::copy_n(data.begin()+static_cast<std::ptrdiff_t>(first-begin),static_cast<std::size_t>(last-first),
                output.begin()+static_cast<std::ptrdiff_t>(first-offset));
  }
  return output;
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
  p->objects.commit({&metadata,1});
  return NativeFilesystemWriter(std::move(p));
}
NativeFilesystemWriter NativeFilesystemWriter::resume(ObjectStoreWriter objects) {
  auto snapshot=objects.snapshot();auto model=decode(snapshot);
  return NativeFilesystemWriter(std::make_unique<Impl>(std::move(objects),std::move(model)));
}

NativeFilesystemWriter NativeFilesystemWriter::compact(const NativeFilesystemSnapshot& source,
    std::shared_ptr<StoreByteIo> target,StoreIdentity id,StoreCompressionOptions compression) {
  if(!source.impl_)fail(Code::invalid_operation,"moved-from native compaction source");
  if(id==source.identity() || id==StoreIdentity{} || !target)
    fail(Code::invalid_operation,"native compaction requires a new target identity");
  try {
    auto model=decode(source.impl_->objects);model.identity=id;
    const auto metadata=encode(model);
    return resume(ObjectStoreWriter::compact_replacing(source.impl_->objects,
        std::move(target),id,kNamespaceObject,metadata,compression));
  }catch(const std::bad_alloc&){fail(Code::limit,"native compaction allocation failure");}
  catch(const NativeFsError&){throw;}
  catch(const std::exception&){fail(Code::storage_failure,"native compaction failed; retain source and partial target");}
}

NativeFsCommit NativeFilesystemWriter::apply(std::span<const NativeFileMutation> operations) {
  if(poisoned())fail(Code::storage_failure,"native writer is terminal");
  if(operations.empty() || operations.size()>16)fail(Code::limit,"native operation count");
  std::uint64_t input=0;
  for(const auto& op:operations) {
    if(static_cast<int>(op.operation)<0 || static_cast<int>(op.operation)>static_cast<int>(NativeFileOperation::remove))
      fail(Code::invalid_operation,"unknown native operation");
    if(op.data.size()>kStoreMaxObjectBytes)fail(Code::limit,"native write size budget");
    input+=op.data.size();if(input>kStoreMaxTransactionBytes)fail(Code::limit,"native input budget");
  }
  bool committing=false;
  try {
    auto snapshot=impl_->objects.snapshot_for_staging();
    // Metadata is read and integrity-checked even for a namespace-only/no-op
    // request. It must still exactly bind the cached last-ack model.
    if(object_bytes(snapshot,kNamespaceObject)!=encode(impl_->model))
      fail(Code::corrupt,"native cached namespace differs from backing record");
    Stage stage(impl_->model,snapshot);
    for(const auto& op:operations)stage.apply(op);
    validate_structure(stage.model,true);
    NativeFsCommit result;result.created_inodes=std::move(stage.created);
    if(!stage.changed){result.storage=snapshot.info();result.sequence=impl_->model.sequence;return result;}
    auto changes=stage.mutations();result.sequence=stage.model.sequence;
    committing=true;
    try{result.storage=impl_->objects.commit(changes);}
    catch(const std::exception&){
      if(impl_->objects.poisoned())impl_->poisoned=true;
      fail(Code::storage_failure,"native transaction storage commit failed");
    }
    impl_->model=std::move(stage.model);return result;
  }catch(const std::bad_alloc&){fail(Code::limit,"native allocation failure");}
  catch(const NativeFsError& e){
    if(e.code()==Code::corrupt || (e.code()==Code::storage_failure && (!committing || impl_->objects.poisoned())))impl_->poisoned=true;
    throw;
  }catch(const std::exception&){impl_->poisoned=true;fail(Code::storage_failure,"native backing read failed");}
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
bool NativeFilesystemWriter::poisoned() const noexcept{return !impl_ || impl_->poisoned || impl_->objects.poisoned();}
}
