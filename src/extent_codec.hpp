#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
namespace flitzfs::detail {
// Experimental internal compression mechanisms, not an archive reader or an
// on-disk filesystem codec registry. No HIP initialization.
inline constexpr std::uint32_t kMaxExtentCodecBytes=16U*1024U*1024U;
enum class ExtentCodecErrorCode {corrupt,unsupported_codec};
class ExtentCodecError final:public std::runtime_error {
 public:
  ExtentCodecError(ExtentCodecErrorCode code,const char* message):std::runtime_error(message),code_(code){}
  ExtentCodecErrorCode code() const noexcept{return code_;}
 private:
  ExtentCodecErrorCode code_;
};
bool extent_codec_available(std::uint8_t kind) noexcept;
// Borrowed disjoint source/destination, exact output <=16 MiB. Output must be
// private: failed decoding may leave partial bytes. Integrity belongs to the
// enclosing immutable record, not this decoder. Closed nonrecursive composition;
// scratch <= one output-sized buffer.
void decode_extent(std::uint8_t kind,std::uint8_t fill,
    std::span<const std::byte> source,std::span<std::byte> output);
}
