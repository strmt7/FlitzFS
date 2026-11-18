#pragma once
#include "flitzfs/windows_path.hpp"
#include <windows.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace flitzfs {
// Pin the parents associated with a local absolute name. Leaf opens and
// creation retain their own identity and exclusive-creation checks.
class VolumeDirectoryPin {
 public:
  explicit VolumeDirectoryPin(const std::wstring& directory) {
    handle_=CreateFileW(directory.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
        OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(handle_==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot pin a volume path parent");
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if(GetFileType(handle_)!=FILE_TYPE_DISK ||
        !GetFileInformationByHandleEx(handle_,FileAttributeTagInfo,&attributes,sizeof(attributes)) ||
        !(attributes.FileAttributes&FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) {
      CloseHandle(handle_);
      throw std::runtime_error("The volume path requires ordinary local directories");
    }
  }
  ~VolumeDirectoryPin(){if(handle_!=INVALID_HANDLE_VALUE)CloseHandle(handle_);}
  VolumeDirectoryPin(VolumeDirectoryPin&& other) noexcept:
      handle_(std::exchange(other.handle_,INVALID_HANDLE_VALUE)) {}
  VolumeDirectoryPin(const VolumeDirectoryPin&)=delete;
  VolumeDirectoryPin& operator=(const VolumeDirectoryPin&)=delete;
 private:
  HANDLE handle_=INVALID_HANDLE_VALUE;
};

inline std::vector<VolumeDirectoryPin> pin_volume_path_parents(const std::wstring& path) {
  if(path.size()<4 || path[1]!=L':' || path[2]!=L'\\' || !ordinary_windows_file_path(path))
    throw std::runtime_error("The volume path requires an absolute local name");
  std::vector<VolumeDirectoryPin> parents;
  parents.emplace_back(path.substr(0,3));
  for(auto next=path.find(L'\\',3);next!=path.npos;next=path.find(L'\\',next+1))
    parents.emplace_back(path.substr(0,next));
  return parents;
}
}
