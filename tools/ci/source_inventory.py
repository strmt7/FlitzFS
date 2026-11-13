"""Bound source files AND directory entries before creating the source ZIP."""
from __future__ import annotations
import hashlib
import json
from pathlib import PurePosixPath
import subprocess
import sys
import zipfile

INVENTORY_CAP = 256 * 1024
ENTRY_CAP = 768
FILE_CAP = 2 * 1024 * 1024
EXPANDED_CAP = 8 * 1024 * 1024

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
                not raw_size.isdigit() or len(raw_size) > 7):
            raise ValueError("Source canonical metadata")
        path = path_bytes.decode("utf-8", "strict")
        parts = PurePosixPath(path)
        if (parts.is_absolute() or "\\" in path or ":" in path or
                any(p in ("", ".", "..") for p in path.split("/")) or
                any(ord(c) < 32 or ord(c) == 127 for c in path)):
            raise ValueError("Unsafe source path")
        if path in files:
            raise ValueError("Duplicate source path")
        size = int(raw_size)
        files[path] = dict(oid=oid.decode("ascii"), bytes=size)
        directories.update(str(parent) for parent in parts.parents if str(parent) != ".")
        total += size
        if (size > FILE_CAP or total > EXPANDED_CAP or
                len(files) + len(directories) > ENTRY_CAP or files.keys() & directories):
            raise ValueError("Source file/entry/expanded cap")
    return dict(tracked_files=len(files), directory_entries=len(directories),
                zip_entry_bound=len(files) + len(directories), expanded_bytes=total,
                compressed_cap=4 * 1024 * 1024,
                inventory_sha256=hashlib.sha256(data).hexdigest(),
                files=files, directories=sorted(directories))

def verify_archive(data: bytes, archive: zipfile.ZipFile):
    info = inventory(data)
    entries = archive.infolist()
    if (len(entries) > ENTRY_CAP or
            sum(entry.file_size for entry in entries) > EXPANDED_CAP or
            any(entry.file_size > FILE_CAP for entry in entries)):
        raise ValueError("Source ZIP entry/expanded cap")
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
        with archive.open(entry) as stream:
            body = stream.read(FILE_CAP + 1)
        if len(body) != record["bytes"]:
            raise ValueError("Source ZIP decoded size differs")
        blob = hashlib.sha1(b"blob " + str(len(body)).encode() + b"\0" + body).hexdigest()
        if blob != record["oid"]:
            raise ValueError("Source ZIP file differs from its canonical Git blob")
    return dict(zip_entries=len(entries), expanded_bytes=info["expanded_bytes"],
                inventory_sha256=info["inventory_sha256"], canonical_files_verified=len(info["files"]))

def main():
    result = subprocess.run(["git", "ls-tree", "-r", "-l", "-z", "HEAD"],
                            capture_output=True, check=True, timeout=10)
    if len(sys.argv) == 1:
        info = inventory(result.stdout)
        print(json.dumps({key: value for key, value in info.items()
                          if key not in ("files", "directories")}))
    elif len(sys.argv) == 3 and sys.argv[1] == "--archive":
        from pathlib import Path
        import stat
        path = Path(sys.argv[2])
        physical = path.lstat()
        if not stat.S_ISREG(physical.st_mode) or physical.st_nlink != 1 or physical.st_size > 4 * 1024 * 1024:
            raise ValueError("Source ZIP physical/compressed cap")
        with zipfile.ZipFile(path) as archive:
            print(json.dumps(verify_archive(result.stdout, archive)))
    else:
        raise ValueError("Use source_inventory.py [--archive exact-source.zip]")

if __name__ == "__main__":
    main()
