#include "dense_volume_backing.hpp"
#include "record_digest.hpp"

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
constexpr std::uint64_t slots_per_group = (unit - header_bytes) / run_bytes;
constexpr auto group_units = directory_units + slots_per_group;
constexpr auto maximum_file_bytes = static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
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
  if (next != runs.end() && end_of(current) == next->first &&
      current->second.target + current->second.units == next->second.target) {
    current->second.units += next->second.units;
    runs.erase(next);
  }
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

std::array<std::byte, header_bytes> anchor_record(const Geometry& geometry) {
  std::array<std::byte, header_bytes> bytes{};
  std::memcpy(bytes.data(), anchor_magic, 8);
  put(bytes, 8, 1, 4);
  put(bytes, 16, geometry.logical_limit);
  put(bytes, 24, geometry.physical_limit);
  std::copy(geometry.volume.begin(), geometry.volume.end(), bytes.begin() + 32);
  finish_record(bytes);
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

std::optional<Geometry> read_anchor(const StoreByteIo& io, std::uint64_t end, unsigned slot) {
  std::array<std::byte, header_bytes> bytes;
  if (!try_read(io, end, slot * unit, bytes) || !verified(bytes)) return {};
  if (std::memcmp(bytes.data(), anchor_magic, 8) || get(bytes, 8, 4) != 1 || get(bytes, 12, 4))
    fail(Code::corrupt, "intact dense backing version or header is unsupported");
  const Geometry geometry{identity_at(bytes), get(bytes, 16), get(bytes, 24)};
  geometry.check(Code::corrupt);
  if (end > geometry.physical_limit)
    fail(Code::corrupt, "dense backing exceeds its physical extent limit");
  return geometry;
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
}

struct DenseVolumeBacking::Impl {
  const std::shared_ptr<StoreByteIo> physical;
  const Geometry geometry;
  const bool writable;
  std::uint64_t physical_end, admitted_end = 0, used_slots = 0;
  bool fenced = false, needs_open_barrier = false, pending_release = false;
  std::optional<unsigned> anchor_repair;
  Runs logical;
  std::vector<Group> groups;
  std::set<std::uint64_t> available_groups, dirty_groups;
  mutable std::shared_mutex mutex;

  Impl(std::shared_ptr<StoreByteIo> io, Geometry layout, bool write, std::uint64_t end)
      : physical(std::move(io)), geometry(layout), writable(write), physical_end(end) {}

  std::uint64_t logical_end() const noexcept {
    return logical.empty() ? 0 : end_of(std::prev(logical.end())) * unit;
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

  void load_groups() {
    for (std::uint64_t index = 0; group_start(index) * unit < physical_end; ++index) {
      auto a = read_directory(*physical, geometry, physical_end, index, 0);
      auto b = read_directory(*physical, geometry, physical_end, index, 1);
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
        for (const auto& [physical_at, run] : group.directory.physical) {
          insert(logical, run.target, {physical_at, run.units});
          used_slots += run.units;
        }
      }
      group.available = first_free(group.directory.physical, group.available);
      if (group.available < group_end(index)) available_groups.insert(index);
      groups.push_back(std::move(group));
    }
  }

  void begin_writes() {
    // The ordinary file may still contain a previous owner's cached writes.
    // Make recovered ownership stable before writing any data into its holes.
    if (!needs_open_barrier) return;
    if (anchor_repair) write(*anchor_repair * unit, anchor_record(geometry));
    for (std::size_t index = 0; index < groups.size(); ++index) {
      const auto& group = groups[index];
      if (group.repair_slot) write_directory(index, *group.repair_slot, group.directory.sequence);
    }
    physical->flush();
    anchor_repair.reset();
    for (auto& group : groups) group.repair_slot.reset();
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
    if (group.directory.sequence == (std::numeric_limits<std::uint64_t>::max)())
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
  if (a && b && *a != *b) fail(Code::corrupt, "dense backing anchors disagree");
  auto state = std::make_unique<Impl>(std::move(io), a ? *a : *b, writable, end);
  if (!a || !b) state->anchor_repair = a ? 1 : 0;
  state->load_groups();
  state->needs_open_barrier = writable;
  return std::shared_ptr<DenseVolumeBacking>(new DenseVolumeBacking(std::move(state)));
}

StoreIdentity DenseVolumeBacking::volume_identity() const noexcept { return state_->geometry.volume; }
std::uint64_t DenseVolumeBacking::logical_limit() const noexcept { return state_->geometry.logical_limit; }
StoreIoContract DenseVolumeBacking::contract() const noexcept {
  auto contract = state_->physical->contract();
  contract.isolated_range_reclamation = state_->writable;
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
    const auto boundary = mapped ? end_of(run) * unit : next->first * unit;
    const auto count = static_cast<std::size_t>((std::min)(std::uint64_t(output.size()), boundary - at));
    if (mapped)
      state_->physical->read_exact(run->second.target * unit + at - run->first * unit, output.first(count));
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

} // namespace flitzfs::detail
