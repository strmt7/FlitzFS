#!/usr/bin/env python3
"""Native Crawl4AI research on bounded disposable hosted VMs only.

This product includes software developed by UncleCode (https://x.com/unclecode)
as part of Crawl4AI (https://github.com/unclecode/crawl4ai).
Never imported by the filesystem.
"""
import argparse
import asyncio
import hashlib
from html.parser import HTMLParser
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

ROCM_WINDOWS_HARDWARE_SOURCES = (
    ("amd-observed-windows-driver", "https://www.amd.com/en/resources/support-articles/release-notes/RN-RAD-WIN-26-10-2.html", "32.0.32015.12012", "ROCm"),
    ("amd-rocm-10-1-driver", "https://www.amd.com/en/resources/support-articles/release-notes/RN-RAD-ROCM-10-01.html", "ROCm", "9070"),
    ("amd-rocm-windows-compatibility", "https://rocm.docs.amd.com/en/latest/compatibility/compatibility-matrix.html", "10.1.0", "9070"),
    ("amd-rocm-current-license", "https://rocm.docs.amd.com/en/latest/about/license.html", "License", "HIP"),
)


WINDOWS_GPU_MEMORY_SOURCES = (
    ("windows-gpu-memory-query", "https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiadapter3-queryvideomemoryinfo", "QueryVideoMemoryInfo", "NodeIndex"),
    ("windows-gpu-memory-info", "https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/ns-dxgi1_4-dxgi_query_video_memory_info", "CurrentUsage", "process"),
    ("windows-gpu-memory-segments", "https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/ne-dxgi1_4-dxgi_memory_segment_group", "NON_LOCAL", "system"),
    ("windows-host-memory-status", "https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-globalmemorystatusex", "GlobalMemoryStatusEx", "volatile"),
    ("windows-host-memory-fields", "https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/ns-sysinfoapi-memorystatusex", "ullAvailPageFile", "commit"),
)

WINDOWS_ADAPTER_TOPOLOGY_SOURCES = (
    ("windows-d3d12-node-count", "https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-getnodecount", "GetNodeCount", "physical"),
    ("windows-d3d12-create-device", "https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-d3d12createdevice", "D3D12CreateDevice", "LUID"),
    ("windows-d3d12-multi-adapter", "https://learn.microsoft.com/en-us/windows/win32/direct3d12/multi-engine", "GetNodeCount", "node mask"),
)

ROCM_CURRENT_SOURCES = (
    ("rocm-current-release", "https://rocm.docs.amd.com/en/latest/about/release-notes.html", "ROCm", "10."),
    ("rocm-selected-release", "https://rocm.docs.amd.com/en/docs-10.1.0/about/release-notes.html", "10.1.0", "Windows"),
    ("rocm-current-install", "https://rocmdocs.amd.com/en/latest/install/rocm.html", "ROCm", "Windows"),
)


ROCM_RUNTIME_SOURCES = (
    ("hip-current-streams", "https://rocm.docs.amd.com/projects/HIP/en/docs-10.1.0/doxygen/html/group___stream.html", "hipStreamCreateWithPriority", "hipStreamDestroy"),
    ("hip-current-memory", "https://rocm.docs.amd.com/projects/HIP/en/docs-10.1.0/doxygen/html/group___memory.html", "hipMalloc", "hipFree"),
    ("hip-current-errors", "https://rocm.docs.amd.com/projects/HIP/en/docs-10.1.0/reference/error_codes.html", "hipError", "context"),
    ("windows-current-tdr", "https://learn.microsoft.com/en-us/windows-hardware/drivers/display/timeout-detection-and-recovery", "Timeout", "process"),
)



WINDOWS_RUNTIME_TRUST_SOURCES = (
    ("windows-runtime-trust", "https://learn.microsoft.com/en-us/windows/win32/api/wintrust/nf-wintrust-winverifytrust", "WinVerifyTrust", "ERROR_SUCCESS"),
    ("windows-runtime-trust-policy", "https://learn.microsoft.com/en-us/windows/win32/api/wintrust/ns-wintrust-wintrust_data", "WTD_CACHE_ONLY_URL_RETRIEVAL", "WTD_UI_NONE"),
    ("windows-runtime-search", "https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw", "LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR", "LOAD_LIBRARY_SEARCH_SYSTEM32"),
    ("windows-runtime-system-directory", "https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getsystemdirectoryw", "GetSystemDirectoryW", "return"),
)
WINDOWS_PROCESS_RETIREMENT_SOURCES = (
    ("windows-process-termination", "https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-terminateprocess", "TerminateProcess", "asynchronous"),
    ("windows-job-termination", "https://learn.microsoft.com/en-us/windows/win32/api/jobapi2/nf-jobapi2-terminatejobobject", "TerminateJobObject", "pending"),
)


