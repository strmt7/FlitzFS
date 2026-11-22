#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace flitzfs::detail {

enum class MaintenanceBackingOperation {read,write,flush,control};

inline std::uint64_t saturated_backing_sum(std::uint64_t a,std::uint64_t b) noexcept {
  const auto maximum=(std::numeric_limits<std::uint64_t>::max)();
  return b>maximum-a?maximum:a+b;
}

struct MaintenanceBackingCounter {
  std::uint64_t attempts=0,completed=0,attempted_bytes=0,completed_bytes=0,elapsed_ns=0;

  void add(const MaintenanceBackingCounter& other) noexcept {
    attempts=saturated_backing_sum(attempts,other.attempts);
    completed=saturated_backing_sum(completed,other.completed);
    attempted_bytes=saturated_backing_sum(attempted_bytes,other.attempted_bytes);
    completed_bytes=saturated_backing_sum(completed_bytes,other.completed_bytes);
    elapsed_ns=saturated_backing_sum(elapsed_ns,other.elapsed_ns);
  }
};

struct MaintenanceBackingUse {
  const MaintenanceBackingCounter& operator[](MaintenanceBackingOperation kind) const noexcept {
    return counters[static_cast<std::size_t>(kind)];
  }
  std::uint64_t elapsed_ns() const noexcept {
    std::uint64_t total=0;
    for(const auto& counter:counters)total=saturated_backing_sum(total,counter.elapsed_ns);
    return total;
  }
  void add(const MaintenanceBackingUse& other) noexcept {
    for(std::size_t i=0;i<counters.size();++i)counters[i].add(other.counters[i]);
  }

 private:
  friend class MaintenanceBackingCall;
  std::array<MaintenanceBackingCounter,static_cast<std::size_t>(MaintenanceBackingOperation::control)+1> counters{};
};

// Only a serialized maintenance call admits observations from its own
// thread. Ordinary foreground traffic pays no clock query or counter update.
class MaintenanceBackingScope final {
 public:
  explicit MaintenanceBackingScope(MaintenanceBackingUse& scope) noexcept
      :previous_(std::exchange(current_,&scope)) {}
  ~MaintenanceBackingScope(){current_=previous_;}
  MaintenanceBackingScope(const MaintenanceBackingScope&)=delete;
  MaintenanceBackingScope& operator=(const MaintenanceBackingScope&)=delete;

 private:
  friend class MaintenanceBackingCall;
  inline static thread_local MaintenanceBackingUse* current_=nullptr;
  MaintenanceBackingUse* previous_;
};

class MaintenanceBackingCall final {
 public:
  explicit MaintenanceBackingCall(MaintenanceBackingOperation kind,std::uint64_t bytes=0,
      bool observe=true) noexcept
      :use_(observe?MaintenanceBackingScope::current_:nullptr),kind_(kind),bytes_(bytes) {
    if(use_)started_=Clock::now();
  }
  MaintenanceBackingCall(const MaintenanceBackingCall&)=delete;
  MaintenanceBackingCall& operator=(const MaintenanceBackingCall&)=delete;
  ~MaintenanceBackingCall() {
    if(!use_)return;
    const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-started_).count();
    use_->counters[static_cast<std::size_t>(kind_)].add({
        1,finished_?1ULL:0ULL,bytes_,finished_?bytes_:0,
        elapsed>0?static_cast<std::uint64_t>(elapsed):1});
  }
  void complete() noexcept {finished_=true;}

 private:
  using Clock=std::chrono::steady_clock;
  MaintenanceBackingUse* use_;
  MaintenanceBackingOperation kind_;
  std::uint64_t bytes_;
  Clock::time_point started_{};
  bool finished_=false;
};

// Hold recent slow steps across cheaper phases of the same preparation.
// Recover only as newly measured backing work supplies evidence.
class MaintenanceBackingTiming final {
 public:
  void observe(std::uint64_t elapsed_ns) noexcept {
    if(!elapsed_ns)return;
    const auto decay=predicted_ns_/4+(predicted_ns_%4!=0);
    predicted_ns_=(std::max)(elapsed_ns,predicted_ns_-decay);
  }
  std::uint64_t predicted_ns() const noexcept {return predicted_ns_;}

 private:
  std::uint64_t predicted_ns_=0;
};

} // namespace flitzfs::detail
