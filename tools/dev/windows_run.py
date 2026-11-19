#!/usr/bin/env python3
"""Run local development commands in a resource-bounded Windows process tree.

No shell, elevation, services, driver changes or global environment changes.
The Job Object is assigned at process creation and kills its owned tree if this
supervisor exits. Logs, temporary files and the receipt use a new run directory.
This is resource containment, not a security sandbox or a kernel-fault guarantee.
"""
from __future__ import annotations

import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import os
from pathlib import Path, PureWindowsPath
import shutil
import subprocess
import sys
import time
import uuid


# SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX
_UNATTENDED_ERROR_MODE = 0x0001 | 0x0002 | 0x8000
_LOADER_ERRORS = {
    0xC000007B: "invalid executable or DLL image",
    0xC0000135: "required DLL not found",
    0xC0000139: "required DLL entry point not found",
    0xC0000142: "DLL initialization failed",
}


def _cmake_command_index(command: list[str], index: int) -> int:
    """Find the command launched through CMake's utility wrappers."""
    while (index + 2 < len(command)
           and PureWindowsPath(command[index]).name.casefold() in {"cmake", "cmake.exe"}
           and command[index + 1] == "-E"):
        subcommand = command[index + 2]
        if subcommand == "time":
            index += 3
            continue
        if subcommand == "chdir":
            index += 4
            continue
        if subcommand != "env":
            break
        index += 3
        while index < len(command):
            argument = command[index]
            if argument == "--":
                index += 1
                break
            if argument == "--modify":
                index += 2
            elif "=" in argument:
                index += 1
            else:
                break
    return index


