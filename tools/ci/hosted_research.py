#!/usr/bin/env python3
"""Native Crawl4AI research on bounded disposable hosted VMs only.

This product includes software developed by UncleCode (https://x.com/unclecode)
as part of Crawl4AI (https://github.com/unclecode/crawl4ai).
Never imported by the filesystem.
"""
import argparse
import asyncio
import hashlib
import importlib.metadata
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys

PROVIDER_REF = "4e6a1bb0d144611a09dcbc65276faa676e923882"
PROVIDER_VERSION = "0.9.5+flitzzip.portable2"
SOURCES = (
    ("sqlite-wal", "https://www.sqlite.org/wal.html", "checkpoint", "synchronous"),
    ("sqlite-testing", "https://www.sqlite.org/testing.html", "crash", "crash"),
    ("linux-fsync", "https://man7.org/linux/man-pages/man2/fsync.2.html", "directory", "directory"),
    ("windows-cache", "https://learn.microsoft.com/en-us/windows/win32/fileio/file-caching", "write-through", "hardware"),
    ("openssl-digest", "https://docs.openssl.org/3.5/man3/EVP_DigestInit/", "EVP_Digest", "EVP_Digest"),
)
PROCESS_SOURCES = (
    ("windows-multiple-wait", "https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjects", "WaitForMultipleObjects", "closed"),
    ("windows-crt-thread", "https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/beginthread-beginthreadex?view=msvc-170", "_beginthreadex", "CloseHandle"),
    ("windows-thread-priority", "https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadpriority", "THREAD_PRIORITY_LOWEST", "THREAD_PRIORITY_LOWEST"),
    ("windows-handle-close", "https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-closehandle", "CloseHandle", "thread"),
)
AMD_SOURCES = (
    ("amd-windows-deployment", "https://rocm.docs.amd.com/projects/install-on-windows/en/docs-7.2/conceptual/deployment-guidelines.html", "deployment", "Applications do not"),
    ("amd-windows-install", "https://rocm.docs.amd.com/projects/install-on-windows/en/latest/install/install.html", "HIP SDK", "CLI"),
    ("amd-windows-requirements", "https://rocm.docs.amd.com/projects/install-on-windows/en/latest/reference/system-requirements.html", "System requirements", "gfx1201"),
    ("amd-windows-components", "https://rocm.docs.amd.com/projects/install-on-windows/en/latest/conceptual/component-support.html", "ROCm", "Runtime"),
)


def guard():
    if os.name != "posix" or os.environ.get("GITHUB_ACTIONS") != "true" or os.environ.get("RUNNER_ENVIRONMENT") != "github-hosted":
        raise RuntimeError("Requires a disposable GitHub-hosted Linux VM")
    groups = Path("/proc/self/cgroup").read_text().strip().splitlines()
    if len(groups) != 1 or not groups[0].startswith("0::/"):
        raise RuntimeError("Explicit cgroup-v2 limits required")
    group = Path("/sys/fs/cgroup") / groups[0].split("::", 1)[1].lstrip("/")
    memory = (group / "memory.max").read_text().strip()
    pids = (group / "pids.max").read_text().strip()
    quota, period = (group / "cpu.max").read_text().split()
    if memory == "max" or not 0 < int(memory) <= 3 * 1024**3:
        raise RuntimeError("Memory cap absent/too large")
    if pids == "max" or not 0 < int(pids) <= 256:
        raise RuntimeError("Task cap absent/too large")
    if quota == "max" or not 0 < int(quota) <= 2 * int(period):
        raise RuntimeError("CPU cap absent/too large")
    return {"memory_max": int(memory), "tasks_max": int(pids), "cpu_max": [int(quota), int(period)]}


