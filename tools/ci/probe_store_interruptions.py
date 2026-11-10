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


BASIC_PHASES=("basic-upgrade","basic-update","basic-compound")
BASIC_SCOPE="FLTXNS03 ordinary-file writer termination: upgrade, replacement and compound data/namespace/basic metadata; not physical power loss or mounted writes"

def campaign_limits(partitions):
    return dict(partition_seconds=180,campaign_seconds=180*partitions,process_seconds=15,
                cases=1024,events=256,event_bytes=256,queued_events=8,fixture_bytes=3145728,
                windows_process_bytes=805306368,windows_cpu_percent=20,
                posix_address_bytes=536870912,posix_cpu_seconds=15)

def digest_valid(value):
    return isinstance(value,str) and len(value)==64 and all(c in "0123456789abcdef" for c in value)

def basic_states(events):
    acknowledgements=[event for event in events if event.get("op")=="ack"]
    if len(acknowledgements)!=2 or [event.get("generation") for event in acknowledgements]!=[4,6]:
        raise RuntimeError("basic baseline/candidate acknowledgement identities")
    states={event["generation"]:event.get("state_digest") for event in acknowledgements}
    if not all(digest_valid(value) for value in states.values()) or states[4]==states[6]:
        raise RuntimeError("basic state closure digests missing/unchanged")
    return states

def validate_basic_result(result,acknowledged,states):
    if any(type(result.get(key)) is not int for key in ("recovered","continued")):
        raise RuntimeError("basic recovery generation type")
    if not isinstance(states,dict) or set(states)!={4,6}:
        raise RuntimeError("basic reference closure states missing")
    recovered=result["recovered"]
    if not acknowledged<=recovered<=6 or result["continued"]<=recovered:
        raise RuntimeError("basic acknowledged generation/continuation oracle")
    if result.get("state_digest")!=states[4 if recovered==4 else 6]:
        raise RuntimeError("basic recovery is not the complete verified baseline/candidate state")
    if not digest_valid(result.get("continued_digest")) or result["continued_digest"]==result["state_digest"]:
        raise RuntimeError("basic continuation closure missing/unchanged")

def basic_expected(report):
    if type(report.get("schema")) is not int or report["schema"]!=3 or report.get("campaign")!="basic" or report.get("scope")!=BASIC_SCOPE or report.get("platform")!="nt":
        raise RuntimeError("basic campaign schema/scope identity")
    parts=report.get("partitions")
    if type(parts) is not int or parts not in (1,2) or report.get("limits")!=campaign_limits(parts):
        raise RuntimeError("basic campaign resource contract changed")
    coverage=report.get("coverage")
    identities=[(policy,phase) for policy in ("raw","auto") for phase in BASIC_PHASES]
    if not isinstance(coverage,list) or any(not isinstance(c,dict) for c in coverage) or [(c.get("policy"),c.get("phase")) for c in coverage]!=identities:
        raise RuntimeError("basic campaign omits/duplicates/relabels a required phase")
    expected=[]
    for entry in coverage:
        events=entry.get("events")
        if not isinstance(events,list) or not 2<len(events)<=MAX_EVENTS:
            raise RuntimeError("basic complete event trace absent/out of bounds")
        if any(not isinstance(event,dict) for event in events):
            raise RuntimeError("basic complete event type")
        states=basic_states(events)
        if events[0].get("op")!="ack" or events[-1].get("op")!="ack":
            raise RuntimeError("basic trace baseline/candidate ordering")
        pending=None;flushes=0;acknowledged=0
        for cut,event in enumerate(events,1):
            op=event.get("op")
            if type(event.get("event")) is not int or event["event"]!=cut or op not in ("ack","write-before","write-after","flush-before","flush-after"):
                raise RuntimeError("basic complete event identity")
            if any(type(event.get(key)) is not int or event[key]<0 for key in ("offset","bytes","generation")):
                raise RuntimeError("basic complete event field types")
            if event["offset"]+event["bytes"]>3145728 or event["bytes"]>65536:
                raise RuntimeError("basic complete event byte budget")
            fields={"event","op","offset","bytes","generation"}|({"state_digest"} if op=="ack" else set())
            if set(event)!=fields:
                raise RuntimeError("basic event scope/fields")
            if op=="ack":
                if pending is not None or event["offset"] or event["bytes"]:
                    raise RuntimeError("basic acknowledgement overlaps I/O")
                acknowledged=event["generation"]
            elif op.endswith("-before"):
                if pending is not None or event["generation"] or "state_digest" in event:
                    raise RuntimeError("basic nested/unverified I/O event")
                pending=(op.removesuffix("-before"),event["offset"],event["bytes"])
                if pending[0]=="flush" and (event["offset"] or event["bytes"]):
                    raise RuntimeError("basic flush trace fields")
            else:
                if pending!=(op.removesuffix("-after"),event["offset"],event["bytes"]) or event["generation"] or "state_digest" in event:
                    raise RuntimeError("basic I/O event pairing")
                if pending[0]=="flush":
                    flushes+=1
                pending=None
            expected.append((entry["policy"],entry["phase"],cut,op,acknowledged,states))
        if pending is not None or flushes!=3 or acknowledged!=6:
            raise RuntimeError("basic trace lacks complete three-barrier publication")
        completed=entry.get("completed") or {}
        if type(completed.get("acknowledged")) is not int or completed["acknowledged"]!=6 or completed.get("forced_exit") is not None:
            raise RuntimeError("basic full-trace completion oracle")
        validate_basic_result(completed,6,states)
    if not 0<len(expected)<=MAX_CASES or type(report.get("planned_cases")) is not int or report["planned_cases"]!=len(expected):
        raise RuntimeError("basic complete fault inventory changed/exceeded")
    return expected