WINDOWS_ADAPTER_IDENTITY_SOURCES = (
    ("windows-adapter-enumeration", "https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgifactory1-enumadapters1", "EnumAdapters1", "DXGI_ERROR_NOT_FOUND"),
    ("windows-adapter-description", "https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ns-dxgi-dxgi_adapter_desc1", "AdapterLuid", "DedicatedVideoMemory"),
)

WINDOWS_TARGET_SOURCES = (
    ("windows11-current-release", "https://learn.microsoft.com/en-us/windows/release-health/windows11-release-information", "Windows 11", "Version"),
    ("windows-sdk-current", "https://learn.microsoft.com/en-us/windows/apps/windows-sdk/downloads", "Windows SDK", "10.0."),
    ("windows-current-driver-policy", "https://support.microsoft.com/en-us/windows/hardware/drivers/the-windows-driver-policy", "WHCP", "enforcement"),
    ("windows-current-fsd-reference", "https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/_ifsk/", "file system", "driver"),
)
WINDOWS_FRAMEWORK_SOURCES = (
    ("winfsp-current-design", "https://winfsp.dev/doc/WinFsp-Design/", "FSD", "kernel"),
    ("winfsp-current-service", "https://winfsp.dev/doc/WinFsp-Service-Architecture/", "service", "Launcher"),
    ("winfsp-current-compatibility", "https://winfsp.dev/doc/NTFS-Compatibility/", "NTFS", "Flush"),
    ("winfsp-current-testing", "https://winfsp.dev/doc/WinFsp-Testing/", "test", "winfsp-tests"),
)
WINDOWS_ALTERNATIVE_SOURCES = (
    ("dokan-current-design", "https://dokan-dev.github.io/", "Dokan", "driver"),
    ("winbtrfs-current-design", "https://github.com/maharmstone/btrfs", "WinBtrfs", "Windows 11"),
    ("windows-current-projfs", "https://learn.microsoft.com/en-us/windows/win32/projfs/projected-file-system", "provider", "write"),
    ("windows-current-cloud-files", "https://learn.microsoft.com/en-us/windows/win32/cfapi/build-a-cloud-file-sync-engine", "Cloud", "hydration"),
)
ROCM_STREAM_SOURCES = (
    ("hip-current-stream-management", "https://rocm.docs.amd.com/projects/HIP/en/docs-10.1.0/reference/hip_runtime_api/modules/stream_management.html", "hipStreamCreateWithPriority", "hipStreamDestroy"),
)


# Current-served primary references. A successful fetch does not establish a
# recent document revision or qualify Windows 11 client behavior.
WINDOWS_BASIC_SOURCES = (
    ("windows-basic-info", "https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_basic_info", "FILE_BASIC_INFO", "ChangeTime"),
    ("windows-file-attributes", "https://learn.microsoft.com/en-us/windows/win32/fileio/file-attribute-constants", "FILE_ATTRIBUTE_NORMAL", "READONLY"),
    ("windows-set-file-time", "https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-setfiletime", "SetFileTime", "0xFFFFFFFF"),
    ("windows-kernel-basic-info", "https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_file_basic_information", "FILE_BASIC_INFORMATION", "-2"),
)
WINDOWS_SECURITY_SOURCES = (
    ("windows-file-security", "https://learn.microsoft.com/en-us/windows/win32/fileio/file-security-and-access-rights", "security descriptor", "access"),
    ("windows-relative-security", "https://learn.microsoft.com/en-us/windows/win32/secauthz/absolute-and-self-relative-security-descriptors", "SE_SELF_RELATIVE", "relative"),
    ("windows-security-inheritance", "https://learn.microsoft.com/en-us/windows/win32/secauthz/automatic-propagation-of-inheritable-aces", "inherit", "ACE"),
    ("windows-set-security", "https://learn.microsoft.com/en-us/windows/win32/api/aclapi/nf-aclapi-setsecurityinfo", "SetSecurityInfo", "inherit"),
)
WINDOWS_PROTOCOL_SOURCES = (
    ("windows-current-fsa", "https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/", "File System Algorithms", "2026"),
    ("windows-current-fscc", "https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/", "File System Control Codes", "2026"),
    ("windows-protocol-basic-info", "https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/16023025-8a78-492f-8b96-c873b042ac50", "FileBasicInformation", "ChangeTime"),
    ("windows-kernel-standard-info", "https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_file_standard_information", "FILE_STANDARD_INFORMATION", "AllocationSize"),
)