async def capture(args):
    limits = guard()
    sys.path.insert(0, str(args.provider))
    from tools.crawl4ai_research import fetch
    from crawl4ai import AsyncWebCrawler, BrowserConfig
    if importlib.metadata.version("crawl4ai") != PROVIDER_VERSION:
        raise RuntimeError("Unexpected native provider version")
    args.output.mkdir(parents=False, exist_ok=False)
    rows = []
    async with AsyncWebCrawler(config=BrowserConfig(headless=True, verbose=False, use_persistent_context=False)) as owner:
        selected = {"process": PROCESS_SOURCES, "amd": AMD_SOURCES, "durability": SOURCES}[args.source_group]
        for name, url, marker, term in selected:
            row = await fetch(owner, url, marker, args.output / (name + ".native.json"))
            row["name"] = name
            rows.append(row)
            print(json.dumps(row), flush=True)
            native = json.loads((args.output / row["capture"]).read_text(encoding="utf-8"))
            result = native.get("native_result") or {}
            print(json.dumps({"native_location": name, "result_url": result.get("url"),
                              "redirected_url": result.get("redirected_url"),
                              "native_error": str(result.get("error_message", ""))[:400]}), flush=True)
            if row["success"]:
                markdown = result.get("markdown") or ""
                if isinstance(markdown, dict):
                    markdown = markdown.get("raw_markdown", "")
                markdown = str(markdown)
                with (args.output / (name + ".md")).open("x", encoding="utf-8") as out:
                    out.write(markdown)
                if args.source_group == "amd":
                    links = sorted(set(re.findall(r"\]\((https://rocm\.docs\.amd\.com/[^\s)#]+)", markdown)))
                    selected_links = [url for url in links if any(word in url for word in
                                      ("deployment", "license", "release-notes", "system-requirements"))][:24]
                    print(json.dumps({"native_navigation": name, "official_links": selected_links}), flush=True)
                offset = markdown.lower().rfind(term.lower())
                print(json.dumps({"review": name, "excerpt": markdown[max(0, offset-200):offset+1100]}), flush=True)
    receipt = {
        "experimental": True, "commit": os.environ.get("GITHUB_SHA"),
        "provider_repository": "strmt7/FlitzZip", "provider_commit": PROVIDER_REF,
        "provider_version": PROVIDER_VERSION, "python": sys.version, "source_group": args.source_group,
        "limits": limits, "page_timeout_seconds": 45, "robots_checked": True,
        "external_llm": False, "persistent_profile": False, "sources": rows,
    }
    data = (json.dumps(receipt, indent=2) + "\n").encode()
    with (args.output / "receipt.json").open("xb") as out:
        out.write(data)
    print(json.dumps({"receipt_sha256": hashlib.sha256(data).hexdigest(),
                      "accepted": sum(r["success"] for r in rows), "total": len(rows)}), flush=True)
    if not any(r["success"] for r in rows):
        raise RuntimeError("No substantive source extracted")
    if args.source_group in ("process", "amd") and not all(r["success"] for r in rows):
        raise RuntimeError("Requested source group requires every native capture")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--provider", required=True, type=Path)
    parser.add_argument("--home", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--runtime", action="store_true")
    parser.add_argument("--source-group", choices=("durability", "process", "amd"), default="durability")
    args = parser.parse_args()
    guard()
    args.provider = args.provider.resolve(strict=True)
    args.home = args.home.resolve()
    args.output = args.output.resolve()
    if args.home.is_relative_to(args.provider) or args.output.is_relative_to(args.provider):
        raise RuntimeError("Cache/captures must stay outside provider source")
    if shutil.disk_usage(args.output.parent).free < 4 * 1024**3:
        raise RuntimeError("Insufficient research disk reserve")
    actual = subprocess.run(["git", "-C", str(args.provider), "rev-parse", "HEAD"],
                            check=True, capture_output=True, text=True, timeout=5).stdout.strip()
    if actual != PROVIDER_REF:
        raise RuntimeError("Unreviewed provider revision")
    if args.runtime:
        asyncio.run(capture(args))
        return
    sys.path.insert(0, str(args.provider))
    from tools import crawl4ai_tool
    if crawl4ai_tool.VERSION != PROVIDER_VERSION:
        raise RuntimeError("Adapter version mismatch")
    directory, interpreter, identity = crawl4ai_tool.environment_paths(args.home)
    recorded = json.loads((directory / "flitzzip-install.json").read_text())
    if recorded.get("lock_sha256") != identity or recorded.get("version") != PROVIDER_VERSION:
        raise RuntimeError("Hash-locked environment not admitted")
    env = crawl4ai_tool.tool_environment(args.home)
    crawl4ai_tool.verify_versions(interpreter, env)
    code = crawl4ai_tool.run_source_python([
        str(interpreter), str(Path(__file__).resolve()), "--runtime",
        "--provider", str(args.provider), "--home", str(args.home), "--output", str(args.output),
        "--source-group", args.source_group,
    ], env, timeout=300)
    if code:
        raise SystemExit(code)


if __name__ == "__main__":
    main()
