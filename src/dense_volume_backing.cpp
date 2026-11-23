#include "dense_volume_backing.hpp"
#include "record_digest.hpp"
#include "store_file.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <utility>

namespace flitzfs::detail {
namespace {
using Code = NativeFsErrorCode;
using Bytes = std::vector<std::byte>;
constexpr auto unit = kStoreCommitUnit;
constexpr std::uint64_t anchor_units = 2, directory_units = 2;
constexpr std::size_t header_bytes = 80, digest_at = 48, run_bytes = 16;
constexpr std::size_t intent_digest_at = 216, anchor_extension_bytes = intent_digest_at + Digest{}.size();
constexpr auto anchor_v2_bytes = header_bytes + anchor_extension_bytes;
constexpr auto maximum_move_units = kStoreMaxTransactionBytes / unit;
constexpr std::uint64_t slots_per_group = (unit - header_bytes) / run_bytes;
constexpr auto group_units = directory_units + slots_per_group;
constexpr auto maximum_file_bytes = static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
constexpr auto maximum_sequence = (std::numeric_limits<std::uint64_t>::max)();
constexpr char anchor_magic[] = "FLTDENS1", directory_magic[] = "FLTDIR01";

[[noreturn]] void fail(Code code, const char* message) { throw NativeFsError(code, message); }

struct Run {
  std::uint64_t target, units;
  bool operator==(const Run&) const = default;
};
// Source and target use commit-unit ordinals. Inverse indexes use the same
// run operations. Absolute physical ordinals keep directory gaps discontinuous.
using Runs = std::map<std::uint64_t, Run>;

std::uint64_t end_of(Runs::const_iterator run) noexcept { return run->first + run->second.units; }

bool adjacent(Runs::const_iterator left, Runs::const_iterator right) noexcept {
  return end_of(left) == right->first && left->second.target + left->second.units == right->second.target;
}

void merge_next(Runs& runs, Runs::iterator current) noexcept {
  const auto next = std::next(current);
  if (next != runs.end() && adjacent(current, next)) {
    current->second.units += next->second.units;
    runs.erase(next);
  }
}

void insert(Runs& runs, std::uint64_t first, Run incoming) {
  const auto next = runs.lower_bound(first);
  const auto previous = next == runs.begin() ? runs.end() : std::prev(next);
  if ((previous != runs.end() && end_of(previous) > first) ||
      (next != runs.end() && first + incoming.units > next->first))
    fail(Code::corrupt, "overlapping dense extent ownership");
  auto current = previous;
  if (previous != runs.end() && end_of(previous) == first &&
      previous->second.target + previous->second.units == incoming.target) {
    current->second.units += incoming.units;
  } else {
    current = runs.emplace_hint(next, first, incoming);
  }
  merge_next(runs, current);
}

// Remove a slice of one existing run. Prepare a split before shortening the
// old mapping so allocation failure preserves every unrelated live mapping.
void erase(Runs& runs, std::uint64_t first, std::uint64_t end) {
  const auto run = std::prev(runs.upper_bound(first));
  const auto old_end = end_of(run);
  if (first == run->first) {
    if (end == old_end) {
      runs.erase(run);
    } else {
      auto suffix = runs.extract(run);
      suffix.mapped().target += end - first;
      suffix.mapped().units = old_end - end;
      suffix.key() = end;
      runs.insert(std::move(suffix));
    }
    return;
  }
  if (end < old_end)
    runs.emplace(end, Run{run->second.target + end - run->first, old_end - end});
  run->second.units = first - run->first;
}

std::uint64_t first_free(const Runs& physical, std::uint64_t first) {
  auto next = physical.upper_bound(first);
  if (next != physical.begin()) --next;
  for (; next != physical.end() && next->first <= first; ++next)
    first = (std::max)(first, end_of(next));
  return first;
}

constexpr std::uint64_t count_slots(std::uint64_t bytes) noexcept {
  const auto units = bytes / unit;
  const auto remaining = units > anchor_units ? units - anchor_units : 0;
  const auto tail = remaining % group_units;
  return remaining / group_units * slots_per_group + (tail > directory_units ? tail - directory_units : 0);
}

constexpr std::uint64_t group_start(std::uint64_t group) noexcept {
  return anchor_units + group * group_units;
}

constexpr std::uint64_t payload_start(std::uint64_t group) noexcept {
  return group_start(group) + directory_units;
}

constexpr std::uint64_t group_at(std::uint64_t physical_unit) noexcept {
  return (physical_unit - anchor_units) / group_units;
}

constexpr std::uint64_t slot_end(std::uint64_t count) noexcept {
  if (!count) return 0;
  const auto last = count - 1;
  return (payload_start(last / slots_per_group) + last % slots_per_group + 1) * unit;
}

std::uint64_t get(std::span<const std::byte> bytes, std::size_t at, unsigned count = 8) {
  std::uint64_t result = 0;
  for (unsigned i = 0; i < count; ++i)
    result |= std::uint64_t(std::to_integer<unsigned>(bytes[at + i])) << (i * 8);
  return result;
}

void put(std::span<std::byte> bytes, std::size_t at, std::uint64_t value, unsigned count = 8) {
  for (unsigned i = 0; i < count; ++i)
    bytes[at + i] = static_cast<std::byte>(value >> (i * 8));
}

StoreIdentity identity_at(std::span<const std::byte> bytes) {
  StoreIdentity result;
  std::copy_n(bytes.begin() + 32, result.size(), result.begin());
  return result;
}

auto checksum(std::span<const std::byte> bytes) noexcept {
  return record_digest(bytes.first(digest_at), bytes.subspan(header_bytes));
}

bool verified(std::span<const std::byte> bytes) noexcept {
  const auto digest = checksum(bytes);
  return std::equal(digest.begin(), digest.end(), bytes.begin() + digest_at);
}

void finish_record(std::span<std::byte> bytes) noexcept {
  const auto digest = checksum(bytes);
  std::copy(digest.begin(), digest.end(), bytes.begin() + digest_at);
}

struct Geometry {
  StoreIdentity volume;
  std::uint64_t logical_limit, physical_limit;
  bool operator==(const Geometry&) const = default;

