#pragma once

#include "native_fs_fixture.hpp"
#include "../src/store_file.hpp"

namespace native_fixture {

class DenseTailRam final : public StoreByteIo, public detail::DenseFileTailIo {
 public:
  Ram bytes;
  bool truncate_qualified=true;
  std::uint64_t extent() const override {
    const std::lock_guard lock(mutex_);
    return bytes.extent();
  }
  StoreIoContract contract() const noexcept override {return {bytes.failure_unit,true,false,truncate_qualified};}
  bool concurrent_reads_supported() const noexcept override {return true;}
  void admit_write(std::uint64_t end) override {
    const std::lock_guard lock(mutex_);
    bytes.admit_write(end);
  }
  void read_exact(std::uint64_t offset,std::span<std::byte> out) const override {
    const std::lock_guard lock(mutex_);
    bytes.read_exact(offset,out);
  }
  void write_exact(std::uint64_t offset,std::span<const std::byte> in) override {
    const std::lock_guard lock(mutex_);
    bytes.write_exact(offset,in);
  }
  void flush() override {
    const std::lock_guard lock(mutex_);
    bytes.flush();
  }
  std::optional<std::uint64_t> allocated_file_bytes(std::uint64_t expected) const override {
    return expected==extent()?std::optional{expected}:std::nullopt;
  }
  void shorten_file(std::uint64_t expected,std::uint64_t retained) override {
    const std::lock_guard lock(mutex_);
    if(!truncate_qualified || bytes.extent()!=expected || retained>=expected)
      throw std::runtime_error("unqualified or stale modeled extent");
    const auto fail=bytes.calls++==bytes.fail_at;
    if(fail && !bytes.failure)throw std::runtime_error("injected pre-truncation failure");
    const auto end=fail && bytes.failure==2?retained+(expected-retained)/2:retained;
    bytes.live.resize(static_cast<std::size_t>(end));
    if(fail) {
      bytes.stable.resize(static_cast<std::size_t>((std::min)(end,std::uint64_t(bytes.stable.size()))));
      throw std::runtime_error("injected interrupted truncation");
    }
  }
  static std::shared_ptr<DenseTailRam> replay(const DenseTailRam& from) {
    const std::lock_guard lock(from.mutex_);
    auto io=std::make_shared<DenseTailRam>();
    io->bytes.live=io->bytes.stable=from.bytes.stable;
    io->bytes.failure_unit=from.bytes.failure_unit;
    io->bytes.capacity=from.bytes.capacity;
    io->truncate_qualified=from.truncate_qualified;
    return io;
  }
 private:
  mutable std::mutex mutex_;
};

} // namespace native_fixture
