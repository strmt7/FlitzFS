#!/usr/bin/env python3
"""Disposable-hosted CocoIndex routing; no owner-PC or product-runtime use."""
import argparse
import datetime
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import stat
import subprocess
import sys
import time
import urllib.parse
import urllib.request
import venv
import zipfile

MIB=1024**2
GIB=1024**3
VERSION="0.2.43"
WHEEL_URL="https://github.com/cocoindex-io/cocoindex-code/releases/download/v0.2.43/cocoindex_code-0.2.43-py3-none-any.whl"
WHEEL_SHA="76b23b3c176f3cdace1f51ea1a332f4285e74fd296449e827be2134dbbdc885d"
MODEL="Snowflake/snowflake-arctic-embed-xs"
QUERIES=(
    "native filesystem inode namespace sparse extent mutation commit and metadata encoding",
    "transactional object store record integrity replicated checkpoints and durable publication",
    "GPU worker supervision CPU fallback recovery configuration and runtime failure",
    "compression codec selection allocation budget stored decoded digest and decompression",
    "compacted copy source preservation destination identity and safe reclamation",
)
SUFFIXES={".c",".h",".cc",".cpp",".hpp",".cxx",".hip",".py",".ps1",".rs"}
def require(ok,why):
    if not ok: raise RuntimeError(why)
def encoded(value):
    return (json.dumps(value,sort_keys=True,ensure_ascii=True,indent=2)+"\n").encode()
def save(path,value):
    with path.open("xb") as file: file.write(encoded(value))
def digest(data):
    return hashlib.sha256(data).hexdigest()
def read_json(path,limit=16*MIB):
    require(path.stat().st_size<=limit,"JSON receipt exceeds budget")
    return json.loads(path.read_bytes())
def cgroup():
    require(sys.platform=="linux" and platform.machine().lower() in ("x86_64","amd64") and
        os.environ.get("GITHUB_ACTIONS")=="true" and os.environ.get("RUNNER_ENVIRONMENT")=="github-hosted",
        "semantic execution requires disposable hosted Linux x86-64; never the owner PC")
    groups=Path("/proc/self/cgroup").read_text().splitlines()
    require(len(groups)==1 and groups[0].startswith("0::/"),"bounded cgroup-v2 execution required")
    group=Path("/sys/fs/cgroup")/groups[0].split("::",1)[1].lstrip("/")
    memory=(group/"memory.max").read_text().strip()
    tasks=(group/"pids.max").read_text().strip()
    quota,period=(group/"cpu.max").read_text().split()
    swap=(group/"memory.swap.max").read_text().strip()
    require(memory!="max" and 0<int(memory)<=3*GIB and tasks!="max" and 0<int(tasks)<=256 and
        quota!="max" and 0<int(quota)<=2*int(period) and swap=="0","process-tree resource limits absent")
    return dict(memory_bytes=int(memory),tasks=int(tasks),cpu_quota=[int(quota),int(period)],swap_bytes=0)