  void check(Code code) const {
    if (volume == StoreIdentity{} || !logical_limit || logical_limit % unit ||
        logical_limit > maximum_file_bytes || physical_limit > maximum_file_bytes ||
        !count_slots(physical_limit))
      fail(code, "invalid dense backing geometry");
  }
};

struct TailMove {
  std::uint64_t source, destination, logical, units;
  bool operator==(const TailMove&) const = default;
};

struct DirectoryChange {
  std::uint64_t sequence;
  Digest before, after;
  bool operator==(const DirectoryChange&) const = default;
};

struct MoveIntent {
  TailMove range;
  DirectoryChange source, destination;
  Digest payload;
  bool operator==(const MoveIntent&) const = default;
};

struct Anchor {
  std::uint64_t sequence = 0; // Zero is the original, unextended format.
  std::optional<MoveIntent> move;
  bool operator==(const Anchor&) const = default;
};

struct OpenAnchor {
  Geometry geometry;
  Anchor anchor;
};

bool in_payload(std::uint64_t first, std::uint64_t count, std::uint64_t end) noexcept {
  if (first < payload_start(0) || first >= end || count > end - first) return false;
  const auto group = group_at(first);
  return first >= payload_start(group) && count <= group_start(group + 1) - first;
}

Digest digest_at_offset(std::span<const std::byte> bytes, std::size_t at) noexcept {
  Digest digest;
  std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(at), digest.size(), digest.begin());
  return digest;
}

void put_digest(std::span<std::byte> bytes, std::size_t at, const Digest& digest) noexcept {
  std::copy(digest.begin(), digest.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at));
}

Bytes anchor_record(const Geometry& geometry, const Anchor& anchor = {}) {
  Bytes bytes(anchor.sequence ? anchor_v2_bytes : header_bytes);
  std::memcpy(bytes.data(), anchor_magic, 8);
  put(bytes, 8, anchor.sequence ? 2 : 1, 4);
  if (anchor.sequence) put(bytes, 12, anchor_extension_bytes, 4);
  put(bytes, 16, geometry.logical_limit);
  put(bytes, 24, geometry.physical_limit);
  std::copy(geometry.volume.begin(), geometry.volume.end(), bytes.begin() + 32);
  const auto base = std::span(bytes).first(header_bytes);
  // The original reader recognizes the intact base and refuses version 2.
  // It must not silently choose the older peer during the format transition.
  finish_record(base);
  if (!anchor.sequence) return bytes;
  auto extension = std::span(bytes).subspan(header_bytes);
  put(extension, 0, anchor.sequence);
  if (anchor.move) {
    const auto& [range, source, destination, payload] = *anchor.move;
    put(extension, 8, range.source);
    put(extension, 16, range.destination);
    put(extension, 24, range.logical);
    put(extension, 32, range.units);
    put(extension, 40, source.sequence);
    put(extension, 48, destination.sequence);
    put_digest(extension, 56, payload);
    put_digest(extension, 88, source.before);
    put_digest(extension, 120, source.after);
    put_digest(extension, 152, destination.before);
    put_digest(extension, 184, destination.after);
  }
  put_digest(extension, intent_digest_at, record_digest(base, extension.first(intent_digest_at)));
  return bytes;
}

// Read each peer separately so a localized I/O failure can use its mirror.
bool try_read(const StoreByteIo& io, std::uint64_t end, std::uint64_t at, std::span<std::byte> out) {
  if (at > end || out.size() > end - at) return false;
  try {
    io.read_exact(at, out);
    return true;
  } catch (const std::runtime_error&) {
    return false;
  }
}

std::optional<OpenAnchor> read_anchor(const StoreByteIo& io, std::uint64_t end, unsigned slot) {
  std::array<std::byte, header_bytes> bytes;
  if (!try_read(io, end, slot * unit, bytes) || !verified(bytes)) return {};
  const auto version = get(bytes, 8, 4), extension_bytes = get(bytes, 12, 4);
  if (std::memcmp(bytes.data(), anchor_magic, 8) ||
      !((version == 1 && !extension_bytes) || (version == 2 && extension_bytes == anchor_extension_bytes)))
    fail(Code::corrupt, "intact dense backing version or header is unsupported");
  const Geometry geometry{identity_at(bytes), get(bytes, 16), get(bytes, 24)};
  geometry.check(Code::corrupt);
  if (end > geometry.physical_limit)
    fail(Code::corrupt, "dense backing exceeds its physical extent limit");
  if (version == 1) return OpenAnchor{geometry, {}};
  std::array<std::byte, anchor_extension_bytes> extension;
  if (!try_read(io, end, slot * unit + header_bytes, extension) ||
      record_digest(bytes, std::span(extension).first(intent_digest_at)) != digest_at_offset(extension, intent_digest_at))
    return {};
  const auto sequence = get(extension, 0);
  if (!sequence || sequence == maximum_sequence) fail(Code::corrupt, "invalid dense anchor generation");
  if (sequence % 2 == 0) {
    if (std::any_of(extension.begin() + 8, extension.begin() + intent_digest_at, [](auto b) { return b != std::byte{}; }))
      fail(Code::corrupt, "settled dense anchor contains a relocation");
    return OpenAnchor{geometry, {sequence, {}}};
  }
  const TailMove range{get(extension, 8), get(extension, 16), get(extension, 24), get(extension, 32)};
  const auto source = get(extension, 40), destination = get(extension, 48);
  const auto logical_end = geometry.logical_limit / unit;
  if (!range.units || range.units > maximum_move_units || range.logical >= logical_end ||
      range.units > logical_end - range.logical || !in_payload(range.source, range.units, end / unit) ||
      !in_payload(range.destination, range.units, end / unit) || group_at(range.destination) >= group_at(range.source) ||
      !source || source == maximum_sequence || !destination || destination == maximum_sequence)
    fail(Code::corrupt, "dense relocation intent is out of bounds");
  return OpenAnchor{geometry, {sequence, MoveIntent{range,
      {source, digest_at_offset(extension, 88), digest_at_offset(extension, 120)},
      {destination, digest_at_offset(extension, 152), digest_at_offset(extension, 184)},
      digest_at_offset(extension, 56)}}};
}

