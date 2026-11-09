#!/usr/bin/env python3
"""Capture official dependency releases and installed build tools off-host."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from prepare_hosted_tools import ARTIFACTS

REPO="strmt7/FlitzFS"
PINS=(
    ("meta/zstd","v1.5.7","f8745da6ff1ad1e7bab384bd1f9d742439278e99"),
    ("lz4/lz4","v1.11.0","c72c7c0ec132ad2ce2fe9fad7b0c728d9faee2fc"),
    ("BLAKE3-team/BLAKE3","1.8.7","f3149ec5bb5449af877ba20377a11008ff499fa2"),
    ("winfsp/winfsp","v2.1",None),
    ("actions/checkout","v7.0.1","3d3c42e5aac5ba805825da76410c181273ba90b1"),
    ("actions/setup-python","v7.1.0","6d4fd22b20465ccc0f7354476588e8df7c9ca0e0"),
    ("actions/upload-artifact","v7.0.2","cf430e030ddbb5b0abf93d22962f4752f3646cd9"),
)
class Capture:
    def __init__(self,root):
        self.root=root;self.receipts=[];self.end=time.monotonic()+180
        self.token=os.environ.get("GH_TOKEN")
    def api(self,path):
        if time.monotonic()>self.end:
            raise TimeoutError("dependency audit time budget")
        url="https://api.github.com/"+path
        if not path.startswith(("repos/","repositories/")) or "\n" in path:
            raise RuntimeError("unsupported API scope")
        headers={"Accept":"application/vnd.github+json","X-GitHub-Api-Version":"2022-11-28",
                 "User-Agent":"FlitzFS-hosted-dependency-audit"}
        if self.token:
            headers["Authorization"]="Bearer "+self.token
        request=urllib.request.Request(url,headers=headers)
        try:
            response=urllib.request.urlopen(request,timeout=15)
        except urllib.error.HTTPError as error:
            response=error
        with response:
            status=response.status
            body=response.read(1024*1024+1)
            link=response.headers.get("Link","")
        if len(body)>1024*1024:
            raise RuntimeError("dependency API response budget")
        name="api-"+str(len(self.receipts)+1)+".json"
        (self.root/name).write_bytes(body)
        self.receipts.append(dict(url=url,status=status,native_file=name,
            sha256=hashlib.sha256(body).hexdigest(),
            captured_utc=datetime.datetime.now(datetime.timezone.utc).isoformat()))
        (self.root/"receipts.json").write_text(json.dumps(self.receipts,indent=2)+"\n",encoding="utf-8")
        return status,json.loads(body),link
    def tag(self,repo,tag):
        path="repos/"+repo+"/git/ref/tags/"+urllib.parse.quote(tag,safe="")
        for _ in range(3):
            status,value,_=self.api(path)
            if status!=200:
                return None
            item=value["object"]
            if item["type"]=="commit":
                return item["sha"]
            if item["type"]!="tag":
                return None
            path="repos/"+repo+"/git/tags/"+item["sha"]
        raise RuntimeError("annotated tag depth budget")

def command_version(command):
    path=shutil.which(command)
    if not path:
        return None
    result=subprocess.run([path,"--version"],capture_output=True,text=True,timeout=5,check=True)
    return result.stdout[:4096].splitlines()[:3]

def main():
    if os.environ.get("GITHUB_ACTIONS")!="true" or os.environ.get("RUNNER_ENVIRONMENT")!="github-hosted":
        raise RuntimeError("disposable hosted VM required; never the owner PC")
    parser=argparse.ArgumentParser();parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args();root=args.output.resolve()
    temporary=Path(os.environ["RUNNER_TEMP"]).resolve()
    if not root.is_relative_to(temporary) or root==temporary or shutil.disk_usage(temporary).free<4*1024**3:
        raise RuntimeError("fresh bounded hosted output/headroom required")
    root.mkdir(exist_ok=False)
    capture=Capture(root);rows=[]
    for repo,declared,pin in PINS:
        status,value,_=capture.api("repos/"+repo+"/releases/latest")
        if status!=200 or value.get("prerelease") or value.get("draft"):
            raise RuntimeError("official stable release unavailable: "+repo)
        tag=value["tag_name"];resolved=capture.tag(repo,tag)
        rows.append(dict(repository=repo,declared_tag=declared,latest_stable=tag,
            declared_commit=pin,latest_commit=resolved,
            current=tag==declared and (pin is None or pin==resolved),
            release_url=value["html_url"],published_at=value["published_at"]))
    tools={}
    for repo in ("Kitware/CMake","llvm/llvm-project"):
        status,value,_=capture.api("repos/"+repo+"/releases/latest")
        if status!=200 or value.get("prerelease") or value.get("draft"):
            raise RuntimeError("stable build-tool release unavailable")
        tools[repo]=dict(latest_stable=value["tag_name"],release_url=value["html_url"])
        tool="cmake" if repo=="Kitware/CMake" else "llvm"
        for (kind,platform),artifact in ARTIFACTS.items():
            if kind!=tool:
                continue
            artifact_version,url,size,digest=artifact
            asset=next((a for a in value["assets"] if a["browser_download_url"]==url),None)
            if asset is None or asset["size"]!=size or asset.get("digest")!="sha256:"+digest:
                raise RuntimeError("official tool artifact pin differs from latest release identity")
            if value["tag_name"] not in ("v"+artifact_version,"llvmorg-"+artifact_version):
                raise RuntimeError("staged build-tool version behind latest stable")
    report=dict(experimental=True,commit=os.environ["GITHUB_SHA"],
        scope="Known native dependency pins and Actions; installed-tool inventory. Dependabot is checked separately in authenticated GitHub Security UI.",
        dependencies=rows,build_tool_releases=tools,
        installed=dict(python=sys.version,cmake=command_version("cmake"),ctest=command_version("ctest"),clang=command_version("clang++")),
        declared_tool_work_remaining=dict(reviewed_research_python="3.13",
            note="Research-provider lock/runtime freshness and broader portability qualification remain open"),
        dependabot=dict(scope="Not queried by this workflow; authenticated GitHub Security UI is authoritative"),
        native_api_receipts=capture.receipts)
    (root/"audit.json").write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
    print(json.dumps(dict(commit=report["commit"],pins_current=all(x["current"] for x in rows),
        build_tools=tools,installed=report["installed"])))
    if not all(row["current"] for row in rows):
        raise RuntimeError("Dependency/action pin is behind latest stable or tag identity differs")
if __name__=="__main__":
    main()
