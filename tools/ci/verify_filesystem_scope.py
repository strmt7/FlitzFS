#!/usr/bin/env python3
"""Guard filesystem product paths against accidentally restoring archive support."""
from pathlib import Path
import subprocess
import sys
root=Path(__file__).resolve().parents[2]
for directory in ("src","include"):
    for path in (root/directory).rglob("*"):
        if path.suffix not in (".cpp",".hpp",".h",".hip"):
            continue
        data=path.read_text(encoding="utf-8")
        for token in ("FlzipReader","open_flzip","flzip_reader.hpp","flzip-list",
                      "flzip-verify","flzip-probe","SUZP","SUZF"):
            if token in data:
                raise RuntimeError("Archive product route present: "+str(path.relative_to(root)))
cmake=(root/"CMakeLists.txt").read_text(encoding="utf-8")
for token in ("flzip_reader.cpp","FLITZFS_WITH_ZLIB","FLITZFS_REQUIRE_ZLIB",
              "archive_index.cpp","test_flzip_cli"):
    if token in cmake:
        raise RuntimeError("Archive dependency restored in filesystem build: "+token)
if len(sys.argv)==2:
    binary=sys.argv[1]
    result=subprocess.run([binary,"--help"],capture_output=True,text=True,timeout=5)
    if result.returncode!=0 or "flzip" in result.stdout.lower():
        raise RuntimeError("Filesystem help advertises archive support")
    for command in ("flzip-list","flzip-verify","flzip-probe"):
        result=subprocess.run([binary,command,"nonexistent-input.flzip"],
                              capture_output=True,text=True,timeout=5)
        if result.returncode!=2 or result.stderr:
            raise RuntimeError("Removed archive command was handled: "+command)
print("PASS: filesystem product has no archive reader, CLI route or archive serialization dependency")
