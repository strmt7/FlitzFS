#include "native_lz4_decode_wire.hpp"
#include "store_codec.hpp"
#include "native_fs_fixture.hpp"
#include "native_lz4_fixture.hpp"
#include "flitzfs/native_session.hpp"
#include <iterator>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
using namespace flitzfs;
using namespace flitzfs::detail;
using namespace flitzfs::native_lz4_decode;
using Bytes=std::vector<std::byte>;

void require(bool condition,const char* message) {
  if(!condition)throw std::runtime_error(message);
}

template<class Action> void refused(Action&& action,const char* message) {
  try {action();}
  catch(const std::invalid_argument&) {return;}
  throw std::runtime_error(message);
}

void capture_from_indexed_session() {
  const auto io=std::make_shared<native_fixture::Ram>();
  const auto identity=native_fixture::identity();
  const StoreCompressionOptions policy{StoreCompressionMode::fast,1,false};
  const auto create_session=[&](const auto& image) {
    auto writer=NativeFilesystemWriter::initialize_indexed(
        PagedObjectStoreWriter::create_mirrored(image,identity,{4U*1024U*1024U},policy));
    auto session=NativeFileSession::create(std::move(writer));
    session.create_entry("read-ahead");
    return session;
  };
  auto session=create_session(io);
  const auto handle=session.open("read-ahead",{true,true,false});
  auto expected=native_lz4_fixture::source();
  const auto length=static_cast<std::uint32_t>(expected.size());
  if(store_lz4_available()){
    const auto alternate=native_lz4_fixture::alternative_block(expected);
    const NativeLz4ExtentCandidate offered{0,alternate};
    (void)session.write_storage_with_lz4_result(handle,0,expected,{&offered,1});
  }else (void)session.write(handle,0,expected);
  NativeLz4CapturedFrame captured;
  const auto acquired=session.capture_native_lz4(handle,0,kNativeExtentBytes,captured);
  require(acquired==store_lz4_available(),"CPU LZ4 availability and native frame capture differ");
  if(!acquired) {
    require(captured.encoded().empty() && session.read(handle,0,length)==expected,
        "CPU fallback lost its raw-only file");
    std::cout<<"native_lz4_capture raw_fallback_exact=1\n";
    return;
  }
  require(captured.key().image==identity && captured.key().decoded_bytes>length &&
      captured.encoded().size()>1,"read-ahead did not return one complete indexed LZ4 record");
  const auto preserved=captured;
  const auto writes=io->writes;
  require(!session.capture_native_lz4(handle,0,preserved.encoded().size()-1,captured) && captured.encoded().empty() &&
      !session.capture_native_lz4(handle,kNativeExtentBytes,kNativeExtentBytes,captured) && io->writes==writes,
      "budget or EOF allowed a read-ahead frame");
  const Bytes post_gap{std::byte{0x63}};
  (void)session.write(handle,2*kNativeExtentBytes,post_gap);
  require(!session.capture_native_lz4(handle,kNativeExtentBytes,kNativeExtentBytes,captured),
      "read-ahead crossed a sparse hole");

  StoreCodecDecoder decoder;
  const auto prepare=[&](const NativeLz4CapturedFrame& frame,bool indexed) {
    constexpr NativeLz4DecodeGrant grant{1,256U*1024U};
    const NativeLz4DecodeSource source{frame.key(),frame.encoded()};
    const auto exchange=indexed?NativeLz4ReadExchange::prepare_captured({&frame,1},153,grant):
        NativeLz4ReadExchange::prepare({&source,1},153,grant);
    const auto request=decode_native_lz4_request(exchange.request()->bytes(),grant,1);
    const NativeLz4DecodedFrame output{source.key,decoder.decode(kStoreLz4,source.encoded,source.key.decoded_bytes)};
    const auto reply=encode_native_lz4_reply(request,{&output,1},WorkerExecution::cpu_semantics);
    return exchange.accept(reply,WorkerExecution::cpu_semantics);
  };
  auto before=io->read_bytes;
  require(session.read(handle,0,length)==expected,"CPU baseline read failed");
  const auto read_bytes=io->read_bytes-before;
  auto ready=prepare(preserved,true);
  before=io->read_bytes;
  require(session.read(handle,0,length,ready.get())==expected && ready->consumed()==1 &&
      ready->indexed_consumed()==1,"the captured frame did not feed the indexed file read");
  const auto ready_bytes=io->read_bytes-before;
  require(read_bytes>=ready_bytes+preserved.encoded().size(),"an indexed ready hit fetched its compressed payload");

  ready=prepare(preserved,false);
  const auto stored=std::ranges::search(io->live,preserved.encoded());
  require(!stored.empty(),"could not locate the retained fixture payload");
  // Watch the compressed bytes directly. Warm metadata may avoid page reads.
  io->corrupt_read_offset=static_cast<std::uint64_t>(stored.begin()-io->live.begin());
  io->corrupt_read_number=(std::numeric_limits<std::uint64_t>::max)();
  io->matching_reads=0;
  require(session.read(handle,0,length,ready.get())==expected && ready->consumed()==1 &&
      !ready->indexed_consumed() && io->matching_reads>0,
      "a generic ready result bypassed stored verification");
  io->corrupt_read_offset=native_fixture::Ram::no_read_fault;

  // Independent images deliberately reuse the same namespace and object IDs.
  // The paged index must distinguish their stored records even at the same generation.
  auto another=create_session(std::make_shared<native_fixture::Ram>());
  const auto other_handle=another.open("read-ahead",{true,true,false});
  auto other_bytes=expected;
  other_bytes[101]^=std::byte{1};
  (void)another.write(other_handle,0,other_bytes);
  require(another.capture_native_lz4(other_handle,0,kNativeExtentBytes,captured) &&
      captured.key().image==identity && captured.key().object==preserved.key().object &&
      captured.key().generation==preserved.key().generation &&
      captured.key().decoded_digest!=preserved.key().decoded_digest,
      "duplicate identity fixture did not bind a different stored record");
  ready=prepare(preserved,true);
  require(another.read(other_handle,0,length,ready.get())==other_bytes && !ready->consumed(),
      "a copied result matched a different stored digest");

  ready=prepare(preserved,true);
  expected[29]^=std::byte{1};
  (void)session.write(handle,29,std::span(expected).subspan(29,1));
  require(session.read(handle,0,length,ready.get())==expected && !ready->consumed(),
      "a prepared frame from before a write reached a changed file");
  require(session.capture_native_lz4(handle,0,kNativeExtentBytes,captured) && captured.key()!=preserved.key(),
      "the new file extent did not get a fresh capture identity");
  ready=prepare(captured,true);

  // Search the owned synthetic backing for each of the two encoded copies.
  // Only the first copy is damaged, so ordinary demand can use its peer.
  const auto prefix=captured.encoded().first((std::min)(captured.encoded().size(),std::size_t{4096}));
  const auto primary=std::search(io->live.begin(),io->live.end(),prefix.begin(),prefix.end());
  require(primary!=io->live.end() &&
      std::search(std::next(primary),io->live.end(),prefix.begin(),prefix.end())!=io->live.end(),
      "could not locate two copies of the fixture frame");
  *primary^=std::byte{1};
  const auto original_writes=io->writes;
  require(!session.capture_native_lz4(handle,0,kNativeExtentBytes,captured) && captured.encoded().empty() &&
      !session.poisoned() && io->writes==original_writes,"speculation admitted damage or changed writable state");
  require(session.read(handle,0,length)==expected,"ordinary CPU mirror read lost bytes after capture refused");
  require(session.read(handle,0,length,ready.get())==expected && !ready->consumed(),
      "cached bytes displaced known primary-damage recovery");
  std::cout<<"native_lz4_capture owned=1 exact=1 old_key_missed=1 sparse_hole_refused=1 budget_refused=1"
      <<" bad_primary_skipped=1 cpu_recovery_exact=1 digest_miss=1 known_damage_bypassed_cache=1"
      <<" generic_retained_verification=1 index_hit=1 cpu_backing_bytes="<<read_bytes
      <<" ready_backing_bytes="<<ready_bytes<<'\n';
}

