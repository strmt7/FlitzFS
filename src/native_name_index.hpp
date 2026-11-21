#pragma once
#include "flitzfs/paged_store.hpp"
#include <compare>
#include <optional>
#include <string>
#include <string_view>

namespace flitzfs::detail {

// Reserved in the new indexed namespace. Existing namespace layouts have
// separate object-domain rules and do not admit this index.
inline constexpr std::uint64_t kNativeNameIndexObject = 3ULL << 61;
inline constexpr std::uint64_t kNativeIndexedInodeLimit = 1ULL << 61;
inline constexpr std::uint64_t kNativeIndexedDataObject = 1ULL << 61;
enum class NativeIndexKind : std::uint32_t { names=1, names_and_extents=2, names_extents_and_references=3 };

struct NativeNameKey {
  std::uint64_t parent = 0;
  std::string name;
  auto operator<=>(const NativeNameKey&) const = default;
};
struct NativeNameEntry {
  NativeNameKey key;
  std::uint64_t value = 0;
  bool operator==(const NativeNameEntry&) const = default;
};
struct NativeNameEdit {
  NativeNameKey key;
  std::optional<std::uint64_t> expected;
  std::optional<std::uint64_t> replacement;
};
struct NativeNameIndexRoot {
  std::uint64_t page = 0, next_page = 1, entries = 0;
  std::uint32_t height = 0;
  NativeIndexKind kind=NativeIndexKind::names;
};
NativeNameKey native_extent_key(std::uint64_t inode,std::uint64_t block);
std::uint64_t native_extent_block(std::string_view key);
NativeNameKey native_group_reference_key(std::uint64_t base,std::uint64_t member);

// Checked ordered names in one immutable paged-store snapshot. Borrow the
// snapshot on its control thread. Names supplied here are already canonical;
// Windows name policy and inode/parent consistency belong to the namespace.
class NativeNameIndex final {
 public:
  explicit NativeNameIndex(const PagedObjectStoreSnapshot&);
  NativeNameIndex(PagedObjectStoreSnapshot&&) = delete;
  std::optional<std::uint64_t> find(const NativeNameKey&) const;
  std::vector<NativeNameEntry> list(std::uint64_t parent, std::string_view after,
      std::size_t maximum,bool* more=nullptr) const;
  std::optional<NativeNameEntry> next_extent(const std::optional<NativeNameKey>& after) const;
  std::uint64_t extent_count(std::uint64_t inode) const;
  std::uint64_t group_reference_count(std::uint64_t base) const;
  std::uint64_t size() const noexcept { return root_.entries; }
  NativeIndexKind kind() const noexcept { return root_.kind; }
  void verify_all() const;

  // Initialize the reserved object domain in a new namespace.
  static PagedStoreInfo initialize(PagedObjectStoreWriter&,
      std::vector<StoreMutation> accompanying = {},NativeIndexKind kind=NativeIndexKind::names);
  // Compare each name with its expected binding, then publish the name edits
  // and the caller's owned inode/data records together. An absent expected
  // binding requires an unused name. An absent replacement erases a name.
  static PagedStoreInfo commit(PagedObjectStoreWriter&, std::span<const NativeNameEdit>,
      std::vector<StoreMutation> accompanying = {});
  // Prepare the identical COW mutations for size comparison before publication.
  // The caller may promote an existing mixed index when publishing a grouped root.
  static std::vector<StoreMutation> prepare_commit(const PagedObjectStoreSnapshot&,
      std::span<const NativeNameEdit>,std::vector<StoreMutation>,bool promote_references=false);

 private:
  std::span<const std::byte> query_page(std::uint64_t,std::vector<std::byte>& scratch) const;
  std::uint64_t count_parent(std::uint64_t parent) const;
  const PagedObjectStoreSnapshot& store_;
  NativeNameIndexRoot root_;
};

} // namespace flitzfs::detail