struct Directory {
  std::uint64_t sequence = 0;
  Runs physical;
};

Bytes directory_record(const Geometry& geometry, std::uint64_t group, std::uint64_t sequence,
    const Runs& physical) {
  if (!sequence || physical.size() > slots_per_group)
    fail(Code::limit, "dense directory generation or record limit");
  Bytes bytes(header_bytes + physical.size() * run_bytes);
  std::memcpy(bytes.data(), directory_magic, 8);
  put(bytes, 8, 1, 4);
  put(bytes, 12, physical.size(), 4);
  put(bytes, 16, group);
  put(bytes, 24, sequence);
  std::copy(geometry.volume.begin(), geometry.volume.end(), bytes.begin() + 32);
  auto at = header_bytes;
  for (const auto& [first, run] : physical) {
    put(bytes, at, run.target);
    put(bytes, at + 8, first - payload_start(group), 4);
    put(bytes, at + 12, run.units, 4);
    at += run_bytes;
  }
  finish_record(bytes);
  return bytes;
}

std::optional<Directory> read_directory(const StoreByteIo& io, const Geometry& geometry,
    std::uint64_t end, std::uint64_t group, unsigned slot) {
  const auto offset = (group_start(group) + slot) * unit;
  std::array<std::byte, header_bytes> header;
  if (!try_read(io, end, offset, header)) return {};
  const auto records = get(header, 12, 4);
  if (records > slots_per_group) return {};
  Bytes bytes(header_bytes + static_cast<std::size_t>(records) * run_bytes);
  std::copy(header.begin(), header.end(), bytes.begin());
  if ((records && !try_read(io, end, offset + header_bytes, std::span(bytes).subspan(header_bytes))) ||
      !verified(bytes)) return {};
  Directory result{get(bytes, 24), {}};
  if (std::memcmp(bytes.data(), directory_magic, 8) || get(bytes, 8, 4) != 1 ||
      get(bytes, 16) != group || identity_at(bytes) != geometry.volume || !result.sequence)
    fail(Code::corrupt, "intact dense directory has invalid framing or identity");
  const auto payload = payload_start(group);
  const auto physical_end = (std::min)(end / unit, payload + slots_per_group);
  const auto logical_end = geometry.logical_limit / unit;
  auto previous_end = payload;
  for (std::size_t at = header_bytes; at < bytes.size(); at += run_bytes) {
    const auto logical = get(bytes, at);
    const auto first = payload + get(bytes, at + 8, 4);
    const auto units = get(bytes, at + 12, 4);
    if (!units || first < previous_end || first >= physical_end || units > physical_end - first ||
        logical >= logical_end || units > logical_end - logical)
      fail(Code::corrupt, "dense directory references an unowned extent");
    const auto previous = result.physical.empty() ? result.physical.end() : std::prev(result.physical.end());
    if (previous != result.physical.end() && first == previous_end &&
        logical == previous->second.target + previous->second.units)
      fail(Code::corrupt, "dense directory runs are not canonical");
    result.physical.emplace_hint(result.physical.end(), first, Run{logical, units});
    previous_end = first + units;
  }
  return result;
}

struct Group {
  Directory directory;
  std::uint64_t available = 0;
  std::optional<unsigned> repair_slot;
};

Digest directory_digest(const Geometry& geometry, std::uint64_t index, const Directory& directory) {
  return digest_at_offset(directory_record(geometry, index, directory.sequence, directory.physical), digest_at);
}

bool owns(const Runs& runs, std::uint64_t physical, const TailMove& move) noexcept {
  const auto next = runs.upper_bound(physical);
  if (next == runs.begin()) return false;
  const auto run = std::prev(next);
  return physical < end_of(run) && move.units <= end_of(run) - physical &&
      run->second.target + physical - run->first == move.logical;
}

struct DirectoryRepair {
  std::uint64_t index;
  unsigned first_slot;
  bool both;
};
}

struct DenseVolumeBacking::Impl {
  const std::shared_ptr<StoreByteIo> physical;
  DenseFileTailIo* const file;
  const Geometry geometry;
  const bool writable;
  std::uint64_t physical_end, admitted_end = 0, used_slots = 0;
  bool fenced = false, needs_open_barrier = false, pending_release = false;
  std::optional<unsigned> anchor_repair;
  Anchor anchor;
  std::array<std::optional<DirectoryRepair>, 2> move_repairs;
  std::optional<TailMove> read_fallback; // Guarded by the inverse-index lock.
  Runs logical;
  std::vector<Group> groups;
  std::size_t occupied_groups = 0; // One past the last group owning payload units.
  std::set<std::uint64_t> available_groups, dirty_groups;
  mutable std::shared_mutex mutex;

  Impl(std::shared_ptr<StoreByteIo> io, Geometry layout, bool write, std::uint64_t end)
      : physical(std::move(io)), file(dynamic_cast<DenseFileTailIo*>(physical.get())), geometry(layout),
        writable(write), physical_end(end) {}

  std::uint64_t logical_end() const noexcept {
    return logical.empty() ? 0 : end_of(std::prev(logical.end())) * unit;
  }

  std::uint64_t retained_end() const noexcept {
    if (!occupied_groups) return unit + (anchor.sequence ? anchor_v2_bytes : header_bytes);
    return end_of(std::prev(groups[occupied_groups - 1].directory.physical.end())) * unit;
  }

