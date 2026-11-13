#!/usr/bin/env python3
"""Explicit native Windows GUI profile; never weakens the console CPU audit."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
from check_portable_cpu import pe_import_names, number, require

def audit_volume_binary(data, role):
    require(sys.platform == "win32", "Volume application audit requires Windows")
    require(role in ("application", "harness"), "Unknown application binary role")
    require(0 < len(data) <= 32*1024*1024, "Application executable size cap")
    names, delayed = pe_import_names(data)
    base = {"kernel32.dll", "advapi32.dll", "bcrypt.dll", "ntdll.dll", "user32.dll"}
    allowed = base | ({"shell32.dll", "gdi32.dll"} if role == "application" else set())
    require(bool(names) and set(names) <= allowed and not delayed,
            "Non-admitted application dependency: " + repr(names))
    pe = number(data, 0x3c, "<I")
    optional = pe + 24
    require(number(data, optional + 68, "<H") == (2 if role == "application" else 3),
            "Application/harness subsystem differs")
    flags = number(data, optional + 70, "<H")
    require(flags & 0x160 == 0x160, "ASLR, high-entropy VA or NX missing")
    return dict(role=role, format="PE32+", architecture="AMD64",
                bytes=len(data), sha256=hashlib.sha256(data).hexdigest(),
                os_imports=sorted(set(names)), delay_imports=False,
                aslr=True, high_entropy_va=True, nx=True)


def copy_notices(destination, source_commit):
    """NEW hosted output only; preserve all selected static-library notices."""
    root = Path(__file__).resolve().parents[2]
    temporary = Path(os.environ["RUNNER_TEMP"]).resolve()
    prefix = "flitzfs-volume-notices-"
    suffix = destination.name[len(prefix):]
    require(destination.resolve().parent == temporary and destination.name.startswith(prefix) and
            len(suffix) == 32 and all(c in "0123456789abcdef" for c in suffix),
            "Notices require a fresh owned hosted directory")
    inputs = (
        ("FlitzFS-LICENSE.txt", root / "LICENSE", "strmt7/FlitzFS", source_commit),
        ("BLAKE3-LICENSE-Apache2.txt", root / "third_party/blake3/LICENSE_A2", "BLAKE3-team/BLAKE3", "1.8.7"),
        ("BLAKE3-LICENSE-LLVM.txt", root / "third_party/blake3/LICENSE_A2LLVM", "BLAKE3-team/BLAKE3", "1.8.7"),
        ("BLAKE3-LICENSE-CC0.txt", root / "third_party/blake3/LICENSE_CC0", "BLAKE3-team/BLAKE3", "1.8.7"),
        ("Zstandard-LICENSE.txt", root / ".ci-deps/zstd/LICENSE", "meta/zstd", "f8745da6ff1ad1e7bab384bd1f9d742439278e99"),
        ("LZ4-LICENSE.txt", root / ".ci-deps/lz4/LICENSE", "lz4/lz4", "aa4647fad575d01a874aaaadcdbe8e6a40e072f5"),
    )
    captures = []
    for name, path, repository, revision in inputs:
        require(path.is_file() and not path.is_symlink(), "Missing/linked dependency notice")
        with path.open("rb") as stream:
            data = stream.read(65537)
        require(0 < len(data) <= 65536, "Dependency notice byte cap")
        captures.append((name, data, repository, revision))
    # mkdir and every child open reject existing outputs at the OS operation.
    destination.mkdir()
    records = []
    for name, data, repository, revision in captures:
        with (destination / name).open("xb") as stream:
            stream.write(data)
        records.append(dict(file=name, bytes=len(data), sha256=hashlib.sha256(data).hexdigest(),
                            source_repository=repository, source_revision=revision))
    notice = (
        "EXPERIMENTAL DEVELOPMENT ARTIFACT; NOT A RELEASE\n"
        "FlitzFS corresponding source: https://github.com/strmt7/FlitzFS/tree/" + source_commit + "\n"
        "Build: tools/ci/build_windows_native.ps1 -PortableCpu -VolumeApp\n"
        "The workflow and tools verify dependency pins; dependency notices are preserved beside this file.\n"
        "BLAKE3 source identity and licenses: third_party/blake3/provenance.json in the corresponding source.\n"
        "No installer, driver, HIP distribution or production filesystem qualification is included.\n"
    )
    source_bytes = notice.encode("utf-8")
    require(len(source_bytes) <= 4096, "Source reference cap")
    with (destination / "SOURCE.txt").open("xb") as stream:
        stream.write(source_bytes)
    records.append(dict(file="SOURCE.txt", bytes=len(source_bytes),
                        sha256=hashlib.sha256(source_bytes).hexdigest(),
                        source_repository="strmt7/FlitzFS", source_revision=source_commit))
    return records

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--harness", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--notices", type=Path, required=True)
    args = parser.parse_args()
    require(os.environ.get("GITHUB_ACTIONS") == "true" and
            os.environ.get("RUNNER_ENVIRONMENT") == "github-hosted" and
            os.environ.get("RUNNER_OS") == "Windows", "Disposable hosted Windows only")
    def capture(path, role):
        with path.open("rb") as stream:
            data = stream.read(32*1024*1024 + 1)
        return audit_volume_binary(data, role)
    results = [capture(args.app, "application"), capture(args.harness, "harness")]
    source_commit = os.environ["GITHUB_SHA"]
    require(len(source_commit) == 40 and all(c in "0123456789abcdef" for c in source_commit), "Source identity")
    notices = copy_notices(args.notices, source_commit)
    receipt = dict(experimental=True, source_commit=source_commit,
                   scope="Native CPU application imports; not a mount/GPU/release qualification",
                   binaries=results, notices=notices)
    with args.receipt.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(receipt, stream, indent=2)
        stream.write("\n")
    print(json.dumps(receipt, sort_keys=True))

if __name__ == "__main__":
    main()
