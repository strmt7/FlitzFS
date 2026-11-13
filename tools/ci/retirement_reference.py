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

SDK_LOCK_BLOB = "0025dc6f9faedc910680b268ddf8b2fe36c09fc8"


def checkout_representation(canonical: bytes, physical: bytes) -> str:
    if len(canonical) > 128 * 1024 or len(physical) > 128 * 1024:
        raise ValueError("source representation cap")
    if physical == canonical:
        return "exact"
    # Admit only this whole-file checkout spelling, never arbitrary newline
    # cleanup, a BOM, mixed endings, encodings or content modifications.
    if b"\r" not in canonical and physical == canonical.replace(b"\n", b"\r\n"):
        return "crlf-checkout"
    raise ValueError("physical source is not a closed canonical representation")


def canonical_source(root: Path, name: str, expected: str):
    if not re.fullmatch("[0-9a-f]{40}", expected):
        raise ValueError("canonical blob pin required")
    actual = git(root, "rev-parse", "HEAD:" + name).decode().strip()
    if actual != expected:
        raise ValueError("canonical source object differs")
    size_text = git(root, "cat-file", "-s", expected).decode().strip()
    if not re.fullmatch("[0-9]{1,6}", size_text) or not 0 < int(size_text) <= 128 * 1024:
        raise ValueError("canonical source size cap")
    data = git(root, "cat-file", "blob", expected)
    if len(data) != int(size_text) or blob(data) != expected:
        raise ValueError("canonical source bytes differ")
    physical = read_regular(root / name)
    try:
        representation = checkout_representation(data, physical)
    except ValueError as error:
        raise ValueError(f"source representation rejected: path={name} canonical_bytes={len(data)} "
                         f"physical_bytes={len(physical)} physical_sha256={hashlib.sha256(physical).hexdigest()} "
                         f"physical_crlf={physical.count(bytes((13, 10)))}") from error
    return data, {"path": name, "canonical_git_blob": expected,
                  "canonical_bytes": len(data), "canonical_sha256": hashlib.sha256(data).hexdigest(),
                  "physical_bytes": len(physical), "physical_sha256": hashlib.sha256(physical).hexdigest(),
                  "representation": representation}


def new_file(path: Path, data: bytes) -> None:
    if len(data) > 128 * 1024:
        raise ValueError("generated file cap")
    with path.open("xb") as stream:
        stream.write(data)

def same_git_root(reported: str, expected: Path) -> bool:
    """Bind the actual directory despite Git/native separator spelling."""
    if not isinstance(reported, str) or not reported or len(reported) > 1024:
        return False
    candidate = Path(reported)
    if not candidate.is_absolute() or candidate.anchor.casefold() != expected.anchor.casefold():
        return False
    try:
        target = expected.resolve()
        identity = target.stat()
        if not identity.st_ino or not identity.st_dev:
            return False
        return candidate.resolve().samefile(target)
    except (OSError, ValueError, RuntimeError):
        return False

def prepare(control: Path, variant: str, verify: bool = False) -> None:
    required = {"GITHUB_ACTIONS": "true", "RUNNER_ENVIRONMENT": "github-hosted",
                "RUNNER_OS": "Windows", "FLITZFS_ALLOW_RETIREMENT_REFERENCE": "1"}
    if os.name != "nt" or any(os.environ.get(k) != v for k, v in required.items()):
        raise ValueError("explicit disposable hosted Windows required")
    head = os.environ.get("GITHUB_SHA", "")
    if not re.fullmatch("[0-9a-f]{40}", head):
        raise ValueError("exact control SHA required")
    if (Path(os.environ["GITHUB_WORKSPACE"]).resolve() != control.resolve() or
            not same_git_root(git(control, "rev-parse", "--show-toplevel").decode().strip(), control) or
            git(control, "rev-parse", "HEAD").decode().strip() != head):
        raise ValueError("control source identity differs")
    reference = control / ".ci-retirement"
    root_info = reference.lstat()
    if (not stat.S_ISDIR(root_info.st_mode) or
            getattr(root_info, "st_file_attributes", 0) & 0x400):
        raise ValueError("ordinary private reference root required")
    if (git(reference, "rev-parse", "HEAD").decode().strip() != REFERENCE or
            not same_git_root(git(reference, "rev-parse", "--show-toplevel").decode().strip(), reference)):
        raise ValueError("exact private reference checkout required")
    git(reference, "diff", "--quiet", "HEAD", "--")
    sources = {}
    representations = []
    for name, expected in PINS.items():
        data, record = canonical_source(reference, name, expected)
        sources[name] = data
        representations.append(dict(record, role="historical"))
    _, record = canonical_source(control, "tests/gpu_probe_budget.hpp", BUDGET_BLOB)
    representations.append(dict(record, role="control-budget"))
    old_lock, old_record = canonical_source(reference, "tools/winfsp-sdk-lock.json", SDK_LOCK_BLOB)
    current_lock, current_record = canonical_source(control, "tools/winfsp-sdk-lock.json", SDK_LOCK_BLOB)
    if old_lock != current_lock:
        raise ValueError("SDK identity differs")
    representations.extend((dict(old_record, role="historical-sdk"),
                            dict(current_record, role="control-sdk")))
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
        "source_representations": representations,
        "generated": [{"name": str(path.relative_to(reference)), "bytes": len(data),
                       "sha256": hashlib.sha256(data).hexdigest()} for path, data in outputs.items()],
        "gpu_executed": False, "owner_execution": False}, indent=2) + "\n").encode())


