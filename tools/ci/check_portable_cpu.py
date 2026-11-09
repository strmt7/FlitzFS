#!/usr/bin/env python3
"""Hosted-only relocation/dependency qualification; synthetic NEW files only."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import sys
import tempfile

MIB=1024**2
_JOB=None

def require(condition,message):
    if not condition:
        raise RuntimeError(message)

def span(data,offset,size):
    require(0<=offset<=len(data) and 0<=size<=len(data)-offset,"executable range is outside file")
    return data[offset:offset+size]

def number(data,offset,fmt):
    return struct.unpack(fmt,span(data,offset,struct.calcsize(fmt)))[0]

def audit_binary(data):
    """Read the actual loader metadata; never execute ldd or inspect host PATH."""
    require(0<len(data)<=32*MIB,"portable executable size exceeds qualification budget")
    if sys.platform=="linux":
        require(span(data,0,7)==b"\x7fELF\x02\x01\x01","expected little-endian ELF64")
        require(number(data,18,"<H")==62,"expected x86-64 ELF")
        require(number(data,16,"<H")==2,"expected a static executable")
        offset=number(data,32,"<Q")
        entry_size=number(data,54,"<H")
        count=number(data,56,"<H")
        require(entry_size==56 and 0<count<=256,"ELF program-header budget differs")
        span(data,offset,entry_size*count)
        kinds=[]
        for index in range(count):
            base=offset+index*entry_size
            kind=number(data,base,"<I")
            kinds.append(kind)
            span(data,number(data,base+8,"<Q"),number(data,base+32,"<Q"))
        require(1 in kinds and not ({2,3}&set(kinds)),"ELF has a dynamic loader or dynamic dependencies")
        return {"format":"ELF64","architecture":"x86_64","dynamic_loader":False,"program_headers":count}
    require(sys.platform=="win32","unsupported portability platform")
    require(span(data,0,2)==b"MZ","expected DOS/PE header")
    pe=number(data,0x3c,"<I")
    require(span(data,pe,4)==b"PE\0\0","expected PE signature")
    require(number(data,pe+4,"<H")==0x8664,"expected AMD64 PE")
    sections=number(data,pe+6,"<H")
    optional_size=number(data,pe+20,"<H")
    optional=pe+24
    require(1<=sections<=96 and optional_size>=240,"PE section/optional header budget differs")
    span(data,optional,optional_size)
    require(number(data,optional,"<H")==0x20b,"expected PE32+")
    require(number(data,optional+108,"<I")>=14,"PE loader directory table missing")
    section_start=optional+optional_size
    span(data,section_start,sections*40)
    def rva(value,size):
        candidates=[]
        for i in range(sections):
            base=section_start+i*40
            virtual=number(data,base+12,"<I")
            raw_size=number(data,base+16,"<I")
            raw_offset=number(data,base+20,"<I")
            if virtual<=value and value-virtual<=raw_size and size<=raw_size-(value-virtual):
                candidates.append(raw_offset+value-virtual)
        require(len(candidates)==1,"PE RVA has no unique file-backed section")
        span(data,candidates[0],size)
        return candidates[0]
    table=optional+112
    imports_rva=number(data,table+8,"<I")
    imports_size=number(data,table+12,"<I")
    require(imports_rva!=0 and 20<=imports_size<=65536,"PE imports absent or exceed budget")
    # Delay-load imports would bypass the ordinary dependency proof.
    require(number(data,table+13*8,"<I")==0 and number(data,table+13*8+4,"<I")==0,
            "PE delay-load dependencies are not admitted")
    imports=rva(imports_rva,imports_size)
    names=[]
    for index in range(min(imports_size//20,256)):
        descriptor=span(data,imports+20*index,20)
        if descriptor==bytes(20):
            break
        name_rva=number(descriptor,12,"<I")
        rva(name_rva,1)
        name=bytearray()
        for j in range(256):
            byte=span(data,rva(name_rva+j,1),1)[0]
            if byte==0:
                break
            name.append(byte)
        else:
            raise RuntimeError("PE import name exceeds budget")
        require(bool(name),"empty PE import name")
        names.append(bytes(name).decode("ascii").lower())
    else:
        raise RuntimeError("PE import directory lacks bounded terminator")
    # These names are OS components. In particular, UCRT/VC runtime, codec,
    # SDK and GPU DLLs are NOT admitted by a broad api-ms/ext-ms prefix.
    allowed={"kernel32.dll","advapi32.dll","bcrypt.dll","ntdll.dll"}
    require(bool(names) and set(names)<=allowed,"portable PE imports a non-admitted dependency: "+repr(names))
    return {"format":"PE32+","architecture":"AMD64","os_imports":sorted(set(names)),"delay_imports":False}

def contain():
    global _JOB
    if sys.platform=="linux":
        import resource
        os.nice(19)
        resource.setrlimit(resource.RLIMIT_AS,(512*MIB,512*MIB))
        resource.setrlimit(resource.RLIMIT_CPU,(30,30))
        resource.setrlimit(resource.RLIMIT_CORE,(0,0))
        resource.setrlimit(resource.RLIMIT_FSIZE,(32*MIB,32*MIB))
        return
    from ctypes import wintypes as w
    class Basic(ctypes.Structure):
        _fields_=[("process_time",ctypes.c_longlong),("job_time",ctypes.c_longlong),
            ("flags",w.DWORD),("min_ws",ctypes.c_size_t),("max_ws",ctypes.c_size_t),
            ("active",w.DWORD),("affinity",ctypes.c_size_t),("priority",w.DWORD),("scheduling",w.DWORD)]
    class IO(ctypes.Structure):
        _fields_=[(name,ctypes.c_ulonglong) for name in
            ("read_ops","write_ops","other_ops","read_bytes","write_bytes","other_bytes")]
    class Extended(ctypes.Structure):
        _fields_=[("basic",Basic),("io",IO),("process_memory",ctypes.c_size_t),
            ("job_memory",ctypes.c_size_t),("peak_process",ctypes.c_size_t),("peak_job",ctypes.c_size_t)]
    class Rate(ctypes.Structure):
        _fields_=[("flags",w.DWORD),("rate",w.DWORD)]
    api=ctypes.WinDLL("kernel32",use_last_error=True)
    api.CreateJobObjectW.argtypes=[ctypes.c_void_p,w.LPCWSTR];api.CreateJobObjectW.restype=w.HANDLE
    api.SetInformationJobObject.argtypes=[w.HANDLE,ctypes.c_int,ctypes.c_void_p,w.DWORD]
    api.SetInformationJobObject.restype=w.BOOL
    api.GetCurrentProcess.restype=w.HANDLE
    api.AssignProcessToJobObject.argtypes=[w.HANDLE,w.HANDLE];api.AssignProcessToJobObject.restype=w.BOOL
    _JOB=api.CreateJobObjectW(None,None)
    require(bool(_JOB),"cannot create bounded qualification job")
    limits=Extended()
    limits.basic.process_time=15*10**7
    limits.basic.flags=0x2000|0x100|0x200|0x8|0x2|0x20
    limits.basic.active=2
    limits.basic.priority=0x40 # IDLE_PRIORITY_CLASS
    limits.process_memory=512*MIB
    limits.job_memory=768*MIB
    require(bool(api.SetInformationJobObject(_JOB,9,ctypes.byref(limits),ctypes.sizeof(limits))),
            "cannot set qualification memory/process/time/priority limits")
    rate=Rate(5,2000)
    require(bool(api.SetInformationJobObject(_JOB,15,ctypes.byref(rate),ctypes.sizeof(rate))),
            "cannot set qualification CPU cap")
    require(bool(api.AssignProcessToJobObject(_JOB,api.GetCurrentProcess())),
            "cannot contain qualification process before creating files or children")
    # Keep the handle until interpreter exit. Closing it earlier would terminate
    # this process too; OS teardown then contains every remaining descendant.

def child_limits():
    import resource
    resource.setrlimit(resource.RLIMIT_AS,(256*MIB,256*MIB))
    resource.setrlimit(resource.RLIMIT_CPU,(5,5))

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    require(os.environ.get("GITHUB_ACTIONS")=="true" and os.environ.get("RUNNER_ENVIRONMENT")=="github-hosted",
            "portable qualification requires a disposable GitHub-hosted VM; never the owner PC")
    require(sys.platform in ("linux","win32") and platform.machine().lower() in ("x86_64","amd64"),
            "unqualified OS/architecture")
    parser=argparse.ArgumentParser()
    parser.add_argument("binary",type=Path)
    parser.add_argument("--codecs",choices=("raw","native"),required=True)
    args=parser.parse_args()
    source=args.binary.resolve(strict=True)
    require(source.is_file() and not source.is_symlink(),"input executable must be an ordinary owned build artifact")
    require(source.stat().st_size<=32*MIB,"executable exceeds budget")
    original=source.read_bytes()
    audit=audit_binary(original)
    contain()
    parent=Path(os.environ["RUNNER_TEMP"]).resolve(strict=True)
    require(shutil.disk_usage(parent).free>=2*1024**3,"qualification requires 2 GiB free headroom")
    fixture=Path(tempfile.mkdtemp(prefix="flitzfs-portable-",dir=parent))
    stage=fixture/"package staging"
    stage.mkdir()
    name="flitzfs.exe" if sys.platform=="win32" else "flitzfs"
    staged=stage/name
    with staged.open("xb") as file:
        file.write(original)
    if sys.platform=="linux":
        staged.chmod(0o700)
    relocated=fixture/"relocated \u0394 \u4e2d \U0001f680 package"
    stage.rename(relocated) # Both paths are inside this newly owned VM fixture.
    binary=relocated/name
    require(not stage.exists() and digest(binary)==hashlib.sha256(original).hexdigest(),"relocation changed executable")
    require(list(relocated.iterdir())==[binary],"package must contain exactly the executable")
    work=fixture/"unrelated \u039b working directory"
    work.mkdir()
    for leaf in ("empty-path","home","temp"):
        (fixture/leaf).mkdir()
    env={"PATH":str(fixture/"empty-path"),"HOME":str(fixture/"home"),
         "TEMP":str(fixture/"temp"),"TMP":str(fixture/"temp"),"TMPDIR":str(fixture/"temp"),
         "LANG":"C","LC_ALL":"C"}
    if sys.platform=="win32":
        require(bool(os.environ.get("SystemRoot")),"Windows OS root identity unavailable")
        env["SystemRoot"]=os.environ["SystemRoot"]
        env["WINDIR"]=os.environ["SystemRoot"]
    checks=[]
    def call(arguments,expected=0):
        options={"preexec_fn":child_limits} if sys.platform=="linux" else {}
        result=subprocess.run([str(binary),*[str(a) for a in arguments]],cwd=work,env=env,
            capture_output=True,timeout=10,**options)
        require(len(result.stdout)+len(result.stderr)<=16384,"diagnostic output exceeded budget")
        require(result.returncode==expected,
                "relocated command failed: "+repr(arguments)+": "+result.stderr.decode("utf-8","replace"))
        checks.append({"command":str(arguments[0]),"returncode":result.returncode})
        return result.stdout
    require(b"filesystem" in call(["--help"]),"help output not recognized")
    payload=(b"FlitzFS portable CPU byte-exact fixture\n"*1024+bytes(range(256))*64)*4
    require(len(payload)<MIB,"payload fixture budget exceeded")
    names=["ordinary input.bin","input \u03a9 \u4e2d \U0001f680.bin"]
    if sys.platform=="linux":
        names.append(os.fsdecode(b"native-"+bytes([255])+b" input.bin"))
    for index,leaf in enumerate(names):
        source_file=work/leaf
        with source_file.open("xb") as file:
            file.write(payload)
        image=work/("image \u03a9 \u4e2d "+str(index)+".flfs")
        output=work/("restored \u03a9 \u4e2d "+str(index)+".bin")
        before=digest(source_file)
        text=call(["pack",source_file,image,"--chunk-kib","64","--level","3"])
        if args.codecs=="native":
            require(b"CPU Zstandard chunks: 0\n" not in text and image.stat().st_size<len(payload)//4,
                    "native package did not actually use bundled compression")
        else:
            require(b"CPU Zstandard chunks: 0\n" in text,"raw package unexpectedly requires a codec")
        image_before=digest(image)
        call(["info",image])
        call(["verify",image])
        call(["probe",image,"65530","32"])
        call(["extract",image,output])
        require(output.read_bytes()==payload,"relocated decoded bytes differ")
        call(["pack",source_file,image],1)
        call(["extract",image,source_file],1)
        require(digest(source_file)==before and digest(image)==image_before,"refusal changed an existing file")
        manifest=work/("catalog \u03a9 "+str(index)+".flcatalog")
        with manifest.open("x",encoding="utf-8",newline="\n") as file:
            # The backing leaf is ASCII so this checks an OS Unicode parent
            # independently of the catalog's deliberately stricter name grammar.
            ascii_image=work/("catalog-backing-"+str(index)+".flfs")
            with ascii_image.open("xb") as backing:
                backing.write(image.read_bytes())
            file.write("FLITZFS-CATALOG-1\t1\nvirtual.bin\t"+ascii_image.name+"\n")
        call(["catalog-list",manifest])
        call(["catalog-verify",manifest])
        call(["catalog-probe",manifest,"virtual.bin","65530","32"])
    report={"experimental":True,"commit":os.environ["GITHUB_SHA"],"platform":sys.platform,
        "codecs":args.codecs,"binary_sha256":digest(binary),"binary_bytes":len(original),
        "loader_audit":audit,"package_files":[name],"development_environment_inherited":False,
        "path_is_empty_directory":True,"relocated":True,"unicode_paths":True,
        "native_non_utf8_paths":sys.platform=="linux","commands":checks,"payload_bytes":len(payload),
        "resource_bounds":{"windows_job_memory_mib":768,"windows_cpu_percent":20,
            "linux_address_space_mib":512,"child_timeout_seconds":10,"max_fixture_bytes":8*MIB}}
    total=sum(p.stat().st_size for p in fixture.rglob("*") if p.is_file() and p!=binary)
    require(total<=8*MIB,"synthetic fixture exceeded aggregate budget")
    report["fixture_bytes_excluding_binary"]=total
    with (fixture/"portability.json").open("x",encoding="utf-8") as file:
        json.dump(report,file,indent=2,ensure_ascii=True);file.write("\n")
    print(json.dumps(report,ensure_ascii=True))
if __name__=="__main__":
    main()
