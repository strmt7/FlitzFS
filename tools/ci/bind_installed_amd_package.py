#!/usr/bin/env python3
"""Bind an explicitly approved AMD package to a relocatable native build."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import stat

from check_portable_cpu import pe_import_names, require
from hip_runtime_exports import pe_function_exports


UNIFORM_WORKER = "flitzfs_hip_lz4_worker.exe"
UNIFORM_MODULE = "hip-uniform-lz4/flitzfs_rocm_uniform_lz4_candidate.dll"
UNIFORM_TRIAL_EVIDENCE = "uniform-write-trial-qualification.json"
ANCHOR_WORKER = "flitzfs_hip_worker.exe"
ANCHOR_MODULE = "hip/flitzfs_rocm_kernels.dll"
ANCHOR_EVIDENCE = "shared-base-qualification.json"
READ_WORKER = "native-lz4-read/flitzfs_native_lz4_read_worker.exe"
READ_MODULE = "native-lz4-read/flitzfs_rocm_native_lz4_decode.dll"
READ_EVIDENCE = "native-lz4-read-qualification.json"
GPU_ARCHITECTURE = re.compile(r"gfx[a-z0-9]{1,29}\Z")
LICENSE_PATH = re.compile(r"(?:LICENSE|licenses(?:/[A-Za-z0-9][A-Za-z0-9._-]*)+)\Z")
SHA256 = re.compile(r"[a-f0-9]{64}\Z")
HIP_RUNTIME = re.compile(r"amdhip64(?:_[1-9][0-9]{0,2})?\.dll\Z")
WORKER_SYSTEM_IMPORTS = {
    "advapi32.dll", "bcrypt.dll", "crypt32.dll", "dxgi.dll", "kernel32.dll",
    "ntdll.dll", "psapi.dll", "wintrust.dll",
}


def bounded_file(path: Path, maximum: int) -> bytes:
    state = path.lstat()
    require(stat.S_ISREG(state.st_mode) and state.st_nlink == 1
            and not (getattr(state, "st_file_attributes", 0) & 0x400)
            and 0 < state.st_size <= maximum, f"Unqualified package file {path.name}")
    with path.open("rb") as incoming:
        data = incoming.read(maximum + 1)
    require(len(data) == state.st_size, f"Package file changed during read {path.name}")
    return data


def exact_digest(value: object) -> bool:
    return isinstance(value, str) and SHA256.fullmatch(value) is not None and value != "0" * 64


def integer(profile: dict, key: str, minimum: int, maximum: int) -> int:
    value = profile.get(key)
    require(type(value) is int and minimum <= value <= maximum, f"Invalid AMD profile field {key}")
    return value


def unique_object(pairs: list[tuple[str, object]]) -> dict:
    value = dict(pairs)
    require(len(value) == len(pairs), "Duplicate manifest field")
    return value


def bind(package: Path, approved: str) -> tuple[str, list[str]]:
    require(SHA256.fullmatch(approved) is not None, "A trusted manifest SHA-256 is required")
    manifest = bounded_file(package / "qualification-manifest.json", 64 * 1024)
    require(hashlib.sha256(manifest).hexdigest() == approved, "Package manifest digest mismatch")
    data = json.loads(manifest, object_pairs_hook=unique_object)
    runtime = data["runtime_basename"]
    require(isinstance(runtime, str) and HIP_RUNTIME.fullmatch(runtime) is not None
            and data["runtime_shipped"] is False and data["kpack_shipped"] is False,
            "Package does not identify the installed AMD runtime contract")
    uniform = data.get("uniform_runtime_packaged") is True
    write_trial = data.get("uniform_write_trial_profile")
    require("uniform_write_trial_profile" not in data or (uniform and isinstance(write_trial, dict)),
            "Write trial profile requires the uniform worker and module")
    anchor = data.get("shared_base_profile")
    read = data.get("native_lz4_read_profile")
    require(("shared_base_profile" not in data or isinstance(anchor, dict))
            and ("native_lz4_read_profile" not in data or isinstance(read, dict))
            and (uniform or anchor is not None or read is not None),
            "Package must identify at least one approved AMD route")
    files = data["files"]
    require(isinstance(files, list) and 0 < len(files) <= 64, "Package inventory out of bounds")
    names = [record.get("path") if isinstance(record, dict) else None for record in files]
    require(all(isinstance(name, str) for name in names) and len({name.casefold() for name in names}) == len(names),
            "Package inventory has invalid or repeated paths")
    selected = ["qualification-manifest.json"]

    def verified(name: str, limit: int) -> tuple[bytes, bytes]:
        candidates = [record for record in files if record["path"] == name]
        require(len(candidates) == 1, f"Unique qualified package entry required for {name}")
        record = candidates[0]
        contents = bounded_file(package / name, limit)
        digest = hashlib.sha256(contents)
        require(len(contents) == record["bytes"] and digest.hexdigest() == record["sha256"],
                f"Package artifact digest or size mismatch for {name}")
        selected.append(name)
        return contents, digest.digest()

    def array_literal(value: bytes) -> str:
        return ", ".join(f"std::byte{{0x{byte:02x}}}" for byte in value)

    def artifact(name: str, field: str, worker: bool, required_exports: tuple[str, ...] = ()) -> str:
        contents, digest = verified(name, (8 if worker else 128) * 1024 * 1024)
        imports, delayed = pe_import_names(contents, allow_delay=True)
        allowed = WORKER_SYSTEM_IMPORTS if worker else WORKER_SYSTEM_IMPORTS | {runtime}
        require(bool(imports) and set(imports + delayed) <= allowed
                and (not worker or not delayed), f"Unexpected PE dependencies for {name}")
        if required_exports:
            missing = set(required_exports) - pe_function_exports(contents)
            require(not missing, f"Approved module lacks its dispatch entry: {', '.join(sorted(missing))}")
        return f"inline constexpr std::array {field}{{{array_literal(digest)}}};\n"

    zero = "inline constexpr std::array<std::byte,32> {}{{}};\n"
    result = ("#pragma once\n#include <array>\n#include <cstddef>\n#include <cstdint>\n"
              f"#define FLITZFS_INSTALLED_AMD_HAS_WRITE_TRIAL {int(write_trial is not None)}\n"
              f"#define FLITZFS_INSTALLED_AMD_HAS_ANCHOR {int(anchor is not None)}\n"
              f"#define FLITZFS_INSTALLED_AMD_HAS_NATIVE_READ {int(read is not None)}\n"
              "namespace flitzfs::installed_amd_package {\n"
              f'inline constexpr wchar_t runtime_basename[]=L"{runtime}";\n'
              f"inline constexpr bool has_uniform={'true' if uniform else 'false'};\n")
    result += artifact(UNIFORM_WORKER, "worker_sha256", True) if uniform else zero.format("worker_sha256")
    result += artifact(UNIFORM_MODULE, "uniform_module_sha256", False) if uniform else zero.format("uniform_module_sha256")
    if write_trial is not None:
        evidence = write_trial.get("qualification_sha256")
        runtime_digest = write_trial.get("runtime_sha256")
        require(exact_digest(evidence) and exact_digest(runtime_digest), "Write trial provenance digests are required")
        _, receipt_digest = verified(UNIFORM_TRIAL_EVIDENCE, 256 * 1024)
        require(receipt_digest.hex() == evidence, "Write trial qualification digest mismatch")
        fields = {
            "vendor_id": integer(write_trial, "vendor_id", 0x1002, 0x1002),
            "device_id": integer(write_trial, "device_id", 1, 0xffff),
            "subsystem_id": integer(write_trial, "subsystem_id", 0, 0xffffffff),
            "revision": integer(write_trial, "revision", 0, 0xff),
            "worker_memory_mib": integer(write_trial, "worker_memory_mib", 64, 0xffffffff),
            "memory_share_divisor": integer(write_trial, "memory_share_divisor", 16, 0xffffffff),
            "worker_cpu_rate": integer(write_trial, "worker_cpu_rate", 100, 2000),
            "worker_timeout_ms": integer(write_trial, "worker_timeout_ms", 1, 40000),
            "maximum_identity_queries": integer(write_trial, "maximum_identity_queries", 1, 64),
            "maximum_batch_chunks": integer(write_trial, "maximum_batch_chunks", 2, 0xffffffff),
            "maximum_launch_chunks": integer(write_trial, "maximum_launch_chunks", 1, 0xffffffff),
        }
        require(fields["maximum_launch_chunks"] <= fields["maximum_batch_chunks"],
                "Write trial launch exceeds its approved batch")
        separate_requests = write_trial.get("separate_requests", False)
        require(type(separate_requests) is bool, "Invalid write trial separate_requests")
        await_later_writes = write_trial.get("await_later_writes", False)
        require(type(await_later_writes) is bool and (separate_requests or not await_later_writes),
                "Invalid write trial await_later_writes")
        allow_smaller_writes = write_trial.get("allow_smaller_writes", False)
        require(type(allow_smaller_writes) is bool and (await_later_writes or not allow_smaller_writes),
                "Invalid write trial allow_smaller_writes")
        result += f"inline constexpr bool write_trial_separate_requests={str(separate_requests).lower()};\n"
        result += f"inline constexpr bool write_trial_await_later_writes={str(await_later_writes).lower()};\n"
        result += f"inline constexpr bool write_trial_allow_smaller_writes={str(allow_smaller_writes).lower()};\n"
        result += f"inline constexpr std::array write_trial_runtime_sha256{{{array_literal(bytes.fromhex(runtime_digest))}}};\n"
        result += "".join(f"inline constexpr std::uint32_t write_trial_{key}={value}U;\n" for key, value in fields.items())
    if anchor is not None:
        evidence = anchor.get("qualification_sha256")
        runtime_digest = anchor.get("runtime_sha256")
        search_mode = anchor.get("search_mode", "copy")
        require(search_mode in ("copy", "reference-xor"), "Invalid anchor profile field search_mode")
        reference_xor = search_mode == "reference-xor"
        require(exact_digest(evidence) and exact_digest(runtime_digest), "Anchor provenance digests are required")
        _, receipt_digest = verified(ANCHOR_EVIDENCE, 256 * 1024)
        require(receipt_digest.hex() == evidence, "Anchor qualification digest mismatch")
        fields = {
            "vendor_id": integer(anchor, "vendor_id", 0x1002, 0x1002),
            "device_id": integer(anchor, "device_id", 1, 0xffff),
            "subsystem_id": integer(anchor, "subsystem_id", 0, 0xffffffff),
            "revision": integer(anchor, "revision", 0, 0xff),
            "minimum_members": integer(anchor, "minimum_members", 1, 0xffffffff),
            "maximum_members": integer(anchor, "maximum_members", 1, 0xffffffff),
            "worker_memory_mib": integer(anchor, "worker_memory_mib", 64, 0xffffffff),
            "memory_share_divisor": integer(anchor, "memory_share_divisor", 16, 0xffffffff),
            "worker_cpu_rate": integer(anchor, "worker_cpu_rate", 100, 2000),
            "worker_timeout_ms": integer(anchor, "worker_timeout_ms", 1, 40000),
            "maximum_identity_queries": integer(anchor, "maximum_identity_queries", 1, 64),
            "duty_ppm": integer(anchor, "duty_ppm", 1, 100000),
            "dispatches_per_wait": integer(anchor, "dispatches_per_wait", 1, 4)
            if "dispatches_per_wait" in anchor else 1,
            "maximum_dispatch_members": integer(anchor, "maximum_dispatch_members", 1 if reference_xor else 4, 0xffffffff)
            if "maximum_dispatch_members" in anchor else 0,
        }
        require(fields["minimum_members"] <= fields["maximum_members"], "Inverted anchor work range")
        maximum_dispatch = fields["maximum_dispatch_members"]
        require(maximum_dispatch <= fields["maximum_members"], "Anchor dispatch exceeds approved batch work")
        dispatch_exports = ("flitzfs_rocm_anchor_window_api_v11",) if maximum_dispatch > 3 else ()
        if reference_xor:
            require(maximum_dispatch != 0, "Reference search requires a dispatch grant")
            require(fields["dispatches_per_wait"] == 1, "Reference search requires a single dispatch per wait")
            if maximum_dispatch <= 3:
                dispatch_exports = ("flitzfs_rocm_anchor_window_api_v2",)
            dispatch_exports += ("flitzfs_rocm_anchor_match_api_v1",
                                 "flitzfs_rocm_anchor_sparse_xor_arena_api_v8",
                                 "flitzfs_rocm_anchor_reference_api_v10")
        result += artifact(ANCHOR_WORKER, "anchor_worker_sha256", True)
        result += artifact(ANCHOR_MODULE, "anchor_module_sha256", False, dispatch_exports)
        result += f"inline constexpr bool anchor_reference_xor={'true' if reference_xor else 'false'};\n"
        result += f"inline constexpr std::array anchor_runtime_sha256{{{array_literal(bytes.fromhex(runtime_digest))}}};\n"
        result += "".join(f"inline constexpr std::uint32_t anchor_{key}={value}U;\n" for key, value in fields.items())
    if read is not None:
        evidence = read.get("qualification_sha256")
        runtime_digest = read.get("runtime_sha256")
        architecture = read.get("architecture")
        require(exact_digest(evidence) and exact_digest(runtime_digest), "Read provenance digests are required")
        require(isinstance(architecture, str) and GPU_ARCHITECTURE.fullmatch(architecture) is not None,
                "Invalid native LZ4 read architecture")
        _, receipt_digest = verified(READ_EVIDENCE, 256 * 1024)
        require(receipt_digest.hex() == evidence, "Read qualification digest mismatch")
        fields = {
            "vendor_id": integer(read, "vendor_id", 0x1002, 0x1002),
            "device_id": integer(read, "device_id", 1, 0xffff),
            "subsystem_id": integer(read, "subsystem_id", 0, 0xffffffff),
            "revision": integer(read, "revision", 0, 0xff),
            "minimum_frames": integer(read, "minimum_frames", 1, 0x7fffffff),
            "maximum_frames": integer(read, "maximum_frames", 1, 0x7fffffff),
            "worker_memory_mib": integer(read, "worker_memory_mib", 64, 0xffffffff),
            "memory_share_divisor": integer(read, "memory_share_divisor", 16, 0xffffffff),
            "worker_cpu_rate": integer(read, "worker_cpu_rate", 100, 2000),
            "worker_timeout_ms": integer(read, "worker_timeout_ms", 1, 40000),
            "maximum_identity_queries": integer(read, "maximum_identity_queries", 1, 64),
            "wave_mask": integer(read, "wave_mask", 1, 3),
            "minimum_lead_ms": integer(read, "minimum_lead_ms", 1, 0xffffffff),
        }
        require(fields["minimum_frames"] <= fields["maximum_frames"], "Inverted native LZ4 read frame grant")
        slab_bytes = integer(read, "maximum_slab_bytes", 1, fields["worker_memory_mib"] * 1024 * 1024)
        result += artifact(READ_WORKER, "read_worker_sha256", True)
        result += artifact(READ_MODULE, "read_module_sha256", False, ("flitzfs_native_lz4_decode_api",))
        result += f'inline constexpr wchar_t read_architecture[]=L"{architecture}";\n'
        result += f"inline constexpr std::uint64_t read_maximum_slab_bytes={slab_bytes}ULL;\n"
        result += f"inline constexpr std::array read_runtime_sha256{{{array_literal(bytes.fromhex(runtime_digest))}}};\n"
        result += "".join(f"inline constexpr std::uint32_t read_{key}={value}U;\n" for key, value in fields.items())
    for record in files:
        name = record["path"]
        if isinstance(name, str) and LICENSE_PATH.fullmatch(name):
            verified(name, 1024 * 1024)
    return result + "}\n", selected


def header(package: Path, approved: str) -> str:
    return bind(package, approved)[0]


def write_if_changed(path: Path, text: str) -> None:
    if not path.exists() or path.read_text(encoding="ascii") != text:
        path.write_text(text, encoding="ascii", newline="\n")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--manifest-sha256", required=True)
    parser.add_argument("--header", required=True, type=Path)
    parser.add_argument("--cmake-plan", type=Path)
    args = parser.parse_args()
    content, files = bind(args.package, args.manifest_sha256)
    write_if_changed(args.header, content)
    if args.cmake_plan:
        files = "\n".join(f'  "{name}"' for name in files)
        write_if_changed(args.cmake_plan, f"set(FLITZFS_APPROVED_AMD_FILES\n{files}\n)\n")


if __name__ == "__main__":
    main()
