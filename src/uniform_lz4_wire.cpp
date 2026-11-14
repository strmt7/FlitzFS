#include "flitzfs/uniform_lz4_wire.hpp"
#include "flitzfs/gpu_lz4_verify.hpp"
#include "record_digest.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>
namespace flitzfs {
namespace {
using Bytes=std::vector<std::byte>;
constexpr std::array<std::byte,8> request_magic{
  std::byte{'F'},std::byte{'L'},std::byte{'F'},std::byte{'S'},
  std::byte{'L'},std::byte{'R'},std::byte{'Q'},std::byte{2}};
constexpr auto reply_magic=[] {auto m=request_magic;m[6]=std::byte{'P'};return m;}();
[[noreturn]] void invalid(const char* why){throw std::invalid_argument(why);}
void put(std::span<std::byte> to,std::size_t at,std::uint64_t value,unsigned width){
  for(unsigned i=0;i<width;++i)to[at+i]=static_cast<std::byte>((value>>(8*i))&255);
}
std::uint64_t get(UniformLz4Bytes from,std::size_t at,unsigned width) noexcept{
  std::uint64_t n=0;for(unsigned i=0;i<width;++i)n|=std::uint64_t(std::to_integer<unsigned>(from[at+i]))<<(8*i);
  return n;
}
std::array<std::byte,16> shape(const uniform_lz4::Layout& p) noexcept{
  std::array<std::byte,16> s{};put(s,0,p.input_bytes,8);put(s,8,p.chunk_bytes,4);put(s,12,p.chunks,4);return s;
}
void owned(const PreparedUniformLz4& batch,std::uint64_t id){
  if(!id || !batch.ready())invalid("invalid uniform LZ4 owned source/identity");
}
void backend(WorkerExecution e){
  if(e!=WorkerExecution::cpu_semantics && e!=WorkerExecution::hip)invalid("unknown uniform LZ4 backend");
}
Bytes header(const PreparedUniformLz4& batch,std::uint64_t id,WorkerExecution e,bool reply,std::uint64_t payload){
  owned(batch,id);Bytes f(kUniformLz4HeaderBytes);
  const auto& magic=reply?reply_magic:request_magic;std::copy(magic.begin(),magic.end(),f.begin());
  put(f,8,id,8);put(f,16,batch.layout().input_bytes,8);put(f,24,batch.layout().chunks,4);
  put(f,28,batch.layout().chunk_bytes,4);put(f,32,reply?static_cast<std::uint32_t>(e):0,4);
  put(f,40,payload,8);std::copy(batch.digest().begin(),batch.digest().end(),f.begin()+48);return f;
}
void framing(UniformLz4Bytes frame,const UniformLz4Limits& limits,bool reply){
  if(!valid_uniform_lz4_limits(limits) || frame.size()<kUniformLz4HeaderBytes ||
      frame.size()>(reply?limits.reply_bytes:limits.request_bytes) ||
      !std::equal((reply?reply_magic:request_magic).begin(),(reply?reply_magic:request_magic).end(),frame.begin()) ||
      !get(frame,8,8) || get(frame,16,8)!=limits.input_bytes || get(frame,24,4)!=limits.chunks ||
      get(frame,28,4)!=limits.chunk_bytes || get(frame,36,4)!=0)
    invalid("invalid uniform LZ4 framing/window");
}
template<class Size,class View>
std::uint64_t verify_parts(const PreparedUniformLz4& batch,std::uint32_t count,
    Size&& size,View&& view){
  if(!batch.ready() || count!=batch.layout().chunks)invalid("uniform LZ4 candidate count");
  const auto capacity=batch.layout().output_stride-uniform_lz4::guard_bytes;
  std::uint64_t total=0;
  for(std::uint32_t i=0;i<count;++i){
    const auto n=size(i);if(!n || n>capacity || !uniform_lz4::add(total,n,total))
      invalid("uniform LZ4 candidate extent");
  }
  if(total>batch.limits().reply_bytes-kUniformLz4HeaderBytes-std::uint64_t(count)*8)
    invalid("uniform LZ4 candidate payload exceeds admitted reply");
  if(!gpu_lz4_cpu_verification_available())throw std::runtime_error("independent CPU LZ4 verification unavailable");
  Bytes scratch(batch.layout().chunk_bytes);
  for(std::uint32_t i=0;i<count;++i){
    const auto input=batch.input(i);
    if(verify_gpu_lz4_candidate(input,view(i),std::span(scratch).first(input.size()))!=GpuLz4Verification::verified)
      invalid("uniform LZ4 candidate does not reproduce immutable source");
  }
  return total;
}
}
bool valid_uniform_lz4_limits(const UniformLz4Limits& l) noexcept{
  uniform_lz4::Layout p{};
  if(!uniform_lz4::make_layout(l.input_bytes,l.chunk_bytes,p) || p.chunks!=l.chunks ||
      p.total_bytes!=l.device_bytes)return false;
  std::uint64_t reply=0,request=0,table=0,payload=0;
  if(!uniform_lz4::add(kUniformLz4HeaderBytes,l.input_bytes,request) ||
      !uniform_lz4::multiply(l.chunks,8,table) ||
      !uniform_lz4::multiply(l.chunks,p.output_stride-uniform_lz4::guard_bytes,payload) ||
      !uniform_lz4::add(kUniformLz4HeaderBytes,table,reply) ||
      !uniform_lz4::add(reply,payload,reply))return false;
  constexpr auto maximum=std::uint64_t((std::numeric_limits<std::int32_t>::max)());
  return l.request_bytes==request && l.reply_bytes==reply && request<=maximum && reply<=maximum &&
      p.total_bytes<=(std::numeric_limits<std::size_t>::max)();
}
UniformLz4Limits uniform_lz4_limits(const uniform_lz4::Layout& p){
  if(!uniform_lz4::valid_layout(p,p.total_bytes))invalid("invalid uniform LZ4 layout");
  UniformLz4Limits l{p.input_bytes,p.total_bytes,kUniformLz4HeaderBytes+p.input_bytes,
      kUniformLz4HeaderBytes+std::uint64_t(p.chunks)*(8+p.output_stride-uniform_lz4::guard_bytes),
      p.chunk_bytes,p.chunks};
  if(!valid_uniform_lz4_limits(l))invalid("uniform LZ4 transport representation exceeded");return l;
}
void PreparedUniformLz4::swap(PreparedUniformLz4& other) noexcept{
  std::swap(layout_,other.layout_);std::swap(limits_,other.limits_);
  source_.swap(other.source_);digest_.swap(other.digest_);
}
PreparedUniformLz4::PreparedUniformLz4(PreparedUniformLz4&& other) noexcept{swap(other);}
PreparedUniformLz4& PreparedUniformLz4::operator=(PreparedUniformLz4&& other) noexcept{
  if(this!=&other){PreparedUniformLz4 replacement(std::move(other));swap(replacement);}return *this;
}
bool PreparedUniformLz4::ready() const noexcept{
  return !source_.empty() && source_.size()==layout_.input_bytes && uniform_lz4::valid_layout(layout_,limits_.device_bytes);
}
PreparedUniformLz4 PreparedUniformLz4::prepare(UniformLz4Bytes input,const UniformLz4Limits& limits){
  if(!valid_uniform_lz4_limits(limits) || input.size()!=limits.input_bytes)invalid("uniform LZ4 source not admitted");
  PreparedUniformLz4 batch;batch.limits_=limits;
  if(!uniform_lz4::make_layout(input.size(),limits.chunk_bytes,batch.layout_))invalid("uniform LZ4 layout refused");
  batch.source_.assign(input.begin(),input.end());batch.digest_=detail::record_digest(shape(batch.layout_),batch.source_);
  return batch;
}
UniformLz4Bytes PreparedUniformLz4::input(std::uint32_t i) const{
  uniform_lz4::Chunk part{};
  if(!ready() || !uniform_lz4::chunk(layout_,i,part))invalid("uniform LZ4 source index");
  return std::span(source_).subspan(std::size_t(i)*layout_.chunk_bytes,part.input_bytes);
}
Bytes encode_uniform_lz4_request(const PreparedUniformLz4& batch,std::uint64_t id){
  auto f=header(batch,id,WorkerExecution::cpu_semantics,false,batch.layout().input_bytes);
  f.reserve(static_cast<std::size_t>(batch.limits().request_bytes));f.insert(f.end(),batch.plaintext().begin(),batch.plaintext().end());return f;
}
UniformLz4Limits inspect_uniform_lz4_request(UniformLz4Bytes frame,std::uint64_t request_capacity,
    std::uint64_t reply_capacity){
  if(frame.size()<kUniformLz4HeaderBytes || frame.size()!=request_capacity)
    invalid("uniform request capacity mismatch");
  uniform_lz4::Layout p{};
  if(!uniform_lz4::make_layout(get(frame,16,8),static_cast<std::uint32_t>(get(frame,28,4)),p))
    invalid("uniform request layout not representable");
  const auto limits=uniform_lz4_limits(p);
  if(limits.request_bytes!=request_capacity || limits.reply_bytes!=reply_capacity)
    invalid("uniform request exceeds independent channel capacities");
  framing(frame,limits,false);return limits;
}
UniformLz4Request decode_uniform_lz4_request(UniformLz4Bytes frame,const UniformLz4Limits& limits){
  framing(frame,limits,false);
  if(get(frame,32,4)!=0 || get(frame,40,8)!=limits.input_bytes || frame.size()!=limits.request_bytes)
    invalid("invalid uniform LZ4 request extent");
  uniform_lz4::Layout p{};(void)uniform_lz4::make_layout(limits.input_bytes,limits.chunk_bytes,p);
  const auto digest=detail::record_digest(shape(p),frame.subspan(kUniformLz4HeaderBytes));
  if(!std::equal(digest.begin(),digest.end(),frame.begin()+48))invalid("uniform LZ4 source digest");
  auto batch=PreparedUniformLz4::prepare(frame.subspan(kUniformLz4HeaderBytes),limits);
  if(batch.digest()!=digest)invalid("uniform LZ4 source changed during copy");
  return {get(frame,8,8),std::move(batch)};
}
void verify_uniform_lz4_candidates(const PreparedUniformLz4& batch,const UniformLz4Candidates& c){
  if(!batch.ready() || c.sizes.size()!=batch.layout().chunks)invalid("uniform LZ4 candidate directory");
  std::uint64_t total=0;
  for(auto n:c.sizes)if(!uniform_lz4::add(total,n,total) || total>c.payload.size())invalid("uniform LZ4 payload directory");
  if(total!=c.payload.size())invalid("non-dense uniform LZ4 candidate payload");
  std::size_t at=0;
  (void)verify_parts(batch,batch.layout().chunks,[&](auto i){return c.sizes[i];},
      [&](auto i){auto s=std::span(c.payload).subspan(at,c.sizes[i]);at+=c.sizes[i];return s;});
}
Bytes encode_uniform_lz4_reply(const PreparedUniformLz4& batch,std::uint64_t id,
    const UniformLz4Candidates& c,WorkerExecution e){
  owned(batch,id);backend(e);verify_uniform_lz4_candidates(batch,c);
  auto f=header(batch,id,e,true,c.payload.size());
  const auto directory=std::size_t(batch.layout().chunks)*8;
  f.resize(kUniformLz4HeaderBytes+directory);
  for(std::uint32_t i=0;i<batch.layout().chunks;++i){
    put(f,kUniformLz4HeaderBytes+8ULL*i,batch.input(i).size(),4);
    put(f,kUniformLz4HeaderBytes+8ULL*i+4,c.sizes[i],4);
  }
  f.insert(f.end(),c.payload.begin(),c.payload.end());return f;
}
void verify_uniform_lz4_reply(UniformLz4Bytes f,const PreparedUniformLz4& batch,
    std::uint64_t id,WorkerExecution e){
  owned(batch,id);backend(e);framing(f,batch.limits(),true);
  if(get(f,8,8)!=id || get(f,32,4)!=static_cast<std::uint32_t>(e) ||
      !std::equal(batch.digest().begin(),batch.digest().end(),f.begin()+48))invalid("uniform LZ4 reply identity/backend");
  const auto directory=std::size_t(batch.layout().chunks)*8;
  if(f.size()<kUniformLz4HeaderBytes+directory)invalid("truncated uniform LZ4 directory");
  const auto payload=f.subspan(kUniformLz4HeaderBytes+directory);
  if(payload.size()!=get(f,40,8))invalid("uniform LZ4 payload length");
  std::uint64_t total=0;
  for(std::uint32_t i=0;i<batch.layout().chunks;++i){
    const auto decoded=get(f,kUniformLz4HeaderBytes+8ULL*i,4);
    const auto stored=get(f,kUniformLz4HeaderBytes+8ULL*i+4,4);
    if(decoded!=batch.input(i).size() || !stored || stored>batch.layout().output_stride-uniform_lz4::guard_bytes ||
        !uniform_lz4::add(total,stored,total) || total>payload.size())invalid("uniform LZ4 reply block extent");
  }
  if(total!=payload.size())invalid("non-dense uniform LZ4 reply");
  std::size_t at=0;
  const auto size=[&](auto i){return static_cast<std::uint32_t>(get(f,kUniformLz4HeaderBytes+8ULL*i+4,4));};
  (void)verify_parts(batch,batch.layout().chunks,size,[&](auto i){
    const auto n=size(i);auto s=payload.subspan(at,n);at+=n;return s;});
}
UniformLz4Candidates accept_uniform_lz4_reply(UniformLz4Bytes f,const PreparedUniformLz4& batch,
    std::uint64_t id,WorkerExecution e){
  verify_uniform_lz4_reply(f,batch,id,e);
  const auto directory=std::size_t(batch.layout().chunks)*8;
  const auto payload=f.subspan(kUniformLz4HeaderBytes+directory);
  UniformLz4Candidates result;result.sizes.reserve(batch.layout().chunks);
  for(std::uint32_t i=0;i<batch.layout().chunks;++i)
    result.sizes.push_back(static_cast<std::uint32_t>(get(f,kUniformLz4HeaderBytes+8ULL*i+4,4)));
  result.payload.assign(payload.begin(),payload.end());return result;
}
Bytes uniform_lz4_upload(const PreparedUniformLz4& batch){
  if(!batch.ready())invalid("uniform LZ4 upload source");
  const auto& p=batch.layout();Bytes slab(static_cast<std::size_t>(p.total_bytes));
  std::copy(batch.plaintext().begin(),batch.plaintext().end(),slab.begin()+static_cast<std::ptrdiff_t>(p.input_offset));
  for(std::uint32_t i=0;i<p.chunks;++i){
    const auto guard=p.output_offset+std::uint64_t(i)*p.output_stride+p.output_stride-uniform_lz4::guard_bytes;
    std::fill_n(slab.begin()+static_cast<std::ptrdiff_t>(guard),uniform_lz4::guard_bytes,std::byte{0xa7});
  }
  return slab;
}
UniformLz4Candidates verify_uniform_lz4_readback(const PreparedUniformLz4& batch,UniformLz4Bytes slab){
  if(!batch.ready() || slab.size()!=batch.layout().total_bytes)invalid("uniform LZ4 readback size");
  const auto& p=batch.layout();
  if(!std::equal(batch.plaintext().begin(),batch.plaintext().end(),slab.begin()+static_cast<std::ptrdiff_t>(p.input_offset)) ||
      !std::all_of(slab.begin()+static_cast<std::ptrdiff_t>(p.input_offset+p.input_bytes),
          slab.begin()+static_cast<std::ptrdiff_t>(p.output_offset),[](auto b){return b==std::byte{0};}))
    invalid("uniform LZ4 source/alignment guard changed");
  std::uint64_t total=0;
  for(std::uint32_t i=0;i<p.chunks;++i){
    const auto n=get(slab,std::size_t(i)*8,8);
    const auto start=p.output_offset+std::uint64_t(i)*p.output_stride,guard=start+p.output_stride-uniform_lz4::guard_bytes;
    if(!n || n>p.output_stride-uniform_lz4::guard_bytes ||
        !uniform_lz4::add(total,n,total) ||
        !std::all_of(slab.begin()+static_cast<std::ptrdiff_t>(guard),
            slab.begin()+static_cast<std::ptrdiff_t>(guard+uniform_lz4::guard_bytes),[](auto b){return b==std::byte{0xa7};}))
      invalid("uniform LZ4 output extent/guard");
  }
  (void)verify_parts(batch,p.chunks,[&](auto i){return get(slab,std::size_t(i)*8,8);},
      [&](auto i){return slab.subspan(static_cast<std::size_t>(p.output_offset+std::uint64_t(i)*p.output_stride),
          static_cast<std::size_t>(get(slab,std::size_t(i)*8,8)));});
  UniformLz4Candidates result;result.sizes.reserve(p.chunks);result.payload.reserve(static_cast<std::size_t>(total));
  for(std::uint32_t i=0;i<p.chunks;++i){
    const auto n=static_cast<std::uint32_t>(get(slab,std::size_t(i)*8,8));result.sizes.push_back(n);
    const auto bytes=slab.subspan(static_cast<std::size_t>(p.output_offset+std::uint64_t(i)*p.output_stride),n);
    result.payload.insert(result.payload.end(),bytes.begin(),bytes.end());
  }
  return result;
}
}
