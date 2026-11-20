#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace flitzfs {
struct HipAdapterHardware {
  std::uint32_t vendor_id=0,device_id=0,subsystem_id=0,revision=0;
  bool operator==(const HipAdapterHardware&) const noexcept=default;
};

enum class WindowsAmdDisplayStatus { unavailable, absent, not_ready, ready };

struct WindowsAmdDisplayDevice {
  std::wstring instance_id;
  std::uint32_t devnode_status=0,problem_code=0;
  bool observed=false;
  bool ready() const noexcept;
};

struct WindowsAmdDisplaySnapshot {
  std::vector<WindowsAmdDisplayDevice> devices;
  bool complete=false;
  WindowsAmdDisplayStatus status(const HipAdapterHardware* expected=nullptr) const noexcept;
  // Match the full PCI signature. Two identical cards are admitted together
  // only if the status of each matching device is ready.
  std::vector<std::wstring> bind(const HipAdapterHardware&) const;
};

// Configuration Manager reads only. These APIs open no GPU context.
WindowsAmdDisplaySnapshot observe_windows_amd_displays() noexcept;
bool bound_windows_amd_displays_ready(std::span<const std::wstring>) noexcept;
}