def validate_basic_report(report):
    expected=basic_expected(report)
    partition=report.get("partition");parts=report["partitions"]
    if type(partition) is not int:
        raise RuntimeError("basic unmerged partition identity")
    validate_partitions(partition,parts)
    rows=report.get("results")
    if not isinstance(rows,list) or type(report.get("cases")) is not int or report["cases"]!=len(rows):
        raise RuntimeError("basic case count differs")
    ordinals=set()
    for row in rows:
        if not isinstance(row,dict) or any(type(row.get(key)) is not int for key in ("cut","acknowledged")):
            raise RuntimeError("basic fault row types")
        ordinal=row.get("ordinal")
        if type(ordinal) is not int or ordinal not in range(partition,len(expected),parts) or ordinal in ordinals:
            raise RuntimeError("basic missing/misrouted/duplicate fault ordinal")
        policy,phase,cut,op,acknowledged,states=expected[ordinal]
        if (row.get("policy"),row.get("phase"),row.get("cut"),row.get("operation"),row.get("acknowledged"))!=(policy,phase,cut,op,acknowledged):
            raise RuntimeError("basic cut/acknowledgement differs from complete trace")
        if type(row.get("forced_exit")) is not int or row["forced_exit"]==0:
            raise RuntimeError("basic writer termination not confirmed")
        validate_basic_result(row,acknowledged,states);ordinals.add(ordinal)
    if ordinals!=set(range(partition,len(expected),parts)):
        raise RuntimeError("basic partition omits required fault points")


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
        self.context=dict(operation=args[1],policy=args[4],phase=args[5],pid=self.process.pid)

    def read(self):
        while True:
            if self.error:
                raise RuntimeError("probe output reader failed") from self.error
            remaining=min(self.end,DEADLINE)-time.monotonic()
            if remaining<=0:
                limit="overall campaign" if DEADLINE<=self.end else "individual process"
                raise TimeoutError(limit+" deadline: "+json.dumps(dict(**self.context,
                    returncode=self.process.poll(),campaign_seconds=180,process_seconds=15)))
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

