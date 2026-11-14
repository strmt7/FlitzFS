#include "store_codec.hpp"
#include "zstd_frame.hpp"
#include "repeat_bytes.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#ifdef FLITZFS_HAS_LZ4
#include <lz4.h>
#endif
#ifdef FLITZFS_STORE_HAS_ZSTD
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
static_assert(ZSTD_VERSION_NUMBER>=10504,"Zstandard 1.5.4 or newer required");
#endif

namespace flitzfs {
bool store_lz4_available() noexcept {
#ifdef FLITZFS_HAS_LZ4
  return LZ4_versionNumber()>=11101;
#else
  return false;
#endif
}
std::uint32_t store_zstd_version() noexcept {
#ifdef FLITZFS_STORE_HAS_ZSTD
  return ZSTD_versionNumber();
#else
  return 0;
#endif
}
bool store_zstd_available() noexcept {return store_zstd_version()>=10504;}
namespace detail {
namespace {
using Bytes=std::vector<std::byte>;
[[noreturn]] void fail(const char* message) {
  throw std::runtime_error(std::string("FlitzFS store codec: ")+message);
}
#ifdef FLITZFS_STORE_HAS_ZSTD
// Every library allocation (including headers) is counted before malloc.
// Context reuse avoids allocator churn; default single-threaded operation only.
struct Budget {
  std::size_t limit=0,used=0,peak=0;
  struct alignas(std::max_align_t) Header {std::size_t bytes;};
  static void* allocate(void* opaque,std::size_t size) noexcept {
    auto& b=*static_cast<Budget*>(opaque);
    if(b.used>b.limit || size>b.limit-b.used ||
        sizeof(Header)>b.limit-b.used-size) return nullptr;
    const auto total=size+sizeof(Header);
    auto* header=static_cast<Header*>(std::malloc(total));
    if(!header) return nullptr;
    header->bytes=total;b.used+=total;b.peak=(std::max)(b.peak,b.used);
    return header+1;
  }
  static void release(void* opaque,void* pointer) noexcept {
    if(!pointer) return;
    auto& b=*static_cast<Budget*>(opaque);
    auto* header=static_cast<Header*>(pointer)-1;
    b.used-=header->bytes;std::free(header);
  }
  ZSTD_customMem callbacks() noexcept {return {allocate,release,this};}
};
void checked(std::size_t code,const char* operation) {if(ZSTD_isError(code))fail(operation);}
#endif
// KMP over at most 8192 sampled bytes proposes the shortest period <=4096;
// verify EVERY input byte before admitting it. No probabilistic equality.
std::size_t pattern_period(std::span<const std::byte> input) {
  if(input.size()<2) return 0;
  std::array<std::uint16_t,8192> prefix{};
  const auto n=(std::min)(input.size(),prefix.size());
  for(std::size_t i=1;i<n;++i) {
    auto j=std::size_t(prefix[i-1]);
    while(j && input[i]!=input[j]) j=prefix[j-1];
    if(input[i]==input[j]) ++j;
    prefix[i]=static_cast<std::uint16_t>(j);
  }
  const auto period=n-prefix[n-1];
  if(!period || period>kStoreMaxPatternBytes || period>=input.size()) return 0;
  if(!std::equal(input.begin()+static_cast<std::ptrdiff_t>(period),input.end(),input.begin())) return 0;
  return period;
}
}
bool store_match_sequences_available() noexcept {
#ifdef FLITZFS_STORE_HAS_SEQUENCE_API
  return ZSTD_versionNumber()==ZSTD_VERSION_NUMBER;
#else
  return false;
#endif
}
void validate_store_compression(StoreCompressionOptions options) {
  if((options.mode!=StoreCompressionMode::raw && options.mode!=StoreCompressionMode::automatic && options.mode!=StoreCompressionMode::fast) ||
      options.zstd_level<1 || options.zstd_level>9 ||
      (options.require_zstd && (options.mode!=StoreCompressionMode::automatic || !store_zstd_available())))
    fail("invalid policy or required bounded Zstandard unavailable");
}
bool store_codec_framing(std::uint32_t codec,std::uint32_t logical,std::uint32_t stored) noexcept {
  if(logical>kStoreMaxObjectBytes || stored>logical) return false;
  switch(codec) {
    case kStoreRaw:return stored==logical;
    case kStoreZero:return logical>0 && stored==0;
    case kStorePattern:return stored>0 && stored<=kStoreMaxPatternBytes && stored<logical;
    case kStoreLz4:return logical>0 && stored>0 && stored<logical;
    case kStoreZstd:return stored>0 && stored<logical;
    default:return false;
  }
}
struct StoreCodecDecoder::Impl {
  explicit Impl(std::size_t cap) {
#ifdef FLITZFS_STORE_HAS_ZSTD
    budget.limit=cap;
#else
    (void)cap;
#endif
  }
#ifdef FLITZFS_STORE_HAS_ZSTD
  Budget budget{kStoreDecoderMemoryCap};
  std::unique_ptr<ZSTD_DCtx,decltype(&ZSTD_freeDCtx)> context{nullptr,&ZSTD_freeDCtx};
#endif
};
struct StoreCodecEncoder::Impl {
  StoreCompressionOptions options;
  StoreCodecDecoder verifier;
#ifdef FLITZFS_STORE_HAS_ZSTD
  Budget budget{kStoreEncoderMemoryCap};
  std::unique_ptr<ZSTD_CCtx,decltype(&ZSTD_freeCCtx)> context{nullptr,&ZSTD_freeCCtx};
#endif
  std::size_t allocation_limit=0,lz4_peak=0;
  std::vector<std::max_align_t> lz4_state;
  explicit Impl(StoreCompressionOptions o,std::size_t cap):options(o),allocation_limit(cap){
#ifdef FLITZFS_STORE_HAS_ZSTD
    budget.limit=cap;
#else
    (void)cap;
#endif
  }
#ifdef FLITZFS_STORE_HAS_ZSTD
  ZSTD_CCtx* prepare_context() {
    if(!context) {
      context.reset(ZSTD_createCCtx_advanced(budget.callbacks()));
      if(!context) fail("compressor memory admission");
    }
    auto* cctx=context.get();
    checked(ZSTD_CCtx_reset(cctx,ZSTD_reset_session_and_parameters),"compressor reset");
    checked(ZSTD_CCtx_setParameter(cctx,ZSTD_c_compressionLevel,options.zstd_level),"compression level");
    checked(ZSTD_CCtx_setParameter(cctx,ZSTD_c_windowLog,20),"compression window");
    checked(ZSTD_CCtx_setParameter(cctx,ZSTD_c_hashLog,18),"compression hash table");
    checked(ZSTD_CCtx_setParameter(cctx,ZSTD_c_chainLog,18),"compression chain table");
    checked(ZSTD_CCtx_setParameter(cctx,ZSTD_c_nbWorkers,0),"single-threaded compression");
    checked(ZSTD_CCtx_setParameter(cctx,ZSTD_c_contentSizeFlag,1),"compression size field");
    checked(ZSTD_CCtx_setParameter(cctx,ZSTD_c_checksumFlag,1),"compression checksum");
    return cctx;
  }
#endif
};
StoreCodecDecoder::StoreCodecDecoder(std::size_t cap) {
  if(cap>kStoreDecoderMemoryCap) fail("decoder allocation cap exceeds hard ceiling");
  impl_=std::make_unique<Impl>(cap);
}
StoreCodecDecoder::~StoreCodecDecoder()=default;
StoreCodecEncoder::StoreCodecEncoder(StoreCompressionOptions options,std::size_t cap) {
  validate_store_compression(options);
  if(cap>kStoreEncoderMemoryCap) fail("encoder allocation cap exceeds hard ceiling");
  impl_=std::make_unique<Impl>(options,cap);
}
StoreCodecEncoder::~StoreCodecEncoder()=default;
std::size_t StoreCodecDecoder::peak_bytes() const noexcept {
#ifdef FLITZFS_STORE_HAS_ZSTD
  return impl_->budget.peak;
#else
  return 0;
#endif
}
std::size_t StoreCodecEncoder::peak_bytes() const noexcept {
#ifdef FLITZFS_STORE_HAS_ZSTD
  return impl_->budget.peak+impl_->lz4_peak;
#else
  return impl_->lz4_peak;
#endif
}
std::vector<std::byte> StoreCodecDecoder::decode(std::uint32_t codec,
    std::span<const std::byte> payload,std::uint32_t logical) {
  if(payload.size()>(std::numeric_limits<std::uint32_t>::max)() ||
      !store_codec_framing(codec,logical,static_cast<std::uint32_t>(payload.size())))
    fail("unknown codec or invalid stored/logical extent");
  if(codec==kStoreZstd && !native_zstd_frame(payload)) fail("noncanonical Zstandard magic");
  if(codec==kStoreZstd && !store_zstd_available()) fail("Zstandard recovery unavailable");
  if(codec==kStoreLz4 && !store_lz4_available()) fail("LZ4 recovery unavailable");
  Bytes output(logical);
  if(codec==kStoreRaw) std::copy(payload.begin(),payload.end(),output.begin());
  else if(codec==kStoreZero) {} // Value-initialized bytes are zero.
  else if(codec==kStorePattern) repeat_bytes(payload,output);
  else if(codec==kStoreLz4) {
#ifdef FLITZFS_HAS_LZ4
    const auto n=LZ4_decompress_safe(reinterpret_cast<const char*>(payload.data()),
        reinterpret_cast<char*>(output.data()),static_cast<int>(payload.size()),static_cast<int>(logical));
    if(n<0 || static_cast<std::uint32_t>(n)!=logical)fail("LZ4 decoding failed");
#else
    fail("LZ4 recovery unavailable");
#endif
  } else {
#ifdef FLITZFS_STORE_HAS_ZSTD
    // Reject concatenation, skippable/unknown-size frames and dictionaries.
    const auto frame=ZSTD_findFrameCompressedSize(payload.data(),payload.size());
    const auto content=ZSTD_getFrameContentSize(payload.data(),payload.size());
    if(ZSTD_isError(frame) || frame!=payload.size() || content!=logical ||
        ZSTD_getDictID_fromFrame(payload.data(),payload.size())!=0) fail("noncanonical Zstandard frame");
    if(!impl_->context) {
      impl_->context.reset(ZSTD_createDCtx_advanced(impl_->budget.callbacks()));
      if(!impl_->context) fail("decoder memory admission");
    }
    // Single-pass output is explicitly bounded; streaming window limits do not
    // apply here. The custom allocator independently caps all library memory.
    const auto n=ZSTD_decompressDCtx(impl_->context.get(),output.data(),output.size(),
                                   payload.data(),payload.size());
    if(ZSTD_isError(n) || n!=output.size()) fail("Zstandard decoding failed");
#else
    fail("Zstandard recovery unavailable");
#endif
  }
  return output;
}
EncodedStoreObject StoreCodecEncoder::encode(Bytes input) {
  return encode_impl(std::move(input),true);
}
EncodedStoreObject StoreCodecEncoder::encode_impl(Bytes input,bool cpu_fast) {
  if(input.size()>kStoreMaxObjectBytes) fail("object exceeds codec limit");
  EncodedStoreObject result;
  if(impl_->options.mode==StoreCompressionMode::raw || input.empty()) {
    result.payload=std::move(input);return result;
  }
  if(std::all_of(input.begin(),input.end(),[](auto b){return b==std::byte{0};})) result.codec=kStoreZero;
  else if(const auto period=pattern_period(input)) {
    result.codec=kStorePattern;
    result.payload.assign(input.begin(),input.begin()+static_cast<std::ptrdiff_t>(period));
  }
#ifdef FLITZFS_STORE_HAS_ZSTD
  // Zero and a one-byte motif cannot be improved by a complete Zstandard frame.
  if(impl_->options.mode==StoreCompressionMode::automatic && store_zstd_available() && result.codec!=kStoreZero && !(result.codec==kStorePattern && result.payload.size()==1)) {
    auto* context=impl_->prepare_context();
    Bytes candidate(ZSTD_compressBound(input.size()));
    const auto n=ZSTD_compress2(context,candidate.data(),candidate.size(),input.data(),input.size());
    if(ZSTD_isError(n)) fail("bounded Zstandard compression failed");
    const auto best=result.codec==kStoreRaw?input.size():result.payload.size();
    if(n<best && n+16<=input.size()) {
      candidate.resize(n);result.codec=kStoreZstd;result.payload=std::move(candidate);
    }
  }
#endif
#ifdef FLITZFS_HAS_LZ4
  if(cpu_fast && impl_->options.mode==StoreCompressionMode::fast && store_lz4_available() &&
      result.codec!=kStoreZero && !(result.codec==kStorePattern && result.payload.size()==1)) {
    static_assert(alignof(std::max_align_t)>=8);
    // The official extState API initializes caller-owned, 8-byte-aligned state.
    // No hidden compressor allocation or streaming dictionary dependency.
    const auto size=LZ4_sizeofState();
    if(size>0 && static_cast<std::size_t>(size)<=65536U) {
      const auto words=(static_cast<std::size_t>(size)+sizeof(std::max_align_t)-1)/sizeof(std::max_align_t);
      const auto state_bytes=words*sizeof(std::max_align_t);
      if(state_bytes<=impl_->allocation_limit) {
        try {
          if(impl_->lz4_state.empty())impl_->lz4_state.resize(words);
          impl_->lz4_peak=(std::max)(impl_->lz4_peak,state_bytes);
          Bytes candidate(input.size()); // Strictly nonexpanding output capacity.
          const auto n=LZ4_compress_fast_extState(impl_->lz4_state.data(),
              reinterpret_cast<const char*>(input.data()),reinterpret_cast<char*>(candidate.data()),
              static_cast<int>(input.size()),static_cast<int>(candidate.size()),1);
          const auto best=result.codec==kStoreRaw?input.size():result.payload.size();
          if(n>0 && static_cast<std::size_t>(n)<best) {
            candidate.resize(static_cast<std::size_t>(n));
            result.codec=kStoreLz4;result.payload=std::move(candidate);
          }
        }catch(const std::bad_alloc&){} // Pressure keeps the already-owned raw/pattern result.
      }
    }
  }
#else
  (void)cpu_fast;
#endif
  if(result.codec==kStoreRaw) {result.payload=std::move(input);return result;}
  // All compressed candidates remain private until independent CPU readback
  // agrees byte-for-byte. Codec errors cannot poison or mutate backing storage.
  const auto decoded=impl_->verifier.decode(result.codec,result.payload,static_cast<std::uint32_t>(input.size()));
  if(decoded!=input) fail("compression round-trip mismatch");
  result.decoded_digest=record_digest(input);
  return result;
}
EncodedStoreObject StoreCodecEncoder::encode_with_lz4(Bytes input,std::span<const std::byte> candidate) {
  if(input.size()>kStoreMaxObjectBytes)fail("object exceeds codec limit");
  if(impl_->options.mode==StoreCompressionMode::raw || input.empty() ||
      candidate.empty() || candidate.size()>=input.size() || !store_lz4_available())
    return encode(std::move(input));
  Bytes owned;
  bool valid=false;
  try {
    valid=impl_->verifier.decode(kStoreLz4,candidate,static_cast<std::uint32_t>(input.size()))==input;
    if(valid)owned.assign(candidate.begin(),candidate.end()); // Retain no borrowed worker bytes.
  }catch(const std::exception&){valid=false;}
  if(!valid)return encode(std::move(input));
  // A valid fast candidate avoids running the CPU compressor a second time.
  // The automatic policy still compares its ordinary CPU baseline by size.
  auto baseline=encode_impl(Bytes(input),false);
  if(owned.size()>=baseline.payload.size())return baseline;
  return {kStoreLz4,std::move(owned),record_digest(input),false,0};
}
EncodedStoreObject StoreCodecEncoder::encode_with_matches(Bytes input,std::span<const StoreMatchProposal> matches) {
#ifdef FLITZFS_STORE_HAS_SEQUENCE_API
  if(store_match_sequences_available() && impl_->options.mode==StoreCompressionMode::automatic &&
      input.size()<=kStoreMaxMatchInputBytes && !matches.empty() && matches.size()<=kStoreMaxMatchProposals) {
    auto baseline=encode(Bytes(input));
    // Zero and a one-byte motif cannot be beaten by a Zstandard frame.
    if(baseline.payload.size()<16)return baseline;
    try {
      std::vector<ZSTD_Sequence> sequences;
      sequences.reserve(matches.size());
      std::uint32_t cursor=0;
      for(const auto& match:matches) {
        const auto [position,distance,length]=match;
        if(position<cursor || position>input.size() || !distance || distance>position ||
            length<4 || length>input.size()-position ||
            !std::equal(input.begin()+position,input.begin()+position+length,input.begin()+position-distance))
          return baseline;
        sequences.push_back({distance,position-cursor,length,0});
        cursor=position+length;
      }
      auto* context=impl_->prepare_context();
      checked(ZSTD_CCtx_setParameter(context,ZSTD_c_validateSequences,1),"sequence validation");
      checked(ZSTD_CCtx_setParameter(context,ZSTD_c_minMatch,4),"sequence minimum match");
      Bytes candidate(ZSTD_compressBound(input.size()));
      const auto n=ZSTD_compressSequences(context,candidate.data(),candidate.size(),
          sequences.data(),sequences.size(),input.data(),input.size());
      if(ZSTD_isError(n) || n>baseline.payload.size() || n+16>input.size())return baseline;
      candidate.resize(n);
      if(impl_->verifier.decode(kStoreZstd,candidate,static_cast<std::uint32_t>(input.size()))!=input)
        return baseline;
      return {kStoreZstd,std::move(candidate),record_digest(input),true,
          static_cast<std::uint32_t>(baseline.payload.size())};
    }catch(const std::exception&) {
      // An optional suggestion cannot prevent the ordinary CPU result.
      impl_->context.reset();
    }
    return baseline;
  }
#else
  (void)matches;
#endif
  return encode(std::move(input));
}
} // namespace detail
} // namespace flitzfs
