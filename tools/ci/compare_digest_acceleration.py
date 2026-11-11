#!/usr/bin/env python3
"""Same-commit Windows CPU digest and RAM-object measurements; never owner-host use."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import threading
import time

CORPORA=("zeros","uniform","motifs","numeric","text","noise")
MODES=("raw","auto6")
END=time.monotonic()+180

def run(binary,*args):
    if time.monotonic()>=END:
        raise RuntimeError("digest campaign deadline")
    child=subprocess.Popen([str(binary),*args],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    if child.stdout is None or child.stderr is None:
        child.kill();child.wait(timeout=5)
        raise RuntimeError("digest pipe ownership")
    output=[bytearray(),bytearray()]
    errors=[]
    def read(stream,index,cap):
        try:
            while True:
                block=stream.read(65536)
                if not block:
                    break
                if len(output[index])+len(block)>cap:
                    raise RuntimeError("digest output bound")
                output[index].extend(block)
        except BaseException as error:
            errors.append(error)
    threads=[threading.Thread(target=read,args=(child.stdout,0,6*1024*1024),daemon=True),
             threading.Thread(target=read,args=(child.stderr,1,16384),daemon=True)]
    for thread in threads:
        thread.start()
    deadline=min(END,time.monotonic()+20)
    try:
        while child.poll() is None:
            if errors:
                raise RuntimeError("digest output reader") from errors[0]
            if time.monotonic()>=deadline:
                raise TimeoutError("digest child deadline")
            time.sleep(0.01)
        status=child.wait(timeout=5) # Actual retirement before next admission.
        for thread in threads:
            thread.join(timeout=5)
        if any(thread.is_alive() for thread in threads) or errors:
            raise RuntimeError("digest readers did not retire cleanly")
        if status:
            raise RuntimeError("digest child failed: "+output[1].decode("utf-8","replace"))
        return bytes(output[0])
    finally:
        if child.poll() is None:
            child.kill()
        child.wait(timeout=5)
        for stream in (child.stdout,child.stderr):
            stream.close()
        for thread in threads:
            thread.join(timeout=5)
        if any(thread.is_alive() for thread in threads):
            raise RuntimeError("digest reader lifetime")

def positive(value):
    return type(value) in (int,float) and math.isfinite(value) and value>0

def micro(value,policy):
    if type(value.get("schema")) is not int or value["schema"]!=1 or value.get("complete") is not True or len(value.get("samples",[]))!=30:
        raise RuntimeError("digest microbenchmark closure")
    if policy=="auto":
        if value.get("backend")!="windows-x64-upstream-asm" or type(value.get("degree")) is not int or value["degree"] not in (4,8,16):
            raise RuntimeError("hosted auto binary did not admit source-verified SIMD")
    elif value.get("backend")!="portable" or type(value.get("degree")) is not int or value["degree"]!=1:
        raise RuntimeError("explicit scalar binary did not select portable fallback")
    expected={(size,sample,mode) for size in (4096,65536,1048576)
              for sample in range(5) for mode in ("scalar-oracle","production")}
    observed=set()
    for row in value["samples"]:
        identity=(row.get("bytes"),row.get("sample"),row.get("mode"))
        if any(type(row.get(key)) is not int for key in ("bytes","sample","hashed_bytes")) or identity not in expected or identity in observed or row.get("hashed_bytes")!=8388608 or not positive(row.get("ms")):
            raise RuntimeError("digest microbenchmark missing/duplicate/unsafe sample")
        observed.add(identity)
    if observed!=expected:
        raise RuntimeError("digest microbenchmark incomplete")
    return value

def validate_stdio(default,text,binary):
    payload=bytes((0,10,13,10,26,128,255))
    translated=payload.replace(b"\n",b"\r\n")
    if binary!=payload or text!=translated or default not in (payload,translated):
        raise RuntimeError("current Windows CRT stdout byte probe differs")
    return dict(payload_bytes=len(payload),text_bytes=len(text),binary_bytes=len(binary),
                default_bytes=len(default),default_translates_lf=default==translated,
                binary_sha256=hashlib.sha256(binary).hexdigest(),complete=True)

def main():
    if os.name!="nt" or os.environ.get("GITHUB_ACTIONS")!="true" or os.environ.get("RUNNER_ENVIRONMENT")!="github-hosted":
        raise RuntimeError("disposable hosted Windows VM required")
    parser=argparse.ArgumentParser()
    parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[2]
    head=subprocess.run(["git","-C",str(root),"rev-parse","HEAD"],capture_output=True,text=True,check=True,timeout=5).stdout.strip()
    paths={policy:root/("build-digest-"+policy)/"Release" for policy in ("auto","scalar")}
    report=dict(schema=1,commit=head,scope="CPU digest and native object transactions in RAM; no GPU, storage-device or mounted-filesystem speed claim",
        controls=dict(same_source=True,same_codec=True,threads_per_digest=1,process_bytes=536870912,
            windows_cpu_percent=20,process_cpu_seconds=15,process_wall_seconds=20,campaign_seconds=180,
            stdout_bytes=6291456,stderr_bytes=16384,object_bytes=4194304,samples=3,micro_samples=5),
        host=dict(platform=platform.platform(),processor=platform.processor(),cpus=os.cpu_count()),
        microbenchmarks={},encoded_images=[],samples=[],summary=[],complete=False)
    try:
        report["active_stage"]="stdio-mode-probe"
        probe=paths["auto"]/"flitzfs_digest_tests.exe"
        report["stdio"]=validate_stdio(run(probe,"--stdio-default"),run(probe,"--stdio-text"),run(probe,"--stdio-binary"))
        report["active_stage"]="microbenchmarks"
        measurements={};report["microbenchmarks"]=measurements
        for policy in ("auto","scalar"):
            report["active_stage"]=dict(microbenchmark=policy)
            measurements[policy]=micro(json.loads(run(paths[policy]/"flitzfs_digest_tests.exe","--benchmark")),policy)
        samples=[];images=[]
        report.update(samples=samples,encoded_images=images)
        for kind in CORPORA:
            for mode in MODES:
                report["active_stage"]=dict(corpus=kind,mode=mode)
                # One decoded-verified complete image per policy, held in bounded
                # parent RAM only. No filesystem image is written to a real volume.
                encoded=[run(paths[policy]/"flitzfs_store_comparison.exe",mode,kind,"dump-image") for policy in ("auto","scalar")]
                if encoded[0]!=encoded[1] or not 0<len(encoded[0])<=5*1024*1024:
                    raise RuntimeError("same-source encoded image bytes differ across CPU backends")
                images.append(dict(corpus=kind,mode=mode,bytes=len(encoded[0]),sha256=hashlib.sha256(encoded[0]).hexdigest()))
                for repeat in range(3):
                    for policy in (("auto","scalar") if repeat%2==0 else ("scalar","auto")):
                        row=json.loads(run(paths[policy]/"flitzfs_store_comparison.exe",mode,kind,"measure"))
                        if row.get("logical_bytes")!=4194304 or not 0<row.get("physical_bytes",0)<=5242880 or row.get("active_payload_bytes",5242881)>4194304:
                            raise RuntimeError("RAM-object corpus/physical/payload bounds")
                        if not all(positive(row.get(key)) for key in ("write_ms","read_verify_ms","reopen_verify_ms")):
                            raise RuntimeError("RAM-object timing fields")
                        if row["physical_bytes"]!=images[-1]["bytes"]:
                            raise RuntimeError("timed image geometry differs from byte-verified image: "+json.dumps(dict(physical=row["physical_bytes"],stream=images[-1]["bytes"])))
                        samples.append(dict(corpus=kind,mode=mode,policy=policy,repeat=repeat,**row))
        summary=[]
        for kind in CORPORA:
            for mode in MODES:
                data={}
                for policy in ("auto","scalar"):
                    rows=[r for r in samples if (r["corpus"],r["mode"],r["policy"])==(kind,mode,policy)]
                    data[policy]={key:statistics.median(r[key] for r in rows)
                        for key in ("physical_bytes","active_payload_bytes","write_ms","read_verify_ms","reopen_verify_ms")}
                if any(data["auto"][key]!=data["scalar"][key] for key in ("physical_bytes","active_payload_bytes")):
                    raise RuntimeError("CPU policy changed stored bytes")
                summary.append(dict(corpus=kind,mode=mode,**data))
        report.update(summary=summary,complete=True)
        report.pop("active_stage",None)
        micro_summary=[]
        for size in (4096,65536,1048576):
            times={mode:statistics.median(r["ms"] for r in measurements["auto"]["samples"]
                   if r["bytes"]==size and r["mode"]==mode) for mode in ("production","scalar-oracle")}
            micro_summary.append(dict(bytes=size,**times,observed_ratio=times["scalar-oracle"]/times["production"]))
        result_summary=dict(commit=head,complete=True,image_pairs=len(images),transaction_samples=len(samples),
            micro_summary=micro_summary,stdio=report["stdio"],summary=summary)

    except BaseException as error:
        report.update(complete=False,error=type(error).__name__+": "+str(error))
        raise
    finally:
        with args.output.open("x",encoding="utf-8") as output:
            json.dump(report,output,indent=2);output.write("\n")
        if report["complete"] is not True:
            print(json.dumps(dict(commit=head,complete=False,active_stage=report.get("active_stage"),
                error=report.get("error"),retained_samples=len(report["samples"]))))
    print(json.dumps(result_summary))

if __name__=="__main__":
    main()
