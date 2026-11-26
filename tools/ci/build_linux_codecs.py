#!/usr/bin/env python3
"""Build exact Zstandard/LZ4 sources in bounded private Linux fixtures."""
import argparse
import json
import os
from pathlib import Path
import resource
import shutil
import subprocess
import sys

PINS={"zstd":"916e8e70b33317d469a41ee837e47317807815c8",
      "lz4":"aa4647fad575d01a874aaaadcdbe8e6a40e072f5"}
def run(args,seconds=60):
    return subprocess.run(args,check=True,timeout=seconds,capture_output=False)

def revision(source):
    return subprocess.run(["git","-C",str(source),"rev-parse","HEAD"],check=True,
        capture_output=True,text=True,timeout=5).stdout.strip()

def source_clean(source,pin):
    head=revision(source)
    if head!=pin:
        raise RuntimeError("native codec source identity differs")
    subprocess.run(["git","-C",str(source),"diff","--exit-code","HEAD","--"],
        check=True,timeout=5,capture_output=True)
    return head

def cap_resource(kind,budget):
    for limit in resource.getrlimit(kind):
        if limit!=resource.RLIM_INFINITY:
            budget=min(budget,limit)
    resource.setrlimit(kind,(budget,budget))

def main():
    if sys.platform!="linux":
        raise RuntimeError("native source codec build uses Linux-specific tooling")
    cap_resource(resource.RLIMIT_AS,4*1024**3)
    cap_resource(resource.RLIMIT_CPU,300)
    cap_resource(resource.RLIMIT_CORE,0)
    cap_resource(resource.RLIMIT_FSIZE,512*1024**2)
    parser=argparse.ArgumentParser();parser.add_argument("--sanitize",action="store_true");args=parser.parse_args()
    root=Path(__file__).resolve().parents[2];output=root/".ci-deps/linux-codecs"
    commit=revision(root)
    if shutil.disk_usage(root).free<3*1024**3:
        raise RuntimeError("native codec build requires 3 GiB free headroom")
    output.mkdir(exist_ok=False)
    build_type="Debug" if args.sanitize else "Release"
    flags=["-DCMAKE_BUILD_TYPE="+build_type]
    if args.sanitize:
        flags+=["-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g",
                "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g",
                "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined"]
    libraries={}
    for codec in ("zstd","lz4"):
        source=root/".ci-deps"/codec;source_clean(source,PINS[codec])
        build=output/(codec+"-build")
        options=(["-DZSTD_BUILD_PROGRAMS=OFF","-DZSTD_BUILD_TESTS=OFF","-DZSTD_BUILD_SHARED=OFF",
                  "-DZSTD_BUILD_STATIC=ON","-DZSTD_MULTITHREAD_SUPPORT=OFF",
                  "-DZSTD_LEGACY_SUPPORT=ON","-DZSTD_LEGACY_LEVEL=5"] if codec=="zstd" else
                 ["-DBUILD_SHARED_LIBS=OFF","-DBUILD_STATIC_LIBS=ON","-DLZ4_BUILD_CLI=OFF",
                  "-DLZ4_BUILD_LEGACY_LZ4C=OFF","-DLZ4_BUILD_MULTITHREAD=OFF",
                  "-DLZ4_STATIC_MULTITHREAD=OFF","-DLZ4_BUNDLED_MODE=ON"])
        target="libzstd_static" if codec=="zstd" else "lz4_static"
        library="libzstd.a" if codec=="zstd" else "liblz4.a"
        run(["cmake","-S",str(source/"build/cmake"),"-B",str(build),*flags,*options])
        run(["cmake","--build",str(build),"--target",target,"--parallel","2"],180)
        found=list(build.rglob(library))
        if len(found)!=1:
            raise RuntimeError("expected one source-pinned static codec library")
        libraries[codec]=str(found[0]);source_clean(source,PINS[codec])
    cache=output/"codec-cache.cmake"
    with cache.open("x",encoding="utf-8") as file:
        for codec,variable in (("zstd","ZSTD"),("lz4","LZ4")):
            for suffix,path,typ in (("INCLUDE",str(root/".ci-deps"/codec/"lib"),"PATH"),
                                    ("LIBRARY",libraries[codec],"FILEPATH")):
                if "]==]" in path or "\n" in path:
                    raise RuntimeError("invalid cache path")
                file.write("set(FLITZFS_"+variable+"_"+suffix+" [==["+path+"]==] CACHE "+typ+" \"\" FORCE)\n")
    if github_env:=os.environ.get("GITHUB_ENV"):
        with Path(github_env).open("a",encoding="utf-8") as file:
            file.write("FLITZFS_CODEC_CACHE="+str(cache)+"\n")
    report=dict(experimental=True,commit=commit,source_commits=PINS,
        libraries=libraries,sanitized=args.sanitize,cache=str(cache),parallel_workers=2)
    with (output/"build-receipt.json").open("x",encoding="utf-8") as file:
        json.dump(report,file,indent=2);file.write("\n")
    print(json.dumps(report))
if __name__=="__main__":
    main()
