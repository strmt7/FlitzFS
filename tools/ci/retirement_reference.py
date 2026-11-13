"""Experimental historical-source preparation; never launches or mounts anything."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess

REFERENCE = "d1e66f6132d0a5672378bebbe9880e19af105f75"
PINS = {
    "tests/winfsp_write_probe.cpp": "581b13c1df9569564d87a8d9c319364df6784e78",
    "CMakeLists.txt": "042668b83046453dbcca37a46e43dea812307404",
    "tools/ci/build_windows_native.ps1": "1ba865d5a7fb0830621b2ae01ddf37869a556985",
    "src/windows/bounded_process.cpp": "879124c5487e3c805c96a896978e3bcebe279a7c",
}
BUDGET_BLOB = "7e933291b0917ac3a1d765779341cb4cff8494e1"
ANCHOR = (
    '      extended_report(fixture,profile,"ack",GetCurrentProcessId(),view,session.stats().contexts);}\n'
    "    wait_for_supervised_termination(session);"
)
CPP_MAIN = r'''
// Diagnostic controller only. The historical entrypoint above is never invoked.
int wmain(int argc,wchar_t** argv) {try {
  require_hosted_opt_in();
  require(environment(L"FLITZFS_ALLOW_RETIREMENT_REFERENCE")==L"1",
      "historical reference is isolated hosted-only");
  require(argc==2,"one fixed reference mode required");
  const std::wstring_view mode(argv[1]);
  if(mode==L"--reference-child"){restart_source(true);return 0;}
  require(mode==L"--run-reference","unknown reference mode");
  const gpu_probe_fixture::Budget controller_budget;
  wchar_t path[512]{};const auto n=GetModuleFileNameW(nullptr,path,512);
  require(n&&n<512,"reference executable identity");
  const auto lease=std::make_shared<flitzfs::ProcessObserver>();
  flitzfs::ProcessBudget budget;budget.timeout_ms=15000;
  const auto result=flitzfs::run_bounded_process(
      fs::path(std::wstring(path,n)),{L"--reference-child"},budget,lease);
  std::cout<<"retirement_result variant=@VARIANT@ source_process="<<result.process_id
      <<" exit="<<result.exit_code<<" timed_out="<<result.timed_out
      <<" cancelled="<<result.cancelled<<" exit_confirmed="<<result.exit_confirmed
      <<" peak_commit_bytes="<<result.peak_committed_bytes
      <<" controller_peak_commit_bytes="<<controller_budget.peak_commit()<<"\n"<<std::flush;
  // No image access, verifier, remount, retry or replacement after unknown exit.
  require(result.process_id&&result.exit_confirmed&&!result.cancelled&&
      result.timed_out&&result.exit_code==ERROR_TIMEOUT,"reference retirement unqualified");
  require(lease->process_id()==result.process_id&&
      lease->wait_for_exit(0)==flitzfs::ProcessObservation::exited,"reference exact lease");
  Fixture fixture(result.process_id);
  const auto phase=reference_phase_text(result.process_id);
  const auto phase_bytes=read_all(fixture.root/L"reference-phase.txt",phase.size());
  require(phase_bytes.size()==phase.size()&&std::equal(phase_bytes.begin(),phase_bytes.end(),
      reinterpret_cast<const std::byte*>(phase.data())),"reference phase identity/contexts");
  std::cout<<phase<<std::flush;
  emit_extended(result.process_id,"crash-restart","ack");
  require_removed(fixture.root/L"mount");
  const auto view=flitzfs::NativeFilesystemSnapshot::open(
      flitzfs::ObjectStoreSnapshot::open_file(fixture.root/L"native.fltx"));
  view.verify_all();
  require(view.identity()==identity()&&!view.needs_session_recovery()&&
      view.list().size()==1&&view.stat("sample.bin").inode==2&&
      view.read("sample.bin",0,10000)==expected_data()&&
      fs::file_size(fixture.root/L"native.fltx")<=image_cap,"reference CPU bytes");
  std::cout<<"retirement_cpu_verified variant=@VARIANT@ source_process="
      <<result.process_id<<" bytes=7001\n"<<std::flush;
  return 0;
}catch(const std::exception& e){std::cerr<<"retirement reference: "<<e.what()<<"\n";return 1;}}
'''

def once(text: str, needle: str, replacement: str) -> str:
    if text.count(needle) != 1:
        raise ValueError("reference anchor must occur exactly once")
    return text.replace(needle, replacement, 1)

def transform(source: str, variant: str) -> str:
    if variant not in ("held", "closed"):
        raise ValueError("closed reference variant required")
    source = once(source, '#include "winfsp_session.hpp"',
                  '#include "winfsp_session.hpp"\n#include "gpu_probe_budget.hpp"')
    source = once(source, "int wmain(int argc,wchar_t** argv)",
                  "int historical_unused_entrypoint(int argc,wchar_t** argv)")
    close = ""
    if variant == "closed":
        close = r'''
    checked(CloseHandle(std::exchange(file.value,INVALID_HANDLE_VALUE)),
        "reference checked client handle close");
    const auto drain_deadline=GetTickCount64()+2000;
    while(session.stats().contexts&&GetTickCount64()<drain_deadline&&
        !session.requires_offline()&&!session.dispatcher_stopped())Sleep(10);
    require(session.stats().contexts==0&&!session.requires_offline()&&
        !session.dispatcher_stopped(),"reference client contexts did not retire");
'''
    contexts = 1 if variant == "held" else 0
    phase_function = (
        "std::string reference_phase_text(DWORD process) {\n"
        f'  return "retirement_phase variant={variant} source_process="+std::to_string(process)+'
        f'" contexts={contexts}\\n";\n}}\n'
    )
    source = once(source, "void restart_source(bool killed) {",
                  phase_function + "void restart_source(bool killed) {")
    marker = close + f'    require(session.stats().contexts=={contexts},"reference phase contexts");\n' + r'''
    {
      const auto text=reference_phase_text(GetCurrentProcessId());
      require(text.size()<=256,"reference phase report cap");
      Handle proof(CreateFileW((fixture.root/L"reference-phase.txt").c_str(),
          GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
      checked(proof.value!=INVALID_HANDLE_VALUE,"exclusive reference phase receipt");
      DWORD accepted=0;
      checked(WriteFile(proof.value,text.data(),static_cast<DWORD>(text.size()),&accepted,nullptr),
          "reference phase receipt write");
      require(accepted==text.size(),"short reference phase receipt");
      checked(FlushFileBuffers(proof.value),"reference phase receipt flush");
    }
'''
    source = once(source, ANCHOR, ANCHOR.replace(
        "    wait_for_supervised_termination(session);",
        marker + "    wait_for_supervised_termination(session);"))
    return source + CPP_MAIN.replace("@VARIANT@", variant)

def git(root: Path, *args: str) -> bytes:
    return subprocess.run(["git", "-C", str(root), *args],
                          env=dict(os.environ, GIT_NO_REPLACE_OBJECTS="1"),
                          check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          timeout=20).stdout

def blob(data: bytes) -> str:
    return hashlib.sha1(b"blob " + str(len(data)).encode("ascii") + b"\0" + data).hexdigest()

def read_regular(path: Path) -> bytes:
    info = path.lstat()
    if (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or
            getattr(info, "st_file_attributes", 0) & 0x400 or info.st_size > 128 * 1024):
        raise ValueError("bounded regular single-link file required")
    with path.open("rb") as stream:
        data = stream.read(128 * 1024 + 1)
    if len(data) > 128 * 1024:
        raise ValueError("reference read cap")
    return data

def new_file(path: Path, data: bytes) -> None:
    if len(data) > 128 * 1024:
        raise ValueError("generated file cap")
    with path.open("xb") as stream:
        stream.write(data)

def prepare(control: Path, variant: str, verify: bool = False) -> None:
    required = {"GITHUB_ACTIONS": "true", "RUNNER_ENVIRONMENT": "github-hosted",
                "RUNNER_OS": "Windows", "FLITZFS_ALLOW_RETIREMENT_REFERENCE": "1"}
    if os.name != "nt" or any(os.environ.get(k) != v for k, v in required.items()):
        raise ValueError("explicit disposable hosted Windows required")
    head = os.environ.get("GITHUB_SHA", "")
    if not re.fullmatch("[0-9a-f]{40}", head):
        raise ValueError("exact control SHA required")
    if (Path(os.environ["GITHUB_WORKSPACE"]).resolve() != control.resolve() or
            git(control, "rev-parse", "HEAD").decode().strip() != head):
        raise ValueError("control source identity differs")
    reference = control / ".ci-retirement"
    if (git(reference, "rev-parse", "HEAD").decode().strip() != REFERENCE or
            git(reference, "rev-parse", "--show-toplevel").decode().strip().casefold()
            != str(reference.resolve()).casefold()):
        raise ValueError("exact private reference checkout required")
    git(reference, "diff", "--quiet", "HEAD", "--")
    sources = {}
    for name, expected in PINS.items():
        data = read_regular(reference / name)
        if blob(data) != expected or git(reference, "rev-parse", "HEAD:" + name).decode().strip() != expected:
            raise ValueError("canonical historical blob differs")
        sources[name] = data
    budget = read_regular(control / "tests/gpu_probe_budget.hpp")
    if blob(budget) != BUDGET_BLOB:
        raise ValueError("qualified controller budget differs")
    if read_regular(reference / "tools/winfsp-sdk-lock.json") != read_regular(control / "tools/winfsp-sdk-lock.json"):
        raise ValueError("SDK identity differs")
    report_root = Path(os.environ["FLITZFS_RETIREMENT_REPORT_ROOT"])
    if (report_root.parent.resolve() != Path(os.environ["RUNNER_TEMP"]).resolve() or
            not re.fullmatch("flitzfs-retirement-reference-[0-9a-f]{32}", report_root.name)):
        raise ValueError("owned report root required")
    gate = json.loads(read_regular(report_root / "ci-gate.json"))
    if gate.get("commit") != head or len(gate.get("jobs", [])) != 13:
        raise ValueError("exact-source ordinary gate receipt required")
    generated = transform(sources["tests/winfsp_write_probe.cpp"].decode(), variant).encode()
    staging = reference / ".retirement-variant"
    generated_path = reference / "tests/flitzfs_retirement_reference.cpp"
    cmake = (
        'cmake_minimum_required(VERSION 3.22)\nproject(FlitzFSRetirementReference LANGUAGES C CXX)\n'
        'add_subdirectory(".." historical)\n'
        'get_target_property(original flitzfs_write_probe SOURCES)\n'
        'if(NOT original STREQUAL "tests/winfsp_write_probe.cpp")\n'
        'message(FATAL_ERROR "Unexpected historical target")\nendif()\n'
        'set_property(TARGET flitzfs_write_probe PROPERTY SOURCES '
        '"${CMAKE_CURRENT_SOURCE_DIR}/../tests/flitzfs_retirement_reference.cpp")\n'
        'target_include_directories(flitzfs_write_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../tests")\n'
    ).encode()
    script = once(sources["tools/ci/build_windows_native.ps1"].decode(),
                  "cmake -S . -B $buildDirectory",
                  'cmake -S "$PSScriptRoot" -B $buildDirectory').encode()
    outputs = {generated_path: generated, staging / "CMakeLists.txt": cmake,
               staging / "build.ps1": script}
    if verify:
        for path, expected in outputs.items():
            if read_regular(path) != expected:
                raise ValueError("generated reference changed")
        return
    staging.mkdir(exist_ok=False)
    for path, data in outputs.items():
        new_file(path, data)
    # The workflow's exact-source CI gate creates this NEW directory first.
    new_file(report_root / "source.json", (json.dumps({
        "experimental": True, "commit": head, "reference": REFERENCE,
        "variant": variant, "canonical_blobs": PINS, "controller_budget_blob": BUDGET_BLOB,
        "generated": [{"name": str(path.relative_to(reference)), "bytes": len(data),
                       "sha256": hashlib.sha256(data).hexdigest()} for path, data in outputs.items()],
        "gpu_executed": False, "owner_execution": False}, indent=2) + "\n").encode())

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--variant", choices=("held", "closed"), required=True)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    prepare(Path(__file__).resolve().parents[2], args.variant, args.verify)

if __name__ == "__main__":
    main()
