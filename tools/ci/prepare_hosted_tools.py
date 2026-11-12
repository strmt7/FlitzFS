#!/usr/bin/env python3
"""Official checksum-pinned CMake/LLVM in fresh hosted-only directories."""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
import platform
from pathlib import Path, PurePosixPath
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request
import zipfile

MIB=1024**2
GIB=1024**3
ARTIFACTS={
    ("cmake","linux"):("4.5.0","https://github.com/Kitware/CMake/releases/download/v4.5.0/cmake-4.5.0-linux-x86_64.tar.gz",
        67002501,"81f0d5b0960fb6adc8197030ffc827fba937640d51e73e39e0e9e40d88c66f6f"),
    ("cmake","win32"):("4.5.0","https://github.com/Kitware/CMake/releases/download/v4.5.0/cmake-4.5.0-windows-x86_64.zip",
        56341654,"301b1728317ccd84e984b02e366a8293735bc5ac618757d3d8f58b578f062411"),
    ("llvm","linux"):("23.1.5","https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.5/LLVM-23.1.5-Linux-X64.tar.zst",
        1198896522,"c3107384835f788768f614db610e87fba7077f125ca950eb02a78bab0a308065"),
}
MAX_ENTRIES=100000
MAX_EXPANDED=50*GIB
MAX_SELECTED=2*GIB
MAX_MEMBER=768*MIB

def hosted():
    if os.environ.get("GITHUB_ACTIONS")!="true" or os.environ.get("RUNNER_ENVIRONMENT")!="github-hosted":
        raise RuntimeError("tool provisioning requires a disposable hosted VM; never the owner PC")
    if sys.version_info<(3,14):
        raise RuntimeError("Python 3.14+ required for bounded native Zstandard tar streams")
    if sys.platform not in ("linux","win32") or platform.machine().lower() not in ("x86_64","amd64"):
        raise RuntimeError("unqualified hosted OS/architecture")
    if sys.platform=="linux":
        import resource
        resource.setrlimit(resource.RLIMIT_AS,(4*GIB,4*GIB))
        resource.setrlimit(resource.RLIMIT_CPU,(720,720))
        resource.setrlimit(resource.RLIMIT_CORE,(0,0))

def archive_name(name):
    if not name or any(c in name for c in ("\\",":","\x00","\n","\r")):
        raise RuntimeError("unsafe archive name")
    path=PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts:
        raise RuntimeError("archive traversal")
    return "/".join(path.parts)

def link_target(name,target,hard=False):
    if not target or PurePosixPath(target).is_absolute() or any(c in target for c in ("\\",":","\x00","\n","\r")):
        raise RuntimeError("unsafe tool link")
    parts=[] if hard else list(PurePosixPath(name).parent.parts)
    for item in PurePosixPath(target).parts:
        if item=="..":
            if not parts:
                raise RuntimeError("tool link escapes output")
            parts.pop()
        elif item!=".":
            parts.append(item)
    if not parts:
        raise RuntimeError("empty tool link target")
    return "/".join(parts)

def download(url,path,expected_bytes,expected_sha):
    end=time.monotonic()+300;digest=hashlib.sha256();total=0
    request=urllib.request.Request(url,headers={"User-Agent":"FlitzFS-hosted-build-tools"})
    with urllib.request.urlopen(request,timeout=30) as response,path.open("xb") as output:
        declared=response.headers.get("Content-Length")
        if declared is not None and int(declared)!=expected_bytes:
            raise RuntimeError("unexpected official artifact length")
        while True:
            if time.monotonic()>end:
                raise TimeoutError("tool download deadline")
            block=response.read(MIB)
            if not block:
                break
            total+=len(block)
            if total>expected_bytes:
                raise RuntimeError("tool download budget")
            digest.update(block);output.write(block)
    if total!=expected_bytes or digest.hexdigest()!=expected_sha:
        raise RuntimeError("official tool artifact digest/length mismatch")
    return dict(url=url,bytes=total,sha256=digest.hexdigest())

def prefix_of(entries):
    names=[PurePosixPath(n).parts for n in entries]
    tops={parts[0] for parts in names if parts}
    if len(tops)==1:
        top=next(iter(tops))
        if top not in ("bin","lib","include","share"):
            return top
    return ""

def choose(entries,prefix,kind):
    selected=set()
    for name,item in entries.items():
        parts=PurePosixPath(name).parts
        rel=parts[1:] if prefix else parts
        if not rel or item["kind"]=="dir":
            continue
        wanted=kind=="cmake"
        if kind=="llvm":
            wanted=(rel[0]=="bin" and rel[-1] in ("clang","clang++","clang-23","lld","ld.lld"))
            wanted=wanted or (rel[0] in ("lib","lib64") and (".so" in rel[-1] or (len(rel)>1 and rel[1]=="clang")))
            wanted=wanted or (len(rel)>1 and rel[:2]==("include","c++"))
        if wanted:
            selected.add(name)
    pending=list(selected)
    while pending:
        name=pending.pop();item=entries[name]
        if item["kind"] in ("symlink","hardlink"):
            target=link_target(name,item["target"],item["kind"]=="hardlink")
            if target not in entries or (prefix and not target.startswith(prefix+"/")):
                raise RuntimeError("selected tool link target missing/outside package")
            if target not in selected:
                selected.add(target);pending.append(target)
    total=0
    for name in selected:
        item=entries[name]
        if item["kind"] not in ("file","symlink","hardlink"):
            raise RuntimeError("selected tool is not an ordinary file/link")
        if item["size"]<0 or item["size"]>MAX_MEMBER:
            raise RuntimeError("tool member size budget")
        if item["kind"]=="file":
            total+=item["size"]
    limit=512*MIB if kind=="cmake" else MAX_SELECTED
    if total>limit:
        raise RuntimeError("tool extraction budget")
    return selected,total

