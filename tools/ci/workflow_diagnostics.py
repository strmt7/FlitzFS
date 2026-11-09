#!/usr/bin/env python3
"""Bounded native Actions/check diagnostics; disposable hosted runner only."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys
import time
import urllib.error
import urllib.request

REPO="strmt7/FlitzFS"
LIMIT=1024*1024
class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self,request,fp,code,msg,headers,newurl):
        return None

class Capture:
    def __init__(self,root):
        self.root=root
        self.receipts=[]
        self.until=time.monotonic()+70
        self.opener=urllib.request.build_opener(NoRedirect)
    def get(self,path):
        if len(self.receipts)>=40 or time.monotonic()>=self.until:
            raise RuntimeError("diagnostic request/time budget")
        url="https://api.github.com/repos/"+REPO+"/"+path
        headers={"Accept":"application/vnd.github+json","X-GitHub-Api-Version":"2022-11-28",
                 "User-Agent":"FlitzFS-hosted-startup-diagnostics"}
        token=os.environ.get("GH_TOKEN")
        if token:
            headers["Authorization"]="Bearer "+token
        receipt=dict(url=url,captured_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
        self.receipts.append(receipt)
        try:
            try:
                response=self.opener.open(urllib.request.Request(url,headers=headers),
                    timeout=min(10,max(0.1,self.until-time.monotonic())))
            except urllib.error.HTTPError as error:
                response=error
            with response:
                receipt["status"]=response.status
                body=response.read(LIMIT+1)
            if len(body)>LIMIT:
                raise RuntimeError("diagnostic response budget")
            name="native-"+str(len(self.receipts))+".json"
            with (self.root/name).open("xb") as output:
                output.write(body)
            receipt.update(native_file=name,bytes=len(body),sha256=hashlib.sha256(body).hexdigest())
            value=json.loads(body)
            return value if receipt["status"]==200 else None
        except Exception as error:
            receipt["error"]=type(error).__name__+": "+str(error)[:1024]
            return None
        finally:
            (self.root/"receipts.json").write_text(json.dumps(self.receipts,indent=2)+"\n",encoding="utf-8")

def main():
    if sys.platform!="linux" or os.environ.get("GITHUB_ACTIONS")!="true" or os.environ.get("RUNNER_ENVIRONMENT")!="github-hosted":
        raise RuntimeError("disposable hosted VM required; never the owner PC")
    import resource
    import signal
    resource.setrlimit(resource.RLIMIT_AS,(512*1024**2,512*1024**2))
    resource.setrlimit(resource.RLIMIT_CPU,(15,15))
    resource.setrlimit(resource.RLIMIT_FSIZE,(64*1024**2,64*1024**2))
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    def deadline(signum,frame):
        raise TimeoutError("diagnostic total wall deadline")
    signal.signal(signal.SIGALRM,deadline)
    parser=argparse.ArgumentParser()
    parser.add_argument("--head-sha",action="append",required=True)
    parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args()
    if not 1<=len(args.head_sha)<=2 or any(not re.fullmatch("[0-9a-f]{40}",s) for s in args.head_sha):
        raise RuntimeError("one or two exact commit identities required")
    temporary=Path(os.environ["RUNNER_TEMP"]).resolve()
    root=args.output.resolve()
    if root==temporary or not root.is_relative_to(temporary) or shutil.disk_usage(temporary).free<4*1024**3:
        raise RuntimeError("fresh owned hosted output and 4 GiB reserve required")
    root.mkdir(exist_ok=False)
    capture=Capture(root)
    signal.setitimer(signal.ITIMER_REAL,80)
    report=dict(commit=os.environ["GITHUB_SHA"],complete=True,heads=[],scope=
        "Native API startup evidence only; successful capture does not mean tests passed or that annotations are exhaustive.")
    try:
        for sha in args.head_sha:
            item=dict(sha=sha,runs=[],checks=[],complete=True)
            report["heads"].append(item)
            runs=capture.get("actions/runs?head_sha="+sha+"&per_page=20")
            if not isinstance(runs,dict) or not isinstance(runs.get("workflow_runs"),list):
                item["complete"]=False
                continue
            if runs.get("total_count",0)>20:
                item["complete"]=False
            for run in runs["workflow_runs"][:20]:
                row=dict(id=run["id"],name=run["name"],status=run["status"],conclusion=run["conclusion"])
                item["runs"].append(row)
                if run["status"]=="completed" and run["conclusion"] not in ("success","skipped"):
                    jobs=capture.get("actions/runs/"+str(run["id"])+"/jobs?filter=all&per_page=100")
                    row["jobs"]=jobs
                    if not isinstance(jobs,dict) or jobs.get("total_count",0)>100:
                        item["complete"]=False
            checks=capture.get("commits/"+sha+"/check-runs?per_page=100")
            if not isinstance(checks,dict) or not isinstance(checks.get("check_runs"),list):
                item["complete"]=False
                continue
            if checks.get("total_count",0)>100:
                item["complete"]=False
            for check in checks["check_runs"]:
                if check["status"]=="completed" and check["conclusion"] not in ("success","skipped","neutral"):
                    output=check.get("output") or {}
                    row=dict(id=check["id"],name=check["name"],conclusion=check["conclusion"],
                        title=output.get("title"),summary=output.get("summary"),text=output.get("text"),
                        annotations_count=output.get("annotations_count"))
                    item["checks"].append(row)
                    if output.get("annotations_count",0):
                        row["annotations"]=capture.get("check-runs/"+str(check["id"])+"/annotations?per_page=100")
                        if not isinstance(row["annotations"],list) or output["annotations_count"]>100:
                            item["complete"]=False
        report["complete"]=all(x["complete"] for x in report["heads"]) and all(
            x.get("status")==200 and not x.get("error") for x in capture.receipts)
    except Exception as error:
        report.update(complete=False,error=type(error).__name__+": "+str(error)[:1024])
    finally:
        report["native_receipts"]=capture.receipts
        data=(json.dumps(report,indent=2)+"\n").encode()
        if len(data)>16*1024**2:
            data=(json.dumps(dict(complete=False,error="diagnostic report budget",
                native_receipts=capture.receipts),indent=2)+"\n").encode()
        with (root/"diagnostics.json").open("xb") as output:
            output.write(data)
    messages=[]
    for head in report["heads"]:
        for check in head["checks"]:
            for annotation in (check.get("annotations") or [])[:20]:
                messages.append(dict(head=head["sha"],check=check["name"],message=annotation.get("message","")[:1600]))
    print("Workflow startup diagnostics: "+json.dumps(dict(complete=report["complete"],
        heads=args.head_sha,native_responses=len(capture.receipts),annotations=messages)))
if __name__=="__main__":
    main()