  bool tail_settled() const noexcept {
    return writable && !fenced && file && physical->contract().isolated_tail_truncation && dirty_groups.empty();
  }

  std::optional<std::uint64_t> shorten_to(std::uint64_t budget) const noexcept {
    const auto retained = retained_end();
    if (!budget || physical_end <= retained) return {};
    const auto minimum = physical_end - (std::min)(budget, physical_end - retained);
    auto end = minimum == retained ? retained : (minimum + unit - 1) / unit * unit;
    if (end >= anchor_units * unit) {
      const auto group = (end / unit - anchor_units) / group_units;
      // Keep either the entire directory pair or none of it, so a later
      // allocation can reuse the surviving group's empty directory.
      if (end > group_start(group) * unit && end < payload_start(group) * unit)
        end = payload_start(group) * unit;
    }
    return end < physical_end ? std::optional{end} : std::nullopt;
  }

  std::optional<TailMove> tail_move(std::uint64_t budget) const noexcept {
    if (budget < unit || !occupied_groups || available_groups.empty() || physical_end != retained_end()) return {};
    const auto source_index = occupied_groups - 1;
    const auto destination_index = *available_groups.begin();
    if (destination_index > source_index) return {};
    const auto& source = groups[source_index].directory;
    const auto& destination = groups[static_cast<std::size_t>(destination_index)];
    const auto last = std::prev(source.physical.end());
    if (destination.available >= last->first || source.sequence == maximum_sequence ||
        destination.directory.sequence == maximum_sequence ||
        (destination_index != source_index && (!destination.directory.sequence || anchor.sequence > maximum_sequence - 2)))
      return {};
    const auto next = destination.directory.physical.lower_bound(destination.available);
    const auto gap_end = next == destination.directory.physical.end() ? group_end(destination_index) : next->first;
    const auto count = (std::min)({budget / unit, maximum_move_units,
        gap_end - destination.available, last->second.units});
    const auto start = last->second.units - count;
    return TailMove{last->first + start, destination.available, last->second.target + start, count};
  }

  void check_writer() const {
    if (!writable || fenced)
      fail(Code::invalid_operation, "dense backing requires an unfenced writer");
  }

  void admit(std::uint64_t end) {
    if (end > geometry.physical_limit) fail(Code::limit, "dense backing physical capacity");
    physical->admit_write((std::max)(end, physical_end));
    admitted_end = (std::max)(admitted_end, end);
  }

  void write(std::uint64_t at, std::span<const std::byte> bytes) {
    const auto end = at + bytes.size();
    if (end > admitted_end) admit(end);
    physical->write_exact(at, bytes);
    physical_end = (std::max)(physical_end, end);
  }

  std::uint64_t group_end(std::uint64_t group) const noexcept {
    return (std::min)(payload_start(group) + slots_per_group, geometry.physical_limit / unit);
  }

  const DirectoryChange* intended_change(std::uint64_t index) const noexcept {
    if (!anchor.move) return nullptr;
    if (index == group_at(anchor.move->range.source)) return &anchor.move->source;
    if (index == group_at(anchor.move->range.destination)) return &anchor.move->destination;
    return nullptr;
  }

  void check_move_peer(std::uint64_t index, const std::optional<Directory>& peer,
      const DirectoryChange& change) const {
    if (!peer) return;
    const auto sequence = peer->sequence;
    if ((sequence != change.sequence && sequence != change.sequence + 1) ||
        directory_digest(geometry, index, *peer) != (sequence == change.sequence ? change.before : change.after))
      fail(Code::corrupt, "dense directory does not match its relocation intent");
  }

  void recover_directory(std::uint64_t index, Group& group, const DirectoryChange& change) {
    const auto& range = anchor.move->range;
    const bool source = index == group_at(range.source);
    auto& directory = group.directory;
    if (!directory.sequence) fail(Code::corrupt, "dense relocation lost both directory peers");
    const bool before = directory.sequence == change.sequence;
    if (before) {
      if (source) {
        if (!owns(directory.physical, range.source, range)) fail(Code::corrupt, "dense move source no longer owned");
        erase(directory.physical, range.source, range.source + range.units);
      } else {
        insert(directory.physical, range.destination, {range.logical, range.units});
      }
      ++directory.sequence;
      if (directory_digest(geometry, index, directory) != change.after)
        fail(Code::corrupt, "dense move did not reconstruct its recorded directory");
    }
    if (source) {
      const auto next = directory.physical.lower_bound(range.source);
      if ((next != directory.physical.begin() && end_of(std::prev(next)) > range.source) ||
          (next != directory.physical.end() && next->first < range.source + range.units))
        fail(Code::corrupt, "dense move left its source allocated");
    } else if (!owns(directory.physical, range.destination, range)) {
      fail(Code::corrupt, "dense move destination is absent from the intended directory");
    }
    if (before || group.repair_slot) {
      const auto first = group.repair_slot.value_or(static_cast<unsigned>(directory.sequence % 2));
      move_repairs[source ? 1 : 0] = DirectoryRepair{index, first, before};
      group.repair_slot.reset();
    }
  }

  bool verified_move_bytes(std::uint64_t physical_at, std::span<std::byte> bytes, const Digest& digest) const {
    return try_read(*physical, physical_end, physical_at * unit, bytes) && record_digest(bytes) == digest;
  }

