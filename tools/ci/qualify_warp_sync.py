"""Experimental offline HIP synchronization API qualification; never load or launch."""
from __future__ import annotations
import hashlib
from pathlib import Path
import stat
import shutil

PROBE = "tests/rocm_compile/warp_sync_probe.hip"
PROBE_BLOB = "0705e743dcf9bec07f1e250b042edc84162d9d8c"
PROBE_SHA256 = "59474113a2a0d5d2dde1a2a7de8b10d6a9b4de41fdd97664ce62b3cabe453201"
HEADER = "include/hip/amd_detail/amd_warp_sync_functions.h"
HEADER_SHA256 = "e6f8f61066b034141ad288a01b82006a0e6f0e8e9e7750ac36153b1602eedfe2"
RUNTIME_SOURCE = "7da43026e4f2ef7600433c38c820732f7a205d22"
HEADER_BLOB = "51d07f6e35298abba5fb9817d61cf6eb18fe5f66"

def require(ok, why):
    if not ok:
        raise ValueError(why)

def canonical_text(raw: bytes, expected: str, limit: int):
    require(isinstance(raw, bytes) and 0 < len(raw) <= limit and b"\0" not in raw,
            "offline source byte bound")
    physical = hashlib.sha256(raw).hexdigest()
    if physical == expected:
        return raw, dict(physical_sha256=physical, physical_bytes=len(raw), representation="exact")
    normalized = raw.replace(b"\r\n", b"\n")
    require(b"\r" not in normalized and raw == normalized.replace(b"\n", b"\r\n") and
            hashlib.sha256(normalized).hexdigest() == expected, "offline source identity differs")
    return normalized, dict(physical_sha256=physical, physical_bytes=len(raw),
                            representation="crlf-checkout")

def regular_info(path: Path, root: Path, limit: int):
    root = root.resolve(strict=True)
    require(path.is_absolute() and path.is_relative_to(root) and
            ".." not in path.parts and path.resolve(strict=True).is_relative_to(root),
            "offline input escaped owned root")
    current = path
    for _ in range(32):
        if current == root:
            break
        info = current.lstat()
        require(not (getattr(info, "st_file_attributes", 0) & 0x400) and
                not current.is_symlink(), "offline input has a reparse ancestor")
        current = current.parent
    else:
        raise ValueError("offline ancestor count bound")
    info = path.lstat()
    require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1 and
            0 < info.st_size <= limit, "offline input regular-file bound")
    return info

def regular(path: Path, root: Path, limit: int):
    regular_info(path, root, limit)
    with path.open("rb") as stream:
        data = stream.read(limit + 1)
    require(len(data) <= limit, "offline input read bound")
    return data

def probe_source(repo: Path):
    raw = regular(repo / PROBE, repo, 4096)
    data, observed = canonical_text(raw, PROBE_SHA256, 4096)
    require(len(data) == 1670, "offline probe canonical size")
    return data, observed

def dependency_header(data: bytes, header: Path) -> bool:
    require(isinstance(data, bytes) and 0 < len(data) <= 1024 * 1024 and b"\0" not in data,
            "offline dependency data bound")
    text = data.decode("utf-8").replace("\\\r\n", "").replace("\\\n", "")
    decoded = []
    i = 0
    while i < len(text):
        char = text[i]
        if char == "$" and i + 1 < len(text) and text[i + 1] == "$":
            decoded.append("$")
            i += 2
        elif char == "\\" and i + 1 < len(text) and text[i + 1] in (" ", "\t", "#", ":", "\\"):
            decoded.append(text[i + 1])
            i += 2
        else:
            decoded.append(char)
            i += 1
    text = "".join(decoded).replace("\\", "/").casefold()
    wanted = header.as_posix().casefold()
    start = 0
    while (start := text.find(wanted, start)) >= 0:
        end = start + len(wanted)
        if (start == 0 or text[start - 1].isspace()) and (end == len(text) or text[end].isspace()):
            return True
        start += 1
    return False