def strict_json(text: str) -> dict:
    if not isinstance(text, str) or len(text) > 16384:
        raise ValueError("bounded reference JSON text required")
    def object_pairs(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate reference JSON field")
            result[key] = value
        return result
    def reject_constant(value):
        raise ValueError("nonfinite reference JSON constant")
    try:
        value = json.loads(text, object_pairs_hook=object_pairs, parse_constant=reject_constant)
    except (ValueError, RecursionError) as error:
        raise ValueError("invalid bounded reference JSON") from error
    if not isinstance(value, dict):
        raise ValueError("reference JSON object required")
    return value


def reference_result(stdout: str, variant: str):
    if variant not in ("held", "closed") or not isinstance(stdout, str) or len(stdout) > 16384:
        raise ValueError("reference control identity/output bounds")
    if not stdout.isascii() or "\0" in stdout:
        raise ValueError("reference control encoding")
    normalized = stdout.replace("\r\n", "\n")
    if "\r" in normalized:
        raise ValueError("invalid reference newline")
    lines = normalized.splitlines()
    if len(lines) != 4:
        raise ValueError("complete one-phase reference output required")
    result = re.fullmatch(
        r"retirement_result variant=(held|closed) source_process=([1-9][0-9]{0,9}) "
        r"exit=1460 timed_out=1 cancelled=0 exit_confirmed=1 "
        r"peak_commit_bytes=([1-9][0-9]{0,11}) controller_peak_commit_bytes=([1-9][0-9]{0,11})",
        lines[0])
    if result is None or result[1] != variant:
        raise ValueError("confirmed exact timeout result required")
    process, peak, controller_peak = map(int, result.group(2, 3, 4))
    if process > 0xffffffff or peak > 128 * 1024 * 1024 or controller_peak > 256 * 1024 * 1024:
        raise ValueError("reference process/resource range")
    return lines, process, peak, controller_peak


def reconcile(stdout: str, phase: str, acknowledgement: str, commit: str, variant: str) -> dict:
    """Reconcile retained observations, never infer kernel cause or payload validity."""
    if (any(not isinstance(text, str) for text in (stdout, phase, acknowledgement, commit)) or
            variant not in ("held", "closed") or not re.fullmatch("[0-9a-f]{40}", commit) or
            len(stdout) > 16384 or len(phase) > 256 or
            len(acknowledgement) > 2048):
        raise ValueError("reference identity/output bounds")
    if any(not text.isascii() or "\0" in text for text in (stdout, phase, acknowledgement)):
        raise ValueError("reference output encoding")
    lines, process, peak, controller_peak = reference_result(stdout, variant)
    contexts = 1 if variant == "held" else 0
    expected_phase = f"retirement_phase variant={variant} source_process={process} contexts={contexts}\n"
    if phase != expected_phase or lines[1] + "\n" != phase:
        raise ValueError("retained source phase differs")
    prefix = "Windows writable extended evidence: "
    if lines[2] != prefix + acknowledgement:
        raise ValueError("raw acknowledgement differs")
    ack = strict_json(acknowledgement)
    expected = {"schema": 2, "profile": "crash-restart", "phase": "ack", "commit": commit,
                "source_process": process, "observer_process": process, "user_bytes": 7001,
                "inode": 2, "file_count": 1, "image_cap": 4 * 1024 * 1024, "contexts": 1,
                "explicit_flush_observed": True, "cpu_verified": True,
                "dispatcher_stopped": False, "gpu": False}
    for key, value in expected.items():
        if key not in ack or type(ack[key]) is not type(value) or ack[key] != value:
            raise ValueError("reference acknowledgement identity/state differs")
    integer_keys = ("namespace_sequence", "generation", "image_bytes", "creation_time",
                    "last_access_time", "last_write_time", "change_time", "attributes", "reservation")
    digest_keys = ("data_digest", "security_digest", "keep_digest")
    if set(ack) != set(expected) | set(integer_keys) | set(digest_keys):
        raise ValueError("closed reference acknowledgement schema required")
    for key in ("namespace_sequence", "generation", "image_bytes", "creation_time",
                "last_access_time", "last_write_time", "change_time", "attributes", "reservation"):
        if type(ack.get(key)) is not int or ack[key] < 0 or ack[key] > 0xffffffffffffffff:
            raise ValueError("reference acknowledgement integer range")
    if (not ack["creation_time"] or ack["keep_digest"] != "0" * 64 or
            not ack["generation"] or not ack["namespace_sequence"] or
            not 0 < ack["image_bytes"] <= ack["image_cap"] or ack["reservation"] < 7001):
        raise ValueError("reference acknowledgement capacity")
    for key in ("data_digest", "security_digest", "keep_digest"):
        if not isinstance(ack.get(key), str) or not re.fullmatch("[0-9a-f]{64}", ack[key]):
            raise ValueError("reference acknowledgement digest")
    if lines[3] != f"retirement_cpu_verified variant={variant} source_process={process} bytes=7001":
        raise ValueError("reference CPU readback observation differs")
    return {"experimental": True, "commit": commit, "reference": REFERENCE, "variant": variant,
            "source_process": process, "phase_contexts": contexts, "acknowledged_contexts": 1,
            "retirement_confirmed": True, "cpu_bytes_observed": 7001,
            "source_peak_commit_bytes": peak, "controller_peak_commit_bytes": controller_peak,
            "acknowledgement_sha256": hashlib.sha256(acknowledgement.encode()).hexdigest(),
            "phase_sha256": hashlib.sha256(phase.encode()).hexdigest(),
            "historical_cause": "unproven", "gpu_executed": False}

def reconcile_files(control: Path, variant: str) -> None:
    # Reverify the trusted generator/old source before reading retained records.
    prepare(control, variant, verify=True)
    root = Path(os.environ["FLITZFS_RETIREMENT_REPORT_ROOT"])
    source = strict_json(read_regular(root / "source.json").decode("utf-8"))
    commit = os.environ["GITHUB_SHA"]
    if (source.get("commit") != commit or source.get("reference") != REFERENCE or
            source.get("variant") != variant or source.get("canonical_blobs") != PINS or
            source.get("controller_budget_blob") != BUDGET_BLOB or
            source.get("gpu_executed") is not False or source.get("owner_execution") is not False):
        raise ValueError("reference source receipt differs")
    stdout = read_regular(root / "stdout.txt").decode("utf-8")
    # Unknown retirement fails before even opening a fixture receipt.
    _, process, _, _ = reference_result(stdout, variant)
    fixture = Path(os.environ["RUNNER_TEMP"]) / f"flitzfs-write-probe-{process}"
    phase = read_regular(fixture / "reference-phase.txt").decode("utf-8")
    acknowledgement = read_regular(fixture / "state.json").decode("utf-8")
    result = reconcile(stdout, phase, acknowledgement, commit, variant)
    new_file(root / "qualification.json", (json.dumps(result, indent=2) + "\n").encode())

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--variant", choices=("held", "closed"), required=True)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--verify", action="store_true")
    mode.add_argument("--reconcile", action="store_true")
    args = parser.parse_args()
    control = Path(__file__).resolve().parents[2]
    if args.reconcile:
        reconcile_files(control, args.variant)
    else:
        prepare(control, args.variant, args.verify)

if __name__ == "__main__":
    main()
