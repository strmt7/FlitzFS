#!/usr/bin/env python3
"""Use pinned current FlitzZip SDK/bootstrap/compile mechanisms on disposable VMs."""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys

SOURCE_REF="b46acb6dd7e48e1fb2dc182210495abb6a3c013f"
REPO=Path(__file__).resolve().parents[2]
GIB=1024**3

def require(ok,why):
    if not ok:
        raise RuntimeError(why)

def digest(path):
    with path.open("rb") as file:
        return hashlib.file_digest(file,"sha256").hexdigest()

def native_git(provider,*args):
    result=subprocess.run(["git","-C",str(provider),*args],check=True,capture_output=True,text=True,timeout=15)
    require(len(result.stdout)<=1024*1024,"source identity output budget")
    return result.stdout.strip()

def load_source(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    require(spec is not None and spec.loader is not None,"reference source loader unavailable")
    module=importlib.util.module_from_spec(spec)
    sys.modules[name]=module
    spec.loader.exec_module(module)
    return module

def run(bounds,command,report,name,seconds=900):
    result=bounds.run_bounded_command(command,timeout_seconds=seconds,
        memory_limit_bytes=2*GIB,response_tail_bytes=12000,response_max_characters=16000)
    report["commands"].append(dict(name=name,argv=command,**result))
    print("ROCm command: "+json.dumps(dict(name=name,**result)),flush=True)
    require(result["exit_code"]==0 and not result["timed_out"] and not result["output_limit_exceeded"],
        "native ROCm qualification command failed: "+name)
    return result

def main():
    require(os.name=="nt" and os.environ.get("GITHUB_ACTIONS")=="true" and
        os.environ.get("RUNNER_ENVIRONMENT")=="github-hosted",
        "disposable native Windows VM required; never the owner PC")
    parser=argparse.ArgumentParser()
    parser.add_argument("--provider",type=Path,required=True)
    parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args()
    temporary=Path(os.environ["RUNNER_TEMP"]).resolve()
    root=args.output.resolve();provider=args.provider.resolve(strict=True)
    require(root!=temporary and root.is_relative_to(temporary) and not root.exists(),
        "qualification requires a NEW owned RUNNER_TEMP fixture")
    require(provider.is_relative_to(REPO) and provider!=REPO and
        not provider.is_symlink() and not provider.is_junction(),"isolated reference checkout required")
    require(native_git(provider,"rev-parse","HEAD")==SOURCE_REF,"FlitzZip mechanism source identity changed")
    require(not native_git(provider,"status","--porcelain","--untracked-files=all"),
        "reference checkout must be unmodified before import")
    own_lock=REPO/"tools/rocm-sdk-lock.json"
    require(own_lock.read_bytes()==(provider/"tools/rocm-sdk-lock.json").read_bytes(),
        "ROCm lock must match the reviewed complete-distribution source")
    root.mkdir()
    report=dict(commit=os.environ["GITHUB_SHA"],provider_repository="strmt7/FlitzZip",
        provider_ref=SOURCE_REF,lock_sha256=digest(own_lock),commands=[],complete=False,
        scope="Complete SDK build, kernel link, worker bundle authentication and CPU-only refusal tests")
    sys.dont_write_bytecode=True
    os.environ["PYTHONDONTWRITEBYTECODE"]="1"
    sys.path.insert(0,str(provider))
    try:
        bootstrap=load_source("flitzfs_reference_rocm",provider/"tools/bootstrap_rocm_sdk.py")
        bounds=load_source("flitzfs_reference_bounds",provider/"mcp/flitzzip_mcp.py")
        lock=bootstrap.read_lock(own_lock)
        report["distribution"]=lock
        needed=lock["archive_bytes"]+lock["unpacked_bytes"]+4*GIB
        free=shutil.disk_usage(root).free
        report["disk_admission"]=dict(available=free,required=needed,reserve=4*GIB)
        require(free>=needed,"insufficient space for the COMPLETE ROCm distribution; no runner cleanup or partial SDK")
        run(bounds,[sys.executable,"-B","-m","unittest","tools.test_bootstrap_rocm_sdk"],
            report,"current-reference-bootstrap-regressions",120)
        sdk_parent=root/"sdk-cache"
        run(bounds,[sys.executable,"-B",str(provider/"tools/bootstrap_rocm_sdk.py"),
            "--parent",str(sdk_parent)],report,"complete-sdk-bootstrap",3600)
        sdk=sdk_parent/"sdk"
        identity=bootstrap.sdk_identity(sdk,lock["version"])
        report["critical_inputs"]=identity
        report["bootstrap_receipt"]=json.loads((sdk_parent/"verified-distribution.json").read_text(encoding="utf-8"))

        # Reuse the unchanged compiler helper. Its explicit include root is a
        # fresh byte-identical source fixture, never a patched upstream checkout.
        inputs=root/"compile-input"
        copies=(("src/gpu/decode_kernel.hip","src/gpu/decode_kernel.hip"),
                ("src/gpu/decode_kernel.hpp","src/gpu/decode_kernel.hpp"),
                ("include/flitzfs/gpu_protocol.hpp","src/flitzfs/gpu_protocol.hpp"),
                ("include/flitzfs/gpu_kernel_api.hpp","src/flitzfs/gpu_kernel_api.hpp"))
        report["kernel_inputs"]=[]
        for original,relative in copies:
            source=REPO/original;target=inputs/relative;target.parent.mkdir(parents=True,exist_ok=True)
            data=source.read_bytes()
            require(0<len(data)<=1024*1024,"kernel source input budget")
            with target.open("xb") as out:
                out.write(data)
            report["kernel_inputs"].append(dict(path=original,sha256=digest(source),bytes=len(data)))
        obj=root/"flitzfs_rocm_decode.obj"
        run(bounds,["powershell.exe","-NoProfile","-ExecutionPolicy","Bypass","-File",
            str(provider/"tools/compile_hip_object.ps1"),"-Source",str(inputs/"src/gpu/decode_kernel.hip"),
            "-Output",str(obj),"-DependencyFile",str(obj)+".d","-RepoRoot",str(inputs),
            "-HipPath",str(sdk),"-Arch","release"],report,"current-sdk-device-compilation",900)
        require(obj.is_file() and 0<obj.stat().st_size<=16*1024*1024,"HIP object output budget")
        report["kernel_object"]=dict(bytes=obj.stat().st_size,sha256=digest(obj))
        build=root/"build"
        cmake=shutil.which("cmake")
        require(bool(cmake),"verified job-local CMake must be prepared first")
        run(bounds,[cmake,"-S",str(REPO/"tests/rocm_compile"),"-B",str(build),"-A","x64",
            "-DFLITZFS_ROCM_SDK="+str(sdk),"-DFLITZFS_ROCM_KERNEL_OBJECT="+str(obj)],
            report,"current-sdk-host-api-and-link-configure")
        run(bounds,[cmake,"--build",str(build),"--config","Release","--parallel","2"],
            report,"current-sdk-host-api-and-kernel-module-link")
        from_path=REPO/"tools/ci"
        sys.path.insert(0,str(from_path))
        from check_portable_cpu import audit_binary, pe_import_names
        probe=build/"Release/flitzfs_rocm_header_probe.exe"
        report["probe_imports"]=audit_binary(probe.read_bytes())
        observed=run(bounds,[str(probe)],report,"header-probe-without-loading-HIP",30)
        actual_hip=re.search(r" HIP=([^\s]+)",observed["stdout"])
        require(actual_hip is not None and actual_hip.group(1)==lock["observed_hip_version"],
            "observed complete-SDK HIP identity differs from its lock record")
        module=build/"Release/flitzfs_rocm_kernels.dll"
        require(module.is_file() and module.stat().st_size<=32*1024*1024,"kernel module output budget")
        report["kernel_module"]=dict(bytes=module.stat().st_size,sha256=digest(module))
        worker=build/"Release/flitzfs_hip_worker.exe"
        worker_probe=build/"Release/flitzfs_hip_worker_probe.exe"
        require(worker.is_file() and 0 < worker.stat().st_size <= 32*1024*1024, "worker executable budget")
        imports, delay=pe_import_names(worker.read_bytes())
        allowed={"kernel32.dll","advapi32.dll","bcrypt.dll","ntdll.dll","dxgi.dll"}
        require(bool(imports) and set(imports) <= allowed and not delay, "worker has eager SDK dependencies")
        report["worker"]=dict(bytes=worker.stat().st_size,sha256=digest(worker),imports=sorted(set(imports)),
            bundle=json.loads((build/"Release/hip-bundle.json").read_text(encoding="utf-8")))
        report["worker_probe_imports"]=audit_binary(worker_probe.read_bytes())
        # Same-size change in a NEW copy. Every original SDK/bundle file stays untouched.
        altered=root/"tampered-worker"
        altered.mkdir()
        (altered/"hip").mkdir()
        with worker.open("rb") as incoming, (altered/worker.name).open("xb") as outgoing:
            shutil.copyfileobj(incoming,outgoing,64*1024)
        for record in report["worker"]["bundle"]["files"]:
            filename=record["name"]
            require(Path(filename).name==filename and filename.endswith(".dll"),"unsafe staged DLL name")
            with (build/"Release/hip"/filename).open("rb") as incoming, (altered/"hip"/filename).open("xb") as outgoing:
                if filename==module.name.lower():
                    byte=incoming.read(1)
                    require(len(byte)==1,"empty staged kernel")
                    outgoing.write(bytes([byte[0]^1]))
                shutil.copyfileobj(incoming,outgoing,64*1024)
        result=run(bounds,[str(worker_probe),str(worker),str(root/"worker-probe"),str(altered/worker.name)],
            report,"bounded-worker-admission-without-HIP",90)
        prefix="HIP worker admission: "
        receipts=[json.loads(line[len(prefix):]) for line in result["stdout"].splitlines() if line.startswith(prefix)]
        require(receipts==[dict(bundle_verified=True,missing_bundle_refused=True,tampered_bundle_refused=True,invalid_wire_refused=True,
            exits_confirmed=4,gpu_executed=False,complete=True)],"worker admission evidence mismatch")
        report["worker_admission"]=receipts[0]
        actual_clang=run(bounds,[str(sdk/"lib/llvm/bin/clang.exe"),"--version"],report,"actual-AMD-compiler-version",30)
        require(lock["observed_clang_version"] in actual_clang["stdout"],
            "observed complete-SDK compiler identity differs from its lock record")
        require(not native_git(provider,"status","--porcelain","--untracked-files=all"),
            "reference source changed during qualification")
        require(bootstrap.sdk_identity(sdk,lock["version"])==identity,
            "SDK critical inputs changed during compilation")
        report["complete"]=True
    except Exception as error:
        report["error"]=type(error).__name__+": "+str(error)
        raise
    finally:
        with (root/"qualification.json").open("x",encoding="utf-8") as out:
            json.dump(report,out,indent=2);out.write("\n")
    print("ROCm qualification: "+json.dumps(dict(commit=report["commit"],complete=report["complete"],
        distribution=report["distribution"]["version"],gpu_executed=False)))
if __name__=="__main__":
    main()