  void load_groups() {
    for (std::uint64_t index = 0; group_start(index) * unit < physical_end; ++index) {
      auto a = read_directory(*physical, geometry, physical_end, index, 0);
      auto b = read_directory(*physical, geometry, physical_end, index, 1);
      const auto* change = intended_change(index);
      if (change) {
        check_move_peer(index, a, *change);
        check_move_peer(index, b, *change);
      }
      Group group;
      group.available = payload_start(index);
      if (!a && !b) {
        // No payload byte can be written before the initial directory is
        // durable. An interrupted empty tail needs no ownership reconstruction.
        if (physical_end > group.available * unit)
          fail(Code::corrupt, "no intact dense directory for an existing data group");
      } else {
        if (a && b && a->sequence == b->sequence) {
          if (a->physical != b->physical) fail(Code::corrupt, "same-generation dense directories disagree");
          group.directory = std::move(*a);
        } else {
          const bool first = a && (!b || a->sequence > b->sequence);
          if (a && b && (first ? a->sequence - b->sequence : b->sequence - a->sequence) != 1)
            fail(Code::corrupt, "dense directory peers have unrelated generations");
          group.repair_slot = first ? 1 : 0;
          group.directory = std::move(first ? *a : *b);
        }
      }
      if (change) recover_directory(index, group, *change);
      for (const auto& [physical_at, run] : group.directory.physical) {
        insert(logical, run.target, {physical_at, run.units});
        used_slots += run.units;
      }
      group.available = first_free(group.directory.physical, group.available);
      if (group.available < group_end(index)) available_groups.insert(index);
      if (!group.directory.physical.empty()) occupied_groups = groups.size() + 1;
      groups.push_back(std::move(group));
    }
    if (anchor.move) {
      const auto& range = anchor.move->range;
      const auto& digest = anchor.move->payload;
      Bytes bytes(static_cast<std::size_t>(range.units * unit));
      if (!verified_move_bytes(range.destination, bytes, digest)) {
        if (!verified_move_bytes(range.source, bytes, digest))
          fail(Code::corrupt, "dense relocation has no verified copy");
        read_fallback = range;
      }
    }
  }

  void write_pair(std::uint64_t base, unsigned first, std::span<const std::byte> record) {
    write(base + first * unit, record);
    physical->flush();
    write(base + (1 - first) * unit, record);
    physical->flush();
  }

  void publish_anchor(const Anchor& next) {
    write_pair(0, static_cast<unsigned>(next.sequence % 2), anchor_record(geometry, next));
    anchor = next;
  }

  void recover_move() {
    if (read_fallback) {
      const auto& range = *read_fallback;
      Bytes bytes(static_cast<std::size_t>(range.units * unit));
      if (!verified_move_bytes(range.source, bytes, anchor.move->payload))
        fail(Code::corrupt, "dense relocation source changed during recovery");
      write(range.destination * unit, bytes);
      physical->flush();
      if (!verified_move_bytes(range.destination, bytes, anchor.move->payload))
        fail(Code::storage_failure, "dense relocation recovery could not verify its destination");
    }
    for (const auto& repair : move_repairs) {
      if (!repair) continue;
      const auto& [index, first, both] = *repair;
      const auto& directory = groups[static_cast<std::size_t>(index)].directory;
      if (both) {
        write_pair(group_start(index) * unit, first,
            directory_record(geometry, index, directory.sequence, directory.physical));
      } else {
        write_directory(index, first, directory.sequence);
        physical->flush();
      }
    }
    publish_anchor({anchor.sequence + 1, {}});
    move_repairs = {};
    // Before reusing the old source, drain readers that used the verified fallback.
    const std::unique_lock lock(mutex);
    read_fallback.reset();
  }

  void begin_writes() {
    // The ordinary file may still contain a previous owner's cached writes.
    // Make recovered ownership stable before writing any data into its holes.
    if (!needs_open_barrier) return;
    if (anchor_repair) write(*anchor_repair * unit, anchor_record(geometry, anchor));
    for (std::size_t index = 0; index < groups.size(); ++index) {
      const auto& group = groups[index];
      if (group.repair_slot) write_directory(index, *group.repair_slot, group.directory.sequence);
    }
    physical->flush();
    anchor_repair.reset();
    for (auto& group : groups) group.repair_slot.reset();
    if (anchor.move) recover_move();
    needs_open_barrier = false;
  }

  void write_directory(std::uint64_t index, unsigned slot, std::uint64_t sequence) {
    const auto& group = groups[static_cast<std::size_t>(index)];
    write((group_start(index) + slot) * unit,
        directory_record(geometry, index, sequence, group.directory.physical));
  }

  void prepare_group(std::uint64_t index, Group& group) {
    if (!group.directory.sequence) {
      write_directory(index, 0, 1);
      write_directory(index, 1, 1);
      physical->flush();
      group.directory.sequence = 1;
    }
    if (group.directory.sequence == maximum_sequence)
      fail(Code::limit, "dense directory generation exhausted");
  }

  void preflight(std::uint64_t projected, std::uint64_t slots) {
    if (projected < logical_end() || projected > geometry.logical_limit)
      fail(Code::limit, "dense backing logical capacity");
    if (slots > count_slots(geometry.physical_limit) - used_slots)
      fail(Code::limit, "dense backing physical capacity");
    admit(slot_end(used_slots + slots));
  }

  std::uint64_t missing_slots(std::uint64_t begin, std::uint64_t end) const {
    if (begin == end) return 0;
    const auto first = begin / unit, last = (end - 1) / unit + 1;
    auto slots = last - first;
    auto current = logical.upper_bound(first);
    if (current != logical.begin() && end_of(std::prev(current)) > first) --current;
    for (; current != logical.end() && current->first < last; ++current)
      slots -= (std::min)(last, end_of(current)) - (std::max)(first, current->first);
    return slots;
  }