class Provider:
    def __init__(self,root,repo):
        self.root=root;self.repo=repo;self.downloads=[];self.commands=[];self.package_bytes=0
        self.expanded_bytes=0;self.wheel_entries=0;self.log_number=0
        self.end=time.monotonic()+1150
        for leaf in ("home","temp","cache","packages","model","mirror","config","runtime"):
            (root/leaf).mkdir()
        self.env={key:os.environ[key] for key in ("PATH","LD_LIBRARY_PATH","HTTPS_PROXY","HTTP_PROXY","NO_PROXY")
                  if key in os.environ}
        self.env.update(HOME=str(root/"home"),TMPDIR=str(root/"temp"),XDG_CACHE_HOME=str(root/"cache"),
            HF_HOME=str(root/"cache/huggingface"),COCOINDEX_CODE_DIR=str(root/"config"),
            COCOINDEX_CODE_RUNTIME_DIR=str(root/"runtime"),COCOINDEX_DISABLE_USAGE_TRACKING="1",
            HF_HUB_DISABLE_TELEMETRY="1",DO_NOT_TRACK="1",LITELLM_TELEMETRY="False",
            TOKENIZERS_PARALLELISM="false",OMP_NUM_THREADS="2",MKL_NUM_THREADS="2",
            OPENBLAS_NUM_THREADS="2",NUMEXPR_NUM_THREADS="2",CUDA_VISIBLE_DEVICES="",
            HIP_VISIBLE_DEVICES="",ROCR_VISIBLE_DEVICES="",PYTHONNOUSERSITE="1",
            PYTHONDONTWRITEBYTECODE="1",PIP_CONFIG_FILE="/dev/null",NO_COLOR="1",TERM="dumb")
        venv.EnvBuilder(with_pip=True).create(root/"venv")
        self.python=root/"venv/bin/python"
        self.ccc=root/"venv/bin/ccc"
    def run(self,args,seconds=180,cwd=None,json_output=False):
        require(time.monotonic()<self.end,"semantic overall deadline")
        require(shutil.disk_usage(self.root).free>=4*GIB,"semantic recovery disk reserve")
        self.log_number+=1
        path=self.root/("command-"+str(self.log_number)+".log")
        start=time.monotonic()
        error_path=self.root/("command-"+str(self.log_number)+".stderr.log")
        with path.open("xb") as output,error_path.open("xb") as errors:
            result=subprocess.run([str(a) for a in args],cwd=cwd or self.root,env=self.env,
                stdout=output,stderr=errors if json_output else subprocess.STDOUT,
                timeout=min(seconds,max(1,int(self.end-time.monotonic()))))
        require(path.stat().st_size+error_path.stat().st_size<=16*MIB,"command output exceeds budget")
        data=path.read_bytes()
        self.commands.append(dict(argv=[str(a) for a in args],returncode=result.returncode,
            log=path.name,sha256=digest(data),stderr_log=error_path.name,
            stderr_sha256=digest(error_path.read_bytes()),seconds=round(time.monotonic()-start,3)))
        save(self.root/"command-receipt-current.json" if self.log_number==1 else
            self.root/("command-receipt-"+str(self.log_number)+".json"),self.commands[-1])
        if result.returncode:
            print((data+error_path.read_bytes())[-8192:].decode("utf-8","replace"),flush=True)
            raise RuntimeError("isolated command failed; native log retained: "+path.name)
        return data
    def pip(self,arguments,seconds=360):
        return self.run([self.python,"-m","pip","--isolated","--disable-pip-version-check","--no-cache-dir",*arguments],seconds)
    def download(self,url,target,limit,sha=None,size=None,git_blob=None):
        require(time.monotonic()<self.end,"download overall deadline")
        require(shutil.disk_usage(self.root).free>=4*GIB+min(limit,size if size is not None else limit),
            "download reserve unavailable")
        parsed=urllib.parse.urlsplit(url)
        require(parsed.scheme=="https" and parsed.hostname in
            ("github.com","files.pythonhosted.org","download.pytorch.org","download-r2.pytorch.org","huggingface.co"),
            "unreviewed download origin")
        request=urllib.request.Request(url,headers={"User-Agent":"FlitzFS-isolated-development"})
        count=0;hash256=hashlib.sha256()
        hash1=hashlib.sha1(("blob "+str(size)+"\0").encode()) if git_blob else None
        started=time.monotonic()
        try:
            with urllib.request.urlopen(request,timeout=30) as response,target.open("xb") as output:
                status=response.status
                require(urllib.parse.urlsplit(response.geturl()).scheme=="https","download redirected away from TLS")
                require(status==200,"native download HTTP failure")
                declared=response.headers.get("Content-Length")
                require(not declared or int(declared)<=limit,"declared download exceeds cap")
                while True:
                    require(time.monotonic()-started<240 and time.monotonic()<self.end,"native download deadline")
                    chunk=response.read(min(MIB,limit-count+1))
                    if not chunk: break
                    count+=len(chunk);require(count<=limit,"native download exceeds cap")
                    output.write(chunk);hash256.update(chunk)
                    if hash1:hash1.update(chunk)
            require(size is None or count==size,"native download size differs")
            require(sha is None or hash256.hexdigest()==sha,"native package digest differs")
            require(not git_blob or hash1.hexdigest()==git_blob,"native Git blob identity differs")
            row=dict(url=url,http_status=status,success=True,bytes=count,sha256=hash256.hexdigest(),
                file=str(target.relative_to(self.root)),captured_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
            self.downloads.append(row)
            save(self.root/("download-"+str(len(self.downloads))+".json"),row)
            return row
        except Exception as error:
            row=dict(url=url,success=False,error=str(error),received_bytes=count,
                retained_bytes=target.stat().st_size if target.exists() else 0,
                partial_sha256=hash256.hexdigest(),file=str(target.relative_to(self.root)))
            self.downloads.append(row)
            save(self.root/("download-"+str(len(self.downloads))+".json"),row)
            raise
    def native_json(self,url,name):
        path=self.root/name
        self.download(url,path,4*MIB)
        return read_json(path,4*MIB)
    def wheel_admit(self,path):
        require(path.suffix==".whl","only wheels are admitted")
        with zipfile.ZipFile(path) as archive:
            entries=archive.infolist();self.wheel_entries+=len(entries)
            require(len(entries)<=20000 and self.wheel_entries<=100000,"wheel entry budget")
            seen=set();expanded=0
            for entry in entries:
                name=entry.filename;pure=PurePosixPath(name)
                require(name and not pure.is_absolute() and "\\" not in name and ":" not in name and
                    all(x not in ("",".","..") for x in pure.parts) and pure.as_posix()==name.rstrip("/") and name not in seen,
                    "unsafe or duplicate wheel path")
                seen.add(name)
                kind=stat.S_IFMT(entry.external_attr>>16)
                require(kind in (0,stat.S_IFREG,stat.S_IFDIR),"wheel link/special entry rejected")
                require(0<=entry.file_size<=GIB,"wheel member budget")
                expanded+=entry.file_size
            require(expanded<=2*GIB and self.expanded_bytes+expanded<=4*GIB,"wheel expanded budget")
            self.expanded_bytes+=expanded
    def provision(self):
        coco=self.root/"packages/cocoindex_code-0.2.43-py3-none-any.whl"
        self.download(WHEEL_URL,coco,MIB,WHEEL_SHA,96682)
        cpu_report=self.root/"cpu-resolver.json"
        self.pip(["install","--dry-run","--only-binary=:all:","--report",cpu_report,
            "--index-url","https://download.pytorch.org/whl/cpu","torch"])
        items=read_json(cpu_report)["install"]
        torch=next(x["metadata"]["version"] for x in items if x["metadata"]["name"].lower()=="torch")
        require(re.fullmatch(r"[A-Za-z0-9.+!-]+",torch) and "+cpu" in torch,"CPU-only Torch wheel unavailable for this interpreter")
        constraints=self.root/"cpu-constraints.txt"
        with constraints.open("x",encoding="utf-8") as file:file.write("torch=="+torch+"\n")
        report=self.root/"provider-resolver.json"
        self.pip(["install","--dry-run","--ignore-installed","--only-binary=:all:","--report",report,
            "--index-url","https://pypi.org/simple","--extra-index-url","https://download.pytorch.org/whl/cpu",
            "-c",constraints,"cocoindex-code[full] @ "+coco.as_uri(),"pip"],480)
        packages=read_json(report)["install"]
        require(0<len(packages)<=220,"provider package-count budget")
        lines=[];identities=[]
        for package in packages:
            metadata=package["metadata"];name=metadata["name"].lower().replace("_","-")
            require(re.fullmatch(r"[a-z0-9][a-z0-9.-]*",name) and
                not name.startswith(("nvidia-","cuda-","triton","rocm-")),"GPU runtime package rejected")
            info=package["download_info"];url=info["url"];sha=info["archive_info"]["hashes"]["sha256"]
            require(re.fullmatch(r"[0-9a-f]{64}",sha),"unqualified wheel digest")
            if name=="torch":
                require(metadata["version"]==torch and "/whl/cpu/" in url,"Torch is not CPU-only")
            if name=="cocoindex-code":
                require(metadata["version"]==VERSION and sha==WHEEL_SHA and url==coco.as_uri(),"CocoIndex pin differs")
                path=coco
            else:
                leaf=urllib.parse.unquote(urllib.parse.urlsplit(url).path).rsplit("/",1)[-1]
                require(leaf and leaf.endswith(".whl") and "/" not in leaf and "\\" not in leaf and ":" not in leaf,
                    "unsafe wheel name")
                path=self.root/"packages"/leaf
                receipt=self.download(url,path,GIB,sha)
                self.package_bytes+=receipt["bytes"]
                require(self.package_bytes<=2*GIB,"aggregate wheel download budget")
            self.wheel_admit(path)
            lines.append(name+" @ "+path.as_uri()+" --hash=sha256:"+sha)
            identities.append(dict(name=name,version=metadata["version"],sha256=sha,native_url=url))
        lock=self.root/"provider.lock"
        with lock.open("x",encoding="utf-8") as file:file.write("\n".join(lines)+"\n")
        self.pip(["install","--no-index","--no-deps","--require-hashes","-r",lock],480)
        self.pip(["check"],60)
        data=self.run([self.python,"-c",
            "import importlib.metadata as m,json,torch; "
            "assert torch.version.cuda is None and torch.version.hip is None; "
            "print(json.dumps({'torch':m.version('torch'),'cocoindex-code':m.version('cocoindex-code'),"
            "'pip':m.version('pip'),'cpu_only':True}))"],60)
        print(data.decode().strip(),flush=True)
        require(self.run([self.ccc,"version"],30).decode().strip()==VERSION,"CocoIndex executable version differs")
        save(self.root/"packages.json",identities)
    def model(self):
        info=self.native_json("https://huggingface.co/api/models/"+MODEL,"model-current.json")
        revision=info["sha"];require(re.fullmatch(r"[0-9a-f]{40}",revision),"model commit unavailable")
        info=self.native_json("https://huggingface.co/api/models/"+MODEL+"/revision/"+revision+"?blobs=true","model-pinned.json")
        require(info["sha"]==revision,"model revision changed")
        rows=[];total=0
        for sibling in info["siblings"]:
            name=sibling["rfilename"];pure=PurePosixPath(name)
            if pure.suffix not in (".json",".txt",".safetensors"):continue
            if pure.parts[0] in ("onnx","openvino"):continue
            require(not pure.is_absolute() and "\\" not in name and ":" not in name and
                all(x not in ("",".","..") for x in pure.parts),"unsafe model path")
            require(len(rows)<24,"model file-count budget")
            size=sibling["size"];require(type(size) is int and 0<size<=160*MIB,"model member budget")
            total+=size;require(total<=256*MIB,"model aggregate budget")
            path=self.root/"model"/pure
            path.parent.mkdir(parents=True,exist_ok=True)
            lfs=sibling.get("lfs")
            sha=lfs["sha256"] if lfs else None
            blob=None if lfs else sibling["blobId"]
            require(re.fullmatch(r"[0-9a-f]{64}",sha) if lfs else
                re.fullmatch(r"[0-9a-f]{40}",blob),"model file identity unavailable")
            rows.append(self.download("https://huggingface.co/"+MODEL+"/resolve/"+revision+"/"+
                urllib.parse.quote(name,safe="/"),path,160*MIB,sha,size,blob))
        names={PurePosixPath(r["file"]).name for r in rows}
        require({"config.json","modules.json","model.safetensors"}<=names and
            ("tokenizer.json" in names or "vocab.txt" in names),"model snapshot incomplete")
        modules=read_json(self.root/"model/modules.json",MIB)
        allowed={"sentence_transformers.models.Transformer","sentence_transformers.models.Pooling",
                 "sentence_transformers.models.Normalize"}
        require(isinstance(modules,list) and modules and all(x["type"] in allowed for x in modules),
            "unreviewed model module")
        save(self.root/"model-receipt.json",dict(repository=MODEL,revision=revision,files=rows,bytes=total))
        return revision
    def inventory(self):
        raw=self.run(["git","-C",self.repo,"ls-files","-z"],30)
        entries=[]
        for raw_name in raw.split(b"\0"):
            if not raw_name:continue
            name=raw_name.decode("utf-8");pure=PurePosixPath(name)
            if pure.parts[0] in ("tests","third_party",".github"):continue
            if pure.suffix not in SUFFIXES and pure.name!="CMakeLists.txt":continue
            require(not pure.is_absolute() and "\\" not in name and
                all(x not in ("",".","..") for x in pure.parts),"unsafe source path")
            source=self.repo/pure
            require(not source.is_symlink() and source.is_file() and source.resolve().is_relative_to(self.repo),
                "source path escaped repository")
            data=source.read_bytes()
            require(len(data)<=MIB and b"\0" not in data,"source file outside text/size bound")
            entries.append(dict(path=name,bytes=len(data),sha256=digest(data)))
        entries.sort(key=lambda x:x["path"])
        require(0<len(entries)<=512 and sum(x["bytes"] for x in entries)<=16*MIB,"source inventory budget")
        return entries
    def route(self,revision):
        entries=self.inventory();binding=digest(encoded(entries))
        save(self.root/"source-inventory.json",entries)
        for entry in entries:
            target=self.root/"mirror"/entry["path"];target.parent.mkdir(parents=True,exist_ok=True)
            data=(self.repo/entry["path"]).read_bytes();require(digest(data)==entry["sha256"],"source changed before copy")
            with target.open("xb") as file:file.write(data)
        global_settings=dict(embedding=dict(provider="sentence-transformers",model=str(self.root/"model"),device="cpu"),
            daemon=dict(idle_timeout_minutes=1,keep_alive_with_mcp=False))
        project=dict(include_patterns=["**/*"],exclude_patterns=[".cocoindex_code/**"],
            language_overrides=[dict(ext="hip",lang="cpp")],max_file_size=MIB)
        save(self.root/"config/global_settings.yml",global_settings)
        (self.root/"mirror/.cocoindex_code").mkdir()
        save(self.root/"mirror/.cocoindex_code/settings.yml",project)
        config_digest=digest(encoded([global_settings,project]))
        indexed=self.run([self.ccc,"index"],360,self.root/"mirror").decode("utf-8","replace")
        require(re.search(r"error:\s*0(?:\D|$)",indexed) is not None and
            re.search(r"Chunks:\s*[1-9][0-9]*",indexed) is not None,"index did not report successful chunks")
        print(indexed[-2200:],flush=True)
        by_path={entry["path"]:entry for entry in entries};receipts=[]
        try:
            for index,query in enumerate(QUERIES):
                raw=self.run([self.ccc,"search","--limit","20","--json",query],120,self.root/"mirror",json_output=True)
                require(len(raw)<=2*MIB,"semantic response budget")
                response=json.loads(raw)
                require(response.get("success") is True and isinstance(response.get("results"),list) and
                    0<len(response["results"])<=20,"semantic query failed or returned no results")
                hits=[]
                for hit in response["results"]:
                    name=hit["file_path"];start=hit["start_line"];end=hit["end_line"]
                    require(name in by_path and type(start) is int and type(end) is int and 1<=start<=end and
                        isinstance(hit["score"],(int,float)) and math.isfinite(hit["score"]) and
                        isinstance(hit["content"],str) and len(hit["content"].encode())<=128*1024,
                        "invalid semantic candidate")
                    source=(self.repo/name).read_text(encoding="utf-8")
                    require(end<=len(source.splitlines())+1 and hit["content"].strip() in source,
                        "semantic candidate does not match live source")
                    hits.append(dict(path=name,start_line=start,end_line=end,score=hit["score"],
                        file_sha256=by_path[name]["sha256"],excerpt=" ".join(hit["content"].split())[:240]))
                require(digest(encoded(self.inventory()))==binding,"live source changed during semantic search")
                row=dict(commit=os.environ["GITHUB_SHA"],repository="strmt7/FlitzFS",source_sha256=binding,
                    config_sha256=config_digest,provider=VERSION,provider_wheel_sha256=WHEEL_SHA,
                    model=MODEL,model_revision=revision,query=query,query_sha256=digest(query.encode()),
                    native_response_sha256=digest(raw),hits=hits,
                    completed_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
                save(self.root/("search-"+str(index)+".json"),row);receipts.append(row)
                distinct=[]
                for hit in hits:
                    if hit["path"] not in {x["path"] for x in distinct}:distinct.append(hit)
                    if len(distinct)==6:break
                print(json.dumps(dict(semantic_query=query,source_sha256=binding,hits=distinct)),flush=True)
        finally:
            self.run([self.ccc,"daemon","stop"],30,self.root/"mirror")
        save(self.root/"routing-receipt.json",dict(experimental=True,commit=os.environ["GITHUB_SHA"],
            repository="strmt7/FlitzFS",source_sha256=binding,source_files=len(entries),config_sha256=config_digest,
            provider_version=VERSION,model_revision=revision,actual_searches=len(receipts),cpu_only=True,
            shared_mcp_modified=False,commands=self.commands))
def main():
    limits=cgroup()
    parser=argparse.ArgumentParser();parser.add_argument("--output",required=True,type=Path);args=parser.parse_args()
    repo=Path(__file__).resolve().parents[2]
    temporary=Path(os.environ["RUNNER_TEMP"]).resolve(strict=True)
    root=args.output.resolve()
    require(root!=temporary and root.is_relative_to(temporary) and
        shutil.disk_usage(temporary).free>=12*GIB,"fresh hosted output and 12 GiB free headroom required")
    root.mkdir(exist_ok=False)
    save(root/"resource-limits.json",dict(limits=limits,python=sys.version,commit=os.environ["GITHUB_SHA"]))
    provider=Provider(root,repo)
    require(provider.run(["git","-C",repo,"rev-parse","HEAD"],30).decode().strip()==os.environ["GITHUB_SHA"],
        "source commit differs from workflow")
    provider.run(["git","-C",repo,"diff","--exit-code","HEAD","--"],30)
    provider.provision()
    revision=provider.model()
    provider.route(revision)
if __name__=="__main__":
    main()
