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


UNIFORM_WORKER = "flitzfs_hip_lz4_worker.exe"
UNIFORM_MODULE = "hip-uniform-lz4/flitzfs_rocm_uniform_lz4_candidate.dll"
ANCHOR_WORKER = "flitzfs_hip_worker.exe"
ANCHOR_MODULE = "hip/flitzfs_rocm_kernels.dll"
ANCHOR_EVIDENCE = "shared-base-qualification.json"
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
    require(type(value) is int and minimum <= value <= maximum, f"Invalid anchor profile field {key}")
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
    anchor = data.get("shared_base_profile")
    require(("shared_base_profile" not in data or isinstance(anchor, dict)) and (uniform or anchor is not None),
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

    def artifact(name: str, field: str, worker: bool) -> str:
        contents, digest = verified(name, (8 if worker else 128) * 1024 * 1024)
        imports, delayed = pe_import_names(contents, allow_delay=True)
        allowed = WORKER_SYSTEM_IMPORTS if worker else WORKER_SYSTEM_IMPORTS | {runtime}
        require(bool(imports) and set(imports + delayed) <= allowed
                and (not worker or not delayed), f"Unexpected PE dependencies for {name}")
        return f"inline constexpr std::array {field}{{{array_literal(digest)}}};\n"

    zero = "inline constexpr std::array<std::byte,32> {}{{}};\n"
    result = ("#pragma once\n#include <array>\n#include <cstddef>\n#include <cstdint>\n"
              f"#define FLITZFS_INSTALLED_AMD_HAS_ANCHOR {int(anchor is not None)}\n"
              "namespace flitzfs::installed_amd_package {\n"
              f'inline constexpr wchar_t runtime_basename[]=L"{runtime}";\n'
              f"inline constexpr bool has_uniform={'true' if uniform else 'false'};\n")
    result += artifact(UNIFORM_WORKER, "worker_sha256", True) if uniform else zero.format("worker_sha256")
    result += artifact(UNIFORM_MODULE, "uniform_module_sha256", False) if uniform else zero.format("uniform_module_sha256")
    if anchor is not None:
        evidence = anchor.get("qualification_sha256")
        runtime_digest = anchor.get("runtime_sha256")
        require(exact_digest(evidence) and exact_digest(runtime_digest), "Anchor provenance digests are required")
        _, receipt_digest = verified(ANCHOR_EVIDENCE, 256 * 1024)
        require(receipt_digest.hex() == evidence, "Anchor qualification digest mismatch")
        fields = {
            "vendor_id": integer(anchor, "vendor_id", 0x1002, 0x1002),
            "device_id": integer(anchor, "device_id", 1, 0xffff),
            "subsystem_id": integer(anchor, "subsystem_id", 0, 0xffffffff),
            "revision": integer(anchor, "revision", 0, 0xff),
            "minimum_members": integer(anchor, "minimum_members", 1, 254),
            "maximum_members": integer(anchor, "maximum_members", 1, 254),
            "worker_memory_mib": integer(anchor, "worker_memory_mib", 64, 0xffffffff),
            "memory_share_divisor": integer(anchor, "memory_share_divisor", 16, 0xffffffff),
            "worker_cpu_rate": integer(anchor, "worker_cpu_rate", 100, 2000),
            "worker_timeout_ms": integer(anchor, "worker_timeout_ms", 1, 40000),
            "maximum_identity_queries": integer(anchor, "maximum_identity_queries", 1, 64),
            "duty_ppm": integer(anchor, "duty_ppm", 1, 100000),
        }
        require(fields["minimum_members"] <= fields["maximum_members"], "Inverted anchor work range")
        result += artifact(ANCHOR_WORKER, "anchor_worker_sha256", True)
        result += artifact(ANCHOR_MODULE, "anchor_module_sha256", False)
        result += f"inline constexpr std::array anchor_runtime_sha256{{{array_literal(bytes.fromhex(runtime_digest))}}};\n"
        result += "".join(f"inline constexpr std::uint32_t anchor_{key}={value}U;\n" for key, value in fields.items())
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
