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
#include <zstd_errors.h>
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
bool store_prefix_frame(std::span<const std::byte> payload,std::size_t expected) noexcept {
#ifdef FLITZFS_STORE_HAS_ZSTD
  if(!native_zstd_frame(payload) || !expected || expected>kStoreMaxMatchInputBytes || payload.size()>=expected)return false;
  ZSTD_frameHeader frame{};
  if(ZSTD_getFrameHeader(&frame,payload.data(),payload.size())!=0 || frame.frameContentSize!=expected ||
      frame.dictID || !frame.checksumFlag || frame.windowSize>2*expected)return false;
  return ZSTD_findFrameCompressedSize(payload.data(),payload.size())==payload.size();
#else
  (void)payload;(void)expected;
  return false;
#endif
}
std::optional<std::vector<std::byte>> encode_store_prefix(std::span<const std::byte> input,
    std::span<const std::byte> prefix,std::size_t exclusive_limit) {
#ifdef FLITZFS_STORE_HAS_ZSTD
  if(input.empty() || input.size()!=prefix.size() || input.size()>kStoreMaxMatchInputBytes ||
      exclusive_limit<=1 || exclusive_limit>input.size())return {};
  Budget budget{kStorePrefixEncoderMemoryCap};
  std::unique_ptr<ZSTD_CCtx,decltype(&ZSTD_freeCCtx)> context(
      ZSTD_createCCtx_advanced(budget.callbacks()),&ZSTD_freeCCtx);
  if(!context ||
      ZSTD_isError(ZSTD_CCtx_setParameter(context.get(),ZSTD_c_compressionLevel,3)) ||
      ZSTD_isError(ZSTD_CCtx_setParameter(context.get(),ZSTD_c_windowLog,17)) ||
      ZSTD_isError(ZSTD_CCtx_setParameter(context.get(),ZSTD_c_checksumFlag,1)) ||
      ZSTD_isError(ZSTD_CCtx_refPrefix(context.get(),prefix.data(),prefix.size())))return {};
  Bytes payload(exclusive_limit-1);
  const auto bytes=ZSTD_compress2(context.get(),payload.data(),payload.size(),input.data(),input.size());
  if(ZSTD_isError(bytes) || !bytes || bytes>=exclusive_limit)return {};
  payload.resize(bytes);
  if(!store_prefix_frame(payload,input.size()))return {};
  return payload;
#else
  (void)input;(void)prefix;(void)exclusive_limit;
  return {};
#endif
}
std::vector<std::byte> decode_store_prefix(std::span<const std::byte> input,
    std::span<const std::byte> prefix) {
#ifdef FLITZFS_STORE_HAS_ZSTD
  if(!store_prefix_frame(input,prefix.size()))fail("invalid prefixed Zstandard frame");
  Budget budget{kStoreDecoderMemoryCap};
  std::unique_ptr<ZSTD_DCtx,decltype(&ZSTD_freeDCtx)> context(
      ZSTD_createDCtx_advanced(budget.callbacks()),&ZSTD_freeDCtx);
  if(!context)fail("prefixed Zstandard decoder memory admission");
  checked(ZSTD_DCtx_refPrefix(context.get(),prefix.data(),prefix.size()),"Zstandard prefix binding");
  Bytes output(prefix.size());
  const auto bytes=ZSTD_decompressDCtx(context.get(),output.data(),output.size(),input.data(),input.size());
  if(ZSTD_isError(bytes) || bytes!=output.size())fail("prefixed Zstandard decoding failed");
  return output;
#else
  (void)input;(void)prefix;
  fail("prefixed Zstandard recovery unavailable");
#endif
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
  ZSTD_CCtx* encoding_context() noexcept {
    if(!context)context.reset(ZSTD_createCCtx_advanced(budget.callbacks()));
    return context.get();
  }
  ZSTD_CCtx* prepare_context() {
    auto* cctx=encoding_context();
    if(!cctx)fail("compressor memory admission");
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
  auto result=encode_compressed(input,true);
  if(result.codec==kStoreRaw) {
    result.payload=std::move(input);
    return result;
  }
  return verify_encoding(std::move(result),input);
}
EncodedStoreObject StoreCodecEncoder::encode_compressed(std::span<const std::byte> input,bool cpu_fast) {
  if(input.size()>kStoreMaxObjectBytes)fail("object exceeds codec limit");
  EncodedStoreObject result;
  if(impl_->options.mode==StoreCompressionMode::raw || input.empty())return result;
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
  return result;
}
EncodedStoreObject StoreCodecEncoder::verify_encoding(EncodedStoreObject result,std::span<const std::byte> input) {
  if(result.codec==kStoreRaw) {
    result.payload.assign(input.begin(),input.end());
    return result;
  }
  // Compressed candidates remain private until independent CPU readback.
  const auto decoded=impl_->verifier.decode(result.codec,result.payload,static_cast<std::uint32_t>(input.size()));
  if(!std::ranges::equal(decoded,input))fail("compression round-trip mismatch");
  result.decoded_digest=record_digest(input);
  return result;
}
ColdStoreEncoding StoreCodecEncoder::try_cold_encoding(
    std::span<const std::byte> input,std::size_t old_size) {
#ifdef FLITZFS_STORE_HAS_ZSTD
  // Indexed data fits one native extent and its envelope. Bound a trial to
  // one Zstandard block, and charge the same allocator as foreground encoding.
  constexpr std::size_t block_bytes=128U*1024U;
  if(impl_->options.mode==StoreCompressionMode::raw || !store_zstd_available() ||
      input.empty() || input.size()>block_bytes || old_size>input.size())return {};
  if(old_size<=16)return {{},true};
  const auto maximum=ZSTD_maxCLevel();
  auto params=ZSTD_getCParams(maximum,input.size(),0);
  auto bytes=ZSTD_estimateCCtxSize_usingCParams(params);
  for(auto level=maximum;!ZSTD_isError(bytes) && bytes>impl_->allocation_limit;) {
    if(--level<=impl_->options.zstd_level)return {{},true};
    params=ZSTD_getCParams(level,input.size(),0);
    bytes=ZSTD_estimateCCtxSize_usingCParams(params);
  }
  if(ZSTD_isError(bytes))return {};
  auto* ctx=impl_->encoding_context();
  if(!ctx || ZSTD_isError(ZSTD_CCtx_reset(ctx,ZSTD_reset_session_and_parameters)) ||
      ZSTD_isError(ZSTD_CCtx_setCParams(ctx,params)) ||
      ZSTD_isError(ZSTD_CCtx_setParameter(ctx,ZSTD_c_nbWorkers,0)) ||
      ZSTD_isError(ZSTD_CCtx_setParameter(ctx,ZSTD_c_contentSizeFlag,1)) ||
      ZSTD_isError(ZSTD_CCtx_setParameter(ctx,ZSTD_c_checksumFlag,1)))return {};
  try {
    Bytes candidate(old_size-1);
    const auto size=ZSTD_compress2(ctx,candidate.data(),candidate.size(),input.data(),input.size());
    if(ZSTD_isError(size))return {{},ZSTD_getErrorCode(size)==ZSTD_error_dstSize_tooSmall};
    if(size>=old_size)return {{},true};
    candidate.resize(size);
    return {verify_encoding({kStoreZstd,std::move(candidate)},input),true};
  }catch(const std::bad_alloc&) {return {};}
#else
  (void)input;(void)old_size;
  return {};
#endif
}
Bytes prepend_lz4_literals(std::span<const std::byte> prefix,std::span<const std::byte> block,
    std::size_t decoded_body_bytes) {
  if(prefix.empty() || block.empty() || block.size()>=decoded_body_bytes ||
      decoded_body_bytes>kStoreMaxObjectBytes || prefix.size()>kStoreMaxObjectBytes-decoded_body_bytes)
    return {};
  const auto token=std::to_integer<unsigned>(block.front());
  std::size_t literals=token>>4,header=1;
  if(literals==15) {
    unsigned extension;
    do {
      if(header==block.size())return {};
      extension=std::to_integer<unsigned>(block[header++]);
      literals+=extension;
      if(literals>decoded_body_bytes)return {};
    }while(extension==255);
  }
  if(literals>decoded_body_bytes || literals>block.size()-header)return {};
  const auto combined=prefix.size()+literals;
  const auto extended=combined>=15;
  const auto token_bytes=extended?2+(combined-15)/255:1;
  const auto remaining=block.subspan(header);
  const auto size=token_bytes+prefix.size()+remaining.size();
  if(size>=prefix.size()+decoded_body_bytes)return {};

  Bytes result;
  result.reserve(size);
  result.push_back(static_cast<std::byte>(((std::min)(combined,std::size_t{15})<<4)|(token&15U)));
  if(extended) {
    auto extra=combined-15;
    while(extra>=255){result.push_back(std::byte{255});extra-=255;}
    result.push_back(static_cast<std::byte>(extra));
  }
  // Inserting bytes before the first literal shifts both ends of each match.
  // The existing offsets, match-length extensions and final literals remain valid.
  result.insert(result.end(),prefix.begin(),prefix.end());
  result.insert(result.end(),remaining.begin(),remaining.end());
  return result;
}
EncodedStoreObject StoreCodecEncoder::encode_with_lz4(std::span<const std::byte> input,
    std::span<const std::byte> candidate,std::uint32_t literal_prefix_bytes) {
  if(input.size()>kStoreMaxObjectBytes)fail("object exceeds codec limit");
  const auto cpu=[&]{return verify_encoding(encode_compressed(input,true),input);};
  if(impl_->options.mode==StoreCompressionMode::raw || candidate.empty() ||
      literal_prefix_bytes>=input.size() || candidate.size()>=input.size()-literal_prefix_bytes ||
      !store_lz4_available())
    return cpu();

  // Validate independently of the prefix. An LZ4 block cannot use newly
  // prepended metadata as a dictionary to pass validation.
  const auto suffix=input.subspan(literal_prefix_bytes);
  Bytes owned;
  try {
    const auto decoded=impl_->verifier.decode(kStoreLz4,candidate,static_cast<std::uint32_t>(suffix.size()));
    if(std::ranges::equal(decoded,suffix)) {
      if(literal_prefix_bytes)
        owned=prepend_lz4_literals(input.first(literal_prefix_bytes),candidate,suffix.size());
      else
        owned.assign(candidate.begin(),candidate.end());
    }
  }catch(const std::exception&) {
    owned.clear();
  }
  if(owned.empty())return cpu();

  // Size the CPU alternative against the immutable input. Validate only
  // the selected CPU representation, and retain owned bytes for publication.
  auto baseline=encode_compressed(input,false);
  const auto best=baseline.codec==kStoreRaw?input.size():baseline.payload.size();
  if(owned.size()>=best)return verify_encoding(std::move(baseline),input);
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
