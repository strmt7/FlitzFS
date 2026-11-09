#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// An executable protocol experiment, NOT a filesystem or accepted disk format.
// No file, device, driver, GPU or platform persistence calls occur in this test.
namespace {
using Byte = std::uint8_t;
using Image = std::vector<Byte>;
using Record = std::array<Byte,128>;
using Payload = std::array<Byte,64>;
constexpr std::size_t kRecordBytes = 128;
constexpr unsigned kTransactions = 3;
std::uint64_t executions = 0;
const auto started = std::chrono::steady_clock::now();

void require(bool value, const char* message) {
  if(!value) throw std::runtime_error(message);
}
void budget() {
  if(++executions > 60000) throw std::runtime_error("model case budget exceeded");
  if(executions % 256 == 0 &&
      std::chrono::steady_clock::now()-started > std::chrono::seconds(20))
    throw std::runtime_error("model time budget exceeded");
}
std::uint32_t crc(std::span<const Byte> bytes) {
  std::uint32_t value = 0xffffffffU;
  for(auto byte:bytes) {
    value ^= byte;
    for(unsigned bit = 0; bit < 8; ++bit)
      value = (value >> 1) ^ ((value & 1U) ? 0xedb88320U : 0U);
  }
  return ~value;
}
void put32(Record& record, std::size_t at, std::uint32_t value) {
  for(unsigned i=0;i<4;++i) record[at+i] = static_cast<Byte>(value >> (8*i));
}
void put64(Record& record, std::size_t at, std::uint64_t value) {
  for(unsigned i=0;i<8;++i) record[at+i] = static_cast<Byte>(value >> (8*i));
}
std::uint32_t get32(std::span<const Byte> record, std::size_t at) {
  std::uint32_t value = 0;
  for(unsigned i=0;i<4;++i) value |= std::uint32_t(record[at+i]) << (8*i);
  return value;
}
std::uint64_t get64(std::span<const Byte> record, std::size_t at) {
  std::uint64_t value = 0;
  for(unsigned i=0;i<8;++i) value |= std::uint64_t(record[at+i]) << (8*i);
  return value;
}
void seal(Record& record) { put32(record,124,crc(std::span<const Byte>(record).first(124))); }
bool sealed(std::span<const Byte> record) {
  return record.size()==kRecordBytes && get32(record,124)==crc(record.first(124));
}
bool zeros(std::span<const Byte> record,std::size_t first,std::size_t last) {
  return std::all_of(record.begin()+static_cast<std::ptrdiff_t>(first),
                     record.begin()+static_cast<std::ptrdiff_t>(last),[](auto b){return b==0;});
}
constexpr std::array<Byte,8> kDataMagic{'M','D','A','T','A','0','0','1'};
constexpr std::array<Byte,8> kRootMagic{'M','R','O','O','T','0','0','1'};
Payload input(unsigned generation) {
  Payload bytes{};
  if(generation) for(std::size_t i=0;i<bytes.size();++i)
    bytes[i]=static_cast<Byte>((i*37u+generation*53u) & 255u);
  return bytes;
}
struct Layout {
  std::size_t unit;
  bool colocated;
  explicit Layout(std::size_t size,bool share=false):unit(size),colocated(share) {
    require(size>=512 && size<=8192 && !(size&(size-1)),"bad model write unit");
  }
  std::size_t root(unsigned slot) const { return slot ? (colocated ? 128 : unit) : 0; }
  std::size_t data(unsigned generation) const { return (generation+1)*unit; }
  std::size_t bytes() const { return (kTransactions+2)*unit; }
};
Record data_record(unsigned generation) {
  Record result{};
  std::copy(kDataMagic.begin(),kDataMagic.end(),result.begin());
  put64(result,8,generation);put32(result,16,64);
  const auto payload=input(generation);
  std::copy(payload.begin(),payload.end(),result.begin()+24);
  seal(result);
  return result;
}
Record root_record(const Layout& layout,unsigned generation) {
  Record result{};
  std::copy(kRootMagic.begin(),kRootMagic.end(),result.begin());
  put64(result,8,generation);
  if(generation) {
    put64(result,16,layout.data(generation));put32(result,24,kRecordBytes);
    const auto data=data_record(generation);
    put32(result,28,get32(data,124));put64(result,32,generation-1);
  }
  seal(result);
  return result;
}
struct Recovered { unsigned generation;Payload bytes; };
std::optional<Recovered> recover(const Image& image,const Layout& layout,bool check_dependencies) {
  if(image.size()!=layout.bytes()) return {};
  std::optional<Recovered> best;
  for(unsigned slot=0;slot<2;++slot) {
    const auto root=std::span<const Byte>(image).subspan(layout.root(slot),kRecordBytes);
    if(!std::equal(kRootMagic.begin(),kRootMagic.end(),root.begin()) ||
        !sealed(root) || !zeros(root,40,124)) continue;
    const auto sequence=get64(root,8);
    if(sequence>kTransactions || (sequence && get64(root,32)!=sequence-1)) continue;
    if(!sequence) {
      if(!zeros(root,16,40)) continue;
      if(!best) best=Recovered{0,{}};
      continue;
    }
    const auto generation=static_cast<unsigned>(sequence);
    if(get64(root,16)!=layout.data(generation) || get32(root,24)!=kRecordBytes) continue;
    const auto data=std::span<const Byte>(image).subspan(layout.data(generation),kRecordBytes);
    if(check_dependencies &&
        (!std::equal(kDataMagic.begin(),kDataMagic.end(),data.begin()) ||
         !sealed(data) || get64(data,8)!=sequence || get32(data,16)!=64 ||
         !zeros(data,20,24) || !zeros(data,88,124) || get32(data,124)!=get32(root,28))) continue;
    Payload bytes{};
    std::copy_n(data.begin()+24,64,bytes.begin());
    if(!best || generation>best->generation) best=Recovered{generation,bytes};
  }
  return best;
}
struct Write { std::size_t offset;Record bytes; };
struct Device {
  Layout layout;
  Image stable;
  std::vector<Write> pending;
  explicit Device(Layout spec):layout(spec),stable(spec.bytes(),0) {
    pending.reserve(2);
    const auto empty=root_record(layout,0);
    std::copy(empty.begin(),empty.end(),stable.begin()+static_cast<std::ptrdiff_t>(layout.root(0)));
    std::copy(empty.begin(),empty.end(),stable.begin()+static_cast<std::ptrdiff_t>(layout.root(1)));
  }
  void write(std::size_t offset,Record bytes) {
    require(offset<=stable.size() && bytes.size()<=stable.size()-offset,"model extent overflow");
    require(pending.size()<2,"unbounded model write queue");
    pending.push_back({offset,bytes});
  }
  void flush(bool truthful) {
    if(!truthful) return; // Negative control: simulated dishonest persistence.
    for(const auto& write:pending)
      std::copy(write.bytes.begin(),write.bytes.end(),stable.begin()+static_cast<std::ptrdiff_t>(write.offset));
    pending.clear();
  }
};

// Enumerate all whole-write subsets and each individual torn prefix/suffix,
// in both application orders. Also destroy a pending write's whole failure
// unit to model collateral damage within that unit. These are scoped cases,
// not every possible bit pattern, device behavior or physical power failure.
template<class Check> bool faults(const Device& device,Check&& check) {
  const auto n=static_cast<unsigned>(device.pending.size());
  require(n<=2,"model pending bound");
  const auto combinations=1u<<n;
  auto emit=[&](unsigned mask,int partial,std::size_t cut,bool suffix,bool reverse) {
    Image candidate=device.stable;
    for(unsigned position=0;position<n;++position) {
      const auto index=reverse ? n-1-position : position;
      const auto& write=device.pending[index];
      std::size_t first=0,last=write.bytes.size();
      if(static_cast<int>(index)==partial) {
        first=suffix ? cut : 0;
        last=suffix ? write.bytes.size() : cut;
      } else if(!(mask&(1u<<index))) continue;
      std::copy(write.bytes.begin()+static_cast<std::ptrdiff_t>(first),
                write.bytes.begin()+static_cast<std::ptrdiff_t>(last),
                candidate.begin()+static_cast<std::ptrdiff_t>(write.offset+first));
    }
    budget();
    return check(candidate);
  };
  if(!n) return emit(0,-1,0,false,false);
  for(bool reverse:{false,true}) {
    for(unsigned mask=0;mask<combinations;++mask)
      if(!emit(mask,-1,0,false,reverse)) return false;
    for(unsigned index=0;index<n;++index)
      for(unsigned mask=0;mask<combinations;++mask) {
        if(mask&(1u<<index)) continue;
        for(std::size_t cut=0;cut<=kRecordBytes;++cut)
          for(bool suffix:{false,true})
            if(!emit(mask,static_cast<int>(index),cut,suffix,reverse)) return false;
      }
  }
  for(const auto& write:device.pending) {
    auto candidate=device.stable;
    const auto begin=(write.offset/device.layout.unit)*device.layout.unit;
    std::fill_n(candidate.begin()+static_cast<std::ptrdiff_t>(begin),device.layout.unit,Byte{0});
    budget();
    if(!check(candidate)) return false;
  }
  return true;
}

struct Protocol {
  bool data_barrier=true;
  bool full_recovery=true;
  bool early_ack=false;
  bool truthful_flush=true;
};
bool exercise(const Layout& layout,Protocol protocol) {
  Device device(layout);
  unsigned acknowledged=0;
  for(unsigned generation=1;generation<=kTransactions;++generation) {
    // The oracle knows only submitted inputs and durable acknowledgements; it
    // does not use a commit header as proof that data was made durable.
    auto checkpoint=[&] {
      return faults(device,[&](const Image& image) {
        const auto state=recover(image,layout,protocol.full_recovery);
        return state && state->generation>=acknowledged && state->generation<=generation &&
               state->bytes==input(state->generation);
      });
    };
    device.write(layout.data(generation),data_record(generation));
    if(!checkpoint()) return false;
    if(protocol.data_barrier) {
      device.flush(protocol.truthful_flush);
      if(!checkpoint()) return false;
    }
    device.write(layout.root(generation%2),root_record(layout,generation));
    if(protocol.early_ack) acknowledged=generation;
    if(!checkpoint()) return false;
    device.flush(protocol.truthful_flush);
    if(!checkpoint()) return false;
    acknowledged=generation;
    if(!checkpoint()) return false;
    const auto stable=recover(device.stable,layout,true);
    if(!stable || stable->generation!=generation || stable->bytes!=input(generation)) return false;
  }
  return true;
}
}

