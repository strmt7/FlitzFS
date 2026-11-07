#include "winfsp_readonly.hpp"
#include "flitzfs/container.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace {
void require(bool pass, const char* message) { if (!pass) throw std::runtime_error(message); }
// Native API uses mutable PWSTR; these callbacks never modify names.
PWSTR name(PCWSTR value) { return const_cast<PWSTR>(value); }
struct Fixture {
  fs::path dir;
  std::vector<fs::path> files;
  Fixture() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned i = 0; i < 100 && dir.empty(); ++i) {
      const auto candidate = fs::temp_directory_path() / ("flitzfs-native-" + std::to_string(stamp) + "-" + std::to_string(i));
      if (fs::create_directory(candidate)) dir = candidate;
    }
    require(!dir.empty(), "isolated test folder creation");
  }
  fs::path file(const char* value) { const auto p = dir / value; files.push_back(p); return p; }
  ~Fixture() {
    for (const auto& path : files) { std::error_code ec; fs::remove(path, ec); }
    std::error_code ec; fs::remove(dir, ec);
  }
};
void write(const fs::path& path, const std::vector<unsigned char>& bytes) {
  std::ofstream output(path, std::ios::binary);
  require(static_cast<bool>(output), "fixture creation");
  if (!bytes.empty()) output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  require(static_cast<bool>(output), "fixture write");
}
// Decode the native records returned by the real WinFsp DLL helper.
std::vector<std::wstring> names(const std::byte* data, ULONG length) {
  std::vector<std::wstring> result;
  std::size_t offset = 0;
  while (offset + sizeof(UINT16) <= length) {
    UINT16 size = 0;
    std::memcpy(&size, data + offset, sizeof(size));
    if (!size) break;
    require(size >= sizeof(FSP_FSCTL_DIR_INFO) && offset + size <= length, "directory record bounds");
    require((size - sizeof(FSP_FSCTL_DIR_INFO)) % sizeof(wchar_t) == 0, "directory character alignment");
    const auto* entry = reinterpret_cast<const FSP_FSCTL_DIR_INFO*>(data + offset);
    require(entry->FileInfo.IndexNumber != 0, "directory stable file identity");
    result.emplace_back(entry->FileNameBuf, (size - sizeof(FSP_FSCTL_DIR_INFO)) / sizeof(wchar_t));
    offset += (size + 7U) & ~std::size_t{7U};
  }
  return result;
}
} // namespace

