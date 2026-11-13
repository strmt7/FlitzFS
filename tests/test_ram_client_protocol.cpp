#include "ram_client_protocol.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
using namespace ram_client_fixture;
namespace {
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
void complete(Sequence& sequence,Step step) {
  require(sequence.begin(step)&&sequence.pending(),"step admission");
  require(sequence.confirm(step)&&!sequence.pending(),"step acknowledgement");
}
void prefix(Sequence& sequence) {
  complete(sequence,Step::initial);complete(sequence,Step::recovered);complete(sequence,Step::rewrite);
}
}
int main(){try{
  constexpr std::array steps{Step::initial,Step::recovered,Step::rewrite,Step::copy_read,Step::published,Step::finish};
  // Every admitted copy count, not an inflated count of repeated assertions.
  for(unsigned rounds=1;rounds<=Sequence::max_copy_rounds;++rounds) {
    Sequence sequence;prefix(sequence);
    for(unsigned i=0;i<rounds;++i)complete(sequence,Step::copy_read);
    require(sequence.copy_rounds()==rounds,"copy count");
    complete(sequence,Step::published);complete(sequence,Step::finish);
    require(sequence.complete(),"unsealed completion");
    for(auto step:steps){auto ended=sequence;require(!ended.begin(step)&&ended.failed(),"terminal sequence reused");}
  }
  // A request never advances state until its exact acknowledgement.
  {
    Sequence sequence;require(sequence.begin(Step::initial),"initial begin");
    require(sequence.phase()==Phase::empty&&!sequence.complete(),"unacknowledged phase advanced");
    require(!sequence.confirm(Step::recovered)&&sequence.failed(),"wrong acknowledgement accepted");
    require(!sequence.begin(Step::initial),"failed sequence retried");
  }
  {
    Sequence sequence;require(sequence.begin(Step::initial),"initial begin");
    require(!sequence.begin(Step::initial)&&sequence.failed(),"overlapping request accepted");
  }
  {
    Sequence sequence;require(!sequence.confirm(Step::initial)&&sequence.failed(),"unsolicited acknowledgement accepted");
  }
  // Foreign enums and each out-of-order publication/mutation boundary fail closed.
  for(auto step:{static_cast<Step>(255),Step::recovered,Step::rewrite,Step::copy_read,Step::published,Step::finish}) {
    Sequence sequence;require(!sequence.begin(step)&&sequence.failed(),"foreign/early step accepted");
  }
  {
    Sequence sequence;prefix(sequence);
    require(!sequence.begin(Step::published)&&sequence.failed(),"publication before copied reads accepted");
  }
  {
    Sequence sequence;prefix(sequence);complete(sequence,Step::copy_read);
    require(!sequence.begin(Step::finish)&&sequence.failed(),"client close before publication accepted");
  }
  {
    Sequence sequence;prefix(sequence);
    for(unsigned i=0;i<Sequence::max_copy_rounds;++i)complete(sequence,Step::copy_read);
    require(!sequence.begin(Step::copy_read)&&sequence.failed(),"copy round cap bypassed");
  }
  {
    Sequence sequence;require(sequence.begin(Step::initial),"initial begin");sequence.abort();
    require(sequence.failed()&&!sequence.pending()&&!sequence.complete()&&!sequence.confirm(Step::initial),
        "aborted request became complete");
  }
  std::cout<<"Bounded RAM client phase semantics passed: copy_counts=256, terminal/overlap/order/cap checks; no Windows IPC, mount or GPU.\n";
  return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<"\n";return 1;}}
