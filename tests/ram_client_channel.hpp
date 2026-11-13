#pragma once
// Test-only finite RAM handoff conversation between the trusted controller and
// its exact supervised filesystem child. No bytes, paths or HANDLEs are sent.
#include "ram_client_protocol.hpp"
#include "supervised_source_fixture.hpp"
namespace ram_client_fixture {
class Channel final {
 public:
  static constexpr std::size_t step_count=6,ack_index=step_count;
  static_assert(static_cast<std::size_t>(Step::finish)+1==step_count);
  explicit Channel(bool client,std::wstring token=source_fixture::random_token())
      :client_(client),token_(std::move(token)) {
    source_fixture::require(source_fixture::valid_token(token_),"invalid RAM client token");
    source_fixture::Local acl;if(client_)source_fixture::owner_security(acl);
    SECURITY_ATTRIBUTES security{sizeof(security),acl.value,FALSE};
    for(std::size_t i=0;i<events_.size();++i) {
      const auto name=event_name(token_,i);
      if(client_) {
        SetLastError(ERROR_SUCCESS);
        events_[i].value=CreateEventW(&security,FALSE,FALSE,name.c_str());
        const auto error=GetLastError();
        source_fixture::checked(events_[i].value!=nullptr,"RAM client event creation");
        source_fixture::require(error!=ERROR_ALREADY_EXISTS,"RAM client event collision");
      }else {
        const auto rights=i==ack_index?SYNCHRONIZE:EVENT_MODIFY_STATE;
        events_[i].value=OpenEventW(rights,FALSE,name.c_str());
        source_fixture::checked(events_[i].value!=nullptr,"RAM client event open");
      }
    }
  }
  Channel(const Channel&)=delete;Channel& operator=(const Channel&)=delete;
  const std::wstring& token() const noexcept{return token_;}
  static std::wstring event_name(std::wstring_view token,std::size_t i) {
    source_fixture::require(source_fixture::valid_token(token)&&i<=ack_index,"RAM client event name bound");
    return L"Local\\FlitzFS.ram-client."+std::wstring(token)+L"."+std::to_wstring(i);
  }
  // Only the host sends; one synchronous outstanding action. A failed wait,
  // role, sequence or signal is terminal, never permission to reset/retry.
  void request(Step step) {
    try {
      source_fixture::require(!client_&&sequence_.begin(step),"RAM host step/role");
      source_fixture::checked(SetEvent(events_[static_cast<std::size_t>(step)].value),"RAM step signal");
      source_fixture::require(WaitForSingleObject(events_[ack_index].value,5000)==WAIT_OBJECT_0,
          "RAM client acknowledgement deadline");
      source_fixture::require(sequence_.confirm(step),"RAM host acknowledgement order");
    }catch(...){sequence_.abort();throw;}
  }
  // The result HANDLE is owned by the exact Child and kept alive until after
  // this wait. It has priority over requests if both are already signaled.
  Step next(HANDLE peer_result) {
    try {
      source_fixture::require(client_&&!sequence_.pending()&&!sequence_.complete()&&
          !sequence_.failed()&&peer_result&&peer_result!=INVALID_HANDLE_VALUE,"RAM client receive/role");
      std::array<HANDLE,step_count+1> waits{};waits[0]=peer_result;
      for(std::size_t i=0;i<step_count;++i)waits[i+1]=events_[i].value;
      const auto result=WaitForMultipleObjects(static_cast<DWORD>(waits.size()),waits.data(),FALSE,5000);
      source_fixture::require(result>WAIT_OBJECT_0&&result<WAIT_OBJECT_0+waits.size(),
          "RAM source ended or client receive deadline");
      const auto step=static_cast<Step>(result-WAIT_OBJECT_0-1);
      source_fixture::require(sequence_.begin(step),"RAM client request order");
      return step;
    }catch(...){sequence_.abort();throw;}
  }
  void acknowledge(Step step) {
    try {
      source_fixture::require(client_&&sequence_.confirm(step),"RAM client acknowledgement/role");
      source_fixture::checked(SetEvent(events_[ack_index].value),"RAM acknowledgement signal");
    }catch(...){sequence_.abort();throw;}
  }
  bool complete() const noexcept{return sequence_.complete();}
  bool failed() const noexcept{return sequence_.failed();}
  unsigned copy_rounds() const noexcept{return sequence_.copy_rounds();}
 private:
  const bool client_;
  const std::wstring token_;
  std::array<source_fixture::Handle,step_count+1> events_;
  Sequence sequence_;
};
} // namespace ram_client_fixture