def execute(binary,root,token,policy,phase,cut,known_states=None):
    args=[str(binary),"write",str(root),token,policy,phase,"0"]
    child=Child(args)
    events=[]
    acknowledged=0
    generations=(6,8) if phase=="compact" else ((4,6) if phase=="files" or phase in BASIC_PHASES else ((2,4) if phase=="checkpoint" else (1,2)))
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
        expected_keys={"recovered","continued","state_digest","continued_digest"} if phase in BASIC_PHASES else {"recovered","continued"}
        if set(result)!=expected_keys or not acknowledged<=result["recovered"]<=generations[-1] or result["continued"]<=result["recovered"]:
            raise RuntimeError("invalid recovery/continuation result")
        if phase in BASIC_PHASES:
            validate_basic_result(result,acknowledged,known_states if cut is not None else basic_states(events))
        recover.finish()
    finally:
        recover.close()
    return events,dict(acknowledged=acknowledged,forced_exit=forced,**result)

def case(binary,policy,phase,cut,known_states=None):
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
        events,result=execute(binary,root,token,policy,phase,cut,known_states)
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

def validate_partitions(partition,partitions):
    if partitions not in (1,2) or not 0<=partition<partitions:
        raise RuntimeError("partition bounds: one full campaign or two disjoint partitions required")

def merge_reports(reports):
    if len(reports)!=2:
        raise RuntimeError("exactly two bounded Windows campaign reports required")
    first=reports[0]
    if first.get("platform")!="nt" or first.get("partitions")!=2:
        raise RuntimeError("partition merge requires Windows reports")
    shared=("schema","commit","platform","partitions","planned_cases","coverage","scope","limits","campaign")
    if first.get("schema") not in (2,3) or not first.get("commit") or not first.get("complete"):
        raise RuntimeError("incomplete or invalid partition report")
    for report in reports:
        if any(report.get(key)!=first.get(key) for key in shared) or not report.get("complete"):
            raise RuntimeError("partition identities/coverage/limits differ or are incomplete")
    if first["schema"]==3:
        for report in reports:
            if report.get("complete") is not True:
                raise RuntimeError("basic complete flag type")
            validate_basic_report(report)
    elif first.get("campaign") is not None:
        raise RuntimeError("schema-2 report cannot claim another campaign")
    if {report.get("partition") for report in reports}!={0,1}:
        raise RuntimeError("missing or repeated campaign partition")
    expected=[]
    for coverage in first["coverage"]:
        for cut,event in enumerate(coverage["events"],1):
            expected.append((coverage["policy"],coverage["phase"],cut,event["op"]))
    if not 0<len(expected)<=MAX_CASES or len(expected)!=first["planned_cases"]:
        raise RuntimeError("partition planned coverage exceeds budget or changed")
    result={}
    for report in reports:
        if report.get("cases")!=len(report["results"]):
            raise RuntimeError("partition result count differs")
        for row in report["results"]:
            ordinal=row.get("ordinal")
            if type(ordinal) is not int or not 0<=ordinal<len(expected) or ordinal in result or ordinal%2!=report["partition"]:
                raise RuntimeError("overlapping/misrouted/missing fault case")
            if (row["policy"],row["phase"],row["cut"],row["operation"])!=expected[ordinal]:
                raise RuntimeError("partition fault identity differs from complete trace")
            if row.get("forced_exit") in (None,0) or not row["acknowledged"]<=row["recovered"]<row["continued"]:
                raise RuntimeError("partition recovery oracle failed")
            result[ordinal]=row
    if set(result)!=set(range(len(expected))):
        raise RuntimeError("partition union omits fault points")
    merged=dict(first)
    merged.update(partition=None,cases=len(result),results=[result[i] for i in range(len(expected))],
                  partition_elapsed_seconds=[r["elapsed_seconds"] for r in reports],
                  elapsed_seconds=sum(r["elapsed_seconds"] for r in reports))
    return merged

