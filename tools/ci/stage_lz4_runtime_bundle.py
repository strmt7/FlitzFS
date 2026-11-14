#!/usr/bin/env python3
"""Stage the qualified private compression module on a disposable Windows VM."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat

from check_portable_cpu import pe_import_names, require
from hip_runtime_exports import pe_function_exports

MAX_MODULE=32*1024*1024
SHA=re.compile(r"[a-f0-9]{64}\Z")
COMMIT=re.compile(r"[a-f0-9]{40}\Z")
DLL=re.compile(r"amdhip64[a-z0-9_]*\.dll\Z")

def ordinary(path: Path,limit: int) -> bytes:
    info=path.lstat()
    require(stat.S_ISREG(info.st_mode) and info.st_nlink==1 and
        not path.is_symlink() and not (getattr(info,"st_file_attributes",0)&0x400) and
        0<info.st_size<=limit,"private compression input type/size")
    data=path.read_bytes()
    require(len(data)==info.st_size,"private compression input changed")
    return data

def coverage_record(proof: dict,obj: bytes,commit: str,*,uniform: bool=False) -> dict:
    require(COMMIT.fullmatch(commit) is not None and
        proof.get("control_commit")==commit and proof.get("complete") is True and
        proof.get("object_inspected") is True and proof.get("physical_inputs_unchanged") is True and
        proof.get("variant")=="bounded-native-wave-entry" and
        proof.get("gpu_executed") is False and proof.get("module_loaded") is False,
        "qualified source-bound compression object required")
    record=proof.get("object",{})
    require(record.get("bytes")==len(obj) and
        record.get("sha256")==hashlib.sha256(obj).hexdigest(),"compression object identity")
    targets=proof.get("compile_targets")
    images=proof.get("entry_objects")
    require(proof.get("runtime_target_fixture") is True and isinstance(targets,list) and
        1<=len(targets)<=16 and all(isinstance(t,str) and re.fullmatch(r"gfx[0-9a-z]+",t) for t in targets) and
        len(set(targets))==len(targets) and isinstance(images,dict) and set(images)==set(targets),
        "compression target coverage not qualified")
    expected={"hip-amdgcn-amd-amdhsa--"+t for t in targets}
    require(set(proof.get("offload_target_ids",[]))==expected and
        len(proof["offload_target_ids"])==len(expected),"compression target image inventory")
    for target in targets:
        image=images[target]
        require(isinstance(image,dict) and image.get("complete") is True and
            image.get("architecture")==target and image.get("loaded") is False and
            image.get("executed") is False and
            image.get("symbols")==["flitzfs_candidate_lz4_wave32","flitzfs_candidate_lz4_wave64"],
            "compression native target body unqualified")
        if uniform:
            require(image.get("uniform_symbols")==["flitzfs_uniform_lz4_wave32","flitzfs_uniform_lz4_wave64"],
                "uniform compression target body unqualified")
    # The source closes the unsupported wave64 entry to refusal on this RDNA
    # fixture. Symbol presence alone must never create wave64 launch authority.
    return dict(targets=[dict(architecture=t,wave_mask=1) for t in targets],
        object_sha256=record["sha256"])

def stage(module: Path,obj: Path,proof_path: Path,output: Path,header: Path,
          receipt: Path,runtime: str,commit: str,*,uniform: bool=False) -> dict:
    require(DLL.fullmatch(runtime) is not None,"SDK-bound runtime basename")
    proof_bytes=ordinary(proof_path,1024*1024)
    proof=json.loads(proof_bytes.decode("utf-8"))
    require(isinstance(proof,dict),"compression coverage receipt type")
    coverage=coverage_record(proof,ordinary(obj,16*1024*1024),commit,uniform=uniform)
    data=ordinary(module,MAX_MODULE)
    imports,delay=pe_import_names(data)
    require(bool(imports) and not delay and set(imports)<={runtime,"kernel32.dll"},
        "private compression module import closure")
    getter="flitzfs_uniform_lz4_kernel_api" if uniform else "flitzfs_candidate_lz4_api"
    require(pe_function_exports(data)=={getter},
        "private compression exact direct getter")
    for path in (output,header,receipt):
        require(not path.exists() and not path.is_symlink(),"NEW compression staging outputs required")
    name="flitzfs_rocm_uniform_lz4_candidate.dll" if uniform else "flitzfs_rocm_lz4_candidate.dll"
    scope="hip_uniform_lz4_bundle" if uniform else "hip_lz4_bundle"
    digest=hashlib.sha256(data).hexdigest()
    result=dict(experimental=True,control_commit=commit,coverage=coverage,
        coverage_receipt_sha256=hashlib.sha256(proof_bytes).hexdigest(),
        files=[dict(name=name,bytes=len(data),sha256=digest)],runtime_basename=runtime,
        imports=sorted(set(imports)),runtime_shipped=False,module_loaded=False,gpu_executed=False)
    if uniform:
        result.update(layout_version=2,layout_bytes=80,getter=getter)
    output.mkdir()
    with (output/name).open("xb") as out:
        out.write(data)
    require(ordinary(output/name,MAX_MODULE)==data,"compression staging copy identity")
    text=(
        f'#pragma once\nnamespace flitzfs::{scope} {{\n'
        'struct File {const wchar_t* name; unsigned long long size; const char* sha256;};\n'
        f'inline constexpr wchar_t kernel[]=L"{name}";\n'
        'inline constexpr hip_detail::Lz4KernelCoverage targets[]={\n'+
        ''.join(f'  {{"{t["architecture"]}",{t["wave_mask"]}}},\n' for t in coverage["targets"])+
        '};\n'
        f'inline constexpr File files[]={{ {{kernel,{len(data)}ULL,"{digest}"}} }};\n'
        '}\n')
    with header.open("x",encoding="ascii",newline="\n") as out:
        out.write(text)
    with receipt.open("x",encoding="utf-8",newline="\n") as out:
        json.dump(result,out,indent=2);out.write("\n")
    return result

def main():
    require(os.name=="nt" and os.environ.get("GITHUB_ACTIONS")=="true" and
        os.environ.get("RUNNER_ENVIRONMENT")=="github-hosted",
        "disposable hosted Windows build required; never the owner PC")
    parser=argparse.ArgumentParser()
    for name in ("module","object","coverage","output","header","receipt"):
        parser.add_argument("--"+name,type=Path,required=True)
    parser.add_argument("--runtime",required=True)
    parser.add_argument("--uniform",action="store_true")
    args=parser.parse_args()
    root=Path(os.environ["RUNNER_TEMP"]).resolve(strict=True)
    for path in (args.module,args.object,args.coverage):
        require(path.resolve(strict=True).is_relative_to(root),"private compression input boundary")
    for path in (args.output,args.header,args.receipt):
        require(path.parent.resolve(strict=True)==path.parent and path.is_relative_to(root) and
            path!=root,"private compression output boundary")
    result=stage(args.module,args.object,args.coverage,args.output,args.header,args.receipt,
        args.runtime,os.environ["GITHUB_SHA"],uniform=args.uniform)
    print("Private LZ4 runtime bundle: "+json.dumps(result),flush=True)

if __name__=="__main__":
    main()

