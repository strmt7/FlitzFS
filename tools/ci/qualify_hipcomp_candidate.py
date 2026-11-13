#!/usr/bin/env python3
"""Compile one audited hipCOMP translation unit on disposable Windows VMs only."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys

REPO=Path(__file__).resolve().parents[2]
SOURCE_REPOSITORY="ROCm/hipCOMP-core"
SOURCE_COMMIT="03df9f7369211ee15d370817165224c73fdc9a46"
SOURCE_DIGEST="8614af434e7bc2ad55587c4a4220e27df6db29962e794123bcfd63e62deb761d"
MECHANISM_COMMIT="b46acb6dd7e48e1fb2dc182210495abb6a3c013f"
ENTRY="src/lowlevel/LZ4CompressionKernels.cu"
ENTRY_DIGEST="85cbb1d61e29aafda8523073e5d03abd9c96a3d7f159520f8c79d14127d76944"
MIB=1024**2
MAX_BYTES=16*MIB
RESERVED={"CON","PRN","AUX","NUL",*(f"COM{i}" for i in range(1,10)),*(f"LPT{i}" for i in range(1,10))}

def require(value,message):
    if not value:raise RuntimeError(message)
def encoded(value):
    return (json.dumps(value,sort_keys=True,ensure_ascii=True,indent=2)+"\n").encode()
def sha(data):
    return hashlib.sha256(data).hexdigest()
def admit_path(name):
    require(isinstance(name,str) and 0<len(name)<=240 and
        re.fullmatch(r"[A-Za-z0-9_./+\-]+",name) is not None,"candidate source path")
    path=PurePosixPath(name)
    require(not path.is_absolute() and path.as_posix()==name and
        all(part not in ("",".","..") and not part.endswith(".") and
            part.split(".")[0].upper() not in RESERVED for part in path.parts),"candidate source path")
    return path
def validate_manifest(value):
    require(isinstance(value,dict) and set(value)=={"schema","repository","commit","source_sha256","files","notices"} and
        type(value["schema"]) is int and value["schema"]==1 and value["repository"]==SOURCE_REPOSITORY and
        value["commit"]==SOURCE_COMMIT and value["source_sha256"]==SOURCE_DIGEST,"candidate manifest identity")
    files=value["files"]
    require(isinstance(files,list) and 0<len(files)<=512,"candidate source count")
    total=0;names=set();previous="";mapping={};entry=None
    for record in files:
        require(isinstance(record,dict) and set(record)=={"path","bytes","sha256"},"candidate source fields")
        path=admit_path(record["path"]);name=path.as_posix()
        require(name>previous and name.casefold() not in names,"candidate source order/alias")
        require(type(record["bytes"]) is int and 0<record["bytes"]<=MIB and
            isinstance(record["sha256"],str) and re.fullmatch(r"[0-9a-f]{64}",record["sha256"]),"candidate source size/digest")
        previous=name;names.add(name.casefold());total+=record["bytes"]
        require(total<=MAX_BYTES,"candidate source aggregate")
        if name==ENTRY:entry=record
        if path.parts[0] in ("src","include"):
            target=("src/"+"/".join(path.parts[1:])) if path.parts[0]=="include" else name
            require(target.casefold() not in mapping,"candidate compiler-input alias")
            mapping[target.casefold()]=(name,target,record)
    require(sha(encoded(files))==SOURCE_DIGEST and entry is not None and
        entry["sha256"]==ENTRY_DIGEST,"candidate source inventory digest")
    notices=value["notices"]
    require(notices==[
        {"path":"LICENSE","bytes":1085,"sha256":"74e309b2672c84f53290161efaa80b59c068ddaca84842cb5e02f1d34eae29ef"},
        {"path":"CMakeLists.txt","bytes":14505,"sha256":"19c5e33cf8f3c4f3a01236d4fa1869d7f7497c84e07bb6b0ee98628692b550e7"}
    ],"candidate notices differ")
    require(len(mapping)>0,"candidate compiler inputs absent")
    return [row for _,row in sorted(mapping.items())],notices
def git(source,*arguments):
    env=os.environ.copy();env["GIT_NO_REPLACE_OBJECTS"]="1"
    result=subprocess.run(["git","-C",str(source),*arguments],capture_output=True,timeout=15,env=env,check=False)
    require(result.returncode==0 and len(result.stdout)<=MIB and len(result.stderr)<=16384,"candidate native Git command")
    return result.stdout
def verify_checkout(source,expected):
    require(not source.is_symlink() and not source.is_junction(),"candidate checkout link")
    source=source.resolve(strict=True)
    require(source!=REPO and source.is_relative_to(REPO),"candidate checkout boundary")
    require(Path(git(source,"rev-parse","--show-toplevel").decode().strip()).resolve(strict=True)==source and
        git(source,"rev-parse","HEAD").decode().strip()==expected,"candidate checkout identity")
    require(not git(source,"status","--porcelain","--untracked-files=all").strip(),"candidate checkout changed")
    return source
def read_blob(source,name,record):
    object_name=SOURCE_COMMIT+":"+name
    size=git(source,"cat-file","-s",object_name).strip()
    require(re.fullmatch(rb"[0-9]{1,9}",size) is not None and int(size)==record["bytes"] and
        0<int(size)<=MIB,"candidate source blob size")
    data=git(source,"cat-file","blob",object_name)
    require(len(data)==record["bytes"] and sha(data)==record["sha256"] and b"\0" not in data,
        "candidate source blob identity")
    return data
def write_new(path,data):
    path.parent.mkdir(parents=True,exist_ok=True)
    with path.open("xb") as file:file.write(data)
def stage(source,output,value):
    mapping,notices=validate_manifest(value)
    entry_record=next(record for original,_,record in mapping if original==ENTRY)
    total=sum(record["bytes"]*(2 if original!=target else 1) for original,target,record in mapping)
    total+=entry_record["bytes"]+sum(notice["bytes"] for notice in notices)
    require(total<=MAX_BYTES,"candidate compiler fixture aggregate")
    require(not output.exists(),"candidate compiler fixture already exists")
    output.mkdir()
    records=[]
    for original,target,record in mapping:
        data=read_blob(source,original,record)
        write_new(output/target,data)
        records.append(dict(source=original,target=target,bytes=len(data),sha256=sha(data)))
        if original!=target:
            write_new(output/original,data)
            records.append(dict(source=original,target=original,bytes=len(data),sha256=sha(data)))
    entry=next(row for row in records if row["source"]==ENTRY)
    compiled=output/"src/lowlevel/LZ4CompressionKernels.hip"
    require(not compiled.exists(),"candidate HIP entry alias")
    write_new(compiled,(output/entry["target"]).read_bytes())
    for notice in notices:write_new(output/"notices"/notice["path"],read_blob(source,notice["path"],notice))
    require(sum(row["bytes"] for row in records)+entry["bytes"]+sum(x["bytes"] for x in notices)<=MAX_BYTES,
        "candidate compiler fixture aggregate")
    return compiled,records
def main():
    require(os.name=="nt" and os.environ.get("GITHUB_ACTIONS")=="true" and
        os.environ.get("RUNNER_ENVIRONMENT")=="github-hosted","disposable hosted Windows VM required; never the owner PC")
    parser=argparse.ArgumentParser()
    for name in ("source","provider","sdk","output"):parser.add_argument("--"+name,type=Path,required=True)
    args=parser.parse_args()
    source=verify_checkout(args.source,SOURCE_COMMIT);provider=verify_checkout(args.provider,MECHANISM_COMMIT)
    temporary=Path(os.environ["RUNNER_TEMP"]).resolve(strict=True)
    require(not args.output.is_symlink() and not args.output.is_junction() and
        not args.sdk.is_symlink() and not args.sdk.is_junction(),"candidate SDK/output link")
    output=args.output.resolve();sdk=args.sdk.resolve(strict=True)
    require(output!=temporary and output.is_relative_to(temporary) and not output.exists() and
        sdk!=temporary and sdk.is_relative_to(temporary),"candidate SDK/output boundary")
    manifest=REPO/"tests/rocm_compile/hipcomp_lz4_source.json"
    require(manifest.stat().st_size<=256*1024,"candidate manifest byte cap")
    value=json.loads(manifest.read_text(encoding="utf-8"))
    from qualify_rocm_sdk import load_source,run,digest
    own_lock=REPO/"tools/rocm-sdk-lock.json"
    require(own_lock.read_bytes()==(provider/"tools/rocm-sdk-lock.json").read_bytes(),"candidate ROCm lock differs")
    sys.dont_write_bytecode=True;os.environ["PYTHONDONTWRITEBYTECODE"]="1"
    sys.path.insert(0,str(provider))
    bootstrap=load_source("flitzfs_candidate_bootstrap",provider/"tools/bootstrap_rocm_sdk.py")
    bounds=load_source("flitzfs_candidate_bounds",provider/"mcp/flitzzip_mcp.py")
    lock=bootstrap.read_lock(own_lock);identity=bootstrap.sdk_identity(sdk,lock["version"])
    report=dict(experimental=True,control_commit=os.environ["GITHUB_SHA"],source_repository=SOURCE_REPOSITORY,
        source_commit=SOURCE_COMMIT,source_inventory_sha256=SOURCE_DIGEST,distribution=lock["version"],
        synthetic_compile_target="gfx1100",commands=[],complete=False,gpu_executed=False,
        module_loaded=False,linked_into_product=False,upstream_build_scripts_executed=False)
    output.mkdir()
    try:
        source_copy=output/"compile-input"
        entry,records=stage(source,source_copy,value)
        report["source_inputs"]=records
        object_path=output/"hipcomp_lz4.obj";dependency=output/"hipcomp_lz4.obj.d"
        run(bounds,["powershell.exe","-NoProfile","-ExecutionPolicy","Bypass","-File",
            str(provider/"tools/compile_hip_object.ps1"),"-Source",str(entry),"-Output",str(object_path),
            "-DependencyFile",str(dependency),"-RepoRoot",str(source_copy),"-HipPath",str(sdk),"-Arch","gfx1100"],
            report,"hipcomp-LZ4-offline-translation-unit",900)
        require(object_path.is_file() and 0<object_path.stat().st_size<=16*MIB and
            dependency.is_file() and 0<dependency.stat().st_size<=MIB,"candidate compiler outputs")
        require(bootstrap.sdk_identity(sdk,lock["version"])==identity,"candidate SDK inputs changed")
        verify_checkout(source,SOURCE_COMMIT);verify_checkout(provider,MECHANISM_COMMIT)
        report["object"]=dict(bytes=object_path.stat().st_size,sha256=digest(object_path))
        report["dependencies"]=dict(bytes=dependency.stat().st_size,sha256=digest(dependency))
        report["complete"]=True
    except Exception as error:
        report["error"]=type(error).__name__+": "+str(error);raise
    finally:
        with (output/"qualification.json").open("x",encoding="utf-8") as file:
            json.dump(report,file,indent=2);file.write("\n")
    print("hipCOMP offline qualification: "+json.dumps(dict(complete=True,source=SOURCE_COMMIT,
        target="gfx1100",gpu_executed=False,module_loaded=False)))
if __name__=="__main__":main()
