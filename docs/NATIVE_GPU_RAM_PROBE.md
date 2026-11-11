# Native recompression and match-worker locked-RAM probe

Experimental G02 integration, 11 November 2026. The publishing commit must pass
its hosted qualification before any owner-device experiment. This source change
does not establish native GPU compression benefit, general performance,
production data safety or physical power-loss resilience.

## Composition and boundaries

The OFF-by-default FLITZFS_BUILD_GPU_RAM_PROBE target builds
flitzfs_gpu_ram_probe on portable Windows with mandatory static Zstandard,
tests and an exact source commit. Ordinary builds, CTest, file operations and
mount callbacks never invoke it. It installs nothing, mounts nothing and calls
no WinFsp API. It accepts a trusted helper path, never an application-data path.

The probe reuses locked_ram_fixture::WorkingSet/Store and the existing native
managed-session volume-head recompression/handoff. The source and target each
have a 2 MiB capacity with separate locked live/stable buffers; the head has its
existing 128 KiB capacity. There is no disk-image backend or pageable backing
fallback. The small ordinary-file output is a report, created atomically with
CREATE_NEW before work. Its ordinary ASCII leaf must be named native-*.json,
which excludes device-name leaves before opening. The resulting handle must
identify an ordinary single-link disk file. Existing output is never overwritten.

A synthetic 64 KiB file contains repeated seeded data plus zeros. A CPU-only
profile runs first, then a separate profile optionally obtains proposals for
the exact matching decoded native extent. Only one proposed-profile match request is
allowed. An additional explicitly reported CPU-only successful exchange primes
transport bookkeeping before measurement; it never launches HIP. Namespace metadata receives no worker request. The retained source,
live file handle, independent selected-image CPU reader and published manifest
must agree byte for byte. The original source image must remain unchanged.

Each copied record supplies actual recompression_feedback(), including whether
the optional suggestion encoding was selected and its ordinary CPU baseline.
Accepted proposals, forwarded proposals and selected encodings remain distinct.
A successful worker with an equal-size or rejected encoding is not a
compression-ratio or speed gain.

## Containment

gpu_probe_budget.hpp factors the existing hardware diagnostic budget without
changing its values: 128 MiB/process, 256 MiB aggregate, two active processes,
20% CPU, below-normal priority, ten seconds of per-process user CPU time and
physical/commit admission. The existing CPU RAM probe retains its separate
strict one-process job; its contract is not loosened.

The worker retains its own creation-time 128 MiB/20%/one-process kill-on-close
job and five-second operation limit. Explicit device allocation remains within
256 KiB, with the existing 64 KiB input, bounded lanes and bounded search effort.
Before any RAM image or GPU request, a malformed-framing warmup must be rejected
before runtime load, and one successful trusted CPU-helper exchange must pass
independent CPU proposal validation and confirmed child/supervisor retirement.
Both costs and all three warmup handle observations are reported. Neither
warmup launches HIP; strict handle equality covers the subsequent native pair.

Unknown child or supervisor retirement terminates the experiment and forbids a
replacement request. Failed, timed-out or malformed replies with confirmed
retirement select ordinary CPU recompression without another worker request.
The driver mode returns status 2 when CPU recovery succeeds but no GPU candidate
was accepted; status 0 alone in CPU-fixture mode is never GPU evidence.

Working-set limits must be restored, all locked buffers released, the handle
count unchanged and the worker/supervisor slot available before success.
Hosted orchestration also bounds the complete process by 15 seconds plus the
existing two-second termination observation, captures bounded output, disposes
its process handle and refuses to continue after unconfirmed retirement.
No services, tasks, reboot persistence, elevation or driver changes are used.

## Explicit modes and trust

- --cpu-fixture NEW_REPORT TRUSTED_HELPER FIXTURE_MODE is GitHub-hosted only.
  It exercises real CPU worker processes using match, match-invalid,
  match-hip-declaration and match-hold. Even a fake HIP declaration cannot
  turn those tests into GPU execution.
- --driver-once NEW_REPORT TRUSTED_WORKER ORDINAL LUID RUNTIME_SHA256 TRUSTED_CPU_HELPER is an
  explicitly requested diagnostic, not an automatic filesystem route. The
  caller must verify source/bundle hashes, trusted containing directories,
  current identity/headroom and prior matching VM qualification. It uses the
  existing pinned installed-runtime worker, never installs a runtime/driver.
  TRUSTED_CPU_HELPER must be the separately hashed portable
  flitzfs_worker_exchange_tests.exe from the same qualified RAM package.
  Its exact match/channel mode is CPU-only; it is diagnostic infrastructure,
  not a product runtime dependency.

Named-object/current-user protection is not a hostile same-user sandbox.
The helper executable and its directories must be trusted, as in WORKER_CHANNEL.md.
The execution tag is a declaration from the trusted worker, not cryptographic
hardware attestation. There is no automatic discovery or retry in this probe.

The five-second diagnostic startup/retirement scope does not widen the
production controller's 1–100 ms result gate. Safe amortization and complete
startup-versus-batch accounting remain G03 work.

## What measurements mean

The report separates source/target payloads, CPU-baseline payloads, active image
plus head, retained source plus target plus head, native preparation/copy/
publication/readback, CPU match preparation, complete worker exchange, reply
validation, profile totals including image destruction, warmup and pair time.
It retains worker exit/category/stage and committed-memory peaks.

The CPU profile always runs first in this single integration fixture. Its
timings are not a fair repeated benchmark or isolated kernel measurement.
They cannot establish superiority over another filesystem. Whole-image/history/
head costs and retained copies must not be omitted from compression claims.

