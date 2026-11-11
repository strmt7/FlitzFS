#include "flitzfs/compression_probe.hpp"
#include "native_fs_fixture.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
namespace {
using namespace flitzfs;
using native_fixture::Bytes;
unsigned checks=0;
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F&& fn) {
  ++checks;try{fn();}catch(const std::invalid_argument&){return;}
  throw std::runtime_error("invalid probe admission accepted");
}
CompressionProbeSource source(std::size_t bytes) {
  return {native_fixture::identity(),7,11,static_cast<std::uint32_t>(bytes),0};
}
Bytes content() {
  const auto motif=native_fixture::data(5003,42);
  Bytes out(StoreCompressionProbe::input_limit);
  for(std::size_t i=0;i<out.size();++i)out[i]=motif[i%motif.size()];
  return out; // Beyond the exact-pattern codec's 4096-byte motif cap.
}
void admission() {
  const auto bytes=content();const auto good=source(bytes.size());
  for(unsigned field=0;field<6;++field) {
    auto bad=good;
    if(field==0)bad.image={};
    if(field==1)bad.generation=0;
    if(field==2)bad.object_id=0;
    if(field==3)bad.object_bytes=0;
    if(field==4)bad.object_bytes=kStoreMaxObjectBytes+1;
    if(field==5)bad.sample_offset=(std::numeric_limits<std::uint32_t>::max)();
    rejects([&]{(void)StoreCompressionProbe::create(bad,bytes);});
  }
  rejects([&]{(void)StoreCompressionProbe::create(good,{});});
  const Bytes large(StoreCompressionProbe::input_limit+1);
  rejects([&]{(void)StoreCompressionProbe::create(source(large.size()),large);});
  rejects([&]{(void)StoreCompressionProbe::create(good,bytes,StoreCompressionProbe::encoder_memory_limit+1);});
  auto range=good;range.sample_offset=1;
  rejects([&]{(void)StoreCompressionProbe::create(range,bytes);});
  if(!store_zstd_available())
    require(!StoreCompressionProbe::create(good,bytes),"raw-only build created a Zstd probe");
}
void profiles_and_source_binding() {
  const auto bytes=content();const auto key=source(bytes.size());
  auto caller=bytes;
  auto optional=StoreCompressionProbe::create(key,caller);require(optional.has_value(),"available probe missing");
  auto probe=std::move(*optional);
  require(optional->state()==CompressionProbeState::inactive && optional->input().empty(),
      "moved-from probe retained ownership");
  caller.back()^=std::byte{1};
  require(std::ranges::equal(probe.input(),bytes),"caller mutation changed owned sample");
  require(!probe.recommendation(key,bytes),"unfinished probe returned advice");
  for(std::size_t i=0;i<StoreCompressionProbe::levels.size();++i) {
    const auto state=probe.advance();
    require(state==(i+1==StoreCompressionProbe::levels.size()?
        CompressionProbeState::complete:CompressionProbeState::profiling),"probe did not yield one profile");
    const auto rows=probe.measurements();
    require(rows.size()==i+1 && rows.back().requested_level==StoreCompressionProbe::levels[i] &&
        rows.back().logical_bytes==bytes.size() && rows.back().stored_payload_bytes<=bytes.size() &&
        rows.back().encoder_peak_bytes<=StoreCompressionProbe::encoder_memory_limit &&
        !rows.back().used_match_proposals,"invalid measured profile bounds");
    if(state==CompressionProbeState::profiling)
      require(!probe.recommendation(key,bytes),"partial observations authorized advice");
  }
  const auto choice=probe.recommendation(key,bytes);require(choice.has_value(),"completed probe lost advice");
  const auto rows=probe.measurements();
  const auto best=std::min_element(rows.begin(),rows.end(),[](const auto& a,const auto& b) {
    return a.stored_payload_bytes!=b.stored_payload_bytes?a.stored_payload_bytes<b.stored_payload_bytes:
        a.requested_level<b.requested_level;
  });
  require(choice->zstd_level==best->requested_level && choice->require_zstd &&
      choice->mode==StoreCompressionMode::automatic,"advice did not select measured payload minimum");
  const auto count=rows.size();
  require(probe.advance()==CompressionProbeState::complete && probe.measurements().size()==count,
      "terminal probe repeated compression");
  for(unsigned field=0;field<5;++field) {
    auto changed=key;
    if(field==0)changed.image[0]^=std::byte{1};
    if(field==1)++changed.generation;
    if(field==2)++changed.object_id;
    if(field==3)--changed.object_bytes;
    if(field==4)++changed.sample_offset;
    require(!probe.recommendation(changed,bytes),"different source tuple reused advice");
  }
  require(!probe.recommendation(key,caller),"changed bytes reused advice");
  require(!probe.recommendation(key,std::span(bytes).first(bytes.size()-1)),"short sample reused advice");
  auto moved=std::move(probe);
  require(probe.state()==CompressionProbeState::inactive && probe.advance()==CompressionProbeState::inactive &&
      probe.measurements().empty() && !probe.recommendation(key,bytes),"moved-from probe produced advice");
  require(moved.recommendation(key,bytes).has_value(),"move lost completed source-bound advice");
}
void terminal_failure_and_hints() {
  const auto bytes=content();const auto key=source(bytes.size());
  auto failed=StoreCompressionProbe::create(key,bytes,0);
  require(failed->advance()==CompressionProbeState::failed && failed->measurements().empty() &&
      !failed->recommendation(key,bytes) && std::ranges::equal(failed->input(),bytes),
      "allocator rejection produced advice or lost the CPU source");
  require(failed->advance()==CompressionProbeState::failed,"failed probe retried");
  const std::array bad{StoreMatchProposal{1,2,4}};
  auto hints=StoreCompressionProbe::create(key,bytes);
  while(hints->state()==CompressionProbeState::profiling)hints->advance(bad);
  require(hints->state()==CompressionProbeState::complete && hints->recommendation(key,bytes).has_value(),
      "invalid optional hints blocked CPU advice");
  for(const auto& row:hints->measurements())require(!row.used_match_proposals,"invalid hints were accepted");
  const std::array valid{StoreMatchProposal{5003,5003,static_cast<std::uint32_t>(bytes.size()-5003)}};
  auto with_hints=StoreCompressionProbe::create(key,bytes);
  while(with_hints->state()==CompressionProbeState::profiling)with_hints->advance(valid);
  require(with_hints->state()==CompressionProbeState::complete &&
      with_hints->recommendation(key,bytes).has_value(),"valid CPU-checked hints broke the probe");
  for(const auto value:{std::byte{0},std::byte{41}}) {
    const Bytes simple(4096,value);
    auto probe=StoreCompressionProbe::create(source(simple.size()),simple);
    require(probe->advance()==CompressionProbeState::complete && probe->measurements().size()==1 &&
        probe->recommendation(source(simple.size()),simple).has_value(),
        "minimal zero/motif payload caused redundant trials");
  }
}
void existing_recompression_integration() {
  const auto bytes=content();auto original=std::make_shared<native_fixture::Ram>();
  const auto identity=native_fixture::identity();
  {
    auto writer=ObjectStoreWriter::create(original,identity,{StoreCompressionMode::raw,6,false});
    const std::array mutation{StoreMutation{11,false,bytes}};
    writer.commit(mutation);
  }
  auto snapshot=ObjectStoreSnapshot::open(original);
  const CompressionProbeSource key{identity,snapshot.info().generation,11,
      static_cast<std::uint32_t>(bytes.size()),0};
  const auto read=snapshot.read(11);auto probe=StoreCompressionProbe::create(key,read);
  const auto old=original->live;const auto writes=original->writes;const auto flushes=original->flushes;
  while(probe->state()==CompressionProbeState::profiling)probe->advance();
  require(original->live==old && original->writes==writes && original->flushes==flushes,
      "read-only profiling wrote backing storage");
  const auto choice=probe->recommendation(key,snapshot.read(11));require(choice.has_value(),"store sample advice missing");
  auto target=std::make_shared<native_fixture::Ram>();auto next=identity;next[0]^=std::byte{1};
  {
    auto writer=ObjectStoreWriter::recompress(snapshot,target,next,*choice);
    require(writer.snapshot().read(11)==bytes,"recommended profile changed recompressed bytes");
  }
  const auto recovered=ObjectStoreSnapshot::open(target);recovered.verify_all();
  require(recovered.read(11)==bytes && snapshot.read(11)==bytes && original->live==old &&
      original->writes==writes && original->flushes==flushes,"recompression changed or lost the source");
  require(recovered.info().stored_payload_bytes<=snapshot.info().stored_payload_bytes,
      "fixture payload did not improve under measured advice");
  require(original->live.size()+original->stable.size()+target->live.size()+target->stable.size()+old.size()<=8U*1024U*1024U,
      "RAM integration fixture exceeded its cap");
}
}
int main() {
  try {
    admission();
    if(store_zstd_available()){profiles_and_source_binding();terminal_failure_and_hints();existing_recompression_integration();}
    std::cout<<"Compression profile probe: checks="<<checks<<" input_cap="<<StoreCompressionProbe::input_limit
        <<" zstd_available="<<std::boolalpha<<store_zstd_available()<<" gpu_executed=false ordinary_file_io=false\n";return 0;
  }catch(const std::exception& error){std::cerr<<"compression probe failure after "<<checks<<": "<<error.what()<<'\n';return 1;}
}
