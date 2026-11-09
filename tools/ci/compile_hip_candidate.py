#!/usr/bin/env python3
"""Offline HIP -> AMDGPU code-object qualification; never load/execute the module.

Inputs: trusted compiler/linker paths and a NEW output directory. Outputs:
object, linked gfx1201 module and source/tool-bound JSON evidence. Linux-only
resource limits bound each compiler process. No network, SDK installation or
GPU APIs. A compiler pass is NOT runtime/hardware qualification.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import resource
import shutil
import struct
import subprocess
import sys

MAX_FILE = 4 * 1024 * 1024


def bounded() -> None:
    """Limit compiler address space, output file size, CPU time and core dumps."""
    resource.setrlimit(resource.RLIMIT_AS, (2 * 1024**3, 2 * 1024**3))
    resource.setrlimit(resource.RLIMIT_FSIZE, (MAX_FILE, MAX_FILE))
    resource.setrlimit(resource.RLIMIT_CPU, (40, 40))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def run(command: list[str]) -> str:
    """Run one trusted compiler tool, no shell, with a 45-second wall limit."""
    p = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True,
                       text=True, timeout=45, check=False, preexec_fn=bounded)
    if p.returncode:
        raise RuntimeError(f"{Path(command[0]).name} failed: {(p.stderr + p.stdout)[-6000:]}")
    return p.stdout


def read_bounded(path: Path) -> bytes:
    """Read a small generated artifact only after checking its byte count."""
    if not 0 < path.stat().st_size <= MAX_FILE:
        raise ValueError("generated artifact exceeds admitted size")
    with path.open("rb") as f:
        data = f.read(MAX_FILE + 1)
    if len(data) > MAX_FILE:
        raise ValueError("artifact grew past its budget")
    return data


def main() -> None:
    """Compile and record exact artifacts in a fresh directory; no device use."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--clang", required=True)
    parser.add_argument("--lld", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != "linux":
        raise ValueError("offline qualifier currently requires Linux resource limits")
    compiler = shutil.which(args.clang)
    linker = shutil.which(args.lld)
    if not compiler or not linker:
        raise ValueError("explicit LLVM compiler and linker are required")
    root = Path(__file__).resolve().parents[2]
    output = args.output.resolve()
    if shutil.disk_usage(output.parent).free < 1024**3:
        raise ValueError("offline compiler requires 1 GiB free storage reserve")
    output.mkdir(exist_ok=False)
    files = [root / "src/gpu/decode_kernel.hip", root / "src/gpu/decode_kernel.hpp",
             root / "include/flitzfs/gpu_protocol.hpp"]
    before = {str(p.relative_to(root)): hashlib.sha256(read_bounded(p)).hexdigest() for p in files}
    version = run([compiler, "--version"]).splitlines()[0]
    linker_version = run([linker, "--version"]).splitlines()[0]
    common = [compiler, "-x", "hip", "--cuda-device-only", "--offload-arch=gfx1201",
              "-fhip-emit-relocatable", "--no-offload-new-driver",
              "-nogpuinc", "-nogpulib", "-std=c++17", "-O2", "-fno-exceptions", "-fno-rtti",
              "-Wall", "-Wextra", "-Werror", "-I", str(root / "include"), str(files[0])]
    obj = output / "decode-gfx1201.o"
    module = output / "decode-gfx1201.hsaco"
    run([*common, "-c", "-o", str(obj)])
    # HIP -c alone produces a linked/bundled code object. Explicit relocatable
    # emission is covered by the pinned LLVM 23.1.5 hip-device-compile.hip tests.
    relocatable = read_bounded(obj)
    if (len(relocatable) < 64 or relocatable[:7] != b"\x7fELF\x02\x01\x01"
            or struct.unpack_from("<HH", relocatable, 16) != (1, 224)):
        raise ValueError("HIP compiler did not emit an AMDGPU relocatable object")
    run([linker, "-shared", "--no-undefined", str(obj), "-o", str(module)])
    data = read_bounded(module)
    # ELF64 little-endian shared AMDGPU, GFX1201 low-byte machine flag (0x4e).
    if len(data) < 64 or data[:7] != b"\x7fELF\x02\x01\x01":
        raise ValueError("compiler did not emit a little-endian ELF64 code object")
    etype, machine = struct.unpack_from("<HH", data, 16)
    flags = struct.unpack_from("<I", data, 48)[0]
    if etype != 3 or machine != 224 or flags & 0xff != 0x4e:
        raise ValueError(f"unexpected device object identity: {etype}, {machine}, {flags}")
    if b"flitzfs_decode_v1" not in data or b"amdhsa.kernels" not in data:
        raise ValueError("kernel symbol or AMDHSA metadata missing")
    after = {str(p.relative_to(root)): hashlib.sha256(read_bounded(p)).hexdigest() for p in files}
    if after != before:
        raise ValueError("kernel source changed during compilation")
    receipt = {"schema": 1, "target": "gfx1201", "kernel": "flitzfs_decode_v1",
               "compiler": version, "linker": linker_version, "source_sha256": before,
               "code_object_sha256": hashlib.sha256(data).hexdigest(), "code_object_bytes": len(data),
               "gpu_runtime_loaded": False, "gpu_executed": False,
               "scope": "offline Linux LLVM HIP compilation and code-object identity only"}
    with (output / "qualification.json").open("x", encoding="utf-8") as f:
        json.dump(receipt, f, indent=2)
        f.write("\n")
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as e:
        print(f"Offline HIP qualification failed: {e}", file=sys.stderr)
        raise SystemExit(1) from e
