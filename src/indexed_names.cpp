#include "indexed_names.hpp"
#include "flitzfs/native_filesystem.hpp"
#include <algorithm>
#include <array>

namespace flitzfs::detail {
namespace {
struct OrdinalRange {
  std::uint16_t first,last;
  std::int32_t delta;
  unsigned step;
};
#include "indexed_ordinal_ranges.inc"

[[noreturn]] void invalid_name() {
  throw NativeFsError(NativeFsErrorCode::invalid_path,"invalid indexed Windows name");
}

char32_t read_utf8(std::string_view input,std::size_t& at) {
  const auto leading=static_cast<unsigned char>(input[at++]);
  if(leading<0x80)return leading;
  if(leading<0xc2 || leading>0xf4)invalid_name();
  const unsigned width=leading<0xe0?2:leading<0xf0?3:4;
  if(width-1>input.size()-at)invalid_name();
  char32_t value=leading & (0x7fU >> width);
  for(unsigned i=1;i<width;++i) {
    const auto byte=static_cast<unsigned char>(input[at++]);
    if((byte&0xc0U)!=0x80U)invalid_name();
    value=(value<<6)|(byte&0x3fU);
  }
  constexpr std::array<char32_t,5> minimum{0,0,0x80,0x800,0x10000};
  if(value<minimum[width] || value>0x10ffff || (value>=0xd800 && value<=0xdfff))invalid_name();
  return value;
}

void append_utf8(std::string& output,char32_t value) {
  if(value<0x80) {
    output.push_back(static_cast<char>(value));
  } else {
    const unsigned width=value<0x800?2:value<0x10000?3:4;
    constexpr std::array<unsigned,5> prefix{0,0,0xc0,0xe0,0xf0};
    output.push_back(static_cast<char>(prefix[width]|(value>>(6*(width-1)))));
    for(unsigned next=width-1;next>0;--next)
      output.push_back(static_cast<char>(0x80U|((value>>(6*(next-1)))&0x3fU)));
  }
}

bool reserved(std::string_view name) noexcept {
  auto stem=name.substr(0,name.find('.'));
  while(!stem.empty() && stem.back()==' ')stem.remove_suffix(1);
  if(stem=="con" || stem=="nul" || stem=="aux" || stem=="prn" ||
      stem=="clock$" || stem=="conin$" || stem=="conout$")return true;
  if(!stem.starts_with("com") && !stem.starts_with("lpt"))return false;
  const auto digit=stem.substr(3);
  return (digit.size()==1 && digit[0]>='0' && digit[0]<='9') ||
      digit=="\xc2\xb9" || digit=="\xc2\xb2" || digit=="\xc2\xb3";
}

bool legacy_character(char32_t c) noexcept {
  return (c>='a' && c<='z') || (c>='0' && c<='9') ||
      c=='.' || c=='-' || c=='_' || c==' ' || c=='(' || c==')';
}
}

char32_t indexed_ordinal_key(char32_t value) noexcept {
  if(value<0x80)return value>='A' && value<='Z'?value+32:value;
  const auto range=std::ranges::lower_bound(ordinal_ranges,value,{},&OrdinalRange::last);
  if(range==ordinal_ranges.end() || value<range->first || (value-range->first)%range->step)return value;
  return static_cast<char32_t>(static_cast<std::int32_t>(value)+range->delta);
}

std::size_t indexed_utf16_length(std::string_view text) {
  std::size_t units=0;
  for(std::size_t at=0;at<text.size();)units+=read_utf8(text,at)>=0x10000?2:1;
  return units;
}

IndexedPathName indexed_path_name(std::string_view path,bool allow_root) {
  if(path.empty() && allow_root)return {};
  if(path.empty() || path.size()>3*kIndexedPathUnits || path.front()=='/' || path.back()=='/')invalid_name();
  IndexedPathName result;
  result.extended=path.size()>240;
  result.key.reserve(path.size());
  for(std::size_t at=0,depth=0;at<path.size();) {
    const auto separator=path.find('/',at);
    const auto end=separator==std::string_view::npos?path.size():separator;
    if(++depth>kIndexedPathDepth || end==at || path[end-1]==' ' || path[end-1]=='.')invalid_name();
    const auto name=path.substr(at,end-at);
    if(name.size()>kIndexedNameBytes)invalid_name();
    const auto key_start=result.key.size();
    std::size_t units=0;
    for(std::size_t cursor=0;cursor<name.size();) {
      const auto scalar=read_utf8(name,cursor);
      if(scalar<32 || scalar=='"' || scalar=='*' || scalar==':' || scalar=='<' || scalar=='>' ||
          scalar=='?' || scalar=='\\' || scalar=='|')invalid_name();
      const auto key=indexed_ordinal_key(scalar);
      append_utf8(result.key,key);
      result.extended=result.extended || scalar>=0x80 || !legacy_character(key);
      units+=scalar>=0x10000?2:1;
    }
    if(units>kIndexedNameUnits || reserved(std::string_view(result.key).substr(key_start)))invalid_name();
    result.extended=result.extended || name.size()>100;
    result.utf16_units+=units;
    if(separator==std::string_view::npos)break;
    ++result.utf16_units;
    result.key.push_back('/');
    at=end+1;
  }
  if(result.utf16_units>kIndexedPathUnits)invalid_name();
  return result;
}

std::string indexed_component_key(std::string_view name) {
  if(name.find('/')!=std::string_view::npos)invalid_name();
  return indexed_path_name(name).key;
}
}
