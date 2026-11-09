#!/usr/bin/env python3
"""Build only the bounded store interruption probe on a disposable hosted VM."""
import os
from pathlib import Path
import subprocess

def run(args,seconds=60):
    subprocess.run(args,check=True,timeout=seconds)

def main():
    if os.environ.get("GITHUB_ACTIONS")!="true" or os.environ.get("RUNNER_ENVIRONMENT")!="github-hosted":
        raise RuntimeError("disposable hosted VM required")
    root=Path(__file__).resolve().parents[2]
    source=root/".ci-deps/zstd"
    dependency=root/".ci-deps/crash-zstd-build"
    build=root/"build-store-crash"
    arch=["-A","x64"] if os.name=="nt" else []
    run(["cmake","-S",str(source/"build/cmake"),"-B",str(dependency),*arch,
         "-DCMAKE_BUILD_TYPE=Release","-DZSTD_BUILD_PROGRAMS=OFF","-DZSTD_BUILD_TESTS=OFF",
         "-DZSTD_BUILD_SHARED=OFF","-DZSTD_BUILD_STATIC=ON","-DZSTD_MULTITHREAD_SUPPORT=OFF"])
    run(["cmake","--build",str(dependency),"--config","Release","--target","libzstd_static","--parallel","2"],120)
    name="zstd_static.lib" if os.name=="nt" else "libzstd.a"
    libraries=list(dependency.rglob(name))
    if len(libraries)!=1:
        raise RuntimeError("expected one pinned CPU compression library")
    run(["cmake","-S",str(root),"-B",str(build),*arch,"-DCMAKE_BUILD_TYPE=Release",
         "-DFLITZFS_BUILD_TESTS=OFF","-DFLITZFS_BUILD_STORE_CRASH_PROBE=ON",
         "-DFLITZFS_WITH_LZ4=OFF","-DFLITZFS_REQUIRE_ZSTD=ON",
         "-DFLITZFS_ZSTD_INCLUDE="+str(source/"lib"),
         "-DFLITZFS_ZSTD_LIBRARY="+str(libraries[0])])
    run(["cmake","--build",str(build),"--config","Release","--target","flitzfs_store_crash_probe","--parallel","2"],120)

if __name__=="__main__":
    main()
