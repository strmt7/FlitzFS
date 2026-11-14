#!/usr/bin/env python3
"""Reproduce unsupported LZ4 storage on an immutable pre-feature CPU core."""
from __future__ import annotations
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

BASELINE="5da33dcae667d23e68a54f70e109b1920a341518"
ROOT=Path(__file__).resolve().parents[2]

def run(argv,seconds):
    result=subprocess.run([str(x) for x in argv],check=False,capture_output=True,text=True,timeout=seconds)
    if len(result.stdout.encode())+len(result.stderr.encode())>1024*1024:
        raise RuntimeError("baseline command output budget")
    if result.returncode:
        raise RuntimeError("baseline build command failed: "+result.stderr[-4000:])
    return result

def main():
    if os.environ.get("GITHUB_ACTIONS")!="true" or os.environ.get("RUNNER_ENVIRONMENT")!="github-hosted":
        raise RuntimeError("disposable hosted runner required")
    source=ROOT/".ci-deps/lz4-store-baseline"
    if run(["git","-C",source,"rev-parse","HEAD"],15).stdout.strip()!=BASELINE:
        raise RuntimeError("immutable LZ4 baseline source mismatch")
    if run(["git","-C",source,"status","--porcelain","--untracked-files=all"],15).stdout.strip():
        raise RuntimeError("baseline source is not pristine")
    cache=ROOT/".ci-deps/linux-codecs/codec-cache.cmake"
    if not cache.is_file():raise RuntimeError("source-pinned native codecs required")
    oracle=ROOT/"tests/test_store_codec.cpp"
    if not oracle.is_file() or oracle.stat().st_size>65536:raise RuntimeError("bounded oracle source required")
    temporary=Path(os.environ["RUNNER_TEMP"]).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="flitzfs-lz4-store-baseline-",dir=temporary) as directory:
        fixture=Path(directory)
        # A private build wrapper consumes the baseline without editing it.
        (fixture/"CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.24)\nproject(Lz4StoreBaseline LANGUAGES C CXX)\n'
            'set(FLITZFS_BUILD_TESTS OFF CACHE BOOL "" FORCE)\n'
            f'add_subdirectory([=[{source.as_posix()}]=] baseline)\n'
            f'add_executable(lz4_store_baseline [=[{oracle.as_posix()}]=])\n'
            'target_compile_features(lz4_store_baseline PRIVATE cxx_std_20)\n'
            f'target_include_directories(lz4_store_baseline PRIVATE [=[{(source/"src").as_posix()}]=] '
            '"${FLITZFS_LZ4_INCLUDE}" "${FLITZFS_ZSTD_INCLUDE}")\n'
            'target_compile_definitions(lz4_store_baseline PRIVATE FLITZFS_TEST_HAS_LZ4=1 FLITZFS_TEST_HAS_ZSTD=1)\n'
            'target_link_libraries(lz4_store_baseline PRIVATE flitzfs_core)\n',encoding="utf-8")
        build=fixture/"build"
        run(["cmake","-C",cache,"-S",fixture,"-B",build,"-DFLITZFS_REQUIRE_LZ4=ON",
            "-DFLITZFS_REQUIRE_ZSTD=ON","-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_C_COMPILER=clang","-DCMAKE_CXX_COMPILER=clang++"],90)
        run(["cmake","--build",build,"--target","lz4_store_baseline","--parallel","2"],240)
        result=subprocess.run([str(build/"lz4_store_baseline")],capture_output=True,text=True,timeout=45)
        expected="store codec failure after 1: FlitzFS store codec: unknown codec or invalid stored/logical extent"
        if result.returncode!=1 or result.stdout.strip() or result.stderr.strip()!=expected:
            raise RuntimeError("baseline did not reproduce the exact missing-decoder behavior: "+result.stderr[-1000:])
        receipt=dict(experimental=True,baseline=BASELINE,head=os.environ["GITHUB_SHA"],
            oracle_sha256=hashlib.sha256(oracle.read_bytes()).hexdigest(),
            baseline_exit=result.returncode,baseline_stderr=result.stderr.strip(),
            independent_library_oracle_passed=True,missing_native_decoder_reproduced=True,gpu_executed=False)
        destination=temporary/("flitzfs-lz4-store-baseline-"+os.environ["GITHUB_SHA"]+".json")
        with destination.open("x",encoding="utf-8") as file:json.dump(receipt,file,indent=2);file.write("\n")
        print("LZ4 store baseline: "+json.dumps(receipt),flush=True)

if __name__=="__main__":main()
