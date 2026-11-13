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

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--harness", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    args = parser.parse_args()
    require(os.environ.get("GITHUB_ACTIONS") == "true" and
            os.environ.get("RUNNER_ENVIRONMENT") == "github-hosted" and
            os.environ.get("RUNNER_OS") == "Windows", "Disposable hosted Windows only")
    def capture(path, role):
        with path.open("rb") as stream:
            data = stream.read(32*1024*1024 + 1)
        return audit_volume_binary(data, role)
    results = [capture(args.app, "application"), capture(args.harness, "harness")]
    receipt = dict(experimental=True, source_commit=os.environ["GITHUB_SHA"],
                   scope="Native CPU application imports; not a mount/GPU/release qualification",
                   binaries=results)
    with args.receipt.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(receipt, stream, indent=2)
        stream.write("\n")
    print(json.dumps(receipt, sort_keys=True))

if __name__ == "__main__":
    main()
