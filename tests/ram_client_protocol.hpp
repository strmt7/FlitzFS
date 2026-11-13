#pragma once
#include <cstdint>
#include <optional>
namespace ram_client_fixture {
// Fixed synthetic test actions only; never an arbitrary path, handle or payload.
enum class Step:std::uint8_t {initial,recovered,rewrite,copy_read,published,finish};
enum class Phase:std::uint8_t {empty,initial,recovered,rewrite,copying,published,finished,failed};
class Sequence final {
 public:
  static constexpr unsigned max_copy_rounds=256;
  bool begin(Step step) noexcept {
    if(pending_||!allowed(step)){abort();return false;}
    pending_=step;return true;
  }
  bool confirm(Step step) noexcept {
    if(phase_==Phase::failed||!pending_||*pending_!=step){abort();return false;}
    pending_.reset();
    switch(step) {
      case Step::initial:phase_=Phase::initial;break;
      case Step::recovered:phase_=Phase::recovered;break;
      case Step::rewrite:phase_=Phase::rewrite;break;
      case Step::copy_read:++copy_rounds_;phase_=Phase::copying;break;
      case Step::published:phase_=Phase::published;break;
      case Step::finish:phase_=Phase::finished;break;
    }
    return true;
  }
  void abort() noexcept {pending_.reset();phase_=Phase::failed;}
  bool complete() const noexcept{return phase_==Phase::finished&&!pending_;}
  bool failed() const noexcept{return phase_==Phase::failed;}
  bool pending() const noexcept{return pending_.has_value();}
  unsigned copy_rounds() const noexcept{return copy_rounds_;}
  Phase phase() const noexcept{return phase_;}
 private:
  bool allowed(Step step) const noexcept {
    switch(step) {
      case Step::initial:return phase_==Phase::empty;
      case Step::recovered:return phase_==Phase::initial;
      case Step::rewrite:return phase_==Phase::recovered;
      case Step::copy_read:return (phase_==Phase::rewrite||phase_==Phase::copying)&&copy_rounds_<max_copy_rounds;
      case Step::published:return phase_==Phase::copying&&copy_rounds_>0;
      case Step::finish:return phase_==Phase::published;
    }
    return false;
  }
  Phase phase_=Phase::empty;
  std::optional<Step> pending_;
  unsigned copy_rounds_=0;
};
} // namespace ram_client_fixture
