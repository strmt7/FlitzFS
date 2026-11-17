#include "native_name_index.hpp"
#include "flitzfs/native_filesystem.hpp"
#include <algorithm>
#include <iterator>
#include <limits>
#include <map>
#include <new>
#include <utility>

namespace flitzfs::detail {
namespace {
using Bytes = std::vector<std::byte>;
using Error = NativeFsErrorCode;
constexpr std::string_view root_magic = "FLTXNT01";
constexpr std::string_view page_magic = "FLTXNP01";
constexpr std::size_t root_bytes = 40, header_bytes = 40, page_bytes = 4096;
constexpr std::size_t maximum_name_bytes = 1024;
constexpr std::uint64_t last_ordinal = (1ULL << 61) - 1;
constexpr std::uint32_t maximum_height = 61;

[[noreturn]] void fail(Error code, const char* message) {
  throw NativeFsError(code, message);
}
void put(std::span<std::byte> bytes, std::size_t at, std::uint64_t value, unsigned width) {
  for (unsigned i = 0; i < width; ++i) bytes[at+i] = static_cast<std::byte>(value >> (8*i));
}
std::uint64_t get(std::span<const std::byte> bytes, std::size_t at, unsigned width) {
  if (at > bytes.size() || width > bytes.size() - at)
    fail(Error::corrupt, "name index field outside record");
  std::uint64_t value = 0;
  for (unsigned i = 0; i < width; ++i)
    value |= std::uint64_t(std::to_integer<unsigned char>(bytes[at+i])) << (8*i);
  return value;
}
bool magic(std::span<const std::byte> bytes, std::string_view expected) {
  return bytes.size() >= expected.size() &&
      std::string_view(reinterpret_cast<const char*>(bytes.data()), expected.size()) == expected;
}
void set_magic(std::span<std::byte> bytes, std::string_view value) {
  for (std::size_t i = 0; i < value.size(); ++i) bytes[i] = static_cast<std::byte>(value[i]);
}
bool index_object(std::uint64_t id) noexcept {
  return (id & ~last_ordinal) == kNativeNameIndexObject;
}
bool page_object(std::uint64_t id, std::uint64_t next_page) noexcept {
  return index_object(id) && (id & last_ordinal) && (id & last_ordinal) < next_page;
}
bool inode_id(std::uint64_t id) noexcept { return id && id < (1ULL << 62); }
bool key_valid(const NativeNameKey& key) {
  return inode_id(key.parent) && !key.name.empty() && key.name.size() <= maximum_name_bytes &&
      key.name.find_first_of("/\\") == std::string::npos && key.name.find('\0') == std::string::npos;
}
std::uint64_t add_count(std::uint64_t a, std::uint64_t b) {
  if (b > (std::numeric_limits<std::uint64_t>::max)() - a)
    fail(Error::corrupt, "name index entry count overflow");
  return a + b;
}
Bytes fetch(const PagedObjectStoreSnapshot& store, std::uint64_t id, std::size_t limit) {
  try { return store.read(id,limit); }
  catch (const std::bad_alloc&) { throw; }
  catch (const std::exception&) { fail(Error::storage_failure, "name index object verification failed"); }
}
void protected_store(const PagedObjectStoreSnapshot& store) {
  if (!store.info().mirrored_payloads)
    fail(Error::invalid_operation, "name index requires protected object payloads");
}
void require_healthy_checkpoint(const PagedObjectStoreSnapshot& store) {
  const auto info=store.info();
  if (info.degraded_roots || info.degraded_metadata || info.degraded_payloads)
    fail(Error::storage_failure, "name index publication requires checkpoint recovery");
}
NativeNameIndexRoot read_root(const PagedObjectStoreSnapshot& store) {
  protected_store(store);
  const auto bytes = fetch(store, kNativeNameIndexObject, root_bytes);
  if (bytes.size() != root_bytes || !magic(bytes, root_magic) || get(bytes,8,4) != 1)
    fail(Error::corrupt, "unsupported name index root");
  NativeNameIndexRoot root{get(bytes,16,8), get(bytes,24,8), get(bytes,32,8),
      static_cast<std::uint32_t>(get(bytes,12,4))};
  if (!root.next_page || root.next_page > last_ordinal + 1 || root.height > maximum_height ||
      bool(root.page) != bool(root.entries) || (!root.page && root.height) ||
      (root.page && !page_object(root.page, root.next_page)))
    fail(Error::corrupt, "invalid name index root");
  return root;
}
Bytes encode_root(const NativeNameIndexRoot& root) {
  Bytes bytes(root_bytes);
  set_magic(bytes, root_magic);
  put(bytes,8,1,4);
  put(bytes,12,root.height,4);
  put(bytes,16,root.page,8);
  put(bytes,24,root.next_page,8);
  put(bytes,32,root.entries,8);
  return bytes;
}

struct Row {
  NativeNameKey key;
  std::uint64_t value = 0, entries = 1;
};
struct Page {
  std::uint64_t id;
  std::uint32_t height;
  std::vector<Row> rows;
};
struct Reference {
  std::uint64_t id, entries;
  std::uint32_t height;
  std::optional<NativeNameKey> lower, upper;
};
Reference root_reference(const NativeNameIndexRoot& root) {
  return {root.page, root.entries, root.height, {}, {}};
}
Reference child_reference(const Page& page, const Reference& parent, std::size_t index) {
  const auto& child = page.rows[index];
  return {child.value, child.entries, page.height-1,
      index ? std::optional<NativeNameKey>(page.rows[index-1].key) : parent.lower, child.key};
}
std::uint64_t entries(const Page& page) {
  std::uint64_t count = 0;
  for (const auto& row : page.rows) count = add_count(count, row.entries);
  return count;
}
std::size_t row_bytes(const Row& row, std::uint32_t height) {
  return 2 + (height ? 24 : 16) + row.key.name.size();
}
std::size_t encoded_size(const Page& page) {
  std::size_t size = header_bytes;
  for (const auto& row : page.rows) size += row_bytes(row, page.height);
  return size;
}
void check_reference(const Page& page, const Reference& reference) {
  if (page.id != reference.id || page.height != reference.height || page.rows.empty() ||
      entries(page) != reference.entries || (reference.lower && page.rows.front().key <= *reference.lower) ||
      (reference.upper && page.rows.back().key != *reference.upper))
    fail(Error::corrupt, "name index page does not match parent");
}
Page read_page(const PagedObjectStoreSnapshot& store, const NativeNameIndexRoot& root,
    const Reference& reference) {
  if (!page_object(reference.id, root.next_page)) fail(Error::corrupt, "invalid name index page reference");
  const auto bytes = fetch(store, reference.id, page_bytes);
  if (bytes.size() < header_bytes || bytes.size() > page_bytes || !magic(bytes, page_magic) ||
      get(bytes,8,4) != 1 || get(bytes,20,4) || get(bytes,24,8) != reference.id ||
      get(bytes,12,4) != reference.height || get(bytes,32,8) != reference.entries)
    fail(Error::corrupt, "invalid name index page header");
  const auto count = static_cast<std::size_t>(get(bytes,16,4));
  const std::size_t fixed = reference.height ? 24 : 16;
  if (!count || count > (bytes.size()-header_bytes)/(fixed+3))
    fail(Error::corrupt, "name index page slot count");
  Page page{reference.id,reference.height,{}};
  page.rows.reserve(count);
  std::size_t at = header_bytes + 2*count;
  for (std::size_t i = 0; i < count; ++i) {
    const auto first = get(bytes,header_bytes+2*i,2);
    const auto end = i+1 == count ? bytes.size() : get(bytes,header_bytes+2*(i+1),2);
    if (first != at || end > bytes.size() || end <= first || end-first <= fixed ||
        end-first-fixed > maximum_name_bytes)
      fail(Error::corrupt, "name index page slot boundary");
    Row row{{get(bytes,at,8),
        std::string(reinterpret_cast<const char*>(bytes.data()+at+fixed),end-at-fixed)},
        get(bytes,at+8,8), reference.height ? get(bytes,at+16,8) : 1};
    if (!key_valid(row.key) || (!page.rows.empty() && row.key <= page.rows.back().key) ||
        (reference.height ? (!page_object(row.value,root.next_page) || !row.entries) : !inode_id(row.value)))
      fail(Error::corrupt, "invalid name index row");
    page.rows.push_back(std::move(row));
    at = static_cast<std::size_t>(end);
  }
  check_reference(page, reference);
  return page;
}
Bytes encode_page(const Page& page) {
  const auto size = encoded_size(page);
  if (page.rows.empty() || size > page_bytes) fail(Error::limit, "name index page capacity");
  Bytes bytes(size);
  set_magic(bytes, page_magic);
  put(bytes,8,1,4);
  put(bytes,12,page.height,4);
  put(bytes,16,page.rows.size(),4);
  put(bytes,24,page.id,8);
  put(bytes,32,entries(page),8);
  std::size_t at = header_bytes + 2*page.rows.size();
  for (std::size_t i = 0; i < page.rows.size(); ++i) {
    const auto& row = page.rows[i];
    put(bytes,header_bytes+2*i,at,2);
    put(bytes,at,row.key.parent,8);
    put(bytes,at+8,row.value,8);
    if (page.height) put(bytes,at+16,row.entries,8);
    at += page.height ? 24 : 16;
    for (const auto c : row.key.name) bytes[at++] = static_cast<std::byte>(c);
  }
  return bytes;
}

std::size_t lower_slot(const Page& page, const NativeNameKey& key) {
  const auto found = std::lower_bound(page.rows.begin(),page.rows.end(),key,
      [](const Row& row,const NativeNameKey& value){ return row.key < value; });
  return static_cast<std::size_t>(found-page.rows.begin());
}
bool list_names(const PagedObjectStoreSnapshot& store, const NativeNameIndexRoot& root,
    const Reference& reference, const NativeNameKey& after, std::size_t maximum,
    std::vector<NativeNameEntry>& output) {
  const auto page = read_page(store, root, reference);
  const auto first = std::upper_bound(page.rows.begin(),page.rows.end(),after,
      [](const NativeNameKey& key,const Row& row){ return key < row.key; });
  for (auto it = first; it != page.rows.end(); ++it) {
    if (page.height) {
      const auto child = child_reference(page,reference,static_cast<std::size_t>(it-page.rows.begin()));
      if (!list_names(store,root,child,after,maximum,output)) return false;
    } else {
      if (it->key.parent != after.parent) return false;
      output.push_back({it->key,it->value});
      if (output.size() == maximum) return false;
    }
    if (it->key.parent > after.parent) return false;
  }
  return true;
}
std::uint64_t check_pages(const PagedObjectStoreSnapshot& store, const NativeNameIndexRoot& root,
    const Reference& reference) {
  const auto page = read_page(store,root,reference);
  std::uint64_t count = 1;
  if (page.height) {
    for (std::size_t i = 0; i < page.rows.size(); ++i)
      count = add_count(count,check_pages(store,root,child_reference(page,reference,i)));
  }
  return count;
}

Row describe(const Page& page) { return {page.rows.back().key, page.id, entries(page)}; }
class Editor {
 public:
  Editor(const PagedObjectStoreSnapshot& store, NativeNameIndexRoot root, std::size_t budget)
      : store_(store), root_(root), first_new_(root.next_page), budget_(budget) {}

