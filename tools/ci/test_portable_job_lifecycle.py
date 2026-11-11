#!/usr/bin/env python3
"""Actual private-job contention; hosted Windows only, no fixture files."""
import json
import os
import subprocess
import sys
import check_portable_cpu as checker

def close_child(child):
    failures=[]
    try:
        if child.poll() is None:
            child.kill()
        child.wait(timeout=5)
    except Exception as error:
        failures.append(error)
    for stream in (child.stdin,child.stdout):
        try:
            if stream is not None and not stream.closed:
                stream.close()
        except Exception as error:
            failures.append(error)
    if failures:
        raise ExceptionGroup("private lifecycle child cleanup",failures)

def main():
    checker.require(sys.platform=="win32" and os.environ.get("GITHUB_ACTIONS")=="true" and
        os.environ.get("RUNNER_ENVIRONMENT")=="github-hosted","disposable hosted Windows VM required")
    checker.contain()
    baseline=checker.wait_for_job_slot()
    checker.require(baseline["active"]==1 and baseline["active_limit"]==2,"private job admission")
    child=subprocess.Popen([sys.executable,"-I","-c","import sys; sys.stdin.buffer.read(1)"],
        stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    released=False
    try:
        checker.require(child.stdin is not None and child.stdout is not None,"owned lifecycle pipes required")
        def release():
            nonlocal released
            checker.require(child.poll() is None,"contention child exited before release evidence")
            released=True
            child.stdin.write(b"c");child.stdin.flush();child.stdin.close()
        ready=checker.wait_for_job_slot(on_busy=release)
        checker.require(released and ready["maximum_active_observed"]==2 and ready["active"]==1 and
            ready["total"]==baseline["total"]+1,"job guard admitted before exact competing child retirement")
        checker.require(child.wait(timeout=3)==0,"contention child failed")
    finally:
        close_child(child)
    result=subprocess.run([sys.executable,"-I","-c","print('ok')"],capture_output=True,timeout=3)
    after=checker.wait_for_job_slot()
    checker.require(result.returncode==0 and result.stdout.strip()==b"ok" and after["active"]==1 and
        after["total"]==baseline["total"]+2,"one launch after retirement failed/extra process admitted")
    print(json.dumps(dict(schema=1,portable_job_lifecycle=True,baseline=baseline,retired=ready,
        continued=after,controlled_children=2,launch_retries=0,files_created=False)),flush=True)

if __name__=="__main__":
    main()
