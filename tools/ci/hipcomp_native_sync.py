"""Closed transform of the pinned candidate header; data only, never a runtime."""
from __future__ import annotations
import hashlib
import stat
from pathlib import Path

SOURCE_SHA256 = "11db6cb48273de0d105c3131795ab958f0c99760a47e6d26093b9d897490a112"
SOURCE_BYTES = 34435
SYNCHRONIZATION = "#define SYNCWARP1()\n#define SYNCWARP(m)\n"
NATIVE_SYNCHRONIZATION = (
    "#if !defined(__HIP_PLATFORM_AMD__) || defined(HIP_DISABLE_WARP_SYNC_BUILTINS)\n"
    '#error "The explicit native-sync candidate requires the qualified HIP builtins"\n'
    "#endif\n"
    "#define SYNCWARP1() __syncwarp()\n"
    "#define SYNCWARP(m) __syncwarp(static_cast<unsigned long long>(m))\n"
)
EMPTY_CHECK = "  if (hashed_offset == NULL_OFFSET) {\n    return false;\n  }"
BOUNDED_CHECK = (
    "  if (hashed_offset == NULL_OFFSET ||\n"
    "      static_cast<position_type>(hashed_offset) > decomp_idx) {\n"
    "    return false;\n"
    "  }"
)
DISTANCE_CHECK = "  if (decomp_idx - offset > MAX_OFFSET) {"
BOUNDED_DISTANCE = "  if (offset >= decomp_idx || decomp_idx - offset > MAX_OFFSET) {"

def replace_once(source: str, old: str, new: str) -> str:
    if source.count(old) != 1:
        raise ValueError("candidate native-sync anchor must occur exactly once")
    return source.replace(old, new, 1)

def patch_header(data: bytes):
    if (not isinstance(data, bytes) or len(data) != SOURCE_BYTES or
            hashlib.sha256(data).hexdigest() != SOURCE_SHA256):
        raise ValueError("candidate header is not the exact reviewed canonical blob")
    text = data.decode("utf-8")
    text = replace_once(text, SYNCHRONIZATION, NATIVE_SYNCHRONIZATION)
    text = replace_once(text, EMPTY_CHECK, BOUNDED_CHECK)
    text = replace_once(text, DISTANCE_CHECK, BOUNDED_DISTANCE)
    result = text.encode("utf-8")
    if not 0 < len(result) - len(data) <= 4096:
        raise ValueError("candidate native-sync growth bound")
    return result, dict(transform="native-wave-sync", canonical_bytes=len(data),
                        canonical_sha256=SOURCE_SHA256, bytes=len(result),
                        sha256=hashlib.sha256(result).hexdigest(),
                        upstream_checkout_changed=False, gpu_executed=False)


def stage_header(root: Path, records: list, extra_bytes: int):
    """Only called inside a freshly created private candidate compiler fixture."""
    root = root.resolve(strict=True)
    header = root / "src/LZ4Kernels.cuh"
    backup = root / "native-sync-original-LZ4Kernels.cuh"
    if (header.resolve(strict=True) != header or
            not header.is_relative_to(root) or not backup.is_relative_to(root)):
        raise ValueError("native-sync private paths escaped the new fixture")
    info = header.lstat()
    if (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or
            getattr(info, "st_file_attributes", 0) & 0x400 or info.st_size != SOURCE_BYTES):
        raise ValueError("native-sync private header type/size")
    matches = [row for row in records if row["target"] == "src/LZ4Kernels.cuh"]
    if len(matches) != 1 or matches[0]["sha256"] != SOURCE_SHA256:
        raise ValueError("native-sync staged canonical record differs")
    data = header.read_bytes()
    patched, proof = patch_header(data)
    if (type(extra_bytes) is not int or extra_bytes < 0 or
            sum(row["bytes"] for row in records) + extra_bytes + len(patched) > 16 * 1024 * 1024):
        raise ValueError("native-sync complete private input budget")
    # Retain the canonical private copy before replacing its private include path.
    # The original checkout is never touched. Exclusive creation refuses collision.
    with backup.open("xb") as stream:
        stream.write(data)
    if backup.read_bytes() != data:
        raise ValueError("native-sync canonical retention failed")
    header.unlink()
    with header.open("xb") as stream:
        stream.write(patched)
    result = [dict(row) for row in records]
    for row in result:
        if row["target"] == "src/LZ4Kernels.cuh":
            row.update(bytes=len(patched), sha256=proof["sha256"],
                       canonical_sha256=SOURCE_SHA256, transform="native-wave-sync")
    result.append(dict(source="src/LZ4Kernels.cuh",
                       target="native-sync-original-LZ4Kernels.cuh",
                       bytes=len(data), sha256=SOURCE_SHA256, role="retained-canonical"))
    return result, proof

