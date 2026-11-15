#!/usr/bin/env python3
"""Use a pinned installed CocoIndex CLI with a private, source-bound cache.

Invoke through the resource-bounded development runner. No package/model
installation, shared MCP registration, shared cache writes or online embedding.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
import uuid


def digest(data):
    return hashlib.sha256(data).hexdigest()


def save(path, value):
    with path.open("x", encoding="utf-8") as file:
        json.dump(value, file, indent=2)
        file.write("\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, required=True)
    parser.add_argument("--ccc", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--scope", choices=("documentation", "code"), required=True)
    parser.add_argument("--query", required=True)
    parser.add_argument("--reuse-cache", action="store_true",
                        help="Reuse only an exactly unchanged private source snapshot")
    args = parser.parse_args()
    repo, ccc, model = (p.resolve(strict=True) for p in (args.repository, args.ccc, args.model))
    if not ccc.is_file() or not model.is_dir() or not args.output.is_absolute():
        raise RuntimeError("explicit installed provider/model and absolute new output required")
    root = args.output
    root.mkdir(exist_ok=args.reuse_cache)
    if shutil.disk_usage(root).free < 2 * 1024**3:
        raise RuntimeError("insufficient semantic cache reserve")
    git = shutil.which("git")
    if not git:
        raise RuntimeError("Git is unavailable")
    environment = dict(os.environ)
    environment.pop("PYTHONPATH", None)
    for leaf in ("config", "runtime", "cache", "mirror"):
        (root / leaf).mkdir(exist_ok=args.reuse_cache)
    environment.update(COCOINDEX_CODE_DIR=str(root / "config"),
                       COCOINDEX_CODE_RUNTIME_DIR=str(root / "runtime"),
                       HF_HOME=str(root / "cache"), HF_HUB_OFFLINE="1", TRANSFORMERS_OFFLINE="1",
                       COCOINDEX_DISABLE_USAGE_TRACKING="1", HF_HUB_DISABLE_TELEMETRY="1",
                       DO_NOT_TRACK="1", TOKENIZERS_PARALLELISM="false", OMP_NUM_THREADS="2",
                       MKL_NUM_THREADS="2", OPENBLAS_NUM_THREADS="2", NUMEXPR_NUM_THREADS="2",
                       CUDA_VISIBLE_DEVICES="", HIP_VISIBLE_DEVICES="", ROCR_VISIBLE_DEVICES="",
                       PYTHONNOUSERSITE="1", PYTHONDONTWRITEBYTECODE="1", NO_COLOR="1", TERM="dumb")
    commands = []
    offset = max((int(p.stem.split("-")[1]) for p in root.glob("command-*.stdout")), default=-1) + 1

    def run(command, cwd=repo, timeout=60):
        started = time.monotonic()
        result = subprocess.run([str(p) for p in command], cwd=cwd, env=environment,
                                capture_output=True, timeout=timeout, check=False)
        if len(result.stdout) + len(result.stderr) > 4 * 1024**2:
            raise RuntimeError("semantic output limit")
        number = offset + len(commands)
        with (root / f"command-{number}.stdout").open("xb") as file:
            file.write(result.stdout)
        with (root / f"command-{number}.stderr").open("xb") as file:
            file.write(result.stderr)
        commands.append(dict(arguments=[str(p) for p in command], exit=result.returncode,
                             elapsed_seconds=time.monotonic() - started))
        if result.returncode:
            raise RuntimeError(f"semantic command {number} failed with {result.returncode}")
        return result.stdout

    actual_root = Path(run([git, "rev-parse", "--show-toplevel"]).decode().strip()).resolve()
    if actual_root != repo:
        raise RuntimeError("source Git root mismatch")
    head = run([git, "rev-parse", "HEAD"]).decode().strip()
    patch_digest = digest(run([git, "diff", "--binary", "HEAD", "--"]))
    if run([ccc, "version"]).decode().strip() != "0.2.43":
        raise RuntimeError("installed CocoIndex version differs")
    paths = run([git, "ls-files", "-z"]).decode().split("\0")
    suffixes = {".md"} if args.scope == "documentation" else {".c", ".cpp", ".h", ".hpp", ".py", ".ps1"}
    entries, total = [], 0
    for name in sorted(p for p in paths if p and Path(p).suffix in suffixes):
        source = repo / name
        if source.is_symlink() or not source.resolve().is_relative_to(repo):
            raise RuntimeError("unsafe semantic source")
        data = source.read_bytes()
        total += len(data)
        if len(data) > 1024**2 or total > 16 * 1024**2 or len(entries) >= 512:
            raise RuntimeError("semantic source inventory budget")
        destination = root / "mirror" / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        if args.reuse_cache:
            if not destination.is_file() or destination.read_bytes() != data:
                raise RuntimeError("private cached source differs; use a fresh snapshot")
        else:
            with destination.open("xb") as file:
                file.write(data)
        entries.append(dict(path=name, bytes=len(data), sha256=digest(data)))
    if args.reuse_cache:
        if json.loads((root / "source-inventory.json").read_bytes()) != entries:
            raise RuntimeError("private source inventory differs")
    else:
        save(root / "source-inventory.json", entries)
    settings = dict(embedding=dict(provider="sentence-transformers", model=str(model), device="cpu"),
                    daemon=dict(idle_timeout_minutes=1, keep_alive_with_mcp=False))
    def settings_file(path, value):
        if not args.reuse_cache:
            save(path, value)
            return
        previous = json.loads(path.read_bytes())
        if previous != value:
            save(path.with_name(path.name + ".previous-" + uuid.uuid4().hex + ".json"), previous)
            with path.open("w", encoding="utf-8") as file:
                json.dump(value, file, indent=2)
                file.write("\n")

    settings_file(root / "config" / "global_settings.yml", settings)
    project = root / "mirror" / ".cocoindex_code"
    project.mkdir(exist_ok=args.reuse_cache)
    settings_file(project / "settings.yml", dict(include_patterns=["**/*"],
                  exclude_patterns=[".cocoindex_code/**", ".git/**"], max_file_size=1024**2))
    mirror = root / "mirror"
    try:
        indexed = run([ccc, "index"], mirror, 240).decode("utf-8", "replace")
        indexed += (root / f"command-{offset + len(commands) - 1}.stderr").read_text(encoding="utf-8", errors="replace")
        if not re.search(r"error:\s*0(?:\D|$)", indexed) or not re.search(r"Chunks:\s*[1-9][0-9]*", indexed):
            raise RuntimeError("index did not report complete successful chunks")
        response = run([ccc, "search", "--limit", "5", "--json", args.query], mirror, 90)
        native = json.loads(response)
        if native.get("success") is not True or not 0 < len(native.get("results", [])) <= 5:
            raise RuntimeError("semantic response empty/unsuccessful")
        by_path = {item["path"]: item for item in entries}
        for hit in native["results"]:
            name = hit["file_path"]
            if name not in by_path or digest((repo / name).read_bytes()) != by_path[name]["sha256"]:
                raise RuntimeError("semantic hit source drift")
            excerpt = hit["content"].replace("\r\n", "\n").strip()
            if excerpt not in (repo / name).read_text(encoding="utf-8"):
                raise RuntimeError("semantic excerpt differs from source")
        for entry in entries:
            if digest((repo / entry["path"]).read_bytes()) != entry["sha256"]:
                raise RuntimeError("source inventory changed during search")
        save(root / f"search-native-{offset}.json", native)
        receipt = dict(repository=str(repo), commit=head,
             dirty_patch_sha256=patch_digest, source_files=len(entries), source_bytes=total,
             inventory_sha256=digest((root / "source-inventory.json").read_bytes()),
             provider_version="0.2.43", model=str(model), scope=args.scope, query=args.query,
             native_response_sha256=digest(response), actual_searches=1, cpu_only=True,
             offline_model=True, shared_mcp_modified=False, commands=commands)
    finally:
        run([ccc, "daemon", "stop"], mirror, 20)
    receipt_path = root / f"routing-receipt-{offset}.json"
    save(receipt_path, receipt)
    print(json.dumps(dict(repository=str(repo), commit=head, files=len(entries),
                          receipt=str(receipt_path),
                          hits=[dict(path=h["file_path"], score=h["score"]) for h in native["results"]])))


if __name__ == "__main__":
    main()
