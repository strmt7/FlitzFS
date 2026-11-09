#!/usr/bin/env python3
"""Force-stop owned native image writers at every reported I/O boundary."""
import argparse
import json
import os
from pathlib import Path
import queue
import secrets
import subprocess
import tempfile
import threading
import time

DEADLINE=time.monotonic()+180
MAX_CASES=1024
MAX_EVENTS=256

class Child:
    def __init__(self,args):
        if time.monotonic()>DEADLINE:
            raise RuntimeError("overall interruption deadline")
        self.process=subprocess.Popen(args,stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT,bufsize=0)
        if self.process.stdin is None or self.process.stdout is None:
            self.process.kill();self.process.wait(timeout=5)
            raise RuntimeError("probe pipe ownership")
        self.lines=queue.Queue(maxsize=8)
        self.error=None
        self.stop=threading.Event()
        def reader():
            try:
                while not self.stop.is_set():
                    line=self.process.stdout.readline(257)
                    if len(line)>256 or (line and not line.endswith(b"\n")):
                        raise RuntimeError("probe line budget")
                    while not self.stop.is_set():
                        try:
                            self.lines.put(line,timeout=0.25)
                            break
                        except queue.Full:
                            pass
                    if not line:
                        break
            except BaseException as error:
                self.error=error
        self.thread=threading.Thread(target=reader,daemon=True)
        self.thread.start()
        self.end=time.monotonic()+15

    def read(self):
        while True:
            if self.error:
                raise RuntimeError("probe output reader failed") from self.error
            remaining=min(self.end,DEADLINE)-time.monotonic()
            if remaining<=0:
                raise TimeoutError("probe process deadline")
            try:
                line=self.lines.get(timeout=min(remaining,0.25))
                break
            except queue.Empty:
                pass
        if not line:
            raise RuntimeError("probe ended before completion")
        try:
            value=json.loads(line)
        except (ValueError,UnicodeError) as error:
            raise RuntimeError("probe failed: "+line.decode("utf-8","replace")) from error
        if not isinstance(value,dict):
            raise RuntimeError("invalid probe protocol")
        return value

    def advance(self):
        self.process.stdin.write(b"c")
        self.process.stdin.flush()

    def kill(self):
        if self.process.poll() is not None:
            raise RuntimeError("probe exited before forced termination")
        self.process.kill() # SIGKILL / TerminateProcess, never a user-space exit hook.
        status=self.process.wait(timeout=5)
        if status==0:
            raise RuntimeError("forced termination returned success")
        return status

    def finish(self):
        self.process.stdin.close()
        if self.process.wait(timeout=5)!=0:
            raise RuntimeError("probe completion failed")

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=5) # Confirm exit before file cleanup or recovery.
        self.stop.set()
        self.thread.join(timeout=5)
        if self.thread.is_alive():
            raise RuntimeError("probe output reader did not retire")
        for stream in (self.process.stdin,self.process.stdout):
            if stream is not None and not stream.closed:
                stream.close()

def execute(binary,root,token,policy,phase,cut):
    args=[str(binary),"write",str(root),token,policy,phase,"0"]
    child=Child(args)
    events=[]
    acknowledged=0
    generations=(6,8) if phase=="compact" else ((4,6) if phase=="files" else ((2,4) if phase=="checkpoint" else (1,2)))
    source_digest=None
    forced=None
    try:
        if child.read()!={"ready":True}:
            raise RuntimeError("resource admission handshake")
        child.advance()
        while True:
            event=child.read()
            if event=={"done":True}:
                if cut is not None:
                    raise RuntimeError("requested fault point not reached")
                child.finish()
                break
            if event.get("event")!=len(events)+1 or len(events)>=MAX_EVENTS:
                raise RuntimeError("event order/budget")
            if event.get("op") not in ("ack","write-before","write-after","flush-before","flush-after"):
                raise RuntimeError("unknown event")
            events.append(event)
            if event["op"]=="ack":
                generation=event.get("generation")
                if generation not in generations or generation<acknowledged:
                    raise RuntimeError("acknowledgement oracle")
                acknowledged=generation
                if phase=="compact":
                    digest=event.get("source_digest")
                    if not isinstance(digest,str) or len(digest)!=64 or any(c not in "0123456789abcdef" for c in digest):
                        raise RuntimeError("missing source integrity oracle")
                    if source_digest is not None and digest!=source_digest:
                        raise RuntimeError("source integrity oracle changed")
                    source_digest=digest
            if event["event"]==cut:
                forced=child.kill()
                break
            child.advance()
    finally:
        child.close()
    if acknowledged not in generations:
        raise RuntimeError("no acknowledged baseline")
    recovery_args=[str(binary),"recover",str(root),token,policy,phase,str(acknowledged)]
    if phase=="compact":
        if source_digest is None:
            raise RuntimeError("missing compaction source oracle")
        recovery_args.append(source_digest)
    recover=Child(recovery_args)
    try:
        if recover.read()!={"ready":True}:
            raise RuntimeError("recovery resource admission handshake")
        recover.advance()
        result=recover.read()
        if set(result)!={"recovered","continued"} or not acknowledged<=result["recovered"]<=generations[-1] or result["continued"]<=result["recovered"]:
            raise RuntimeError("invalid recovery/continuation result")
        recover.finish()
    finally:
        recover.close()
    return events,dict(acknowledged=acknowledged,forced_exit=forced,**result)