  Run claim(std::uint64_t logical_at, std::uint64_t wanted) {
    // Some clients flush immediately after reclamation. If they instead
    // allocate first, settle the retired ownership before reusing its bytes.
    if (pending_release) publish();
    if (available_groups.empty()) {
      const auto index = static_cast<std::uint64_t>(groups.size());
      const auto start = payload_start(index);
      if (start >= group_end(index)) fail(Code::limit, "dense backing has no free physical range");
      if ((start + 1) * unit > admitted_end) admit((start + 1) * unit);
      groups.push_back({{}, start, {}});
      available_groups.insert(index);
    }
    const auto index = *available_groups.begin();
    auto& group = groups[static_cast<std::size_t>(index)];
    const auto at = group.available;
    const auto next = group.directory.physical.lower_bound(at);
    const auto end = next == group.directory.physical.end() ? group_end(index) : next->first;
    const auto taken = (std::min)(wanted, end - at);
    if ((at + taken) * unit > admitted_end) admit((at + taken) * unit);
    prepare_group(index, group);
    dirty_groups.insert(index);
    insert(group.directory.physical, at, {logical_at, taken});
    insert(logical, logical_at, {at, taken});
    occupied_groups = (std::max)(occupied_groups, static_cast<std::size_t>(index) + 1);
    used_slots += taken;
    group.available = first_free(group.directory.physical, at);
    if (group.available == group_end(index)) available_groups.erase(index);
    return {at, taken};
  }

  void remove(std::uint64_t first, std::uint64_t end) {
    while (first < end) {
      const auto next = logical.upper_bound(first);
      auto current = next;
      if (next != logical.begin() && end_of(std::prev(next)) > first) current = std::prev(next);
      if (current == logical.end() || current->first >= end) break;
      first = (std::max)(first, current->first);
      const auto until = (std::min)(end, end_of(current));
      const auto physical_at = current->second.target + first - current->first;
      const auto index = (physical_at - anchor_units) / group_units;
      auto& group = groups[static_cast<std::size_t>(index)];
      prepare_group(index, group);
      dirty_groups.insert(index);
      available_groups.insert(index);
      pending_release = true;
      erase(logical, first, until);
      erase(group.directory.physical, physical_at, physical_at + until - first);
      group.available = (std::min)(group.available, physical_at);
      used_slots -= until - first;
      first = until;
    }
    while (occupied_groups && groups[occupied_groups - 1].directory.physical.empty()) --occupied_groups;
  }

  void publish() {
    physical->flush();
    if (dirty_groups.empty()) return;
    // Data reaches stable storage before either directory records its owners.
    // Separate barriers keep one usable peer during a directory update.
    for (const auto index : dirty_groups) {
      const auto sequence = groups[static_cast<std::size_t>(index)].directory.sequence + 1;
      write_directory(index, static_cast<unsigned>(sequence % 2), sequence);
    }
    physical->flush();
    for (const auto index : dirty_groups) {
      const auto sequence = groups[static_cast<std::size_t>(index)].directory.sequence + 1;
      write_directory(index, 1 - static_cast<unsigned>(sequence % 2), sequence);
    }
    physical->flush();
    for (const auto index : dirty_groups) ++groups[static_cast<std::size_t>(index)].directory.sequence;
    dirty_groups.clear();
    pending_release = false;
  }

  void shorten(std::uint64_t end) {
    file->shorten_file(physical_end, end);
    physical->flush();
    if (physical->extent() != end) fail(Code::storage_failure, "dense shorter extent was not confirmed");
    physical_end = end;
    admitted_end = (std::min)(admitted_end, end);
    while (!groups.empty() && group_start(groups.size() - 1) * unit >= end) {
      available_groups.erase(groups.size() - 1);
      groups.pop_back();
    }
  }

  void relocate(const TailMove& move) {
    const auto source_index = group_at(move.source), destination_index = group_at(move.destination);
    const bool cross_group = source_index != destination_index;
    auto& from = groups[static_cast<std::size_t>(source_index)];
    auto& to = groups[static_cast<std::size_t>(destination_index)];
    Directory source_after{from.directory.sequence + 1, from.directory.physical};
    std::optional<Directory> destination_after;
    if (cross_group) destination_after.emplace(Directory{to.directory.sequence + 1, to.directory.physical});
    auto& target = destination_after ? *destination_after : source_after;
    erase(source_after.physical, move.source, move.source + move.units);
    insert(target.physical, move.destination, {move.logical, move.units});
    const auto source_record = directory_record(geometry, source_index, source_after.sequence, source_after.physical);
    const auto destination_record = destination_after ?
        directory_record(geometry, destination_index, target.sequence, target.physical) : Bytes{};

    // Only the control thread changes the inverse index. Prepare a split
    // node privately so readers use the original map until both peers settle.
    const auto next = logical.upper_bound(move.logical);
    if (next == logical.begin()) fail(Code::corrupt, "dense relocation has no source owner");
    const auto original = std::prev(next);
    if (original->second.target + move.logical - original->first != move.source ||
        end_of(original) != move.logical + move.units)
      fail(Code::corrupt, "dense relocation disagrees with inverse ownership");
    Runs split;
    if (move.logical != original->first) split.emplace(move.logical, Run{move.destination, move.units});
    // Preallocate the source's hole-index entry before publishing any change.
    available_groups.insert(source_index);

    Bytes source(static_cast<std::size_t>(move.units * unit)), checked(source.size());
    physical->read_exact(move.source * unit, source);
    write(move.destination * unit, source);
    physical->flush();
    physical->read_exact(move.destination * unit, checked);
    if (source != checked) fail(Code::storage_failure, "dense relocated bytes differ from their source");

    if (cross_group) {
      const MoveIntent intent{move,
          {from.directory.sequence, directory_digest(geometry, source_index, from.directory),
              digest_at_offset(source_record, digest_at)},
          {to.directory.sequence, directory_digest(geometry, destination_index, to.directory),
              digest_at_offset(destination_record, digest_at)},
          record_digest(source)};
      publish_anchor({anchor.sequence + 1, intent});
      write_pair(group_start(destination_index) * unit, static_cast<unsigned>(target.sequence % 2), destination_record);
    }
    write_pair(group_start(source_index) * unit, static_cast<unsigned>(source_after.sequence % 2), source_record);
    if (cross_group) publish_anchor({anchor.sequence + 1, {}});

    // Drain reads already using the previous physical location before later
    // maintenance can reuse or shorten it. Captured logical offsets survive.
    const std::unique_lock lock(mutex);
    auto current = original;
    if (split.empty()) {
      current->second.target = move.destination;
    } else {
      original->second.units = move.logical - original->first;
      current = logical.insert(next, split.extract(split.begin()));
    }
    if (current != logical.begin() && adjacent(std::prev(current), current)) {
      current = std::prev(current);
      merge_next(logical, current);
    }
    merge_next(logical, current);
    from.directory = std::move(source_after);
    from.available = first_free(from.directory.physical, (std::min)(from.available, move.source));
    if (cross_group) {
      to.directory = std::move(*destination_after);
      to.available = first_free(to.directory.physical, to.available);
      if (to.available == group_end(destination_index)) available_groups.erase(destination_index);
      while (occupied_groups && groups[occupied_groups - 1].directory.physical.empty()) --occupied_groups;
    }
  }
};

