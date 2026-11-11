#!/usr/bin/env python3
"""Stage the selected SDK's PE import closure in a new hosted build directory."""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat

from check_portable_cpu import pe_import_names, require

MIB = 1024**2
MAX_FILE = 128 * MIB
MAX_TOTAL = 256 * MIB
MAX_FILES = 32
DLL_NAME = re.compile(r"[a-z0-9_-][a-z0-9_.-]{0,122}\.dll\Z")


def ordinary_file(path: Path) -> int:
    info = path.lstat()
    require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1
            and not (getattr(info, "st_file_attributes", 0) & 0x400),
            f"unsafe DLL source {path.name}")
    require(0 < info.st_size <= MAX_FILE, f"DLL size outside budget: {path.name}")
    return info.st_size


def system_directory() -> Path:
    value = ctypes.create_unicode_buffer(32768)
    count = ctypes.windll.kernel32.GetSystemDirectoryW(value, len(value))
    require(0 < count < len(value), "Windows system directory unavailable")
    return Path(value.value)


def stage(runtime: Path, kernel: Path, output: Path, header: Path) -> dict:
    system = system_directory()
    pending = [runtime, kernel]
    sources: dict[str, tuple[Path, int, str]] = {}
    bytes_total = 0
    while pending:
        path = pending.pop()
        name = path.name.lower()
        require(DLL_NAME.fullmatch(name) is not None, "unsafe bundled DLL name")
        if name in sources:
            require(path == sources[name][0], "conflicting DLL sources")
            continue
        size = ordinary_file(path)
        require(len(sources) < MAX_FILES and size <= MAX_TOTAL - bytes_total, "DLL bundle budget")
        data = path.read_bytes()
        require(len(data) == size, "DLL source changed during import inspection")
        digest = hashlib.sha256(data).hexdigest()
        sources[name] = (path, size, digest)
        bytes_total += size
        imports, delayed = pe_import_names(data, allow_delay=True)
        for dependency in set(imports + delayed):
            require(DLL_NAME.fullmatch(dependency) is not None, "unsafe PE import name")
            # API sets and Windows-owned dependencies are resolved by the OS.
            if dependency.startswith(("api-ms-win-", "ext-ms-win-")):
                continue
            if dependency in (runtime.name.lower(), kernel.name.lower()):
                pending.append(runtime if dependency == runtime.name.lower() else kernel)
                continue
            candidate = runtime.parent / dependency
            if candidate.exists():
                pending.append(candidate)
            else:
                require((system / dependency).is_file(), f"unavailable DLL import: {dependency}")

    output.mkdir()
    records = []
    for name, (source, size, digest) in sorted(sources.items()):
        destination = output / name
        with source.open("rb") as incoming, destination.open("xb") as outgoing:
            shutil.copyfileobj(incoming, outgoing, 64 * 1024)
        with destination.open("rb") as copied:
            require(ordinary_file(destination) == size and
                    hashlib.file_digest(copied, "sha256").hexdigest() == digest, "DLL copy mismatch")
        records.append(dict(name=name, bytes=size, sha256=digest))
    with header.open("x", encoding="ascii", newline="\n") as stream:
        stream.write("#pragma once\nnamespace flitzfs::hip_bundle {\n")
        stream.write("struct File { const wchar_t* name; unsigned long long size; const char* sha256; };\n")
        stream.write(f'inline constexpr wchar_t runtime[] = L"{runtime.name.lower()}";\n')
        stream.write(f'inline constexpr wchar_t kernel[] = L"{kernel.name.lower()}";\n')
        stream.write("inline constexpr File files[] = {\n")
        for record in records:
            stream.write(f'  {{L"{record["name"]}", {record["bytes"]}ULL, "{record["sha256"]}"}},\n')
        stream.write("};\n}\n")
    return dict(files=records, bytes=bytes_total, import_scope="PE normal and delay imports")


def main() -> None:
    require(os.name == "nt" and os.environ.get("GITHUB_ACTIONS") == "true"
            and os.environ.get("RUNNER_ENVIRONMENT") == "github-hosted", "hosted SDK build required")
    parser = argparse.ArgumentParser()
    for name in ("runtime", "kernel", "output", "header", "receipt"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    root = Path(os.environ["RUNNER_TEMP"]).resolve(strict=True)
    for path in (args.output, args.header, args.receipt):
        require(path.resolve().is_relative_to(root) and not path.exists(), "new runner fixture required")
    result = stage(args.runtime, args.kernel, args.output, args.header)
    with args.receipt.open("x", encoding="utf-8") as report:
        json.dump(result, report, indent=2)
        report.write("\n")
    print("HIP worker bundle: " + json.dumps(result), flush=True)


if __name__ == "__main__":
    main()