int main() {
  try {
    const auto initial=executions;
    for(const auto unit:{512u,4096u,8192u}) {
      require(exercise(Layout(unit),{}),"two-barrier candidate lost acknowledged state");
      Protocol one_barrier;one_barrier.data_barrier=false;
      require(exercise(Layout(unit),one_barrier),"one-barrier/full-validation candidate failed");
      Protocol metadata_recovery;metadata_recovery.full_recovery=false;
      require(exercise(Layout(unit),metadata_recovery),"two-barrier/root-only recovery candidate failed");
    }
    const auto accepted_cases=executions-initial;
    Protocol early;early.early_ack=true;
    require(!exercise(Layout(4096),early),"model missed early acknowledgement loss");
    Protocol dishonest;dishonest.truthful_flush=false;
    require(!exercise(Layout(4096),dishonest),"model missed dishonest flush loss");
    Protocol unsafe;unsafe.data_barrier=false;unsafe.full_recovery=false;
    require(!exercise(Layout(4096),unsafe),"model missed unpersisted dependency exposure");
    require(!exercise(Layout(4096,true),{}),"model missed colocated-root write-unit destruction");
    std::cout << "PASS: " << accepted_cases << " positive fault executions; "
              << executions-accepted_cases << " negative-control fault executions; "
              << "three write units; four broken controls detected. RAM model only.\n";
    return 0;
  } catch(const std::exception& error) { std::cerr << error.what() << '\n';return 1; }
}
