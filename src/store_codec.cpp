#include "store_codec.hpp"
#include "repeat_bytes.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#ifdef FLITZFS_STORE_HAS_ZSTD
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
static_assert(ZSTD_VERSION_NUMBER>=10504,"Zstandard 1.5.4 or newer required");
#endif

namespace flitzfs {
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
void validate_store_compression(StoreCompressionOptions options) {
  if((options.mode!=StoreCompressionMode::raw && options.mode!=StoreCompressionMode::automatic) ||
      options.zstd_level<1 || options.zstd_level>9 ||
      (options.require_zstd && (options.mode==StoreCompressionMode::raw || !store_zstd_available())))
    fail("invalid policy or required bounded Zstandard unavailable");
}
bool store_codec_framing(std::uint32_t codec,std::uint32_t logical,std::uint32_t stored) noexcept {
  if(logical>kStoreMaxObjectBytes || stored>logical) return false;
  switch(codec) {
    case kStoreRaw:return stored==logical;
    case kStoreZero:return logical>0 && stored==0;
    case kStorePattern:return stored>0 && stored<=kStoreMaxPatternBytes && stored<logical;
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
  explicit Impl(StoreCompressionOptions o,std::size_t cap):options(o){
#ifdef FLITZFS_STORE_HAS_ZSTD
    budget.limit=cap;
#else
    (void)cap;
#endif
  }
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
  return impl_->budget.peak;
#else
  return 0;
#endif
}
std::vector<std::byte> StoreCodecDecoder::decode(std::uint32_t codec,
    std::span<const std::byte> payload,std::uint32_t logical) {
  if(payload.size()>(std::numeric_limits<std::uint32_t>::max)() ||
      !store_codec_framing(codec,logical,static_cast<std::uint32_t>(payload.size())))
    fail("unknown codec or invalid stored/logical extent");
  if(codec==kStoreZstd && !store_zstd_available()) fail("Zstandard recovery unavailable");
  Bytes output(logical);
  if(codec==kStoreRaw) std::copy(payload.begin(),payload.end(),output.begin());
  else if(codec==kStoreZero) {} // Value-initialized bytes are zero.
  else if(codec==kStorePattern) repeat_bytes(payload,output);
  else {
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
  if(store_zstd_available() && result.codec!=kStoreZero && !(result.codec==kStorePattern && result.payload.size()==1)) {
    if(!impl_->context) {
      impl_->context.reset(ZSTD_createCCtx_advanced(impl_->budget.callbacks()));
      if(!impl_->context) fail("compressor memory admission");
    }
    auto* context=impl_->context.get();
    checked(ZSTD_CCtx_reset(context,ZSTD_reset_session_and_parameters),"compressor reset");
    checked(ZSTD_CCtx_setParameter(context,ZSTD_c_compressionLevel,impl_->options.zstd_level),"compression level");
    checked(ZSTD_CCtx_setParameter(context,ZSTD_c_windowLog,20),"compression window");
    checked(ZSTD_CCtx_setParameter(context,ZSTD_c_hashLog,18),"compression hash table");
    checked(ZSTD_CCtx_setParameter(context,ZSTD_c_chainLog,18),"compression chain table");
    checked(ZSTD_CCtx_setParameter(context,ZSTD_c_nbWorkers,0),"single-threaded compression");
    checked(ZSTD_CCtx_setParameter(context,ZSTD_c_contentSizeFlag,1),"compression size field");
    checked(ZSTD_CCtx_setParameter(context,ZSTD_c_checksumFlag,1),"compression checksum");
    Bytes candidate(ZSTD_compressBound(input.size()));
    const auto n=ZSTD_compress2(context,candidate.data(),candidate.size(),input.data(),input.size());
    if(ZSTD_isError(n)) fail("bounded Zstandard compression failed");
    const auto best=result.codec==kStoreRaw?input.size():result.payload.size();
    if(n<best && n+16<=input.size()) {
      candidate.resize(n);result.codec=kStoreZstd;result.payload=std::move(candidate);
    }
  }
#endif
  if(result.codec==kStoreRaw) {result.payload=std::move(input);return result;}
  // All compressed candidates remain private until independent CPU readback
  // agrees byte-for-byte. Codec errors cannot poison or mutate backing storage.
  const auto decoded=impl_->verifier.decode(result.codec,result.payload,static_cast<std::uint32_t>(input.size()));
  if(decoded!=input) fail("compression round-trip mismatch");
  result.decoded_digest=record_digest(input);
  return result;
}
} // namespace detail
} // namespace flitzfs