DenseVolumeBacking::DenseVolumeBacking(std::unique_ptr<Impl> state) : state_(std::move(state)) {}
DenseVolumeBacking::~DenseVolumeBacking() = default;

bool DenseVolumeBacking::recognizes(const StoreByteIo& io) {
  const auto end = io.extent();
  std::array<std::byte, 8> bytes;
  for (unsigned i = 0; i < 2; ++i) {
    if (try_read(io, end, i * unit, bytes) && !std::memcmp(bytes.data(), anchor_magic, bytes.size()))
      return true;
  }
  return false;
}

std::shared_ptr<DenseVolumeBacking> DenseVolumeBacking::create(std::shared_ptr<StoreByteIo> io,
    StoreIdentity volume, std::uint64_t logical_limit, std::uint64_t physical_limit) {
  const Geometry geometry{volume, logical_limit, physical_limit};
  geometry.check(Code::invalid_operation);
  if (!io || io->extent() || !valid_store_io_contract(io->contract()) || io->contract().isolated_range_reclamation)
    fail(Code::invalid_operation, "dense backing requires a new qualified ordinary stream");
  auto state = std::make_unique<Impl>(std::move(io), geometry, true, 0);
  const auto anchor = anchor_record(geometry);
  state->admit(unit + anchor.size());
  state->write(0, anchor);
  state->write(unit, anchor);
  state->physical->flush();
  return std::shared_ptr<DenseVolumeBacking>(new DenseVolumeBacking(std::move(state)));
}

std::shared_ptr<DenseVolumeBacking> DenseVolumeBacking::open(std::shared_ptr<StoreByteIo> io, bool writable) {
  if (writable && (!valid_store_io_contract(io->contract()) || io->contract().isolated_range_reclamation))
    fail(Code::invalid_operation, "dense reopen requires a qualified ordinary stream");
  const auto end = io->extent();
  const auto a = read_anchor(*io, end, 0), b = read_anchor(*io, end, 1);
  if (!a && !b) fail(Code::corrupt, "no intact dense anchor");
  const bool first = a && (!b || a->anchor.sequence >= b->anchor.sequence);
  const auto& selected = first ? *a : *b;
  const auto& other = first ? b : a;
  if (other && (selected.geometry != other->geometry || selected.anchor.sequence - other->anchor.sequence > 1 ||
      (selected.anchor.sequence == other->anchor.sequence && selected.anchor != other->anchor)))
    fail(Code::corrupt, "dense backing anchors disagree");
  auto state = std::make_unique<Impl>(std::move(io), selected.geometry, writable, end);
  state->anchor = selected.anchor;
  if (!other || selected.anchor != other->anchor) state->anchor_repair = first ? 1 : 0;
  state->load_groups();
  state->needs_open_barrier = writable;
  return std::shared_ptr<DenseVolumeBacking>(new DenseVolumeBacking(std::move(state)));
}

StoreIdentity DenseVolumeBacking::volume_identity() const noexcept { return state_->geometry.volume; }
std::uint64_t DenseVolumeBacking::logical_limit() const noexcept { return state_->geometry.logical_limit; }
StoreIoContract DenseVolumeBacking::contract() const noexcept {
  auto contract = state_->physical->contract();
  contract.isolated_range_reclamation = state_->writable;
  contract.isolated_tail_truncation = false; // Only the outer file can be shortened.
  return contract;
}
bool DenseVolumeBacking::allows_writer() const noexcept { return state_->writable; }
bool DenseVolumeBacking::supports_reclaimable_ranges() const noexcept { return state_->writable; }
bool DenseVolumeBacking::concurrent_reads_supported() const noexcept {
  return state_->physical->concurrent_reads_supported();
}

std::uint64_t DenseVolumeBacking::extent() const {
  const std::shared_lock lock(state_->mutex);
  return state_->logical_end();
}

void DenseVolumeBacking::read_exact(std::uint64_t at, std::span<std::byte> output) const {
  const std::shared_lock lock(state_->mutex);
  const auto end = state_->logical_end();
  if (at > end || output.size() > end - at) fail(Code::corrupt, "dense read exceeds logical extent");
  while (!output.empty()) {
    const auto next = state_->logical.upper_bound(at / unit);
    const auto run = next == state_->logical.begin() ? state_->logical.end() : std::prev(next);
    const bool mapped = run != state_->logical.end() && at < end_of(run) * unit;
    auto boundary = mapped ? end_of(run) * unit : next->first * unit;
    auto position = mapped ? run->second.target * unit + at - run->first * unit : 0;
    if (state_->read_fallback) {
      const auto& range = *state_->read_fallback;
      const auto begin = range.logical * unit, limit = (range.logical + range.units) * unit;
      if (at < begin) {
        boundary = (std::min)(boundary, begin);
      } else if (at < limit) {
        boundary = (std::min)(boundary, limit);
        position = range.source * unit + at - begin;
      }
    }
    const auto count = static_cast<std::size_t>((std::min)(std::uint64_t(output.size()), boundary - at));
    if (mapped)
      state_->physical->read_exact(position, output.first(count));
    else
      std::fill_n(output.begin(), count, std::byte{});
    output = output.subspan(count);
    at += count;
  }
}