def main():
    if os.environ.get("GITHUB_ACTIONS")!="true" or os.environ.get("RUNNER_ENVIRONMENT")!="github-hosted":
        raise RuntimeError("disposable hosted VM required")
    parser=argparse.ArgumentParser()
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--build",type=Path,default=Path("build-store-crash"))
    parser.add_argument("--partition",type=int,default=0)
    parser.add_argument("--partitions",type=int,default=1)
    parser.add_argument("--merge",type=Path,nargs=2)
    parser.add_argument("--campaign",choices=("legacy","basic"),default="legacy")
    args=parser.parse_args()
    if args.merge:
        reports=[]
        for path in args.merge:
            if not path.is_file() or path.stat().st_size>4*1024*1024:
                raise RuntimeError("partition report file budget")
            reports.append(json.loads(path.read_text(encoding="utf-8")))
        report=merge_reports(reports)
        with args.output.open("x",encoding="utf-8") as output:
            json.dump(report,output,indent=2);output.write("\n")
        summary=dict(commit=report["commit"],platform=report["platform"],cases=report["cases"],
            partitions=2,outcome="complete disjoint union; every acknowledged byte recovered and continued")
        if "campaign" in report:
            summary["campaign"]=report["campaign"]
        print(json.dumps(summary))
        return
    validate_partitions(args.partition,args.partitions)
    if args.campaign=="basic" and os.name!="nt":
        raise RuntimeError("basic process campaign requires a disposable hosted Windows VM")
    if args.partitions==2 and os.name!="nt":
        raise RuntimeError("split campaign is a Windows hosted fixture only")
    binary=(args.build/("Release" if os.name=="nt" else "")/
            ("flitzfs_store_crash_probe.exe" if os.name=="nt" else "flitzfs_store_crash_probe")).resolve()
    root=Path(__file__).resolve().parents[2]
    head=subprocess.run(["git","-C",str(root),"rev-parse","HEAD"],check=True,capture_output=True,text=True,timeout=5).stdout.strip()
    rows=[];coverage=[]
    report=dict(schema=2,commit=head,platform=os.name,cases=0,coverage=coverage,results=rows,complete=False,
        partition=args.partition,partitions=args.partitions,planned_cases=0,
        scope="Process termination at I/O boundaries, including reverted native extent reuse, owned ordinary files; not physical power loss or mounted writes",
        limits=dict(partition_seconds=180,campaign_seconds=180*args.partitions,process_seconds=15,
                    cases=1024,events=256,event_bytes=256,queued_events=8,fixture_bytes=3145728,
                    windows_process_bytes=805306368,windows_cpu_percent=20,
                    posix_address_bytes=536870912,posix_cpu_seconds=15))
    if args.campaign=="basic":
        report.update(schema=3,campaign="basic",scope=BASIC_SCOPE)
    started=time.monotonic()
    try:
        ordinal=0
        for policy in ("raw","auto"):
            for phase in (BASIC_PHASES if args.campaign=="basic" else ("mutation","repair","files","compact","checkpoint")):
                report["active_case"]=dict(policy=policy,phase=phase,cut=None)
                events,result=case(binary,policy,phase,None)
                coverage.append(dict(policy=policy,phase=phase,events=events,completed=result))
                states=basic_states(events) if args.campaign=="basic" else None
                for cut in range(1,len(events)+1):
                    if ordinal>=MAX_CASES:
                        raise RuntimeError("aggregate fault case budget")
                    current=ordinal;ordinal+=1
                    if current%args.partitions!=args.partition:
                        continue
                    report["active_case"]=dict(policy=policy,phase=phase,cut=cut,ordinal=current)
                    observed,result=case(binary,policy,phase,cut,states)
                    if observed!=events[:cut]:
                        raise RuntimeError("fault trace changed between cases")
                    rows.append(dict(policy=policy,phase=phase,cut=cut,ordinal=current,
                                     operation=observed[-1]["op"],**result))
        report["planned_cases"]=ordinal
        if len(rows)!=len(range(args.partition,ordinal,args.partitions)):
            raise RuntimeError("fault partition coverage incomplete")
        report["cases"]=len(rows)
        if args.campaign=="basic":
            validate_basic_report(report)
        report["complete"]=True
        report.pop("active_case",None)
    except BaseException as error:
        report["complete"]=False
        report["error"]=type(error).__name__+": "+str(error)
        raise
    finally:
        report["cases"]=len(rows)
        report["elapsed_seconds"]=round(time.monotonic()-started,3)
        with args.output.open("x",encoding="utf-8") as output:
            json.dump(report,output,indent=2);output.write("\n")
        print(json.dumps({key:report.get(key) for key in
            ("commit","platform","campaign","cases","planned_cases","partition","partitions","complete",
             "elapsed_seconds","active_case","error")}),flush=True)

if __name__=="__main__":
    main()
