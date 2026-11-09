#include "flitzfs/object_store.hpp"
#include "store_file.hpp"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs=std::filesystem;
using namespace flitzfs;
using Bytes=std::vector<std::byte>;
void require(bool value,const char* reason) {if(!value) throw std::runtime_error(reason);}
template<class F> void rejects(F&& f,const char* reason) {
  try {f();}catch(const std::exception&){return;}throw std::runtime_error(reason);
}
struct Fixture {
  fs::path root;std::vector<fs::path> owned;
  Fixture() {
    const auto token=std::chrono::steady_clock::now().time_since_epoch().count();
    for(unsigned i=0;i<64;++i) {
      const auto p=fs::temp_directory_path()/("flitzfs-store-owned-"+std::to_string(token)+"-"+std::to_string(i));
      if(fs::create_directory(p)) {root=p;break;}
    }
    if(root.empty()) throw std::runtime_error("cannot create exclusive fixture");
  }
  fs::path file(const std::string& name) {auto p=root/name;owned.push_back(p);return p;}
  ~Fixture() {
    for(const auto& p:owned) {std::error_code ec;fs::remove(p,ec);}
    std::error_code ec;fs::remove(root,ec); // Nonrecursive; exclusively created fixture.
  }
};
Bytes content(std::size_t n) {Bytes b(n);for(std::size_t i=0;i<n;++i)b[i]=static_cast<std::byte>(i%251);return b;}
Bytes file_bytes(const fs::path& path) {
  const auto n=fs::file_size(path);require(n<=2*1024*1024,"fixture read budget");
  Bytes bytes(static_cast<std::size_t>(n));std::ifstream in(path,std::ios::binary);
  if(!bytes.empty())in.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
  require(bool(in),"fixture read failure");return bytes;
}
StoreIdentity id() {StoreIdentity value{};value[0]=std::byte{43};return value;}
}
int main() {
  try {
    const auto* actions=std::getenv("GITHUB_ACTIONS");const auto* environment=std::getenv("RUNNER_ENVIRONMENT");
    require(actions && std::string(actions)=="true" && environment && std::string(environment)=="github-hosted",
            "file-I/O tests require a disposable GitHub-hosted VM; never the owner PC");
    Fixture fixture;
    const auto backing=fixture.file("new-store.fltx");
    const auto original=content(8193),replacement=content(32769);
    {
      // The test declares a protocol assumption; this is not hardware power-loss qualification.
      auto writer=ObjectStoreWriter::create_file(backing,id(),{4096,true});
      const StoreMutation first{1,false,original};writer.commit({&first,1});
      auto snapshot=writer.snapshot();require(snapshot.read(1)==original,"file-backed byte parity");
      const std::array<StoreMutation,2> next{{{1,false,replacement},{2,false,{}}}};
      writer.commit(next);
      require(snapshot.read(1)==original,"file-backed immutable snapshot");
      require(writer.snapshot().read(1)==replacement,"file-backed replacement");
    }
    const auto physical=fs::file_size(backing);
    {
      auto independent=ObjectStoreSnapshot::open_file(backing);
      require(independent.read(1)==replacement && independent.read(2).empty(),"independent CPU file recovery");
      independent.verify_all();
    }
    rejects([&]{(void)ObjectStoreWriter::create_file(backing,id(),{4096,true});},"existing backing overwritten");
    require(fs::file_size(backing)==physical,"existing backing extent changed");
    require(ObjectStoreSnapshot::open_file(backing).read(1)==replacement,"existing backing bytes changed");
    const auto file_bytes_before=file_bytes(backing);
    {
      auto backend=detail::reopen_store_file_for_hosted_test(backing,{4096,true});
      auto wrong=id();wrong[1]=std::byte{1};
      rejects([&]{(void)ObjectStoreWriter::resume(backend,wrong,2);},"wrong image identity resumed");
      rejects([&]{(void)ObjectStoreWriter::resume(backend,id(),1);},"stale image generation resumed");
      Bytes leased_bytes(static_cast<std::size_t>(backend->extent()));backend->read_exact(0,leased_bytes);
      require(leased_bytes==file_bytes_before,"rejected identity/generation changed bytes");
      auto writer=ObjectStoreWriter::resume(backend,id(),2);
      rejects([&]{(void)detail::reopen_store_file_for_hosted_test(backing,{4096,true});},"second OS writer acquired backing");
      rejects([&]{(void)ObjectStoreSnapshot::open_file(backing);},"store reader bypassed OS writer exclusion");
      auto retained=writer.snapshot();
      const StoreMutation added{3,false,Bytes(65537)};
      require(writer.commit({&added,1}).generation==3,"resumed file commit failed");
      rejects([&]{(void)retained.read(3);},"old file snapshot acquired new object");
      require(retained.read(1)==replacement,"old file snapshot bytes changed");
    }
    {
      auto recovered=ObjectStoreSnapshot::open_file(backing);
      require(recovered.info().generation==3 && recovered.read(1)==replacement &&
              recovered.read(2).empty() && recovered.read(3)==Bytes(65537),"resumed file independently reopened incorrectly");
    }

    const auto copied_path=fixture.file("compacted-store.fltx");
    const auto immutable_source=file_bytes(backing);
    auto copy_id=id();copy_id[1]=std::byte{97};
    {
      auto source=ObjectStoreSnapshot::open_file(backing);
      auto second_reader=ObjectStoreSnapshot::open_file(backing);
      rejects([&]{(void)detail::reopen_store_file_for_hosted_test(backing,{4096,true});},"shared store reader allowed writer");
      {
        auto copy=ObjectStoreWriter::compact(source,detail::new_store_file(copied_path,{4096,true}),copy_id);
        require(copy.snapshot().identity()==copy_id && copy.snapshot().read(1)==replacement &&
            copy.snapshot().read(3)==Bytes(65537),"ordinary-file compacted copy mismatch");
      }
      auto reopened=ObjectStoreSnapshot::open_file(copied_path);reopened.verify_all();
      require(reopened.info().generation==source.info().generation+2 && second_reader.read(1)==replacement,"compacted file roots/source reader");
      rejects([&]{(void)detail::new_store_file(copied_path,{4096,true});},"compaction target path overwritten");
    }
    require(file_bytes(backing)==immutable_source,"ordinary-file compaction changed source");
    {
      auto backend=detail::reopen_store_file_for_hosted_test(backing,{4096,true});
      auto writer=ObjectStoreWriter::resume(backend,id(),3);
      require(writer.snapshot().read(1)==replacement,"shared source reader did not release OS exclusion");
    }
    const auto alias=fixture.file("alias.fltx");fs::create_hard_link(backing,alias);
    const auto linked_before=file_bytes(backing);
    rejects([&]{(void)detail::reopen_store_file_for_hosted_test(alias,{4096,true});},"hard-linked writable image admitted");
    require(file_bytes(backing)==linked_before,"hard-link rejection changed image");
    require(fs::remove(alias),"cannot remove owned hard-link fixture");
    const auto missing=fixture.file("missing.fltx");
    rejects([&]{(void)detail::reopen_store_file_for_hosted_test(missing,{4096,true});},"reopen created a missing image");
    require(!fs::exists(missing),"failed reopen created a path");
    const auto sentinel=fixture.file("sentinel");
    {std::ofstream out(sentinel,std::ios::binary);out<<"do not replace";require(bool(out),"sentinel creation");}
    rejects([&]{(void)ObjectStoreWriter::create_file(sentinel,id(),{4096,true});},"sentinel overwritten");
    {
      auto unknown=detail::reopen_store_file_for_hosted_test(sentinel,{4096,true});
      rejects([&]{(void)ObjectStoreWriter::resume(unknown,id(),0);},"non-store file accepted for resume");
    }
    {std::ifstream in(sentinel,std::ios::binary);std::string text;std::getline(in,text);require(text=="do not replace","sentinel contents changed");}
    const auto invalid=fixture.file("invalid-id.fltx");
    rejects([&]{(void)ObjectStoreWriter::create_file(invalid,{}, {4096,true});},"zero identity accepted");
    require(!fs::exists(invalid),"invalid arguments created a file");
    const auto bad_policy=fixture.file("invalid-policy.fltx");
    rejects([&]{(void)ObjectStoreWriter::create_file(bad_policy,id(),{4096,true},
        {StoreCompressionMode::automatic,0,false});},"invalid compression policy accepted");
    require(!fs::exists(bad_policy),"invalid compression policy created a file");
    const auto unknown=fixture.file("unknown-contract.fltx");
    rejects([&]{(void)ObjectStoreWriter::create_file(unknown,id(),{});},"unknown durability contract accepted");
    require(!fs::exists(unknown),"unknown contract created a file");
#ifndef _WIN32
    const auto link=fixture.file("existing-link.fltx");fs::create_symlink(sentinel,link);
    rejects([&]{(void)ObjectStoreWriter::create_file(link,id(),{4096,true});},"final link followed");
    rejects([&]{(void)detail::reopen_store_file_for_hosted_test(link,{4096,true});},"reopen followed a final symlink");
    {std::ifstream in(sentinel);std::string text;std::getline(in,text);require(text=="do not replace","link target modified");}
#endif
    std::cout<<"PASS: hosted-only owned store, qualified resume, OS writer exclusion, snapshots and no-clobber checks; no mount/GPU.\n";
    return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