  void apply(const NativeNameEdit& edit) {
    if (!root_.page) {
      if (edit.expected) fail(Error::invalid_operation, "name index binding changed");
      Page page{allocate(),0,{{edit.key,*edit.replacement,1}}};
      root_.page=page.id;
      root_.entries=1;
      save(std::move(page));
      return;
    }
    const auto previous = root_reference(root_);
    auto replacements = modify(previous,edit);
    if (replacements.empty()) {
      root_.page=0;
      root_.entries=0;
      root_.height=0;
      return;
    }
    if (replacements.size() > 1) {
      if (root_.height == maximum_height) fail(Error::limit, "name index height capacity");
      Page page{allocate(),root_.height+1,std::move(replacements)};
      root_.page=page.id;
      root_.entries=entries(page);
      root_.height=page.height;
      save(std::move(page));
    } else {
      root_.page=replacements.front().value;
      root_.entries=replacements.front().entries;
      while (root_.height) {
        const auto reference = root_reference(root_);
        const auto page = load(reference);
        if (page.rows.size() != 1) break;
        const auto child = child_reference(page,reference,0);
        (void)load(child);
        drop(page.id);
        root_.page=child.id;
        --root_.height;
      }
    }
  }

  void append_to(std::vector<StoreMutation>& records) const {
    records.reserve(records.size()+staged_.size()+1);
    for (const auto& [id,page] : staged_) {
      if (page) records.push_back({id,false,encode_page(*page)});
      else records.push_back({id,true,{}});
    }
    records.push_back({kNativeNameIndexObject,false,encode_root(root_)});
  }