def _unattended_ctest(command: list[str]) -> None:
    """Apply CTest's own non-interactive mode to supported launch forms."""
    index = 0
    while index < len(command):
        index = _cmake_command_index(command, index)
        if (index >= len(command) or
                PureWindowsPath(command[index]).name.casefold() not in {"ctest", "ctest.exe"}):
            return

        # Interactive CTest resets the Windows error mode of its test process.
        # Insert the override before arguments that CTest forwards unchanged.
        index += 1
        while index < len(command) and command[index] not in {"--", "--build-options", "--test-command"}:
            index += 1
        if command[index - 2:index] != ["--interactive-debug-mode", "0"]:
            command[index:index] = ["--interactive-debug-mode", "0"]
            index += 2

        # --build-and-test can launch a fresh CTest process with its own mode.
        # Follow the test command without changing the build or passthrough arguments.
        if index < len(command) and command[index] == "--build-options":
            while index < len(command) and command[index] != "--test-command":
                index += 1
        if index >= len(command) or command[index] != "--test-command":
            return
        index += 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--memory-share", type=int, default=4)
    parser.add_argument("--cpu-percent", type=int, default=50)
    parser.add_argument("--processes", type=int, default=16)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if os.name != "nt" or C.sizeof(C.c_void_p) != 8:
        parser.error("Windows AMD64 Python is required")
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if (not command or not 0 < args.timeout <= 1800 or
            not 4 <= args.memory_share <= 64 or not 1 <= args.cpu_percent <= 75 or
            not 1 <= args.processes <= 128 or any("\0" in value for value in command)):
        parser.error("invalid command or resource policy")
    executable = Path(command[0])
    if not executable.is_absolute() or not executable.is_file() or executable.is_symlink():
        parser.error("executable must be an explicit absolute regular-file path")
    output_root = args.output_root.resolve(strict=True)
    if not output_root.is_dir():
        parser.error("output root must already exist")
    command[0] = str(executable.resolve(strict=True))
    _unattended_ctest(command)
    line = subprocess.list2cmdline(command)
    if len(line) >= 32767:
        parser.error("Windows command line exceeds its bound")

    kernel = C.WinDLL("kernel32", use_last_error=True)
    size_t, u64 = C.c_size_t, C.c_ulonglong

    class Memory(C.Structure):
        _fields_ = [("length", W.DWORD), ("load", W.DWORD)] + [
            (name, u64) for name in ("total_phys", "avail_phys", "total_commit",
                                   "avail_commit", "total_virtual", "avail_virtual", "extended")]

    class Basic(C.Structure):
        _fields_ = [("process_time", C.c_longlong), ("job_time", C.c_longlong),
                    ("flags", W.DWORD), ("min_ws", size_t), ("max_ws", size_t),
                    ("processes", W.DWORD), ("affinity", size_t),
                    ("priority", W.DWORD), ("scheduling", W.DWORD)]

    class Io(C.Structure):
        _fields_ = [("c" + str(i), u64) for i in range(6)]

    class Limits(C.Structure):
        _fields_ = [("basic", Basic), ("io", Io), ("process_memory", size_t),
                    ("job_memory", size_t), ("peak_process", size_t), ("peak_job", size_t)]

    class Cpu(C.Structure):
        _fields_ = [("flags", W.DWORD), ("rate", W.DWORD)]

    class Accounting(C.Structure):
        _fields_ = [("user_time", C.c_longlong), ("kernel_time", C.c_longlong),
                    ("period_user_time", C.c_longlong), ("period_kernel_time", C.c_longlong),
                    ("faults", W.DWORD), ("total", W.DWORD), ("active", W.DWORD), ("terminated", W.DWORD)]

    class Startup(C.Structure):
        _fields_ = [("cb", W.DWORD), ("reserved", W.LPWSTR), ("desktop", W.LPWSTR),
                    ("title", W.LPWSTR)] + [("d" + str(i), W.DWORD) for i in range(8)] + [
                        ("show", W.WORD), ("reserved_size", W.WORD), ("reserved_bytes", C.c_void_p),
                        ("stdin", W.HANDLE), ("stdout", W.HANDLE), ("stderr", W.HANDLE)]

    class StartupEx(C.Structure):
        _fields_ = [("startup", Startup), ("attributes", C.c_void_p)]

    class Process(C.Structure):
        _fields_ = [("process", W.HANDLE), ("thread", W.HANDLE),
                    ("pid", W.DWORD), ("tid", W.DWORD)]

    def api(name, result, *parameters):
        function = getattr(kernel, name)
        function.restype, function.argtypes = result, list(parameters)
        return function

    memory_status = api("GlobalMemoryStatusEx", W.BOOL, C.POINTER(Memory))
    create_job = api("CreateJobObjectW", W.HANDLE, C.c_void_p, W.LPCWSTR)
    set_job = api("SetInformationJobObject", W.BOOL, W.HANDLE, C.c_int, C.c_void_p, W.DWORD)
    query_job = api("QueryInformationJobObject", W.BOOL, W.HANDLE, C.c_int,
                    C.c_void_p, W.DWORD, C.c_void_p)
    init_attributes = api("InitializeProcThreadAttributeList", W.BOOL, C.c_void_p,
                         W.DWORD, W.DWORD, C.POINTER(size_t))
    update_attribute = api("UpdateProcThreadAttribute", W.BOOL, C.c_void_p, W.DWORD,
                           size_t, C.c_void_p, size_t, C.c_void_p, C.c_void_p)
    delete_attributes = api("DeleteProcThreadAttributeList", None, C.c_void_p)
    get_error_mode = api("GetErrorMode", W.UINT)
    set_error_mode = api("SetErrorMode", W.UINT, W.UINT)
    create_process = api("CreateProcessW", W.BOOL, W.LPCWSTR, W.LPWSTR, C.c_void_p,
                         C.c_void_p, W.BOOL, W.DWORD, C.c_void_p, W.LPCWSTR,
                         C.POINTER(StartupEx), C.POINTER(Process))
    in_job = api("IsProcessInJob", W.BOOL, W.HANDLE, W.HANDLE, C.POINTER(W.BOOL))
    resume = api("ResumeThread", W.DWORD, W.HANDLE)
    wait = api("WaitForSingleObject", W.DWORD, W.HANDLE, W.DWORD)
    terminate = api("TerminateJobObject", W.BOOL, W.HANDLE, W.UINT)
    get_exit = api("GetExitCodeProcess", W.BOOL, W.HANDLE, C.POINTER(W.DWORD))
    close = api("CloseHandle", W.BOOL, W.HANDLE)

    def checked(value):
        if not value:
            raise C.WinError(C.get_last_error())
        return value

    memory = Memory(length=C.sizeof(Memory))
    checked(memory_status(C.byref(memory)))
    # Reserve scales with actual capacity; never borrow the last GiB of either
    # pool. A development envelope is not a filesystem format/allocation limit.
    reserve = max(1024**3, memory.total_phys // 16)
    headroom = min(memory.avail_phys, memory.avail_commit) - reserve
    budget = headroom // args.memory_share // (1024**2) * 1024**2
    if headroom <= 0 or budget < 64 * 1024**2:
        raise RuntimeError("insufficient physical/commit headroom")
    disk_reserve = max(1024**3, budget // 4)
    if shutil.disk_usage(output_root).free <= disk_reserve:
        raise RuntimeError("insufficient development-output free space")
    run = output_root / ("local-run-" + uuid.uuid4().hex)
    run.mkdir()
    temporary = run / "tmp"
    temporary.mkdir()
    environment = dict(os.environ)
    environment.update(TEMP=str(temporary), TMP=str(temporary), MSBUILDDISABLENODEREUSE="1",
                       FLITZFS_VALIDATION_MEMORY_BYTES=str(budget))
    environment_block = C.create_unicode_buffer("\0".join(
        key + "=" + value for key, value in sorted(environment.items(), key=lambda item: item[0].upper())) + "\0\0")
    job, process, attributes, initialized = None, Process(), None, False
    started = time.monotonic()
    report = {"command": command, "timeout_seconds": args.timeout, "memory_bytes": budget,
              "cpu_percent": args.cpu_percent, "maximum_processes": args.processes,
              "physical_available_bytes": memory.avail_phys, "commit_available_bytes": memory.avail_commit,
              "reserve_bytes": reserve, "creation_time_job": False, "exit_confirmed": False,
              "disk_reserve_bytes": disk_reserve, "stop_reason": None,
              "tree_retired": False, "timed_out": False, "residual_children": 0}
    error = None
    try:
        import msvcrt
        with (run / "output.log").open("xb", buffering=0) as log, open(os.devnull, "rb") as null:
            job = checked(create_job(None, None))
            limits = Limits()
            limits.basic.flags = 0x2000 | 0x8 | 0x100 | 0x200 | 0x20
            limits.basic.processes, limits.basic.priority = args.processes, 0x4000
            limits.process_memory = limits.job_memory = budget
            checked(set_job(job, 9, C.byref(limits), C.sizeof(limits)))
            cpu = Cpu(flags=1 | 4, rate=args.cpu_percent * 100)
            checked(set_job(job, 15, C.byref(cpu), C.sizeof(cpu)))
            length = size_t()
            init_attributes(None, 2, 0, C.byref(length))
            if C.get_last_error() != 122 or not 0 < length.value <= 65536:
                raise RuntimeError("unexpected attribute-list sizing")
            attributes = C.create_string_buffer(length.value)
            checked(init_attributes(attributes, 2, 0, C.byref(length)))
            initialized = True
            job_value = W.HANDLE(job)
            checked(update_attribute(attributes, 0, 0x2000D, C.byref(job_value),
                                     C.sizeof(job_value), None, None))
            log_handle, null_handle = msvcrt.get_osfhandle(log.fileno()), msvcrt.get_osfhandle(null.fileno())
            os.set_handle_inheritable(log_handle, True)
            os.set_handle_inheritable(null_handle, True)
            handles = (W.HANDLE * 2)(log_handle, null_handle)
            checked(update_attribute(attributes, 0, 0x20002, handles, C.sizeof(handles), None, None))
            startup = StartupEx()
            startup.startup.cb = C.sizeof(StartupEx)
            startup.startup.d7 = 0x100  # STARTF_USESTDHANDLES
            startup.startup.stdin, startup.startup.stdout, startup.startup.stderr = null_handle, log_handle, log_handle
            startup.attributes = C.cast(attributes, C.c_void_p)
            original_mode = get_error_mode()
            launch_mode = original_mode | _UNATTENDED_ERROR_MODE
            # Set the inherited process mode before the child loader can run.
            # This affects only the private supervisor and its children.
            set_error_mode(launch_mode)
            try:
                if (get_error_mode() & _UNATTENDED_ERROR_MODE) != _UNATTENDED_ERROR_MODE:
                    raise RuntimeError("could not establish unattended Windows error mode")
                checked(create_process(command[0], C.create_unicode_buffer(line), None, None, True,
                                       0x80000 | 0x4 | 0x8000000 | 0x400 | 0x4000,
                                       environment_block, None, C.byref(startup), C.byref(process)))
                report["inherited_error_mode"] = launch_mode
            finally:
                set_error_mode(original_mode)
            os.set_handle_inheritable(log_handle, False)
            os.set_handle_inheritable(null_handle, False)
            assigned = W.BOOL()
            checked(in_job(process.process, job, C.byref(assigned)))
            if not assigned.value:
                raise RuntimeError("creation-time job missing")
            report.update(creation_time_job=True, process_id=process.pid)
            if resume(process.thread) == 0xFFFFFFFF:
                raise C.WinError(C.get_last_error())
            while True:
                state = wait(process.process, 100)
                if state == 0:
                    break
                if state != 258:
                    raise C.WinError(C.get_last_error())
                if time.monotonic() - started >= args.timeout:
                    report["timed_out"], report["stop_reason"] = True, "deadline"
                elif log.tell() > 8 * 1024**2:
                    report["stop_reason"] = "output limit"
                elif shutil.disk_usage(output_root).free <= disk_reserve:
                    report["stop_reason"] = "development-output free-space reserve"
                if report["stop_reason"]:
                    checked(terminate(job, 1460))
                    break
            report["exit_confirmed"] = wait(process.process, 2000) == 0
            if not report["exit_confirmed"]:
                raise RuntimeError("command exit not confirmed")
            code = W.DWORD()
            checked(get_exit(process.process, C.byref(code)))
            report["exit_code"] = code.value
            if reason := _LOADER_ERRORS.get(code.value):
                diagnostic = f"Windows loader failure (0x{code.value:08X}): {reason}"
                report["loader_error"] = diagnostic
                log.write(f"\n{diagnostic}\n".encode("ascii"))
            accounting = Accounting()
            # Process signalling may precede the Job's accounting decrement.
            # Observe quiescence before classifying a live descendant as stale.
            deadline = time.monotonic() + 2
            while True:
                checked(query_job(job, 1, C.byref(accounting), C.sizeof(accounting), None))
                if not accounting.active:
                    break
                if time.monotonic() >= deadline:
                    report["residual_children"] = accounting.active
                    checked(terminate(job, 1223))
                    break
                time.sleep(0.01)
            deadline = time.monotonic() + 2
            while True:
                checked(query_job(job, 1, C.byref(accounting), C.sizeof(accounting), None))
                if accounting.active == 0:
                    report["tree_retired"] = True
                    report["job_user_cpu_100ns"] = accounting.user_time
                    report["job_kernel_cpu_100ns"] = accounting.kernel_time
                    break
                if time.monotonic() >= deadline:
                    raise RuntimeError("owned process tree retirement not confirmed")
                time.sleep(0.01)
            measured = Limits()
            checked(query_job(job, 9, C.byref(measured), C.sizeof(measured), None))
            report["peak_job_bytes"] = measured.peak_job
    except Exception as failure:
        error = str(failure)
    finally:
        if job:
            # Even exception/parent-failure paths close a kill-on-close Job;
            # explicit observation never reopens a process by a reusable PID.
            if process.process and not report["tree_retired"]:
                terminate(job, 1223)
                wait(process.process, 2000)
                accounting = Accounting()
                deadline = time.monotonic() + 2
                while query_job(job, 1, C.byref(accounting), C.sizeof(accounting), None):
                    if accounting.active == 0:
                        report["tree_retired"] = True
                        break
                    if time.monotonic() >= deadline:
                        break
                    time.sleep(0.01)
            close(job)
        if process.thread:
            close(process.thread)
        if process.process:
            close(process.process)
        if initialized:
            delete_attributes(attributes)
    report.update(elapsed_seconds=time.monotonic() - started, error=error)
    with (run / "result.json").open("x", encoding="utf-8") as receipt:
        json.dump(report, receipt, indent=2)
        receipt.write("\n")
    # Some build tools intentionally retain owned helper processes. Stop those
    # after the command exits and report the count; only confirmed tree-wide
    # retirement permits success. This does not claim natural helper exit.
    success = (error is None and report["exit_confirmed"] and report["tree_retired"] and
               report["stop_reason"] is None and report.get("exit_code") == 0)
    print(json.dumps({"success": success, "run": str(run), **report}))
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