int main() {
  try {
    Fixture fixture;
    const auto input = fixture.file("source.bin");
    const auto archive = fixture.file("source.flfs");
    std::vector<unsigned char> expected(260'503U);
    for (std::size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<unsigned char>('A' + i % 11U);
    write(input, expected);
    const auto packed = flitzfs::pack_new(input, archive);
#ifdef FLITZFS_TEST_REQUIRES_ZSTD
    require(packed.zstd_chunk_count > 0, "Windows lane must actually exercise Zstandard");
#endif
    const auto empty_input = fixture.file("empty.bin");
    const auto empty_archive = fixture.file("empty.flfs");
    write(empty_input, {});
    (void)flitzfs::pack_new(empty_input, empty_archive);
    const auto original_size = fs::file_size(archive);
    {
      flitzfs::WinFspReadOnly adapter({{"Docs/Alpha.bin", archive}, {"Docs/Beta.bin", empty_archive},
          {"Docs/ a.bin", empty_archive}, {"Docs/(a).bin", empty_archive}});
      FSP_FILE_SYSTEM fs_context{}; // No filesystem creation, driver, dispatcher or mount.
      adapter.bind_callbacks(fs_context);
      const auto& cb = adapter.callbacks();
      const auto params = adapter.parameters();
      require(params.ReadOnlyVolume && params.UmFileContextIsUserContext2 && params.PersistentAcls, "read-only volume contract");
      require(!params.NamedStreams && !params.ReparsePoints && !params.HardLinks && !params.UnicodeOnDisk, "unsupported capabilities advertised");
      require(params.IrpCapacity == 100 && params.MaxComponentLength == 100, "bounded queue/name contract");
      FSP_FSCTL_VOLUME_INFO volume{};
      require(cb.GetVolumeInfo(&fs_context, &volume) == STATUS_SUCCESS && volume.FreeSize == 0 && volume.TotalSize >= expected.size(), "volume information");
      FSP_FSCTL_FILE_INFO info{};
      PVOID file = nullptr, directory = nullptr;
      require(cb.Open(&fs_context, name(L"\\docs\\ALPHA.bin"), FILE_NON_DIRECTORY_FILE,
          FILE_GENERIC_READ, &file, &info) == STATUS_SUCCESS, "native file open");
      const auto file_id = info.IndexNumber;
      require(file && info.FileSize == expected.size() && info.AllocationSize >= info.FileSize, "native metadata");
      require(cb.Open(&fs_context, name(L"\\Docs"), FILE_DIRECTORY_FILE,
          FILE_GENERIC_READ, &directory, &info) == STATUS_SUCCESS, "directory open");
      PVOID rejected = reinterpret_cast<PVOID>(1);
      require(cb.Open(&fs_context, name(L"\\Docs"), FILE_NON_DIRECTORY_FILE, FILE_GENERIC_READ,
          &rejected, &info) == STATUS_FILE_IS_A_DIRECTORY && rejected == nullptr, "directory opened as file");
      require(cb.Open(&fs_context, name(L"\\Docs\\Alpha.bin"), FILE_DIRECTORY_FILE, FILE_GENERIC_READ,
          &rejected, &info) == STATUS_NOT_A_DIRECTORY, "file opened as directory");
      require(cb.Open(&fs_context, name(L"\\missing"), 0, FILE_GENERIC_READ,
          &rejected, &info) == STATUS_OBJECT_NAME_NOT_FOUND, "missing entry status");
      for (const auto* path : {L"\\Docs\\Alpha.bin:ads", L"\\Docs\\Alpha.bin?", L"\\..\\escape", L"\\Docs\\NUL.txt"})
        require(cb.Open(&fs_context, name(path), 0, FILE_GENERIC_READ, &rejected, &info) == STATUS_OBJECT_NAME_INVALID, "invalid name status");
      for (const auto access : {UINT32(FILE_WRITE_DATA), UINT32(FILE_APPEND_DATA), UINT32(DELETE), UINT32(WRITE_DAC), UINT32(GENERIC_ALL)})
        require(cb.Open(&fs_context, name(L"\\Docs\\Alpha.bin"), 0, access, &rejected, &info) == STATUS_MEDIA_WRITE_PROTECTED, "write access accepted");
      require(cb.Open(&fs_context, name(L"\\Docs\\Alpha.bin"), FILE_DELETE_ON_CLOSE,
          FILE_GENERIC_READ, &rejected, &info) == STATUS_MEDIA_WRITE_PROTECTED, "delete-on-close accepted");
      std::array<unsigned char, 2048> buffer{};
      ULONG count = 99;
      const auto start = 128U * 1024U - 117U;
      require(cb.Read(&fs_context, file, buffer.data(), start, static_cast<ULONG>(buffer.size()), &count) == STATUS_SUCCESS && count == buffer.size(), "cross-chunk native read");
      for (std::size_t i = 0; i < count; ++i) require(buffer[i] == expected[start + i], "native byte parity");
      require(cb.Read(&fs_context, file, buffer.data(), expected.size() - 19, 100, &count) == STATUS_SUCCESS && count == 19, "short read across EOF");
      require(cb.Read(&fs_context, file, buffer.data(), expected.size(), 1, &count) == STATUS_END_OF_FILE && !count, "native EOF status");
      require(cb.Read(&fs_context, file, nullptr, UINT64_MAX, 0, &count) == STATUS_SUCCESS && !count, "zero length read");
      require(cb.Read(&fs_context, file, buffer.data(), 0, flitzfs::kMaxSingleReadBytes + 1, &count) == STATUS_INVALID_PARAMETER && !count, "read allocation budget");
      require(cb.Read(&fs_context, file, nullptr, 0, 1, &count) == STATUS_INVALID_PARAMETER && !count, "null read buffer");
      require(cb.Read(&fs_context, directory, buffer.data(), 0, 1, &count) == STATUS_FILE_IS_A_DIRECTORY, "reading a directory");
      cb.Cleanup(&fs_context, file, nullptr, 0);
      require(cb.Read(&fs_context, file, buffer.data(), 0, 1, &count) == STATUS_SUCCESS && count == 1, "cleanup must preserve later reads");
      require(cb.GetFileInfo(&fs_context, file, &info) == STATUS_SUCCESS && info.IndexNumber == file_id, "stable metadata identity");
      require(cb.Flush(&fs_context, nullptr, nullptr) == STATUS_SUCCESS, "read-only volume flush");
      require(cb.Flush(&fs_context, file, &info) == STATUS_SUCCESS, "read-only file flush");
      SIZE_T required = 0;
      require(cb.GetSecurity(&fs_context, file, nullptr, &required) == STATUS_BUFFER_OVERFLOW && required > 0 && required <= 4096, "security size negotiation");
      std::vector<std::byte> descriptor(required);
      require(cb.GetSecurity(&fs_context, file, descriptor.data(), &required) == STATUS_SUCCESS && IsValidSecurityDescriptor(descriptor.data()), "security descriptor validity");
      BOOL present = FALSE, defaulted = FALSE;
      PACL acl = nullptr;
      require(GetSecurityDescriptorDacl(descriptor.data(), &present, &acl, &defaulted) && present && acl && acl->AceCount == 1, "one explicit owner ACE");
      PSID owner = nullptr;
      require(GetSecurityDescriptorOwner(descriptor.data(), &owner, &defaulted) && owner, "descriptor owner");
      PVOID ace_ptr = nullptr;
      require(GetAce(acl, 0, &ace_ptr) != FALSE, "descriptor ACE");
      const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(ace_ptr);
      require(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && EqualSid(owner, const_cast<DWORD*>(&ace->SidStart)), "ACE must belong to owner");
      require((ace->Mask & (FILE_WRITE_DATA | DELETE | WRITE_DAC | WRITE_OWNER)) == 0, "descriptor must not grant writes");
      UINT32 attributes = 0;
      require(cb.GetSecurityByName(&fs_context, name(L"\\Docs"), &attributes, nullptr, nullptr) == STATUS_SUCCESS && (attributes & FILE_ATTRIBUTE_DIRECTORY), "attributes-only security query");
      SIZE_T too_small = 1;
      std::byte sentinel{0x7e};
      require(cb.GetSecurity(&fs_context, file, &sentinel, &too_small) == STATUS_BUFFER_OVERFLOW && sentinel == std::byte{0x7e}, "security copy bounds");
      alignas(FSP_FSCTL_DIR_INFO) std::array<std::byte, 1024> entries{};
      require(cb.ReadDirectory(&fs_context, directory, nullptr, nullptr, entries.data(), static_cast<ULONG>(entries.size()), &count) == STATUS_SUCCESS, "directory enumeration");
      require(names(entries.data(), count) == std::vector<std::wstring>{L".", L"..", L" a.bin", L"(a).bin", L"Alpha.bin", L"Beta.bin"}, "directory record names/order");
      std::vector<std::wstring> collected;
      std::wstring marker;
      for (unsigned page = 0; page < 8; ++page) {
        require(cb.ReadDirectory(&fs_context, directory, nullptr, marker.empty() ? nullptr : marker.data(),
            entries.data(), 128, &count) == STATUS_SUCCESS, "paged directory enumeration");
        const auto batch = names(entries.data(), count);
        if (batch.empty()) break;
        collected.insert(collected.end(), batch.begin(), batch.end());
        marker = batch.back();
      }
      require(collected == std::vector<std::wstring>{L".", L"..", L" a.bin", L"(a).bin", L"Alpha.bin", L"Beta.bin"}, "directory marker continuation");
      require(cb.ReadDirectory(&fs_context, directory, nullptr, name(L"alpha.BIN"), entries.data(), 1024, &count) == STATUS_SUCCESS && names(entries.data(), count) == std::vector<std::wstring>{L"Beta.bin"}, "case folded directory marker");
      require(cb.ReadDirectory(&fs_context, directory, nullptr, nullptr, entries.data(), 1, &count) == STATUS_BUFFER_OVERFLOW && !count, "directory tiny buffer");
      require(cb.ReadDirectory(&fs_context, file, nullptr, nullptr, entries.data(), 1024, &count) == STATUS_NOT_A_DIRECTORY, "enumerating a file");
      require(cb.Create(&fs_context, name(L"\\new"), 0, 0, 0, nullptr, 0, &rejected, &info) == STATUS_MEDIA_WRITE_PROTECTED && !rejected, "create denied");
      require(cb.Overwrite(&fs_context, file, 0, FALSE, 0, &info) == STATUS_MEDIA_WRITE_PROTECTED, "overwrite denied");
      require(cb.Write(&fs_context, file, buffer.data(), 0, 1, FALSE, FALSE, &count, &info) == STATUS_MEDIA_WRITE_PROTECTED && !count, "write denied");
      require(cb.SetBasicInfo(&fs_context, file, 0, 0, 0, 0, 0, &info) == STATUS_MEDIA_WRITE_PROTECTED, "metadata write denied");
      require(cb.SetFileSize(&fs_context, file, 0, FALSE, &info) == STATUS_MEDIA_WRITE_PROTECTED, "truncate denied");
      require(cb.SetVolumeLabel(&fs_context, name(L"new"), &volume) == STATUS_MEDIA_WRITE_PROTECTED, "label write denied");
      require(cb.CanDelete(&fs_context, file, nullptr) == STATUS_MEDIA_WRITE_PROTECTED && cb.SetDelete(&fs_context, file, nullptr, TRUE) == STATUS_MEDIA_WRITE_PROTECTED, "delete denied");
      require(cb.Rename(&fs_context, file, nullptr, nullptr, TRUE) == STATUS_MEDIA_WRITE_PROTECTED, "rename denied");
      require(cb.SetSecurity(&fs_context, file, DACL_SECURITY_INFORMATION, nullptr) == STATUS_MEDIA_WRITE_PROTECTED, "ACL write denied");
      std::atomic<bool> parity{true};
      {
        std::array<std::jthread, 2> threads;
        for (std::size_t t = 0; t < threads.size(); ++t) threads[t] = std::jthread([&, t] {
          for (unsigned i = 0; i < 32; ++i) {
            std::array<unsigned char, 257> data{};
            ULONG n = 0;
            const auto position = (i * 491U + t * 17U) % (expected.size() - data.size());
            if (cb.Read(&fs_context, file, data.data(), position, static_cast<ULONG>(data.size()), &n) != STATUS_SUCCESS || n != data.size()) { parity = false; return; }
            for (std::size_t k = 0; k < data.size(); ++k) if (data[k] != expected[position + k]) parity = false;
          }
        });
      }
      require(parity.load(), "concurrent native read parity");
      cb.Close(&fs_context, file);
      cb.Close(&fs_context, directory);
      require(cb.GetFileInfo(&fs_context, file, &info) == STATUS_INVALID_HANDLE, "closed handle accepted");
      std::array<PVOID, flitzfs::WinFspReadOnly::max_open_handles> handles{};
      for (auto& handle : handles)
        require(cb.Open(&fs_context, name(L"\\Docs\\Alpha.bin"), 0, FILE_GENERIC_READ, &handle, &info) == STATUS_SUCCESS, "bounded handle allocation");
      require(cb.Open(&fs_context, name(L"\\Docs"), 0, FILE_GENERIC_READ, &rejected, &info) == STATUS_INSUFFICIENT_RESOURCES && !rejected, "handle exhaustion must fail closed");
      const auto stale = handles[0];
      cb.Close(&fs_context, stale);
      require(cb.Open(&fs_context, name(L"\\Docs"), 0, FILE_GENERIC_READ, &handles[0], &info) == STATUS_SUCCESS && stale != handles[0], "handle generation reuse");
      cb.Close(&fs_context, stale);
      require(cb.GetFileInfo(&fs_context, handles[0], &info) == STATUS_SUCCESS, "stale close must not close reused slot");
      for (auto handle : handles) cb.Close(&fs_context, handle);
    } // release all backing handles before fixture cleanup
    require(fs::file_size(archive) == original_size, "backing file changed");
    flitzfs::Reader::open(archive).verify_all();
    const auto damaged = fixture.file("damaged.flfs");
    (void)flitzfs::pack_new(input, damaged);
    {
      std::fstream file(damaged, std::ios::binary | std::ios::in | std::ios::out);
      require(static_cast<bool>(file), "open disposable corruption fixture");
      file.seekg(40);
      char value = 0; file.read(&value, 1); file.seekp(40);
      value ^= 0x7f; file.write(&value, 1);
      require(static_cast<bool>(file), "write disposable corruption fixture");
    }
    bool rejected = false;
    try { flitzfs::WinFspReadOnly invalid({{"bad.bin", damaged}}); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "adapter must verify content before exposure");
    std::cout << "PASS: native WinFsp callbacks, owner-only ACL, bounded handles, directory pagination, concurrent reads; NO mount or driver\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
