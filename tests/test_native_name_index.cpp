#include "native_fs_fixture.hpp"
#include "native_name_index.hpp"
#include <iostream>
#include <iterator>

namespace {
using namespace native_fixture;
using namespace flitzfs::detail;
constexpr PagedStoreLimits limits{kStoreMaxPhysicalBytes};
constexpr StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects(NativeFsErrorCode code, F&& action) {
  try { action(); }
  catch (const NativeFsError& error) {
    if (error.code()==code) return;
    throw;
  }
  throw std::runtime_error("name index accepted invalid input");
}
std::shared_ptr<Ram> empty_image() {
  auto ram=std::make_shared<Ram>();
  ram->failure_unit=4096;
  return ram;
}
PagedObjectStoreWriter fresh(const std::shared_ptr<Ram>& ram) {
  auto writer=PagedObjectStoreWriter::create_mirrored(ram,identity(),limits,raw);
  NativeNameIndex::initialize(writer,{{1,false,data(1024,1)}});
  return writer;
}
std::uint64_t word(const Bytes& bytes, std::size_t offset, unsigned width) {
  require(offset+width<=bytes.size(),"test record word boundary");
  std::uint64_t result=0;
  for (unsigned i=0;i<width;++i)
    result|=std::uint64_t(std::to_integer<unsigned char>(bytes[offset+i]))<<(8*i);
  return result;
}
std::string filename(unsigned ordinal, bool long_name) {
  auto name="file-"+std::to_string(100000+ordinal);
  if (long_name) name.resize(1024,static_cast<char>('a'+ordinal%26));
  return name;
}

void ordered_names() {
  constexpr unsigned count=1511, batch_size=64;
  const auto ram=empty_image();
  auto writer=fresh(ram);
  rejects(NativeFsErrorCode::already_exists,[&]{NativeNameIndex::initialize(writer);});
  std::map<NativeNameKey,std::uint64_t> oracle;
  std::vector<NativeNameEdit> batch;
  for (unsigned n=0;n<count;++n) {
    const auto ordinal=n*317%count;
    NativeNameKey key{ordinal%7==0?2U:1U,filename(ordinal,ordinal%11==0)};
    const auto inode=std::uint64_t(ordinal)+100;
    require(oracle.emplace(key,inode).second,"unique test name");
    batch.push_back({std::move(key),{},inode});
    if (batch.size()==batch_size || n+1==count) {
      NativeNameIndex::commit(writer,batch);
      batch.clear();
    }
  }
  const auto retained=writer.snapshot();
  const NativeNameIndex index(retained);
  require(index.size()==oracle.size(),"name count after splits");
  index.verify_all();
  const auto old_inode=oracle.begin()->second;
  const auto old_key=oracle.begin()->first;
  const auto fetch_bytes=ram->read_bytes;
  require(index.find(old_key)==old_inode,"point lookup after splits");
  const auto point_bytes=ram->read_bytes-fetch_bytes;
  require(index.find(oracle.rbegin()->first)==oracle.rbegin()->second,"last key lookup after splits");
  require(!index.find({1,"missing"}) && !index.find({3,"file-100000"}),"absent name");
  require(index.list(3,"",20).empty(),"absent directory range");
  const auto before_scan=ram->read_bytes;
  for (std::uint64_t parent:{1,2}) {
    std::string after;
    auto expected=oracle.lower_bound({parent,{}});
    for (;;) {
      const auto page=index.list(parent,after,37);
      if (page.empty()) break;
      for (const auto& entry:page) {
        require(expected!=oracle.end() && expected->first.parent==parent &&
            entry==NativeNameEntry{expected->first,expected->second},"ordered keyset pagination");
        ++expected;
      }
      after=page.back().key.name;
    }
    require(expected==oracle.end() || expected->first.parent!=parent,"full directory enumeration");
  }
  const auto scan_bytes=ram->read_bytes-before_scan;
  require(point_bytes<scan_bytes/12,"lookup unexpectedly reads most directory pages");
  {
    const auto audit=index.scoped_audit();
    const auto split=oracle.lower_bound({2,{}});
    auto expected=oracle.begin();
    const auto midpoint=std::next(expected,std::distance(expected,split)/2);
    index.visit_directory(1,[&](std::string_view name,std::uint64_t inode,std::uint64_t run_end) {
      if(expected==midpoint) {
        auto nested=split;
        index.visit_directory(2,[&](std::string_view child,std::uint64_t id,std::uint64_t) {
          require(nested!=oracle.end() && child==nested->first.name && id==nested->second,
              "nested directory walk changed key order");
          ++nested;
        });
        require(nested==oracle.end(),"nested directory walk skipped entries");
      }
      require(expected!=split && name==expected->first.name && inode==expected->second,
          "nested reads changed the active directory walk");
      require(run_end>=inode,"directory read window precedes its child");
      auto companion=expected;
      for(auto next=inode;next<run_end;) {
        ++companion;
        require(companion!=split && companion->second==++next,
            "directory read window included an unrequested inode");
      }
      ++expected;
    });
    require(expected==split,"streamed directory walk skipped entries");
    index.visit_directory(3,[](std::string_view,std::uint64_t,std::uint64_t) {
      throw std::runtime_error("empty range returned a directory child");
    });
  }
  const auto recorded=retained.info();
  const auto height=word(retained.read(kNativeNameIndexObject,40),12,4);
  require(height>=2,"test must traverse multiple internal name pages");
  require(retained.read(1,1024)==data(1024,1),"typed exact-size read");
  const auto before_stat=ram->read_bytes;
  require(retained.stat(1)->logical_bytes==1024,"typed read fixture metadata");
  const auto stat_bytes=ram->read_bytes-before_stat;
  const auto before_refusal=ram->read_bytes;
  bool refused=false;
  try { (void)retained.read(1,1023); }
  catch (const std::runtime_error&) { refused=true; }
  require(refused && ram->read_bytes-before_refusal==stat_bytes,
      "typed read must refuse before accessing the payload");

  const NativeNameKey destination{3,"renamed"};
  const std::array rename{NativeNameEdit{old_key,old_inode,{}},
      NativeNameEdit{destination,{},old_inode}};
  NativeNameIndex::commit(writer,rename,{{1,false,data(1024,2)}});
  const auto reopened=PagedObjectStoreSnapshot::open(image(ram->stable,4096),limits);
  const NativeNameIndex current(reopened);
  current.verify_all();
  require(!current.find(old_key) && current.find(destination)==old_inode &&
      reopened.read(1)==data(1024,2),"rename and owned data publication");
  require(index.find(old_key)==old_inode && !index.find(destination) &&
      retained.read(1)==data(1024,1),"old snapshot retains names and data");

  const std::array inconsistent{NativeNameEdit{destination,old_inode,{}},
      NativeNameEdit{old_key,old_inode,5000}};
  const auto writes=ram->writes;
  rejects(NativeFsErrorCode::invalid_operation,[&]{
    NativeNameIndex::commit(writer,inconsistent,{{1,false,data(1024,3)}});
  });
  rejects(NativeFsErrorCode::invalid_operation,[&]{
    NativeNameIndex::commit(writer,rename,{{kNativeNameIndexObject,false,{}}});
  });
  require(ram->writes==writes && !writer.poisoned(),"preflight failure must not write or fence");
  const auto latest=writer.snapshot();
  require(NativeNameIndex(latest).find(destination)==old_inode &&
      latest.read(1)==data(1024,2),"preflight failure preserves state");
  rejects(NativeFsErrorCode::invalid_path,[&]{(void)current.find({1,std::string("a\0b",3)});});
  rejects(NativeFsErrorCode::invalid_path,[&]{(void)current.list(1,"child\\path",20);});
  rejects(NativeFsErrorCode::limit,[&]{(void)current.list(1,"",0);});
  const auto compacted_ram=empty_image();
  auto new_identity=identity();
  new_identity[0]^=std::byte{1};
  auto compacted_writer=PagedObjectStoreWriter::rebuild(reopened,compacted_ram,new_identity,limits,raw);
  const auto compacted=compacted_writer.snapshot();
  const NativeNameIndex rebuilt(compacted);
  rebuilt.verify_all();
  require(rebuilt.size()==count && rebuilt.find(destination)==old_inode &&
      !rebuilt.find(old_key) && compacted.read(1)==data(1024,2),
      "name index and owned records survive checked rebuild");
  require(compacted.info().physical_end<reopened.info().physical_end &&
      retained.read(1)==data(1024,1),"rebuild reclaims space and retains source snapshot");
  std::cout<<"names="<<count<<" height="<<height<<" point_read_bytes="<<point_bytes
      <<" two_directory_scan_bytes="<<scan_bytes<<" objects="<<recorded.objects
      <<" image_bytes="<<recorded.physical_end<<" mirrored_payload_bytes="
      <<recorded.stored_payload_bytes<<" current_before_rebuild_bytes="
      <<reopened.info().physical_end<<" rebuilt_bytes="<<compacted.info().physical_end<<'\n';
}

void split_merge_and_corrupt_framing() {
  const auto ram=empty_image();
  auto writer=fresh(ram);
  std::vector<NativeNameEdit> edits;
  for (unsigned i=0;i<24;++i) edits.push_back({{1,filename(i,true)},{},100+i});
  NativeNameIndex::commit(writer,edits);
  const auto full=writer.snapshot();
  require(word(full.read(kNativeNameIndexObject,40),12,4)>=2,"long names force internal splits");
  NativeNameIndex(full).verify_all();
  for (auto& edit:edits) std::swap(edit.expected,edit.replacement);
  NativeNameIndex::commit(writer,std::span(edits).first(edits.size()-1));
  const auto lone=writer.snapshot();
  const auto root=lone.read(kNativeNameIndexObject,40);
  require(word(root,12,4)==0 && lone.info().objects==3 &&
      NativeNameIndex(lone).find(edits.back().key)==edits.back().expected,"retire merged branch pages");
  NativeNameIndex(lone).verify_all();
  NativeNameIndex::commit(writer,{&edits.back(),1});
  const auto empty=writer.snapshot();
  require(empty.info().objects==2 && !NativeNameIndex(empty).size(),"retire final name page");
  NativeNameIndex(empty).verify_all();
  std::swap(edits.back().expected,edits.back().replacement);
  NativeNameIndex::commit(writer,{&edits.back(),1});
  const auto restored=writer.snapshot();
  const auto page_id=word(restored.read(kNativeNameIndexObject,40),16,8);
  require(page_id>word(root,16,8),"retired page identity was reused");
  auto page=restored.read(page_id,4096);
  // Re-seal a malformed namespace record through the ordinary storage API to
  // check parser invariants even when object integrity checks succeed.
  page[40]=std::byte{0};
  const StoreMutation wrong_slot{page_id,false,std::move(page)};
  writer.commit({&wrong_slot,1});
  const auto malformed=writer.snapshot();
  rejects(NativeFsErrorCode::corrupt,[&]{NativeNameIndex(malformed).verify_all();});
  rejects(NativeFsErrorCode::corrupt,[&]{(void)NativeNameIndex(malformed).find(edits.back().key);});
  const auto before=ram->writes;
  const NativeNameEdit insert{{1,"other"},{},55};
  rejects(NativeFsErrorCode::corrupt,[&]{NativeNameIndex::commit(writer,{&insert,1});});
  require(ram->writes==before && !writer.poisoned(),"malformed name page must fail before publication");
}

void interrupted_rename() {
  const auto ram=empty_image();
  auto writer=fresh(ram);
  const NativeNameEdit first{{1,"original"},{},20};
  NativeNameIndex::commit(writer,{&first,1});
  const auto baseline=ram->stable;
  const auto original=writer.snapshot().info();
  const std::array rename{NativeNameEdit{{1,"original"},20,{}},
      NativeNameEdit{{2,"new"},{},20}};
  const auto bytes=data(1024,9);
  const auto trial=image(baseline,4096);
  auto probe=PagedObjectStoreWriter::resume(trial,identity(),original.generation,limits,raw);
  const auto before=trial->calls;
  const auto published=NativeNameIndex::commit(probe,rename,{{1,false,bytes}});
  const auto operations=trial->calls-before;
  require(operations>=3,"must test actual publication barriers");
  unsigned cases=0, old_roots=0, new_roots=0;
  for (unsigned mode=0;mode<3;++mode) {
    for (std::size_t cut=0;cut<operations;++cut) {
      const auto medium=image(baseline,4096);
      auto interrupted=PagedObjectStoreWriter::resume(medium,identity(),original.generation,limits,raw);
      const auto retained=interrupted.snapshot();
      medium->failure=mode;
      medium->fail_at=medium->calls+cut;
      bool threw=false;
      try { NativeNameIndex::commit(interrupted,rename,{{1,false,bytes}}); }
      catch (const std::runtime_error&) { threw=true; }
      require(threw && interrupted.poisoned(),"interrupted write must fence its writer");
      require(NativeNameIndex(retained).find(first.key)==first.replacement &&
          retained.read(1)==data(1024,1),"interrupted write changed retained bytes");
      const auto recovered=PagedObjectStoreSnapshot::open(image(medium->stable,4096),limits);
      const NativeNameIndex index(recovered);
      index.verify_all();
      require(index.size()==1,"fault exposed partial rename");
      if (recovered.info().checkpoint_generation==original.checkpoint_generation) {
        require(index.find(first.key)==first.replacement && !index.find(rename.back().key) &&
            recovered.read(1)==data(1024,1),"interrupted rename changed acknowledged generation");
        ++old_roots;
      } else {
        require(recovered.info().checkpoint_generation==published.checkpoint_generation && !index.find(first.key) &&
            index.find(rename.back().key)==first.replacement && recovered.read(1)==bytes,
            "published rename or owned data was lost");
        ++new_roots;
      }
      ++cases;
    }
  }
  require(old_roots && new_roots,"failure matrix must exercise both valid generations");
  std::cout<<"rename_fault_cases="<<cases<<" old_roots="<<old_roots<<" new_roots="<<new_roots<<'\n';
}

void recovered_replica_requires_rebuild() {
  const auto ram=empty_image();
  auto writer=fresh(ram);
  const NativeNameEdit first{{1,"kept"},{},20};
  NativeNameIndex::commit(writer,{&first,1});
  const auto snapshot=writer.snapshot();
  const auto root=snapshot.read(kNativeNameIndexObject,40);
  const auto page=snapshot.read(word(root,16,8),4096);
  const auto primary=std::search(ram->live.begin(),ram->live.end(),page.begin(),page.end());
  require(primary!=ram->live.end(),"test name payload copy missing");
  const auto peer=std::search(primary+static_cast<std::ptrdiff_t>(page.size()),
      ram->live.end(),page.begin(),page.end());
  require(peer!=ram->live.end(),"test name payload peer missing");
  *primary^=std::byte{1};
  const NativeNameIndex recovered(snapshot);
  require(recovered.find(first.key)==20 && snapshot.info().degraded_payloads,
      "read must recover a damaged name page from its peer");
  const auto writes=ram->writes;
  const NativeNameEdit next{{1,"pending"},{},21};
  rejects(NativeFsErrorCode::storage_failure,[&]{NativeNameIndex::commit(writer,{&next,1});});
  require(ram->writes==writes && recovered.find(first.key)==20 && !recovered.find(next.key),
      "a recovered replica must not authorize writes");
}
} // namespace

int main() {
  try {
    ordered_names();
    split_merge_and_corrupt_framing();
    interrupted_rename();
    recovered_replica_requires_rebuild();
  } catch (const std::exception& error) {
    std::cerr<<error.what()<<'\n';
    return 1;
  }
}
