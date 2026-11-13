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
import stat
import time
import sys
import zipfile

SOURCE_REF="b46acb6dd7e48e1fb2dc182210495abb6a3c013f"
REPO=Path(__file__).resolve().parents[2]
GIB=1024**3
HIP_RUNTIME_UNAVAILABLE=0x46530002
HIP_INTEGRITY_FAILURE=0x46530006

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

def run(bounds,command,report,name,seconds=900,exit_code=0):
    result=bounds.run_bounded_command(command,timeout_seconds=seconds,
        memory_limit_bytes=2*GIB,response_tail_bytes=12000,response_max_characters=16000)
    report["commands"].append(dict(name=name,argv=command,**result))
    print("ROCm command: "+json.dumps(dict(name=name,**result)),flush=True)
    require(result["exit_code"]==exit_code and not result["timed_out"] and not result["output_limit_exceeded"],
        "native ROCm qualification command failed: "+name)
    return result


def inspect_private_runtime(sdk,build,bundle):
    """Read PE bytes and license metadata only; never load the AMD runtime."""
    records=bundle["files"]
    require(isinstance(records,list) and 1<=len(records)<=32,"private runtime inventory bound")
    files=[];total=0
    for record in records:
        name=record["name"]
        require(isinstance(name,str) and re.fullmatch(r"[a-z0-9_-][a-z0-9_.-]{0,122}\.dll",name),
            "private runtime inventory name")
        path=build/"Release/hip"/name
        info=path.lstat()
        require(stat.S_ISREG(info.st_mode) and info.st_nlink==1 and
            not (getattr(info,"st_file_attributes",0)&0x400) and
            info.st_size==record["bytes"] and 0<info.st_size<=32*1024*1024,
            "private runtime inventory file type/size")
        total+=info.st_size
        require(total<=64*1024*1024,"private runtime inventory total bytes")
        data=path.read_bytes()
        require(len(data)==info.st_size and hashlib.sha256(data).hexdigest()==record["sha256"],
            "private runtime inventory identity")
        from check_portable_cpu import pe_import_names
        imports,delayed=pe_import_names(data,allow_delay=True)
        files.append(dict(**record,imports=sorted(set(imports)),delay_imports=sorted(set(delayed))))
    license_name=re.compile(r"(?:licen[cs]e|notice|copying|copyright|eula|redistribut)[a-z0-9_.-]*",re.I)
    started=time.monotonic();seen=0;licenses=[];license_bytes=0
    for directory,children,names in os.walk(sdk,followlinks=False):
        seen+=1+len(children)+len(names)
        require(seen<=50000 and time.monotonic()-started<=20,"SDK license metadata traversal budget")
        for child in children:
            path=Path(directory)/child
            require(not path.is_symlink() and not path.is_junction(),"SDK license traversal reparse directory")
        for name in names:
            if not license_name.fullmatch(name):
                continue
            path=Path(directory)/name
            info=path.lstat()
            require(stat.S_ISREG(info.st_mode) and info.st_nlink==1 and
                not (getattr(info,"st_file_attributes",0)&0x400) and 0<info.st_size<=1024*1024,
                "SDK license metadata file budget/type")
            license_bytes+=info.st_size
            require(len(licenses)<256 and license_bytes<=8*1024*1024,"SDK license metadata aggregate budget")
            data=path.read_bytes()
            require(len(data)==info.st_size,"SDK license metadata changed")
            # Identity and a bounded preview aid review; no redistribution conclusion is inferred.
            licenses.append(dict(path=path.relative_to(sdk).as_posix(),bytes=info.st_size,
                sha256=hashlib.sha256(data).hexdigest(),
                preview=data[:2048].decode("utf-8",errors="replace")))
    require(bool(licenses),"complete SDK has no discovered license metadata")
    return dict(files=files,bytes=total,licenses=sorted(licenses,key=lambda item:item["path"]),
        license_bytes=license_bytes,entries_observed=seen,loaded=False,gpu_executed=False,
        amd_binaries_exported=False,redistribution_qualified=False)

