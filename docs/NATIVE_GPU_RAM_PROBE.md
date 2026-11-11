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
the exact matching decoded native extent. Only one valid match request is
allowed. Namespace metadata receives no worker request. The retained source,
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
A malformed-framing warmup initializes CPU transport bookkeeping only, before
any RAM image or compute request. The child must reject it before runtime load.

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
- --driver-once NEW_REPORT TRUSTED_WORKER ORDINAL LUID RUNTIME_SHA256 is an
  explicitly requested diagnostic, not an automatic filesystem route. The
  caller must verify source/bundle hashes, trusted containing directories,
  current identity/headroom and prior matching VM qualification. It uses the
  existing pinned installed-runtime worker, never installs a runtime/driver.

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

## First hosted composition result

Implementation 480573e compiled and passed self-contained dependency audit and
the original locked-RAM CPU probe. Its new valid CPU-worker composition reached
the final handle-accounting gate and failed with native GPU/RAM handle growth
(run 43019981741, job 130520457733). The retained failure report confirms
locked_after=0 and cleanup_ok=true. The exact handle delta/cause was not yet
captured. No native GPU/RAM binary was promoted or run on the owner PC.

The next diagnostic retains the zero-growth assertion. It records handle counts
after transport warmup, working-set admission, CPU profile, proposed profile,
working-set restoration and final retirement. If that initial gate fails in
the hosted CPU-fixture mode only, two additional CPU/native-plus-CPU-worker
profile pairs record whether growth is one-time or repeated. The original
failure remains a failure regardless of those observations. This bounded
diagnostic never repeats a GPU request or runs on the driver mode.

Trace positions: 0 baseline, 1 working-set admitted, 2 CPU profile complete,
3 proposed profile complete, 4 working-set restored, 5 final retirement,
6/7 first extra CPU/CPU-worker pair, 8/9 second extra pair. Zero entries can
mean an unreached phase. This is investigation, not a relaxed tolerance,
a presumed OS cache explanation or a repaired leak.

The first diagnostic (c2ba1e2, run 43022295520, job 130528208214)
retained the failure and observed [98,98,98,99,99,99,99,99,99,99].
The first valid worker/native profile added one handle; the following two
CPU/native-plus-CPU-worker pairs added none. This bounds the observed growth
but does not prove ownership, intentional initialization or leak freedom.
The next finer first-occurrence trace separates match preparation, exchange,
reply validation, hinted encoding, publication, context retirement and final
verification. Its eight positions are: 0 before preparation, 1 request ready,
2 synchronous exchange returned, 3 reply validated, 4 hinted record encoded,
5 head published, 6 contexts closed, 7 independent verification complete.
The zero-growth gate and no-owner-execution boundary remain unchanged.