 private:
  const PagedObjectStoreSnapshot& store_;
  NativeNameIndexRoot root_;
  std::uint64_t first_new_;
  std::size_t budget_;
  std::map<std::uint64_t,std::optional<Page>> staged_;

  std::uint64_t allocate() {
    if (root_.next_page > last_ordinal) fail(Error::limit, "name index object identity exhausted");
    return kNativeNameIndexObject | root_.next_page++;
  }
  void stage(std::uint64_t id, std::optional<Page> page) {
    if (staged_.size() >= budget_ && !staged_.contains(id))
      fail(Error::limit, "name index transaction page budget");
    staged_.insert_or_assign(id,std::move(page));
  }
  void save(Page page) {
    const auto id = page.id;
    stage(id,std::move(page));
  }
  void drop(std::uint64_t id) {
    if ((id&last_ordinal) >= first_new_) staged_.erase(id);
    else stage(id,{});
  }
  Page load(const Reference& reference) const {
    const auto found=staged_.find(reference.id);
    if (found==staged_.end()) return read_page(store_,root_,reference);
    if (!found->second) fail(Error::corrupt, "name index references a retired staged page");
    check_reference(*found->second,reference);
    return *found->second;
  }
  std::vector<Row> save_split(Page page) {
    if (page.rows.empty()) {
      drop(page.id);
      return {};
    }
    const auto size=encoded_size(page);
    std::vector<Row> result;
    if (size > page_bytes) {
      const auto payload=size-header_bytes;
      const auto capacity=page_bytes-header_bytes;
      std::size_t cut=0, left_bytes=0, skew=payload;
      for (std::size_t i=0; i+1<page.rows.size(); ++i) {
        left_bytes+=row_bytes(page.rows[i],page.height);
        if (left_bytes>capacity) break;
        const auto right_bytes=payload-left_bytes;
        if (right_bytes>capacity) continue;
        const auto difference=(std::max)(left_bytes,right_bytes)-(std::min)(left_bytes,right_bytes);
        if (difference<skew) { cut=i+1;skew=difference; }
      }
      if (!cut) fail(Error::limit,"name index split does not fit");
      auto middle=page.rows.begin()+static_cast<std::ptrdiff_t>(cut);
      Page right{allocate(),page.height,
          {std::make_move_iterator(middle),std::make_move_iterator(page.rows.end())}};
      page.rows.erase(middle,page.rows.end());
      result.push_back(describe(page));
      result.push_back(describe(right));
      save(std::move(right));
    } else result.push_back(describe(page));
    save(std::move(page));
    return result;
  }
  void merge_adjacent(Page& parent, const Reference& reference, std::size_t index) {
    if (parent.rows.size()<2) return;
    const auto left=(std::min)(index,parent.rows.size()-2);
    auto first=load(child_reference(parent,reference,left));
    auto second=load(child_reference(parent,reference,left+1));
    if (encoded_size(first)+encoded_size(second)-header_bytes>page_bytes) return;
    first.rows.insert(first.rows.end(),std::make_move_iterator(second.rows.begin()),
        std::make_move_iterator(second.rows.end()));
    parent.rows[left]=describe(first);
    parent.rows.erase(parent.rows.begin()+static_cast<std::ptrdiff_t>(left+1));
    save(std::move(first));
    drop(second.id);
  }
  std::vector<Row> modify(const Reference& reference, const NativeNameEdit& edit) {
    auto page=load(reference);
    const auto index=lower_slot(page,edit.key);
    if (page.height) {
      const auto child=(std::min)(index,page.rows.size()-1);
      auto changed=modify(child_reference(page,reference,child),edit);
      const auto position=page.rows.erase(page.rows.begin()+static_cast<std::ptrdiff_t>(child));
      page.rows.insert(position,std::make_move_iterator(changed.begin()),std::make_move_iterator(changed.end()));
      if (!edit.replacement) merge_adjacent(page,reference,child);
    } else {
      auto position=page.rows.begin()+static_cast<std::ptrdiff_t>(index);
      const bool exists=position!=page.rows.end() && position->key==edit.key;
      if ((exists ? std::optional<std::uint64_t>(position->value) : std::nullopt)!=edit.expected)
        fail(Error::invalid_operation, "name index binding changed");
      if (!edit.replacement) page.rows.erase(position);
      else if (exists) position->value=*edit.replacement;
      else page.rows.insert(position,{edit.key,*edit.replacement,1});
    }
    return save_split(std::move(page));
  }
};
void validate_accompanying(std::span<const StoreMutation> records) {
  if (records.size()>=kStoreMaxObjects)
    fail(Error::limit,"name index transaction operation budget");
  for (const auto& record : records) {
    if (index_object(record.id)) fail(Error::invalid_operation,"caller mutation uses name index object domain");
  }
}
} // namespace

NativeNameIndex::NativeNameIndex(const PagedObjectStoreSnapshot& store)
    : store_(store), root_(read_root(store)) {}

std::optional<std::uint64_t> NativeNameIndex::find(const NativeNameKey& key) const {
  if (!key_valid(key)) fail(Error::invalid_path, "invalid name index lookup key");
  auto reference=root_reference(root_);
  while (reference.id) {
    const auto page=read_page(store_,root_,reference);
    const auto index=lower_slot(page,key);
    if (index==page.rows.size()) return {};
    if (!page.height) return page.rows[index].key==key ? std::optional(page.rows[index].value) : std::nullopt;
    reference=child_reference(page,reference,index);
  }
  return {};
}

std::vector<NativeNameEntry> NativeNameIndex::list(std::uint64_t parent, std::string_view after,
    std::size_t maximum) const {
  if (!inode_id(parent) || after.size()>maximum_name_bytes)
    fail(Error::invalid_path, "invalid name index enumeration key");
  const NativeNameKey key{parent,std::string(after)};
  if (!after.empty() && !key_valid(key)) fail(Error::invalid_path, "invalid name index enumeration key");
  if (!maximum || maximum>1024) fail(Error::limit, "name index enumeration page budget");
  std::vector<NativeNameEntry> output;
  if (root_.page) {
    output.reserve(maximum);
    (void)list_names(store_,root_,root_reference(root_),key,maximum,output);
  }
  return output;
}

void NativeNameIndex::verify_all() const {
  const auto reachable=root_.page ? check_pages(store_,root_,root_reference(root_)) : 0;
  std::uint64_t cursor=kNativeNameIndexObject, stored=0;
  for (;;) {
    const auto page=store_.list_page(cursor,64);
    if (page.empty()) break;
    for (const auto& object : page) {
      if (!index_object(object.id)) {
        if (reachable!=stored) fail(Error::corrupt,"unreachable name index pages");
        return;
      }
      if (!page_object(object.id,root_.next_page) || stored==reachable)
        fail(Error::corrupt,"unreachable name index pages");
      ++stored;
    }
    cursor=page.back().id;
  }
  if (reachable!=stored) fail(Error::corrupt,"missing name index pages");
}

PagedStoreInfo NativeNameIndex::initialize(PagedObjectStoreWriter& writer,
    std::vector<StoreMutation> accompanying) {
  validate_accompanying(accompanying);
  const auto snapshot=writer.snapshot();
  protected_store(snapshot);
  require_healthy_checkpoint(snapshot);
  if (snapshot.info().objects) fail(Error::already_exists,"name index requires a new namespace");
  accompanying.push_back({kNativeNameIndexObject,false,encode_root({})});
  return writer.commit(accompanying);
}

PagedStoreInfo NativeNameIndex::commit(PagedObjectStoreWriter& writer, std::span<const NativeNameEdit> edits,
    std::vector<StoreMutation> accompanying) {
  if (edits.empty()) fail(Error::invalid_operation,"name index commit requires an edit");
  if (edits.size()>kStoreMaxObjects)
    fail(Error::limit,"name index transaction operation budget");
  validate_accompanying(accompanying);
  for (const auto& edit : edits) {
    if (!key_valid(edit.key)) fail(Error::invalid_path,"invalid name index edit key");
    if ((edit.expected && !inode_id(*edit.expected)) || (edit.replacement && !inode_id(*edit.replacement)) ||
        edit.expected==edit.replacement)
      fail(Error::invalid_operation,"invalid name index binding edit");
  }
  const auto snapshot=writer.snapshot();
  Editor editor(snapshot,read_root(snapshot),kStoreMaxObjects-accompanying.size()-1);
  for (const auto& edit : edits) editor.apply(edit);
  require_healthy_checkpoint(snapshot);
  editor.append_to(accompanying);
  return writer.commit(accompanying);
}

} // namespace flitzfs::detail