def qualify(bounds, provider: Path, sdk: Path, root: Path, repo: Path, report: dict):
    from qualify_rocm_sdk import run, digest, native_git
    from qualify_hipcomp_candidate import inspect_offload_targets
    result = dict(experimental=True, complete=False, synthetic_target="gfx1100",
                  commands=[], runtime_source=RUNTIME_SOURCE, header_source_blob=HEADER_BLOB,
                  object_loaded=False, object_linked=False, gpu_executed=False,
                  scope="API/type availability and offline object metadata only; not memory-order or runtime proof")
    report["warp_sync_probe"] = result
    require(native_git(repo, "rev-parse", "HEAD:" + PROBE) == PROBE_BLOB,
            "offline probe canonical Git blob differs")
    source, source_observation = probe_source(repo)
    result["source"] = dict(path=PROBE, canonical_bytes=len(source), canonical_sha256=PROBE_SHA256,
                            **source_observation)
    header_raw = regular(sdk / HEADER, sdk, 128 * 1024)
    result["header"] = dict(path=HEADER, physical_bytes=len(header_raw),
                            physical_sha256=hashlib.sha256(header_raw).hexdigest(),
                            expected_canonical_sha256=HEADER_SHA256)
    header, header_observation = canonical_text(header_raw, HEADER_SHA256, 128 * 1024)
    result["header"] = dict(path=HEADER, canonical_bytes=len(header), canonical_sha256=HEADER_SHA256,
                            **header_observation)
    require(shutil.disk_usage(root).free >= 4 * 1024**3 + 20 * 1024**2,
            "offline probe disk reserve unavailable")
    staging = root / "warp-sync"
    require(not staging.exists(), "NEW offline probe fixture required")
    staging.mkdir()
    inputs = staging / "compile-input"
    entry = inputs / "src/gpu/warp_sync_probe.hip"
    entry.parent.mkdir(parents=True)
    with entry.open("xb") as stream:
        stream.write(source)
    obj = staging / "warp_sync.obj"
    deps = staging / "warp_sync.obj.d"
    run(bounds, ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                 str(provider / "tools/compile_hip_object.ps1"), "-Source", str(entry),
                 "-Output", str(obj), "-DependencyFile", str(deps), "-RepoRoot", str(inputs),
                 "-HipPath", str(sdk), "-Arch", "gfx1100"], result, "warp-sync-API-device-compilation", 900)
    for name, path, limit in (("object", obj, 16 * 1024 * 1024),
                              ("dependencies", deps, 1024 * 1024)):
        regular_info(path, staging, limit)
        result[name] = dict(bytes=path.stat().st_size, sha256=digest(path))
    result["selected_header_dependency"] = dependency_header(regular(deps, staging, 1024 * 1024), sdk / HEADER)
    require(result["selected_header_dependency"], "compiler did not report the selected SDK header")
    reader = sdk / "lib/llvm/bin/llvm-readobj.exe"
    regular_info(reader, sdk, 256 * 1024 * 1024)
    result["reader"] = dict(bytes=reader.stat().st_size, sha256=digest(reader))
    run(bounds, [str(reader), "--version"], result, "warp-sync-reader-version", 30)
    headers = run(bounds, [str(reader), "--file-header", str(obj)], result, "warp-sync-host-header", 30)
    require(not headers["output_truncated"] and not headers["stderr"].strip() and
            "COFF-x86-64" in headers["stdout"] and "IMAGE_FILE_MACHINE_AMD64" in headers["stdout"],
            "offline probe host header")
    metadata = run(bounds, [str(reader), "--offloading", str(obj)], result, "warp-sync-target-metadata", 30)
    require(not metadata["output_truncated"] and not metadata["stderr"].strip(),
            "offline probe metadata warning/truncation")
    result["targets"] = inspect_offload_targets(metadata["stdout"], str(obj), obj.stat().st_size)
    require(digest(obj) == result["object"]["sha256"] and
            digest(deps) == result["dependencies"]["sha256"], "offline reader changed probe output")
    require(probe_source(repo)[0] == source and
            canonical_text(regular(sdk / HEADER, sdk, 128 * 1024),
                           HEADER_SHA256, 128 * 1024)[0] == header, "offline inputs changed")
    require(shutil.disk_usage(root).free >= 4 * 1024**3, "offline probe disk reserve consumed")
    result["complete"] = True
