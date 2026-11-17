"""Create and verify the exact tracked source ZIP for an immutable Git commit."""
from __future__ import annotations
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import stat
import subprocess
import sys
import zipfile

INVENTORY_CAP = 256 * 1024
READ_CHUNK = 256 * 1024
# Git's stored ZIP entries may include local and central time/ZIP64 extras
# and a ZIP64 data descriptor. Each path appears in both headers.
# https://github.com/git/git/blob/v2.56.0/archive-zip.c
ENTRY_OVERHEAD = 30 + 46 + 2 * 9 + 20 + 28 + 24
TRAILER_OVERHEAD = 22 + 56 + 20 + 64  # EOCD, ZIP64 trailer/locator, commit comment

def inventory(data: bytes):
    if not isinstance(data, bytes) or not data or len(data) > INVENTORY_CAP or not data.endswith(b"\0"):
        raise ValueError("Source inventory byte/termination cap")
    files = {}
    directories = set()
    total = 0
    for row in data[:-1].split(b"\0"):
        header, path_bytes = row.split(b"\t", 1)
        mode, kind, oid, raw_size = header.split()
        if mode not in (b"100644", b"100755") or kind != b"blob":
            raise ValueError("Source links/submodules are not admitted")
        if (len(oid) != 40 or any(c not in b"0123456789abcdef" for c in oid) or
                not raw_size.isdigit() or len(raw_size) > 20):
            raise ValueError("Source canonical metadata")
        path = path_bytes.decode("utf-8", "strict")
        parts = PurePosixPath(path)
        if (len(path_bytes) > 65535 or parts.is_absolute() or "\\" in path or ":" in path or
                any(p in ("", ".", "..") for p in path.split("/")) or
                any(ord(c) < 32 or ord(c) == 127 for c in path)):
            raise ValueError("Unsafe source path")
        if path in files:
            raise ValueError("Duplicate source path")
        size = int(raw_size)
        files[path] = dict(oid=oid.decode("ascii"), bytes=size)
        directories.update(str(parent) for parent in parts.parents if str(parent) != ".")
        total += size
    if files.keys() & directories:
        raise ValueError("Source path used as both file and directory")
    count = len(files) + len(directories)
    path_bytes = sum(len(name.encode("utf-8")) for name in files)
    path_bytes += sum(len(name.encode("utf-8")) + 1 for name in directories)
    archive_bound = total + 2 * path_bytes + count * ENTRY_OVERHEAD + TRAILER_OVERHEAD
    return dict(tracked_files=len(files), directory_entries=len(directories),
                zip_entry_bound=count, expanded_bytes=total, archive_byte_bound=archive_bound,
                inventory_sha256=hashlib.sha256(data).hexdigest(),
                files=files, directories=sorted(directories))

def verify_archive(info, archive: zipfile.ZipFile):
    entries = archive.infolist()
    if (len(entries) != info["zip_entry_bound"] or
            sum(entry.file_size for entry in entries) != info["expanded_bytes"]):
        raise ValueError("Source ZIP totals differ from Git")
    names = [entry.filename for entry in entries]
    expected = set(info["files"]) | {name + "/" for name in info["directories"]}
    if len(set(names)) != len(names) or set(names) != expected:
        raise ValueError("Source ZIP is not the complete tracked inventory")
    for entry in entries:
        if entry.is_dir():
            if entry.file_size:
                raise ValueError("Source ZIP directory contains bytes")
            continue
        record = info["files"][entry.filename]
        if entry.file_size != record["bytes"]:
            raise ValueError("Source ZIP file size differs from Git")
        remaining = record["bytes"]
        blob = hashlib.sha1(b"blob " + str(remaining).encode() + b"\0")
        with archive.open(entry) as stream:
            while remaining:
                chunk = stream.read(min(remaining, READ_CHUNK))
                if not chunk:
                    raise ValueError("Source ZIP decoded size differs")
                blob.update(chunk)
                remaining -= len(chunk)
            if stream.read(1):
                raise ValueError("Source ZIP decoded size differs")
        if blob.hexdigest() != record["oid"]:
            raise ValueError("Source ZIP file differs from its canonical Git blob")
    return dict(zip_entries=len(entries), expanded_bytes=info["expanded_bytes"],
                inventory_sha256=info["inventory_sha256"], canonical_files_verified=len(info["files"]))

def verify_archive_file(info, path: Path):
    physical = path.lstat()
    if (not stat.S_ISREG(physical.st_mode) or physical.st_nlink != 1 or
            physical.st_size > info["archive_byte_bound"]):
        raise ValueError("Source ZIP physical size differs from the Git-derived bound")
    with zipfile.ZipFile(path) as archive:
        return {**verify_archive(info, archive), "archive_bytes": physical.st_size}


def create_archive(info, commit: str, path: Path):
    if info["archive_byte_bound"] > shutil.disk_usage(path.parent).free // 2:
        raise ValueError("Insufficient space for the source ZIP and its reserve")
    with path.open("xb") as output:
        # Archive canonical blob bytes even on Windows checkouts using CRLF.
        subprocess.run(["git", "-c", "core.autocrlf=false", "-c", "core.eol=lf",
                        "archive", "-0", "--format=zip", commit],
                       stdout=output, check=True, timeout=90)
    return verify_archive_file(info, path)


def main():
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], timeout=10, text=True).strip()
    result = subprocess.run(["git", "ls-tree", "-r", "-l", "-z", commit],
                            capture_output=True, check=True, timeout=10)
    info = inventory(result.stdout)
    if len(sys.argv) == 1:
        print(json.dumps({key: value for key, value in info.items()
                          if key not in ("files", "directories")}))
    elif len(sys.argv) == 3 and sys.argv[1] == "--archive":
        print(json.dumps(verify_archive_file(info, Path(sys.argv[2]))))
    elif len(sys.argv) == 3 and sys.argv[1] == "--create-archive":
        print(json.dumps(create_archive(info, commit, Path(sys.argv[2]))))
    else:
        raise ValueError("Use source_inventory.py [--archive or --create-archive exact-source.zip]")

if __name__ == "__main__":
    main()
