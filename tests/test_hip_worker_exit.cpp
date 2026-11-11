#include "hip_worker_exit.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>
int main() {
  unsigned checks=0;
  const auto require=[&](bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);};
  try {
    for(std::uint32_t base=0x46530001;base<=0x46530006;++base) {
      require(flitzfs::hip_worker_exit_category(base)==base,"legacy category changed");
      require(flitzfs::hip_worker_exit_stage(base)==flitzfs::HipWorkerStage::none,"legacy exit invented a stage");
      for(unsigned detail=1;detail<=15;++detail) {
        const auto stage=static_cast<flitzfs::HipWorkerStage>(detail);
        const auto code=flitzfs::hip_worker_diagnostic_exit(static_cast<flitzfs::HipWorkerExit>(base),stage);
        require(code!=base && flitzfs::hip_worker_exit_category(code)==base,"stage changed recovery category");
        require(flitzfs::hip_worker_exit_stage(code)==stage,"stage was lost");
        require(std::string_view(flitzfs::hip_worker_stage_name(stage))!="unknown","known stage name lost");
      }
    }
    for(const auto code:{0U,1460U,0xffffffffU,0x46531002U,0x4653ff04U,0x46530100U,0x46530107U,0x47530102U}) {
      require(flitzfs::hip_worker_exit_category(code)==code,"foreign or unknown code was normalized");
      require(flitzfs::hip_worker_exit_stage(code)==flitzfs::HipWorkerStage::none,"foreign code invented evidence");
    }
    require(flitzfs::hip_worker_diagnostic_exit(flitzfs::HipWorkerExit::device_fault,
        static_cast<flitzfs::HipWorkerStage>(16))==0x46530005,"unknown stage changed legacy exit");
    const flitzfs::HipWorkerFailure tagged(flitzfs::HipWorkerExit::unsupported_configuration,
        "node identity",flitzfs::HipWorkerStage::adapter_node);
    require(tagged.code==flitzfs::HipWorkerExit::unsupported_configuration &&
        tagged.stage==flitzfs::HipWorkerStage::adapter_node,"exception lost category/stage");
    std::cout<<"HIP exit-stage invariants: checks="<<checks<<" gpu=false\n";
    return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