def oracle_source(canonical: bytes) -> bytes:
    """Extract two exact reviewed functions; compile as bounded CPU integer tests."""
    patched, _ = patch_header(canonical)
    old = canonical.decode("utf-8")
    new = patched.decode("utf-8")
    convert_start = "inline __device__ position_type convertIdx("
    validate_start = "template <typename T>\ninline __device__ bool\nisValidHash("
    write_start = "template <int BLOCK_SIZE>\ninline __device__ void\nwriteSequenceData("
    for marker, value in ((convert_start, old), (validate_start, old),
                          (validate_start, new), (write_start, new)):
        if value.count(marker) != 1:
            raise ValueError("closed CPU oracle source boundary")
    convert = old[old.index(convert_start):old.index(validate_start)].replace("__device__", "")
    validate = new[new.index(validate_start):new.index(write_start)].replace("__device__", "")
    license_prefix = old[:old.index("#include")].replace("#pragma once", "")
    prelude = r'''
#define NDEBUG
#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <initializer_list>
using position_type=std::uint32_t;
using offset_type=std::uint16_t;
using word_type=std::uint32_t;
constexpr position_type MAX_OFFSET=65535;
constexpr offset_type NULL_OFFSET=65535;
static_assert(std::endian::native==std::endian::little);
std::uint64_t reads=0;
template<class R,class T> R readWord(const T* source) {
  R value{};std::memcpy(&value,source,sizeof(value));++reads;return value;
}
'''
    main = r'''
int main() {
  std::array<std::uint8_t,65536> data{};
  for(std::size_t i=0;i<data.size();++i)data[i]=static_cast<std::uint8_t>(i);
  std::array<offset_type,1> table{};
  std::uint64_t old_future=0,valid=0,rejected=0;
  constexpr position_type last=65523;
  for(position_type pos=1;pos<=last;++pos) {
    const auto future=static_cast<offset_type>(pos+1);
    const auto old=convertIdx(future,pos);
    if(old>=pos&&pos-old<=MAX_OFFSET)++old_future;
    for(const auto bad:{future,static_cast<offset_type>(pos),NULL_OFFSET}) {
      table[0]=bad;position_type offset=0;const auto before=reads;
      if(isValidHash<std::uint8_t>(data.data(),table.data(),0,0,pos,offset)||reads!=before)return 1;
      ++rejected;
    }
    for(const auto good:{offset_type{0},static_cast<offset_type>(pos-1)}) {
      table[0]=good;word_type key=0;std::memcpy(&key,data.data()+good,sizeof(key));
      position_type offset=0;const auto before=reads;
      if(!isValidHash<std::uint8_t>(data.data(),table.data(),key,0,pos,offset)||
          offset!=good||reads!=before+1)return 2;
      ++valid;
    }
  }
  if(old_future!=last||valid!=2ULL*last||rejected!=3ULL*last||reads!=valid)return 3;
  std::cout<<"native-sync index oracle: {\"old_future_admissions\":"<<old_future
      <<",\"valid_reads\":"<<valid<<",\"invalid_rejections\":"<<rejected
      <<",\"gpu_executed\":false}\n";
}
'''
    result = (license_prefix + prelude + convert + validate + main).encode("utf-8")
    if len(result) > 64 * 1024:
        raise ValueError("CPU oracle source bound")
    return result


def qualify_index_oracle(canonical, output, bounds, report):
    import json
    import shutil
    from qualify_rocm_sdk import run, digest
    source = oracle_source(canonical)
    root = output / "cpu-index-oracle"
    root.mkdir()
    with (root / "oracle.cpp").open("xb") as stream:
        stream.write(source)
    cmake_source = (
        "cmake_minimum_required(VERSION 3.22)\n"
        "project(NativeSyncIndexOracle LANGUAGES CXX)\n"
        "if(NOT WIN32 OR NOT MSVC)\nmessage(FATAL_ERROR \"Hosted MSVC CPU oracle only\")\nendif()\n"
        "set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)\n"
        "add_executable(native_sync_index_oracle oracle.cpp)\n"
        "target_compile_features(native_sync_index_oracle PRIVATE cxx_std_20)\n"
    )
    with (root / "CMakeLists.txt").open("x", encoding="utf-8", newline="\n") as stream:
        stream.write(cmake_source)
    cmake = shutil.which("cmake")
    if not cmake:
        raise ValueError("qualified job-local CMake required")
    build = root / "build"
    run(bounds, [cmake, "-S", str(root), "-B", str(build), "-A", "x64"],
        report, "native-sync-CPU-index-configure", 120)
    run(bounds, [cmake, "--build", str(build), "--config", "Release", "--parallel", "2"],
        report, "native-sync-CPU-index-build", 180)
    executable = build / "Release/native_sync_index_oracle.exe"
    from qualify_warp_sync import regular_info
    regular_info(executable, root, 2 * 1024 * 1024)
    from check_portable_cpu import audit_binary
    imports = audit_binary(executable.read_bytes())
    result = run(bounds, [str(executable)], report, "native-sync-CPU-index-oracle", 30)
    prefix = "native-sync index oracle: "
    lines = [line[len(prefix):] for line in result["stdout"].splitlines() if line.startswith(prefix)]
    if len(lines) != 1:
        raise ValueError("one complete CPU index oracle result required")
    observed = json.loads(lines[0])
    if observed != dict(old_future_admissions=65523, valid_reads=131046,
                        invalid_rejections=196569, gpu_executed=False):
        raise ValueError("actual CPU index oracle differs")
    return dict(source_bytes=len(source), source_sha256=hashlib.sha256(source).hexdigest(),
                executable_sha256=digest(executable), imports=imports, observed=observed,
                scope="Actual extracted integer/index functions; not GPU synchronization or codec execution")