def copy_exact(source,target,size,mode):
    target.parent.mkdir(parents=True,exist_ok=True)
    remaining=size
    with target.open("xb") as output:
        while remaining:
            block=source.read(min(MIB,remaining))
            if not block:
                raise RuntimeError("truncated tool member")
            output.write(block);remaining-=len(block)
        if source.read(1):
            raise RuntimeError("tool member exceeded declared length")
    if os.name=="posix":
        target.chmod(0o755 if mode&0o111 else 0o644)

MAX_TOOL_WINDOW=2*GIB

def zstd_window(header):
    if len(header)<6 or header[:4]!=b"\x28\xb5\x2f\xfd" or header[4]&8:
        raise RuntimeError("unsupported/truncated tool Zstandard header")
    descriptor=header[4];single=bool(descriptor&32);position=5
    if single:
        size=(1,2,4,8)[descriptor>>6]
        position+=(0,1,2,4)[descriptor&3]
        if position+size>len(header):
            raise RuntimeError("truncated single-segment tool header")
        window=int.from_bytes(header[position:position+size],"little")
        if size==2:
            window+=256
    else:
        field=header[position];base=1<<(10+(field>>3))
        window=base+(base//8)*(field&7)
    if window>MAX_TOOL_WINDOW:
        raise RuntimeError("tool decoder window exceeds explicit 2 GiB cap")
    return window

@contextmanager
def tar_stream(archive,kind):
    if kind=="llvm":
        from compression.zstd import ZstdFile,DecompressionParameter
        with archive.open("rb") as source:
            window=zstd_window(source.read(18))
        available=None
        with Path("/proc/meminfo").open(encoding="ascii") as memory:
            for line in memory:
                fields=line.split()
                if len(fields)==3 and fields[0]=="MemAvailable:" and fields[2]=="kB":
                    available=int(fields[1])*1024
                    break
        if available is None or available<window+2*GIB:
            raise RuntimeError("insufficient verified tool decoder memory headroom")
        # Explicit admitted window, not the smaller stdlib tar-stream default.
        # ZstdFile.close releases its buffered decoder before the next pass.
        with ZstdFile(archive,"rb",options={DecompressionParameter.window_log_max:31}) as decoded:
            with tarfile.open(fileobj=decoded,mode="r|",stream=True) as package:
                yield package
    else:
        with tarfile.open(archive,mode="r|gz",stream=True) as package:
            yield package

def extract_tar(archive,destination,kind):
    entries={};expanded=0;deadline=time.monotonic()+480
    # Metadata/selection pass: no extraction or links yet.
    with tar_stream(archive,kind) as package:
        for member in package:
            if time.monotonic()>deadline:
                raise TimeoutError("tool archive scan deadline")
            name=archive_name(member.name)
            if not name:
                continue
            if name in entries or len(entries)>=MAX_ENTRIES:
                raise RuntimeError("duplicate/member-count tool archive budget")
            typ="file" if member.isfile() else ("dir" if member.isdir() else
                ("symlink" if member.issym() else ("hardlink" if member.islnk() else "special")))
            if member.size<0:
                raise RuntimeError("negative archive member size")
            expanded+=member.size
            if expanded>MAX_EXPANDED:
                raise RuntimeError("expanded archive scan budget")
            entries[name]=dict(kind=typ,size=member.size,mode=member.mode,target=member.linkname)
    prefix=prefix_of(entries);selected,total=choose(entries,prefix,kind)
    if shutil.disk_usage(destination.parent).free<total+4*GIB:
        raise RuntimeError("insufficient admitted extraction reserve")
    destination.mkdir()
    with tar_stream(archive,kind) as package:
        for member in package:
            if time.monotonic()>deadline:
                raise TimeoutError("tool extraction deadline")
            name=archive_name(member.name)
            if name in selected and entries[name]["kind"]=="file":
                source=package.extractfile(member)
                if source is None:
                    raise RuntimeError("missing selected tool stream")
                with source:
                    copy_exact(source,destination/name,member.size,member.mode)
    pending={n for n in selected if entries[n]["kind"] in ("symlink","hardlink")}
    for _ in range(len(pending)+1):
        progressed=False
        for name in list(pending):
            item=entries[name];resolved=link_target(name,item["target"],item["kind"]=="hardlink")
            target=destination/resolved;path=destination/name
            if target.exists():
                if not target.resolve().is_relative_to(destination.resolve()):
                    raise RuntimeError("tool link escaped fresh fixture")
                path.parent.mkdir(parents=True,exist_ok=True)
                if item["kind"]=="hardlink":
                    os.link(target,path)
                else:
                    os.symlink(os.path.relpath(target,path.parent),path)
                pending.remove(name);progressed=True
        if not pending:
            break
        if not progressed:
            raise RuntimeError("cyclic or missing tool link target")
    details=dict(archive_entries=len(entries),archive_expanded_bytes=expanded,
        extracted_files=len(selected),extracted_bytes=total,package_prefix=prefix)
    if kind=="llvm":
        with archive.open("rb") as source:
            details["decoder_window_bytes"]=zstd_window(source.read(18))
        details["decoder_window_cap_bytes"]=MAX_TOOL_WINDOW
    return destination/prefix,details

def extract_zip(archive,destination):
    with zipfile.ZipFile(archive) as package:
        entries={}
        for item in package.infolist():
            name=archive_name(item.filename)
            if not name:
                continue
            mode=item.external_attr>>16
            if stat.S_ISLNK(mode) or (stat.S_IFMT(mode) not in (0,stat.S_IFREG,stat.S_IFDIR)):
                raise RuntimeError("unexpected ZIP special member")
            if name in entries or len(entries)>=MAX_ENTRIES:
                raise RuntimeError("duplicate/too many ZIP entries")
            entries[name]=dict(kind="dir" if item.is_dir() else "file",size=item.file_size,mode=mode)
        prefix=prefix_of(entries);selected,total=choose(entries,prefix,"cmake")
        if shutil.disk_usage(destination.parent).free<total+4*GIB:
            raise RuntimeError("insufficient ZIP extraction reserve")
        destination.mkdir()
        for item in package.infolist():
            name=archive_name(item.filename)
            if name in selected:
                with package.open(item) as source:
                    copy_exact(source,destination/name,item.file_size,entries[name]["mode"])
    return destination/prefix,dict(archive_entries=len(entries),extracted_files=len(selected),
        extracted_bytes=total,package_prefix=prefix)

def tool_version(path,expected):
    result=subprocess.run([str(path),"--version"],capture_output=True,text=True,timeout=15,check=True)
    text=result.stdout[:4096]
    if expected not in text.splitlines()[0]:
        raise RuntimeError("staged build-tool version mismatch")
    return text.splitlines()[0]

def main():
    hosted()
    parser=argparse.ArgumentParser();parser.add_argument("tool",choices=("cmake","llvm"));args=parser.parse_args()
    key=(args.tool,sys.platform)
    if key not in ARTIFACTS:
        raise RuntimeError("unsupported tool/platform combination")
    version,url,size,digest=ARTIFACTS[key]
    temporary=Path(os.environ["RUNNER_TEMP"]).resolve()
    required=10*GIB if args.tool=="llvm" else 5*GIB
    if shutil.disk_usage(temporary).free<required:
        raise RuntimeError("insufficient verified provisioning headroom")
    root=Path(tempfile.mkdtemp(prefix="flitzfs-tool-"+args.tool+"-",dir=temporary)).resolve()
    if not root.is_relative_to(temporary) or any(c in str(root) for c in ("\n","\r")):
        raise RuntimeError("fresh hosted fixture scope invalid")
    archive=root/("package.zip" if sys.platform=="win32" else "package.tar")
    receipt=download(url,archive,size,digest)
    if sys.platform=="win32":
        tools,details=extract_zip(archive,root/"unpacked")
    else:
        tools,details=extract_tar(archive,root/"unpacked",args.tool)
    binary=tools/"bin"
    names=("cmake","ctest") if args.tool=="cmake" else ("clang","clang++","ld.lld")
    versions={}
    for name in names:
        path=binary/(name+(".exe" if sys.platform=="win32" else ""))
        if not path.resolve().is_relative_to(root) or not path.is_file():
            raise RuntimeError("required staged tool missing/outside fixture")
        versions[name]=tool_version(path,version)
    receipt.update(details);receipt.update(experimental=True,tool=args.tool,version=version,
        platform=sys.platform,architecture=platform.machine(),python=sys.version,commit=os.environ["GITHUB_SHA"],versions=versions,
        scope="Official CPU build tools only; no GPU runtime/module/device execution")
    path=temporary/("flitzfs-"+args.tool+"-provision.json")
    with path.open("x",encoding="utf-8") as output:
        json.dump(receipt,output,indent=2);output.write("\n")
    with Path(os.environ["GITHUB_PATH"]).open("a",encoding="utf-8") as output:
        output.write(str(binary)+"\n")
    with Path(os.environ["GITHUB_ENV"]).open("a",encoding="utf-8") as output:
        output.write("FLITZFS_"+args.tool.upper()+"_ROOT="+str(tools)+"\n")
        if args.tool=="llvm":
            output.write("CC="+str(binary/"clang")+"\nCXX="+str(binary/"clang++")+"\n")
    print(json.dumps(receipt))
if __name__=="__main__":
    main()
