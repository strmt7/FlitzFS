#include "gpu_device_readiness.hpp"
#include <windows.h>
#include <cfg.h>
#include <cfgmgr32.h>
#include <algorithm>
#include <array>
#include <cwchar>
#include <string_view>

namespace flitzfs {
namespace {
constexpr wchar_t display_class[]=L"{4d36e968-e325-11ce-bfc1-08002be10318}";
constexpr std::wstring_view amd_pci=L"PCI\\VEN_1002&DEV_";

bool starts_with_id(std::wstring_view id,std::wstring_view prefix) noexcept {
  return !prefix.empty() && id.size()>prefix.size() &&
      CompareStringOrdinal(id.data(),static_cast<int>(prefix.size()),
          prefix.data(),static_cast<int>(prefix.size()),TRUE)==CSTR_EQUAL;
}

struct PciPrefix {
  std::array<wchar_t,64> text{};
  std::size_t length=0;
  std::wstring_view view() const noexcept {return {text.data(),length};}
};

PciPrefix pci_prefix(const HipAdapterHardware& hardware) noexcept {
  PciPrefix prefix;
  if(hardware.vendor_id!=0x1002 || !hardware.device_id || hardware.device_id>0xffff ||
      hardware.revision>0xff)
    return prefix;
  const auto count=std::swprintf(prefix.text.data(),prefix.text.size(),
      L"PCI\\VEN_%04X&DEV_%04X&SUBSYS_%08X&REV_%02X\\",
      hardware.vendor_id,hardware.device_id,hardware.subsystem_id,hardware.revision);
  if(count>0 && static_cast<std::size_t>(count)<prefix.text.size())
    prefix.length=static_cast<std::size_t>(count);
  return prefix;
}

void observe_devnode(const std::wstring& instance,WindowsAmdDisplayDevice& result) noexcept {
  DEVINST node=0;
  if(instance.empty() || CM_Locate_DevNodeW(&node,const_cast<wchar_t*>(instance.c_str()),
      CM_LOCATE_DEVNODE_NORMAL)!=CR_SUCCESS)
    return;
  ULONG state=0,problem=0;
  if(CM_Get_DevNode_Status(&state,&problem,node,0)!=CR_SUCCESS)return;
  result.devnode_status=state;
  result.problem_code=(state&DN_HAS_PROBLEM)?problem:0;
  result.observed=true;
}
}

bool WindowsAmdDisplayDevice::ready() const noexcept {
  return observed && (devnode_status&DN_STARTED) && !(devnode_status&(DN_HAS_PROBLEM|DN_WILL_BE_REMOVED));
}

WindowsAmdDisplayStatus WindowsAmdDisplaySnapshot::status(const HipAdapterHardware* expected) const noexcept {
  using State=WindowsAmdDisplayStatus;
  if(!complete)return State::unavailable;
  if(!expected) {
    if(devices.empty())return State::absent;
    return std::ranges::any_of(devices,&WindowsAmdDisplayDevice::ready)?State::ready:State::not_ready;
  }
  const auto prefix=pci_prefix(*expected);
  bool matched=false;
  for(const auto& device:devices) {
    if(!starts_with_id(device.instance_id,prefix.view()))continue;
    matched=true;
    if(!device.ready())return State::not_ready;
  }
  return matched?State::ready:State::absent;
}

std::vector<std::wstring> WindowsAmdDisplaySnapshot::bind(const HipAdapterHardware& hardware) const {
  if(status(&hardware)!=WindowsAmdDisplayStatus::ready)return {};
  const auto prefix=pci_prefix(hardware);
  std::vector<std::wstring> result;
  for(const auto& device:devices)
    if(starts_with_id(device.instance_id,prefix.view()))result.push_back(device.instance_id);
  return result;
}

WindowsAmdDisplaySnapshot observe_windows_amd_displays() noexcept {
  constexpr ULONG filters=CM_GETIDLIST_FILTER_CLASS|CM_GETIDLIST_FILTER_PRESENT;
  ULONG count=0;
  if(CM_Get_Device_ID_List_SizeW(&count,display_class,filters)!=CR_SUCCESS || !count)return {};
  try {
    std::vector<wchar_t> ids(count);
    if(CM_Get_Device_ID_ListW(display_class,ids.data(),count,filters)!=CR_SUCCESS)return {};
    WindowsAmdDisplaySnapshot result;
    auto current=ids.begin();
    while(current!=ids.end() && *current!=L'\0') {
      const auto end=std::find(current,ids.end(),L'\0');
      if(end==ids.end() || end-current>=MAX_DEVICE_ID_LEN)return {};
      const std::wstring_view id(current,end);
      if(starts_with_id(id,amd_pci)) {
        auto& device=result.devices.emplace_back(std::wstring(id));
        observe_devnode(device.instance_id,device);
      }
      current=end+1;
    }
    if(current==ids.end())return {};
    result.complete=true;
    return result;
  }catch(...){return {};}
}

bool bound_windows_amd_displays_ready(std::span<const std::wstring> instances) noexcept {
  if(instances.empty())return false;
  return std::ranges::all_of(instances,[](const std::wstring& instance) {
    WindowsAmdDisplayDevice observed;
    observe_devnode(instance,observed);
    return observed.ready();
  });
}
}