def main():
    require(os.name=="nt" and os.environ.get("GITHUB_ACTIONS")=="true" and
        os.environ.get("RUNNER_ENVIRONMENT")=="github-hosted",
        "disposable native Windows VM required; never the owner PC")
    parser=argparse.ArgumentParser()
    parser.add_argument("--provider",type=Path,required=True)
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--hipcomp-source",type=Path,help="Optional reviewed codec checkout; offline object only")
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
    hipcomp_source=None
    if args.hipcomp_source is not None:
        from qualify_hipcomp_candidate import verify_checkout,SOURCE_COMMIT as CODEC_SOURCE_COMMIT
        hipcomp_source=verify_checkout(args.hipcomp_source,CODEC_SOURCE_COMMIT)
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
        run(bounds,[sys.executable,"-B",str(REPO/"tools/ci/test_hip_runtime_exports.py")],
            report,"static-runtime-export-parser-regressions",30)
        run(bounds,[sys.executable,"-B",str(REPO/"tools/ci/test_hipcomp_candidate.py")],
            report,"codec-source-admission-regressions",60)
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
        if hipcomp_source is not None:
            candidate=root/"hipcomp-offline"
            run(bounds,[sys.executable,"-B",str(REPO/"tools/ci/qualify_hipcomp_candidate.py"),
                "--source",str(hipcomp_source),"--provider",str(provider),"--sdk",str(sdk),
                "--output",str(candidate)],report,"reviewed-hipcomp-offline-qualification",1200)
            report["hipcomp_offline"]=json.loads((candidate/"qualification.json").read_text(encoding="utf-8"))
            require(report["hipcomp_offline"]["complete"] and report["hipcomp_offline"].get("object_inspected") is True and
                not report["hipcomp_offline"]["gpu_executed"] and
                not report["hipcomp_offline"]["module_loaded"],"candidate offline scope")

        # Reuse the unchanged compiler helper. Its explicit include root is a
        # fresh byte-identical source fixture, never a patched upstream checkout.
        inputs=root/"compile-input"
        copies=(("src/gpu/decode_kernel.hip","src/gpu/decode_kernel.hip"),
                ("src/gpu/decode_kernel.hpp","src/gpu/decode_kernel.hpp"),
                ("include/flitzfs/gpu_protocol.hpp","src/flitzfs/gpu_protocol.hpp"),
                ("include/flitzfs/gpu_kernel_api.hpp","src/flitzfs/gpu_kernel_api.hpp"),
                ("include/flitzfs/gpu_match_protocol.hpp","src/flitzfs/gpu_match_protocol.hpp"),
                ("include/flitzfs/gpu_match_kernel_api.hpp","src/flitzfs/gpu_match_kernel_api.hpp"))
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
        from hip_runtime_exports import pe_function_exports
        binding_prefix="HIP runtime bindings: "
        binding_rows=[json.loads(line[len(binding_prefix):]) for line in observed["stdout"].splitlines()
            if line.startswith(binding_prefix)]
        require(len(binding_rows)==1 and isinstance(binding_rows[0],list) and len(binding_rows[0])==11 and
            all(isinstance(name,str) and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]{0,127}",name)
                for name in binding_rows[0]) and len(set(binding_rows[0]))==11,
            "SDK-derived runtime binding inventory is missing or malformed")
        runtime_name=re.search(r" required_runtime=([^\s]+)",observed["stdout"])
        require(runtime_name is not None and re.fullmatch(r"amdhip64[a-z0-9_]*\.dll",runtime_name.group(1)),
            "SDK-derived runtime basename is missing or malformed")
        runtime=sdk/"bin"/runtime_name.group(1)
        require(runtime.is_file() and 0<runtime.stat().st_size<=128*1024*1024,"runtime export inspection budget")
        exports=pe_function_exports(runtime.read_bytes())
        missing=sorted(set(binding_rows[0])-exports)
        report["runtime_exports"]=dict(runtime=runtime.name,bytes=runtime.stat().st_size,sha256=digest(runtime),
            required=binding_rows[0],direct_function_count=len(exports),missing=missing,loaded=False)
        require(not missing,"SDK runtime lacks required direct function exports: "+repr(missing))
        api_probe=build/"Release/flitzfs_rocm_api_probe.exe"
        report["api_probe"]=dict(bytes=api_probe.stat().st_size,sha256=digest(api_probe),
            imports=audit_binary(api_probe.read_bytes()))
        api_result=run(bounds,[str(api_probe)],report,"typed-device-identity-fake-runtime",30)
        api_prefix="HIP API CPU qualification: "
        api_rows=[json.loads(line[len(api_prefix):]) for line in api_result["stdout"].splitlines()
            if line.startswith(api_prefix)]
        require(len(api_rows)==1 and api_rows[0]["checks"]>=40 and
            api_rows[0]["identity_before_selection"] is True and
            api_rows[0]["missing_exports_refused"]==11 and api_rows[0]["hip_runtime_loaded"] is False and
            api_rows[0]["gpu_executed"] is False and api_rows[0]["complete"] is True,
            "typed identity API evidence mismatch")
        report["api_probe"]["qualification"]=api_rows[0]
        module=build/"Release/flitzfs_rocm_kernels.dll"
        require(module.is_file() and module.stat().st_size<=32*1024*1024,"kernel module output budget")
        kernel_imports,kernel_delayed=pe_import_names(module.read_bytes())
        require(bool(kernel_imports) and not kernel_delayed and
            set(kernel_imports)<={runtime_name.group(1),"kernel32.dll"},
            "kernel-only route requires only the selected runtime and Windows system imports")
        report["kernel_module"]=dict(bytes=module.stat().st_size,sha256=digest(module),
            imports=sorted(set(kernel_imports)))
        worker=build/"Release/flitzfs_hip_worker.exe"
        worker_probe=build/"Release/flitzfs_hip_worker_probe.exe"
        require(worker.is_file() and 0 < worker.stat().st_size <= 32*1024*1024, "worker executable budget")
        imports, delay=pe_import_names(worker.read_bytes())
        allowed={"kernel32.dll","advapi32.dll","bcrypt.dll","ntdll.dll","dxgi.dll","wintrust.dll"}
        require(bool(imports) and set(imports) <= allowed and not delay, "worker has eager SDK dependencies")
        report["worker"]=dict(bytes=worker.stat().st_size,sha256=digest(worker),imports=sorted(set(imports)),
            bundle=json.loads((build/"Release/hip-bundle.json").read_text(encoding="utf-8")))
        report["private_runtime_inventory"]=inspect_private_runtime(sdk,build,report["worker"]["bundle"])
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
        # Own binaries only: no AMD runtime or SDK helper is copied into this fixture.
        driver_fixture=root/"driver-worker"
        bad_driver_fixture=root/"tampered-driver-worker"
        for destination,tampered_kernel in ((driver_fixture,False),(bad_driver_fixture,True)):
            destination.mkdir()
            (destination/"hip").mkdir()
            for source,target in ((worker,destination/worker.name),
                    (module,destination/"hip"/module.name.lower())):
                with source.open("rb") as incoming,target.open("xb") as outgoing:
                    if tampered_kernel and source==module:
                        byte=incoming.read(1)
                        require(len(byte)==1,"empty own kernel")
                        outgoing.write(bytes([byte[0]^1]))
                    shutil.copyfileobj(incoming,outgoing,64*1024)
        report["driver_fixture"]=dict(runtime_copied=False,files=[worker.name,"hip/"+module.name.lower()],
            scope="Explicit expected installed-runtime hash; no automatic route")
        from stage_hip_worker_bundle import system_directory
        kernel32=system_directory()/"kernel32.dll"
        require(kernel32.is_file() and 0<kernel32.stat().st_size<=32*1024*1024,
            "signed Windows metadata fixture size")
        metadata_probe=build/"Release/flitzfs_system_module_probe.exe"
        require(metadata_probe.is_file() and 0<metadata_probe.stat().st_size<=32*1024*1024,
            "metadata probe executable budget")
        metadata_imports,metadata_delayed=pe_import_names(metadata_probe.read_bytes())
        require(bool(metadata_imports) and set(metadata_imports)<=allowed-{"dxgi.dll"} and
            not metadata_delayed,"metadata probe dependency admission")
        report["metadata_probe"]=dict(bytes=metadata_probe.stat().st_size,sha256=digest(metadata_probe),
            imports=sorted(set(metadata_imports)),subject="OS kernel32.dll",subject_sha256=digest(kernel32),
            runtime_loaded=False,gpu_executed=False)
        result=run(bounds,[str(worker_probe),str(worker),str(root/"worker-probe"),str(altered/worker.name),
            str(metadata_probe),report["metadata_probe"]["subject_sha256"],
            str(driver_fixture/worker.name),str(bad_driver_fixture/worker.name)],
            report,"bounded-worker-admission-without-HIP",90)
        prefix="HIP worker admission: "
        receipts=[json.loads(line[len(prefix):]) for line in result["stdout"].splitlines() if line.startswith(prefix)]
        require(receipts==[dict(bundle_verified=True,missing_bundle_refused=True,tampered_bundle_refused=True,invalid_wire_refused=True,
            invalid_match_refused=True,missing_match_runtime_refused=True,
            compiler_environment_isolated=True,system_module_metadata_passed=True,system_runtime_bad_hash_refused=True,
            system_runtime_argument_refused=True,kernel_only_verified=True,kernel_only_full_bundle_refused=True,
            kernel_only_tamper_refused=True,driver_wire_refusals=5,profile_wire_refused=True,
            profile_argument_refused=True,profile_runtime_refused=True,exits_confirmed=20,gpu_executed=False,complete=True)],"worker admission evidence mismatch")
        report["worker_admission"]=receipts[0]
        hardware_probe=build/"Release/flitzfs_hip_hardware_probe.exe"
        hardware_imports,hardware_delayed=pe_import_names(hardware_probe.read_bytes())
        require(bool(hardware_imports) and not hardware_delayed and
            set(hardware_imports)<={"kernel32.dll","advapi32.dll","bcrypt.dll","ntdll.dll","dxgi.dll"},
            "diagnostic OS import policy")
        report["hardware_probe"]=dict(bytes=hardware_probe.stat().st_size,sha256=digest(hardware_probe),
            imports=dict(format="PE32+",architecture="AMD64",os_imports=sorted(set(hardware_imports)),
                delay_imports=False))
        observed_adapters=run(bounds,[str(hardware_probe),"--describe-amd-adapters"],report,
            "bounded-adapter-identity-without-HIP",15)
        adapter_prefix="HIP adapter metadata: "
        adapter_rows=[json.loads(line[len(adapter_prefix):]) for line in observed_adapters["stdout"].splitlines()
            if line.startswith(adapter_prefix)]
        require(len(adapter_rows)==1,"missing bounded adapter metadata")
        adapter_row=adapter_rows[0]
        require(adapter_row["complete"] is True and adapter_row["gpu_executed"] is False and
            adapter_row["hip_initialized"] is False and type(adapter_row["adapters_seen"]) is int and
            0<=adapter_row["adapters_seen"]<64 and isinstance(adapter_row["adapters"],list) and
            len(adapter_row["adapters"])<=adapter_row["adapters_seen"] and
            0<adapter_row["controller_job_peak_commit_bytes"]<=256*1024*1024,
            "adapter metadata resource/scope mismatch")
        luids=set()
        for item in adapter_row["adapters"]:
            require(item["vendor"]==0x1002 and type(item["device"]) is int and
                0<=item["device"]<2**32 and type(item["dedicated_bytes"]) is int and
                0<item["dedicated_bytes"]<2**64 and type(item["dxgi_ordinal"]) is int and
                0<=item["dxgi_ordinal"]<adapter_row["adapters_seen"] and item["probe_node_mask"]==1 and
                isinstance(item["luid"],str) and re.fullmatch(r"[a-f0-9]{16}",item["luid"]) and
                item["luid"]!="0"*16 and item["luid"] not in luids,"invalid adapter identity")
            luids.add(item["luid"])
        report["hardware_probe"]["adapter_metadata"]=adapter_row
        budget_result=run(bounds,[str(hardware_probe),"--inspect-adapter-budget","0100000000000000"],
            report,"bounded-budget-metadata-without-HIP",15)
        budget_prefix="HIP budget metadata: "
        budget_rows=[json.loads(line[len(budget_prefix):]) for line in budget_result["stdout"].splitlines()
            if line.startswith(budget_prefix)]
        require(len(budget_rows)==1,"missing adapter budget metadata")
        budget_row=budget_rows[0]
        require(budget_row["gpu_executed"] is False and budget_row["hip_initialized"] is False and
            budget_row["snapshot_valid"] is False and budget_row["status"] in (2,3,4) and
            budget_row["gate_code"]==3 and
            type(budget_row["phase"]) is int and 1<=budget_row["phase"]<=5 and
            type(budget_row["native_status"]) is int and -2**31<=budget_row["native_status"]<2**31 and
            all(budget_row[key]==0 for key in ("dedicated_bytes","local_budget","local_usage","nonlocal_usage")) and
            0<budget_row["controller_job_peak_commit_bytes"]<=256*1024*1024,
            "fake-LUID budget query did not fail closed without HIP")
        report["hardware_probe"]["budget_metadata"]=budget_row
        for name,args,software in (
            ("unknown_adapter",["--inspect-adapter-topology","0100000000000000"],False),
            ("warp",["--inspect-warp-topology"],True),
        ):
            topology_result=run(bounds,[str(hardware_probe),*args],report,
                "bounded-topology-metadata-"+name,15)
            prefix="GPU topology metadata: "
            rows=[json.loads(line[len(prefix):]) for line in topology_result["stdout"].splitlines()
                if line.startswith(prefix)]
            require(len(rows)==1,"missing topology metadata")
            row=rows[0]
            require(all(row[key] is False for key in ("gpu_executed","hip_initialized",
                    "queues_requested","batch_allocations_requested","kernel_launch_requested","single_node_verified")) and
                row["software_fixture"] is software and
                0<row["controller_job_peak_commit_bytes"]<=256*1024*1024,
                "topology metadata scope/resource mismatch")
            if software:
                require(row["status"]==0 and row["phase"]==10 and row["native_status"]==0 and
                    row["luid_matches"] is True and row["amd_hardware"] is False and
                    1<=row["node_count"]<=32,"native WARP device topology proof failed")
            else:
                require(row["status"]!=0 and row["node_count"]==0 and row["luid_matches"] is False and
                    row["amd_hardware"] is False,"unknown native adapter did not fail closed")
            report["hardware_probe"]["topology_"+name]=row
        result=run(bounds,[str(hardware_probe),"--run-amd-once",str(root/"worker-probe"/worker.name),
            "0","0100000000000000"],report,"one-shot-probe-missing-runtime",15,exit_code=2)
        prefix="HIP hardware probe: "
        rows=[json.loads(line[len(prefix):]) for line in result["stdout"].splitlines() if line.startswith(prefix)]
        require(len(rows)==1 and rows[0]["gpu_result_verified"] is False and
            rows[0]["cpu_bytes_verified"]==12288 and rows[0]["worker_exit"]==HIP_RUNTIME_UNAVAILABLE and
            rows[0]["worker_category"]==HIP_RUNTIME_UNAVAILABLE and rows[0]["worker_stage"]=="none" and
            rows[0]["exit_confirmed"] is True and rows[0]["timed_out"] is False and
            rows[0]["publication_observed"] is False and
            0<rows[0]["peak_commit_bytes"]<=128*1024*1024 and
            0<rows[0]["controller_job_peak_commit_bytes"]<=256*1024*1024,
            "one-shot probe missing-runtime evidence mismatch")
        report["hardware_probe"]["missing_runtime"]=rows[0]
        refused_exits=(HIP_RUNTIME_UNAVAILABLE,HIP_INTEGRITY_FAILURE)
        diagnostic_cases=(
            ("driver_memory",["--run-amd-driver-memory-once",str(driver_fixture/worker.name),
                "0","0100000000000000","0"*64],"HIP memory observation: "),
            ("driver_identity",["--inspect-driver-adapter",str(driver_fixture/worker.name),
                "0","0100000000000000","0"*64],"HIP identity metadata: "),
            ("metadata",["--verify-driver-runtime",str(driver_fixture/worker.name),"0"*64],
                "HIP runtime metadata probe: "),
            ("driver_decode",["--run-amd-driver-once",str(driver_fixture/worker.name),
                "0","0100000000000000","0"*64],"HIP hardware probe: "),
            ("driver_profile",["--profile-amd-driver-match-once",str(driver_fixture/worker.name),
                "0","0100000000000000","0"*64],"HIP match phase profile: "),
            ("driver_match",["--run-amd-driver-match-once",str(driver_fixture/worker.name),
                "0","0100000000000000","0"*64],"HIP match hardware probe: "),
        )
        for name,arguments,prefix in diagnostic_cases:
            result=run(bounds,[str(hardware_probe),*arguments],report,"bounded-"+name+"-refusal",15,exit_code=2)
            values=[json.loads(line[len(prefix):]) for line in result["stdout"].splitlines()
                if line.startswith(prefix)]
            require(len(values)==1,"missing diagnostic refusal receipt")
            value=values[0]
            require(value["worker_exit"] in refused_exits and value["exit_confirmed"] is True and
                value["timed_out"] is False and 0<value["peak_commit_bytes"]<=128*1024*1024 and
                0<value["controller_job_peak_commit_bytes"]<=256*1024*1024,
                "diagnostic process/resource refusal evidence mismatch")
            if name=="driver_memory":
                require(value["metadata_verified"] is False and value["observation"] is None and
                    value["gpu_candidate_accepted"] is False and value["cpu_reference_bytes"]==12288,
                    "failed memory diagnostic fabricated execution metadata")
            elif name=="driver_profile":
                require(value["profile_verified"] is False and value["gpu_candidate_accepted"] is False and
                    value["phases"] is None and value["publication_observed"] is False and
                    value["input_bytes"]==4096 and value["exchange_wall_ns"]>=0,
                    "failed profile fabricated measurements or accepted data")
            elif name=="driver_identity":
                require(value["properties_verified"] is False and value["hip_initialization_confirmed"] is False and
                    value["flitzfs_kernel_load_requested"] is False and value["batch_allocations_requested"] is False and
                    value["kernel_launch_requested"] is False and value["luid"] is None and value["node_mask"] is None and
                    value["luid_matches"] is None and value["node_mask_matches"] is None and
                    value["worker_category"]==value["worker_exit"] and value["worker_stage"]=="none",
                    "identity diagnostic fabricated properties or requested compute")
            elif name=="metadata":
                require(value["verified"] is False and value["gpu_executed"] is False,
                    "metadata probe falsely accepted the runtime")
            else:
                require(value["worker_category"]==value["worker_exit"] and value["worker_stage"]=="none",
                    "pre-runtime refusal invented a diagnostic stage")
                require(value["gpu_result_verified"] is False and value["publication_observed"] is False,
                    "driver diagnostic falsely published output")
                if name=="driver_decode":
                    require(value["cpu_bytes_verified"]==12288 and value["installed_driver_route"] is True,
                        "driver decode CPU fallback mismatch")
                else:
                    require(value["source_bytes_verified"]==4096 and value["proposals"]==0,
                        "driver match CPU fallback mismatch")
            report["hardware_probe"][name+"_refusal"]=value
        actual_clang=run(bounds,[str(sdk/"lib/llvm/bin/clang.exe"),"--version"],report,"actual-AMD-compiler-version",30)
        require(lock["observed_clang_version"] in actual_clang["stdout"],
            "observed complete-SDK compiler identity differs from its lock record")
        require(not native_git(provider,"status","--porcelain","--untracked-files=all"),
            "reference source changed during qualification")
        require(bootstrap.sdk_identity(sdk,lock["version"])==identity,
            "SDK critical inputs changed during compilation")
        package=root/"driver-probe-package"
        package.mkdir()
        copies=(
            (worker,worker.name),
            (module,"hip/"+module.name.lower()),
            (hardware_probe,hardware_probe.name),
            (REPO/"LICENSE","LICENSE"),
            *((REPO/"third_party/blake3"/name,"licenses/blake3/"+name)
                for name in ("LICENSE_A2","LICENSE_A2LLVM","LICENSE_CC0")),
        )
        records=[]
        for source,relative in copies:
            size=source.stat().st_size
            require(source.is_file() and 0<size<=32*1024*1024,"own probe artifact input budget")
            target=package/relative
            target.parent.mkdir(parents=True,exist_ok=True)
            with source.open("rb") as incoming,target.open("xb") as outgoing:
                shutil.copyfileobj(incoming,outgoing,64*1024)
            require(target.stat().st_size==size and digest(target)==digest(source),"own artifact copy identity")
            records.append(dict(path=relative,bytes=size,sha256=digest(target)))
        manifest=dict(experimental=True,commit=os.environ["GITHUB_SHA"],rocm=lock["version"],
            runtime_basename=runtime_name.group(1),runtime_shipped=False,kpack_shipped=False,
            kernel_imports=sorted(set(kernel_imports)),files=records,
            controller_job_mib=256,worker_job_mib=128,cpu_rate=2000,worker_timeout_ms=5000,
            hardware_executed=False)
        with (package/"qualification-manifest.json").open("x",encoding="utf-8") as out:
            json.dump(manifest,out,indent=2);out.write("\n")
        with (package/"EXPERIMENTAL.txt").open("x",encoding="utf-8") as out:
            out.write("Experimental FlitzFS diagnostic binaries. No AMD runtime is included.\n"
                "The default filesystem never invokes these workers. Metadata verification is separate from compute.\n"
                "Use only through the qualified bounded diagnostic and the explicit owner-device safety gate.\n"
                "GPU execution and speed are not qualified by the hosted CPU/refusal tests.\n")
        files=sorted(path for path in package.rglob("*") if path.is_file())
        require(len(files)<=16 and sum(path.stat().st_size for path in files)<=8*1024*1024,
            "own artifact expanded budget")
        archive=root/"driver-probe.zip"
        with zipfile.ZipFile(archive,"x",compression=zipfile.ZIP_DEFLATED,compresslevel=6) as out:
            for path in files:
                out.write(path,path.relative_to(package).as_posix())
        require(0<archive.stat().st_size<=8*1024*1024,"own artifact compressed budget")
        report["driver_probe_artifact"]=dict(bytes=archive.stat().st_size,sha256=digest(archive),
            files=records,runtime_shipped=False,kpack_shipped=False,hardware_executed=False)
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