void DenseVolumeBacking::write_exact(std::uint64_t at, std::span<const std::byte> input) {
  state_->check_writer();
  const auto limit = state_->geometry.logical_limit;
  if (at > limit || input.size() > limit - at) fail(Code::limit, "dense write exceeds logical capacity");
  if (input.empty()) return;
  try {
    state_->begin_writes();
    Bytes initialized;
    while (!input.empty()) {
      std::unique_lock lock(state_->mutex);
      const auto next = state_->logical.upper_bound(at / unit);
      const auto run = next == state_->logical.begin() ? state_->logical.end() : std::prev(next);
      const bool mapped = run != state_->logical.end() && at < end_of(run) * unit;
      std::size_t count;
      if (mapped) {
        const auto physical = run->second.target * unit + at - run->first * unit;
        count = static_cast<std::size_t>((std::min)(std::uint64_t(input.size()), end_of(run) * unit - at));
        lock.unlock();
        state_->write(physical, input.first(count));
      } else if (at % unit == 0 && input.size() >= unit) {
        const auto remaining = next == state_->logical.end() ? limit / unit - at / unit : next->first - at / unit;
        const auto claimed = state_->claim(at / unit, (std::min)(std::uint64_t(input.size()) / unit, remaining));
        count = static_cast<std::size_t>(claimed.units * unit);
        state_->write(claimed.target * unit, input.first(count));
      } else {
        const auto claimed = state_->claim(at / unit, 1);
        count = static_cast<std::size_t>((std::min)(std::uint64_t(input.size()), unit - at % unit));
        // Clear all unwritten bytes before an old physical range is assigned
        // a new logical owner, including after an interrupted prior write.
        initialized.assign(unit, std::byte{});
        std::copy_n(input.begin(), count, initialized.begin() + static_cast<std::ptrdiff_t>(at % unit));
        state_->write(claimed.target * unit, initialized);
      }
      input = input.subspan(count);
      at += count;
    }
  } catch (...) {
    state_->fenced = true;
    throw;
  }
}

void DenseVolumeBacking::flush() {
  state_->check_writer();
  try {
    state_->begin_writes();
    state_->publish();
  } catch (...) {
    state_->fenced = true;
    throw;
  }
}

void DenseVolumeBacking::admit_write(std::uint64_t projected) {
  const std::unique_lock lock(state_->mutex);
  state_->check_writer();
  const auto end = state_->logical_end();
  if (projected < end) fail(Code::limit, "dense admission would reduce logical extent");
  state_->preflight(projected, state_->missing_slots(end, projected));
}

void DenseVolumeBacking::admit_range_write(std::uint64_t projected, std::uint64_t additional) {
  const std::unique_lock lock(state_->mutex);
  state_->check_writer();
  if (additional > projected) fail(Code::limit, "invalid dense range admission");
  state_->preflight(projected, additional / unit + (additional % unit != 0));
}

void DenseVolumeBacking::admit_contiguous_growth(std::uint64_t projected, std::uint64_t begin,
    std::uint64_t end) {
  const std::unique_lock lock(state_->mutex);
  state_->check_writer();
  if (begin > end || end > projected) fail(Code::limit, "invalid dense growth range");
  state_->preflight(projected, state_->missing_slots(begin, end));
}

std::uint64_t DenseVolumeBacking::allocated_extent(std::uint64_t offset, std::uint64_t bytes) const {
  const std::shared_lock lock(state_->mutex);
  const auto limit = state_->geometry.logical_limit;
  if (offset > limit || bytes > limit - offset || offset % unit || bytes % unit)
    fail(Code::limit, "invalid dense allocation query");
  const auto next = state_->logical.lower_bound((offset + bytes) / unit);
  if (next == state_->logical.begin()) return 0;
  const auto end = (std::min)(offset + bytes, end_of(std::prev(next)) * unit);
  return end > offset ? end - offset : 0;
}

void DenseVolumeBacking::reclaim_range(std::uint64_t offset, std::uint64_t bytes) {
  state_->check_writer();
  const auto limit = state_->geometry.logical_limit;
  if (offset > limit || bytes > limit - offset || offset % unit || bytes % unit)
    fail(Code::limit, "invalid dense range retirement");
  if (!bytes) return;
  try {
    state_->begin_writes();
    const std::unique_lock lock(state_->mutex);
    state_->remove(offset / unit, (offset + bytes) / unit);
  } catch (...) {
    state_->fenced = true;
    throw;
  }
}

NativeDenseFileSpace DenseVolumeBacking::file_space() const {
  if (state_->fenced || state_->anchor.move || !state_->dirty_groups.empty())
    fail(Code::invalid_operation, "dense allocation accounting requires settled ownership");
  const auto retained = state_->retained_end();
  return {state_->physical_end, state_->geometry.physical_limit, state_->used_slots * unit,
      (count_slots(retained) - state_->used_slots) * unit,
      state_->physical_end > retained ? state_->physical_end - retained : 0,
      state_->file ? state_->file->allocated_file_bytes(state_->physical_end) : std::nullopt};
}

bool DenseVolumeBacking::tail_work_pending(std::uint64_t budget) const {
  return state_->tail_settled() &&
      (state_->anchor.move || state_->shorten_to(budget).has_value() || state_->tail_move(budget).has_value());
}

bool DenseVolumeBacking::advance_tail(std::uint64_t budget) {
  if (!state_->tail_settled()) return false;
  const bool recovering = state_->anchor.move.has_value();
  const auto end = recovering ? std::optional<std::uint64_t>{} : state_->shorten_to(budget);
  const auto move = recovering || end ? std::optional<TailMove>{} : state_->tail_move(budget);
  if (!recovering && !end && !move) return false;
  try {
    state_->begin_writes();
    if (end) state_->shorten(*end);
    else if (move) state_->relocate(*move);
    return true;
  } catch (...) {
    state_->fenced = true;
    throw;
  }
}

} // namespace flitzfs::detail