def case(binary,policy,phase,cut):
    root=Path(tempfile.mkdtemp(prefix="flitzfs-store-crash-")).resolve()
    token=secrets.token_hex(16)
    marker=root/".owner";sentinel=root/"unrelated.bin";image=root/"image.fltx"
    copied=root/"copy.fltx";retry=root/"retry.fltx";images=(image,copied,retry)
    expected=bytes(range(256))
    try:
        with marker.open("xb") as output:
            output.write(token.encode("ascii"))
        with sentinel.open("xb") as output:
            output.write(expected)
        events,result=execute(binary,root,token,policy,phase,cut)
        if sentinel.read_bytes()!=expected or marker.read_bytes()!=token.encode("ascii"):
            raise RuntimeError("unrelated owned fixture changed")
        if sum(path.stat().st_size for path in images if path.exists())>3*1024*1024:
            raise RuntimeError("fixture physical budget")
        return events,result
    finally:
        # Only these individually recorded names in our exclusively created parent.
        for path in (*images,sentinel,marker):
            path.unlink(missing_ok=True)
        root.rmdir()

def main():
    if os.environ.get("GITHUB_ACTIONS")!="true" or os.environ.get("RUNNER_ENVIRONMENT")!="github-hosted":
        raise RuntimeError("disposable hosted VM required")
    parser=argparse.ArgumentParser()
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--build",type=Path,default=Path("build-store-crash"))
    args=parser.parse_args()
    binary=(args.build/("Release" if os.name=="nt" else "")/
            ("flitzfs_store_crash_probe.exe" if os.name=="nt" else "flitzfs_store_crash_probe")).resolve()
    rows=[];coverage=[]
    for policy in ("raw","auto"):
        for phase in ("mutation","repair","files","compact","checkpoint"):
            events,result=case(binary,policy,phase,None)
            coverage.append(dict(policy=policy,phase=phase,events=events,completed=result))
            for cut in range(1,len(events)+1):
                if len(rows)>=MAX_CASES:
                    raise RuntimeError("fault case budget")
                observed,result=case(binary,policy,phase,cut)
                if observed!=events[:cut]:
                    raise RuntimeError("fault trace changed between cases")
                rows.append(dict(policy=policy,phase=phase,cut=cut,operation=observed[-1]["op"],**result))
    root=Path(__file__).resolve().parents[2]
    head=subprocess.run(["git","-C",str(root),"rev-parse","HEAD"],check=True,capture_output=True,text=True,timeout=5).stdout.strip()
    report=dict(schema=1,commit=head,platform=os.name,cases=len(rows),coverage=coverage,results=rows,
        scope="Process termination at I/O boundaries, including reverted native extent reuse, owned ordinary files; not physical power loss or mounted writes",
        limits=dict(total_seconds=180,process_seconds=15,cases=1024,events=256,event_bytes=256,
                    queued_events=8,fixture_bytes=3145728,
                    windows_process_bytes=805306368,windows_cpu_percent=20,
                    posix_address_bytes=536870912,posix_cpu_seconds=15))
    with args.output.open("x",encoding="utf-8") as output:
        json.dump(report,output,indent=2);output.write("\n")
    print(json.dumps(dict(commit=head,platform=os.name,cases=len(rows),
        scopes=[dict(policy=x["policy"],phase=x["phase"],events=len(x["events"])) for x in coverage],
        outcome="all acknowledged bytes recovered, new commit independently reopened, unrelated fixture unchanged")))

if __name__=="__main__":
    main()