void exercise() {
  const std::array<u8,11> encoded{0x1f,'A',1,0,50,0x50,'B','C','D','E','F'};
  constexpr u32 decoded=75;
  require(valid_block(encoded.data(),encoded.size(),decoded),"valid overlapping native LZ4 block");
  for(u32 size=0;size<encoded.size();++size)
    require(!valid_block(encoded.data(),size,decoded),"truncated native LZ4 block accepted");
  for(const auto offset:{u8{0},u8{2},u8{255}}) {
    auto bad=encoded;bad[2]=offset;
    require(!valid_block(bad.data(),bad.size(),decoded),"invalid backreference accepted");
  }
  require(!valid_block(encoded.data(),encoded.size(),decoded-1) &&
      !valid_block(encoded.data(),encoded.size(),decoded+1),"wrong decoded extent accepted");
  auto bad=encoded;bad[4]=255;
  require(!valid_block(bad.data(),bad.size(),decoded),"overflowing match length accepted");
  bad=encoded;bad[5]=0x40;
  require(!valid_block(bad.data(),bad.size()-1,decoded-1),"short final literal sequence accepted");
  require(!valid_layout({0xffffffffU,0xfffffff0U,0xfffffff0U,version},0xffffffffffffffffULL),
      "invalid allocation product accepted");

  const auto input=std::as_bytes(std::span(encoded));
  StoreIdentity id{};id[0]=std::byte{1};
  NativeLz4DecodeSource frame{{id,2,3,decoded,{}},input};
  if(!store_lz4_available()) {
    refused([&]{(void)NativeLz4DecodeBatch::prepare({&frame,1},{1,4096});},
        "raw-only build admitted a read with no CPU decoder");
    std::cout<<"native_lz4_decode framing=passed cpu_decoder=0\n";
    return;
  }
  StoreCodecDecoder cpu;
  const auto plain=cpu.decode(kStoreLz4,input,decoded);
  require(plain.size()==decoded && plain.front()==std::byte{'A'} && plain.back()==std::byte{'F'},
      "independent LZ4 reference differs");
  frame.key.decoded_digest=record_digest(plain);
  refused([&]{(void)NativeLz4DecodeBatch::prepare({&frame,1},{1,64});},"overgrant slab allocated");
  const std::array frames{frame,frame};
  refused([&]{(void)NativeLz4DecodeBatch::prepare(frames,{1,4096});},"overgrant batch allocated");
  const auto batch=NativeLz4DecodeBatch::prepare(frames,{2,4096});
  const auto layout=batch.layout();
  const auto upload=batch.upload_prefix();
  require(upload.size()==output_at(layout,0) && upload.size()<batch.slab_bytes(),
      "native LZ4 upload retained the output bank");
  Bytes completed(batch.slab_bytes(),kNativeLz4DecodeGuard);
  std::ranges::copy(upload,completed.begin());
  for(u32 i=0;i<layout.chunks;++i) {
    const Record result{static_cast<u32>(encoded.size()),decoded,decoded,0};
    std::memcpy(completed.data()+i*sizeof(Record),&result,sizeof(result));
    std::ranges::copy(plain,completed.begin()+static_cast<std::ptrdiff_t>(output_at(layout,i)));
  }
  const auto accepted=batch.verify(completed);
  require(accepted.size()==2 && accepted[0].bytes==plain && accepted[1].key==frame.key,
      "verified exact native frames changed");
  for(const auto offset:{std::uint64_t{12},output_at(layout,0),output_at(layout,1)+decoded}) {
    auto changed=completed;changed[static_cast<std::size_t>(offset)]^=std::byte{1};
    refused([&]{(void)batch.verify(changed);},"changed completion metadata, byte or guard accepted");
  }
  refused([&]{(void)batch.verify(std::span(completed).first(completed.size()-1));},"short slab accepted");
  auto other=frame;
  ++other.key.object;
  const std::array reading{frame,other};
  const auto exchange=NativeLz4ReadExchange::prepare(reading,37,{2,4096});
  const auto request=decode_native_lz4_request(exchange.request()->bytes(),{2,4096},1);
  require(request.id==37 && request.reply_bytes==exchange.reply_capacity() &&
      request.keys.size()==2 && request.batches.size()==2,"native LZ4 request lost identity or batch limits");
  const std::array output{NativeLz4DecodedFrame{frame.key,plain},NativeLz4DecodedFrame{other.key,plain}};
  const auto reply=encode_native_lz4_reply(request,output,WorkerExecution::cpu_semantics);
  auto ready=exchange.accept(reply,WorkerExecution::cpu_semantics);
  auto stale=frame.key;++stale.generation;
  require(!ready->try_ready(stale,{}) && ready->try_ready(frame.key,{})==plain &&
      !ready->try_ready(frame.key,{}),"native LZ4 result was stale, wrong or accepted twice");
  ready->reject(other.key);
  require(!ready->try_ready(other.key,{}),"rejected ready results remained available");
  for(const auto at:{std::size_t{8},std::size_t{20},std::size_t{32},std::size_t{64}}) {
    auto corrupt=reply;corrupt[at]^=std::byte{1};
    refused([&]{(void)exchange.accept(corrupt,WorkerExecution::cpu_semantics);},
        "changed result id, role, request digest or bytes accepted");
  }
  refused([&]{(void)exchange.accept(reply,WorkerExecution::hip);},"CPU result counted as GPU work");
  refused([&]{(void)decode_native_lz4_request(exchange.request()->bytes(),{1,4096},1);},
      "overgrant frame count accepted");
  refused([&]{(void)NativeLz4ReadExchange::prepare(frames,37,{2,4096});},"duplicate read key accepted");
  auto malformed=Bytes(exchange.request()->bytes().begin(),exchange.request()->bytes().end());
  malformed[64+36]=std::byte{0xff};
  refused([&]{(void)decode_native_lz4_request(malformed,{2,4096},1);},"bad frame extent accepted");
  std::cout<<"native_lz4_wire exact_request_reply_and_one_shot_ready=1\n";
  std::cout<<"native_lz4_decode framing=passed cpu_decoder=1 frames=2 checked_status_digest_and_guards=1\n";
}
}

int main() {
  try {exercise();capture_from_indexed_session();}
  catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
