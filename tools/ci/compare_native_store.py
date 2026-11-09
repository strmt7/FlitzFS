#!/usr/bin/env python3
"""Bounded hosted RAM-backed native-store comparison, not filesystem/disk claims."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import resource
import selectors
import signal
import statistics
import subprocess
import tempfile
import time

BASE="5c7d7a0a1aea82cbcf3f8d70b1a1d340a00d2f1e"
CORPORA=("zeros","uniform","motifs","numeric","text","noise")
MODES=("baseline","raw","auto1","auto6","auto9")
DEADLINE=time.monotonic()+120

def child_limits():
    resource.setrlimit(resource.RLIMIT_AS,(512*1024*1024,512*1024*1024))
    resource.setrlimit(resource.RLIMIT_CPU,(15,15))
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))

def run(binary,mode,kind,action,path=None):
    if time.monotonic()>DEADLINE:
        raise RuntimeError("overall comparison deadline exceeded")
    args=[str(binary),mode,kind,action]+([str(path)] if path else [])
    # Bound both streams while the process runs; communicate() could otherwise
    # allocate unbounded parent RAM before an after-the-fact length check.
    with subprocess.Popen(args,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                          preexec_fn=child_limits,start_new_session=True) as child:
        if child.stdout is None or child.stderr is None:
            child.kill();child.wait(timeout=5)
            raise RuntimeError("comparison output ownership")
        streams=selectors.DefaultSelector()
        output={"stdout":bytearray(),"stderr":bytearray()}
        limits={"stdout":5*1024*1024,"stderr":16384}
        streams.register(child.stdout,selectors.EVENT_READ,"stdout")
        streams.register(child.stderr,selectors.EVENT_READ,"stderr")
        end=time.monotonic()+20
        try:
            while streams.get_map():
                remaining=end-time.monotonic()
                if remaining<=0:
                    raise TimeoutError("comparison child deadline")
                for key,_ in streams.select(min(remaining,0.25)):
                    name=key.data
                    part=os.read(key.fileobj.fileno(),min(65536,limits[name]+1-len(output[name])))
                    if not part:
                        streams.unregister(key.fileobj)
                        continue
                    output[name].extend(part)
                    if len(output[name])>limits[name]:
                        raise RuntimeError("comparison output budget")
            status=child.wait(timeout=max(0.01,end-time.monotonic()))
            if status!=0:
                raise RuntimeError("comparison child failed: "+output["stderr"].decode("utf-8","replace"))
            return bytes(output["stdout"])
        except BaseException:
            if child.poll() is None:
                try:
                    os.killpg(child.pid,signal.SIGKILL)
                except ProcessLookupError:
                    pass
            child.wait(timeout=5)
            raise
        finally:
            streams.close()

def main():
    if os.environ.get("GITHUB_ACTIONS")!="true" or os.environ.get("RUNNER_ENVIRONMENT")!="github-hosted":
        raise RuntimeError("disposable hosted VM required")
    parser=argparse.ArgumentParser()
    parser.add_argument("--baseline",type=Path,required=True)
    parser.add_argument("--candidate",type=Path,required=True)
    parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args()
    repo=Path(__file__).resolve().parents[2]
    head=subprocess.run(["git","-C",str(repo),"rev-parse","HEAD"],capture_output=True,text=True,check=True,timeout=5).stdout.strip()
    data=[]
    # Three alternating/rotating samples per policy/corpus; all complete bytes checked.
    for kind in CORPORA:
        for repeat in range(3):
            shift=repeat%len(MODES)
            order=MODES[shift:]+MODES[:shift]
            for mode in order:
                binary=args.baseline if mode=="baseline" else args.candidate
                row=json.loads(run(binary,mode,kind,"measure"))
                if row["logical_bytes"]!=4*1024*1024 or row["physical_bytes"]>5*1024*1024:
                    raise RuntimeError("comparison corpus/physical budget mismatch")
                if mode.startswith("auto") and row["active_payload_bytes"]>row["logical_bytes"]:
                    raise RuntimeError("compressed payload expansion")
                data.append(dict(corpus=kind,mode=mode,repeat=repeat,**row))
    # Cross-version golden check on full raw images, not just decoder output.
    interop=[]
    root=Path(tempfile.mkdtemp(prefix="flitzfs-native-store-compare-"))
    owned=[]
    try:
        for kind in ("text","noise"):
            previous=run(args.baseline,"baseline",kind,"dump-image")
            current=run(args.candidate,"raw",kind,"dump-image")
            if previous!=current:
                raise RuntimeError("raw native format bytes changed: "+kind)
            path=root/(kind+".fltx")
            with path.open("xb") as output:
                owned.append(path)
                output.write(current)
            for binary,mode in ((args.baseline,"baseline"),(args.candidate,"raw")):
                if run(binary,mode,kind,"verify-image",path)!=b"verified\n":
                    raise RuntimeError("independent raw file recovery")
            interop.append(dict(corpus=kind,bytes=len(current),sha256=hashlib.sha256(current).hexdigest()))
    finally:
        for path in owned:
            path.unlink()
        root.rmdir() # Nonrecursive, uniquely owned fixture only.
    summary=[]
    for kind in CORPORA:
        for mode in MODES:
            rows=[r for r in data if r["corpus"]==kind and r["mode"]==mode]
            summary.append(dict(corpus=kind,mode=mode,
                **{key:statistics.median(r[key] for r in rows)
                   for key in ("physical_bytes","write_ms","read_verify_ms","reopen_verify_ms")}))
    report=dict(schema=1,baseline=BASE,candidate=head,
        scope="native object transactions in RAM; no storage-device or mounted-filesystem measurement",
        controls=dict(corpus_bytes=4194304,samples=3,process_as_bytes=536870912,
                      process_cpu_seconds=15,process_wall_seconds=20,
                      overall_seconds=120,library_threads=1),
        host=dict(platform=platform.platform(),cpu=platform.processor(),cpus=os.cpu_count()),
        interoperability=interop,samples=data,summary=summary)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with args.output.open("x",encoding="utf-8") as output:
        json.dump(report,output,indent=2);output.write("\n")
    print(json.dumps(dict(candidate=head,baseline=BASE,rows=len(data),raw_image_checks=len(interop),summary=summary)))

if __name__=="__main__":
    main()