WINDOWS_OWNERSHIP_SOURCES = (
    ("windows-private-security", "https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-setprivateobjectsecurity", "SetPrivateObjectSecurity", "Token"),
    ("windows-private-security-ex", "https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-setprivateobjectsecurityex", "SetPrivateObjectSecurityEx", "SEF_AVOID_OWNER_CHECK"),
    ("windows-set-object-security", "https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-zwsetsecurityobject", "ZwSetSecurityObject", "WRITE_OWNER"),
    ("windows-kernel-security-merge", "https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-sesetsecuritydescriptorinfo", "SeSetSecurityDescriptorInfo", "security"),
)

WINDOWS_LOCKED_RAM_SOURCES = (
    ("windows-virtual-lock", "https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtuallock", "VirtualLock", "pagefile"),
    ("windows-working-set", "https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-setprocessworkingsetsizeex", "SetProcessWorkingSetSizeEx", "working set"),
    ("windows-working-set-query", "https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-queryworkingsetex", "QueryWorkingSetEx", "pages"),
    ("windows-working-set-flags", "https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-psapi_working_set_ex_block", "PSAPI_WORKING_SET_EX_BLOCK", "Locked"),
)

class SourceMetadata(HTMLParser):
    """Record advisory publisher metadata from the native captured HTML only."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.items = []

    def handle_starttag(self, tag, attrs):
        if len(self.items) >= 32:
            return
        values = dict(attrs)
        if tag == "meta":
            key = (values.get("name") or values.get("property") or "").lower()
            if key in {"ms.date", "ms.topic", "updated_at", "date", "last_modified",
                       "document_id", "document_version_independent_id", "github_feedback_content_git_url",
                       "original_content_git_url", "og:url"}:
                self.items.append({"kind": key, "value": (values.get("content") or "")[:2048]})
        elif tag == "time" and values.get("datetime"):
            self.items.append({"kind": "datetime", "value": values["datetime"][:128]})
        elif tag == "link" and "canonical" in (values.get("rel") or "").lower().split():
            self.items.append({"kind": "canonical", "value": (values.get("href") or "")[:2048]})


def source_metadata(result):
    parser = SourceMetadata()
    html = result.get("html") or ""
    if not isinstance(html, str):
        return {"items": [], "status": "native HTML unavailable", "revision_verified": False}
    # The native capture retains the full body; parsing/logging stays bounded.
    if len(html) > 4 * 1024**2:
        return {"items": [], "status": "native HTML exceeds review cap", "revision_verified": False}
    try:
        parser.feed(html)
        parser.close()
    except Exception as error:
        return {"items": parser.items, "status": type(error).__name__, "revision_verified": False}
    return {"items": parser.items, "status": "extracted" if parser.items else "publisher metadata absent",
            "revision_verified": False}

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



def qualify_source_scope(source_group, row, markdown):
    """Preserve native receipts while rejecting explicitly retired AMD documentation."""
    if source_group.startswith("rocm") or source_group == "amd":
        retired=re.search(r"documentation\s+covers\s+releases\s+through\s+([0-9.]+)",
            markdown,re.IGNORECASE)
        if retired and row["success"]:
            return dict(row,success=False,expected_content=False,
                failure_category="retired_documentation_scope",
                error="Retired AMD documentation covers releases through "+retired.group(1)[:32])
    return row

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
        selected = {"process": PROCESS_SOURCES, "amd": AMD_SOURCES, "durability": SOURCES, "rocm-windows-hardware": ROCM_WINDOWS_HARDWARE_SOURCES, "rocm-current": ROCM_CURRENT_SOURCES, "rocm-runtime": ROCM_RUNTIME_SOURCES, "windows-target": WINDOWS_TARGET_SOURCES,
                    "windows-framework": WINDOWS_FRAMEWORK_SOURCES, "windows-alternatives": WINDOWS_ALTERNATIVE_SOURCES,
                    "rocm-stream": ROCM_STREAM_SOURCES,
                    "windows-basic": WINDOWS_BASIC_SOURCES, "windows-security": WINDOWS_SECURITY_SOURCES,
                    "windows-protocol": WINDOWS_PROTOCOL_SOURCES, "windows-ownership": WINDOWS_OWNERSHIP_SOURCES, "windows-locked-ram": WINDOWS_LOCKED_RAM_SOURCES, "windows-runtime-trust": WINDOWS_RUNTIME_TRUST_SOURCES,
                    "windows-process-retirement": WINDOWS_PROCESS_RETIREMENT_SOURCES,
                    "windows-adapter-identity": WINDOWS_ADAPTER_IDENTITY_SOURCES,
                    "windows-adapter-topology": WINDOWS_ADAPTER_TOPOLOGY_SOURCES,
                    "windows-gpu-memory": WINDOWS_GPU_MEMORY_SOURCES}[args.source_group]
        for name, url, marker, term in selected:
            row = await fetch(owner, url, marker, args.output / (name + ".native.json"))
            row["name"] = name
            native = json.loads((args.output / row["capture"]).read_text(encoding="utf-8"))
            result = native.get("native_result") or {}
            scope_text = result.get("markdown") or ""
            if isinstance(scope_text, dict):
                scope_text = scope_text.get("raw_markdown", "")
            row = qualify_source_scope(args.source_group,row,str(scope_text))
            rows.append(row)
            print(json.dumps(row), flush=True)
            print(json.dumps({"native_location": name, "result_url": result.get("url"),
                              "redirected_url": result.get("redirected_url"),
                              "native_error": str(result.get("error_message", ""))[:400]}), flush=True)
            print(json.dumps({"publisher_metadata": name, **source_metadata(result)}), flush=True)
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
                if args.source_group in ("rocm-runtime", "rocm-stream") or args.source_group.startswith("windows-"):
                    terms = ("GetNodeCount", "D3D12CreateDevice", "physical adapter", "node mask", "EnumAdapters1", "DXGI_ERROR_NOT_FOUND", "AdapterLuid", "DedicatedVideoMemory",
                             "VendorId", "DXGI_ADAPTER_FLAG_SOFTWARE", "WinVerifyTrust", "WTD_CACHE_ONLY_URL_RETRIEVAL", "WTD_UI_NONE",
                             "LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR", "LOAD_LIBRARY_SEARCH_SYSTEM32",
                             "GetSystemDirectoryW", "TerminateProcess", "TerminateJobObject", "asynchronous", "pending",
                             "hipStreamCreateWithPriority", "hipStreamQuery", "hipStreamDestroy",
                             "hipFree", "hipMalloc", "hipErrorContext", "hipErrorLaunch",
                             "hipErrorOutOfMemory", "priority", "process", "26H2", "26H1", "10.0.",
                             "WHCP", "enforcement", "kernel", "driver", "Flush", "cach", "IRP",
                             "service", "security", "hydration", "NTFS", "WriteFile",
                             "CreationTime", "LastAccessTime", "LastWriteTime", "ChangeTime",
                             "FileAttributes", "FILE_ATTRIBUTE_NORMAL", "FILE_ATTRIBUTE_READONLY",
                             "AllocationSize", "EndOfFile", "DeletePending", "SE_SELF_RELATIVE",
                             "inherit", "DACL", "SACL", "2026", "Last updated", "Content source",
                             "Token", "owner", "SEF_AVOID_OWNER_CHECK", "privilege", "WRITE_OWNER")
                    excerpts = []
                    for term in terms:
                        positions = [match.start() for match in re.finditer(re.escape(term), markdown, re.I)]
                        for offset in positions[-2:]:
                            excerpts.append({"term": term, "offset": offset,
                                "text": markdown[max(0, offset-160):offset+950]})
                    print(json.dumps({"runtime_source_review": name,
                        "excerpts": excerpts[:40]}), flush=True)
                if args.source_group in ("rocm-windows-hardware", "rocm-current", "rocm-runtime", "rocm-stream") or args.source_group.startswith("windows-"):
                    print(json.dumps({"current_source_review": name,
                        "opening": markdown[:10000],
                        "body": markdown[(re.search(r"(?m)^# ", markdown).start()
                                          if re.search(r"(?m)^# ", markdown) else 0):][:16000],
                        "headings": [line for line in markdown.splitlines() if line.startswith("#")][:48],
                        "official_links": sorted(set(re.findall(r'https://(?:stable\.repo\.amd\.com|rocm(?:\.docs|docs)?\.amd\.com)/[^\s)<>" ]+', markdown)))[:40]
                    }), flush=True)

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
    if args.source_group in ("process", "amd", "rocm-windows-hardware", "rocm-current", "rocm-runtime", "rocm-stream", "windows-target", "windows-framework", "windows-alternatives", "windows-basic", "windows-security", "windows-protocol", "windows-ownership", "windows-locked-ram", "windows-runtime-trust", "windows-process-retirement", "windows-adapter-identity", "windows-adapter-topology", "windows-gpu-memory") and not all(r["success"] for r in rows):
        raise RuntimeError("Requested source group requires every native capture")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--provider", required=True, type=Path)
    parser.add_argument("--home", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--runtime", action="store_true")
    parser.add_argument("--source-group", choices=("durability", "process", "amd", "rocm-windows-hardware", "rocm-current", "rocm-runtime", "rocm-stream", "windows-target", "windows-framework", "windows-alternatives", "windows-basic", "windows-security", "windows-protocol", "windows-ownership", "windows-locked-ram", "windows-runtime-trust", "windows-process-retirement", "windows-adapter-identity", "windows-adapter-topology", "windows-gpu-memory"), default="rocm-current")
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