The RAM checkpoint restoration is a volatile persistence model while the owner
process lives. It is explicitly not a physical power-loss qualification.

## Qualification and retained failures

The ram-host workflow first compiles/audits the self-contained binaries, then
runs the four CPU-only composition modes, checks byte-exact native recovery,
feedback/fallback, source preservation, resource/handle cleanup and existing-
report/unknown-mode rejection. Failure reports are retained separately; binary
promotion requires every check. No actual GPU executes in that workflow.

At initial publication the new probe and shared-budget refactor are unqualified.
The earlier feedback implementation at d1e66f6 passed the CPU/codec/raw,
sanitizer, native callback and SDK checks. Its complete matrix was 12/13:
mounted crash-restart job 130502518872 again reported timeout with unconfirmed
child exit. [The retained failure](evidence/retirement-failure-d1e66f6.json)
remains D01, with unknown root cause. It is not repaired by a green rerun,
a longer timeout or this composition probe.

Related: RECOMPRESSION_FEEDBACK.md, GPU_RESOURCE_ENVELOPE.md, WORKER_CHANNEL.md,
HOST_RAM_VALIDATION.json, DEVELOPMENT_PLAN.json.

## Handle-accounting investigation and candidate correction

The first valid composition failed at strict handle equality at 480573e.
Diagnostic c2ba1e2 observed [98,98,98,99,99,99,99,99,99,99]: first valid worker
profile added one; two additional CPU/worker pairs stayed flat. Finer 61f16ad
trace [98,98,99,99,99,99,99,99] localized retention to synchronous exchange,
before reply validation and native hinted encoding. An initial snapshot
diagnostic refused an optional NUL code unit; its cleanup succeeded.

At fbd9dd8, bounded current-process snapshots identified exactly one added
WaitCompletionPacket (8 to 9); every other observed type count stayed equal.
Nine isolated SleepConditionVariableSRW calls remained [81,81,81,81,81,81,81,81,81,81].
The native-condition-variable initialization hypothesis was therefore rejected.
The exact internal Windows call creating the packet is not identified.
Project transport directly owns events, mappings, jobs, tokens, process and
thread handles; none of its source directly creates a WaitCompletionPacket.

[The complete retained result](evidence/handle-accounting-fbd9dd8.json) binds
these observations to run 43025095991 and artifact 13857101726, verified SHA256
e627a6416dcec8ccc10a85f5352156f1575fc6ba80b0352f76598eeaab2abf7d.
These are failing diagnostic results, not prior qualification or leak freedom.

The candidate explicitly exercises both actual failure and successful CPU
transport before native measurement, instead of relying on a malformed request
to prime the successful path. A separate new-process hosted accounting mode
runs four independently validated successful exchanges and requires unchanged
counts after the first. All four native positive/negative modes still require
strict before/after equality. This candidate needs its executed matching result;
an unexplained growing count remains failure, never added tolerance.

Temporary snapshot inventory and the isolated native-wait probe were removed
after retaining their evidence. They are not owner-device prerequisites or
ordinary filesystem behavior. Existing source/target preservation and all RAM,
process, device, queue, CPU and time limits remain unchanged.

## Source-evidence artifact budget

The measured fbd9dd8 tree contains 366 tracked files totaling 4,907,006 bytes;
its largest file is 223,618 bytes. Maximum ZIP compression briefly fit the old
2-MiB archive bound, but continued diagnostic source growth exceeded it again.
The source-only hosted artifact is now bounded at 4 MiB compressed, 8 MiB
expanded, 512 tracked files, 768 ZIP entries and 2 MiB per file, with no links
or submodules. These measured source-staging bounds fit within the existing
256-MiB combined HDD fixture cap; callers still verify actual current usage,
the 10-GiB reserve, exact hashes and CREATE_NEW ownership before any staging.
This does not change RAM filesystem backing, image sizes or execution budgets,
and authorizes no automatic owner download or physical-drive filesystem image.

## Executed qualification at 645a1d1

All 13 core jobs and all four native CPU-worker modes passed. Independent
success-only accounting was [98,99,99,99,99]; each composition retained strict
handle equality and zero locked bytes after retirement. The portable package
contains the separately hashed CPU helper and probe, with no SDK/runtime.

One owner synthetic 64-KiB native trial then used the SDK-qualified worker from
480573e. Exact tree comparison proves its GPU source inputs unchanged through
645a1d1. The full source/artifact/byte/timing record is
[evidence/native-gpu-ram-645a1d1.json](evidence/native-gpu-ram-645a1d1.json).

The GPU returned 56 CPU-verified proposals. All reached native candidate
encoding, none was selected, and payload stayed at the CPU baseline of 8,316
bytes. The cold exchange was 706.5 ms; complete CPU/proposed profiles were
5.4/714.2 ms. The experiment completed in 906 ms including warmup and outer
overhead. This is one observation, not kernel timing or a comparative benchmark.

Locked peak was 8,650,752 bytes, explicit device buffers 229,376 bytes,
aggregate job peak 91,250,688 bytes and worker peak 79,966,208 bytes.
Handles remained116→116; locked bytes returned to zero. Postflight found no
owned worker, changed staged input or driver/runtime change. No mount,
driver operation, persistent worker or physical filesystem image occurred.
No physical-power-loss or universal safety qualification follows.

The integration checkpoint is established; G02's useful-benefit exit remains
unmet. Keep ordinary I/O CPU-only. Do not repeat this unchanged GPU fixture.
