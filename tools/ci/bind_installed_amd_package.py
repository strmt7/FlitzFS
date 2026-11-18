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


WORKER = "flitzfs_hip_lz4_worker.exe"
MODULE = "hip-uniform-lz4/flitzfs_rocm_uniform_lz4_candidate.dll"
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


def header(package: Path, approved: str) -> str:
    require(SHA256.fullmatch(approved) is not None, "A trusted manifest SHA-256 is required")
    manifest = bounded_file(package / "qualification-manifest.json", 64 * 1024)
    require(hashlib.sha256(manifest).hexdigest() == approved, "Package manifest digest mismatch")
    data = json.loads(manifest)
    runtime = data["runtime_basename"]
    require(HIP_RUNTIME.fullmatch(runtime) is not None and data["uniform_runtime_packaged"] is True
            and data["runtime_shipped"] is False and data["kpack_shipped"] is False,
            "Package does not identify the installed uniform AMD runtime contract")
    files = data["files"]
    require(isinstance(files, list) and 0 < len(files) <= 64, "Package inventory out of bounds")
    digests: dict[str, bytes] = {}
    for name, limit in ((WORKER, 8 * 1024 * 1024), (MODULE, 128 * 1024 * 1024)):
        candidates = [record for record in files if record["path"] == name]
        require(len(candidates) == 1, f"Unique qualified package entry required for {name}")
        record = candidates[0]
        contents = bounded_file(package / name, limit)
        digest = hashlib.sha256(contents)
        require(len(contents) == record["bytes"] and digest.hexdigest() == record["sha256"],
                f"Package artifact digest or size mismatch for {name}")
        imports, delayed = pe_import_names(contents, allow_delay=True)
        allowed = WORKER_SYSTEM_IMPORTS if name == WORKER else WORKER_SYSTEM_IMPORTS | {runtime}
        require(bool(imports) and set(imports + delayed) <= allowed
                and (name != WORKER or not delayed), f"Unexpected PE dependencies for {name}")
        digests[name] = digest.digest()

    def array_literal(name: str) -> str:
        return ", ".join(f"std::byte{{0x{value:02x}}}" for value in digests[name])

    return ("#pragma once\n#include <array>\n#include <cstddef>\n"
            "namespace flitzfs::installed_amd_package {\n"
            f'inline constexpr wchar_t runtime_basename[]=L"{runtime}";\n'
            f"inline constexpr std::array worker_sha256{{{array_literal(WORKER)}}};\n"
            f"inline constexpr std::array uniform_module_sha256{{{array_literal(MODULE)}}};\n"
            "}\n")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--manifest-sha256", required=True)
    parser.add_argument("--header", required=True, type=Path)
    args = parser.parse_args()
    content = header(args.package, args.manifest_sha256)
    if not args.header.exists() or args.header.read_text(encoding="ascii") != content:
        args.header.write_text(content, encoding="ascii", newline="\n")


if __name__ == "__main__":
    main()
