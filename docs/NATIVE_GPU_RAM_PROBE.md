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

## Hosted failure investigation

The strict before/after handle assertion remains unchanged. No new native
GPU/RAM binary has been promoted or executed on the owner PC.

| Commit | Hosted run / job | Executed result |
| --- | --- | --- |
| 480573e | 43019981741 / 130520457733 | Build, PE audit and original CPU RAM probe passed; new composition failed at final handle equality. Locked bytes returned to zero and cleanup succeeded. |
| c2ba1e2 | 43022295520 / 130528208214 | Handle trace [98,98,98,99,99,99,99,99,99,99]. First valid worker/native profile added one; two further CPU/worker pairs added none. |
| 61f16ad | 43023245074 / 130531412710 | Match trace [98,98,99,99,99,99,99,99]: retention occurs inside synchronous exchange, before reply validation and hinted encoding. |
| 801188e | 43024232697 / 130534742501 | Snapshot diagnostic refused a type-name code unit before native profiles. Snapshot/walk cleanup succeeded; locked bytes remained zero. No ownership conclusion. |

Retained diagnostic artifacts: 13855378157 SHA256
dbdbdba95c8ea11b15ac1629f3680e40c8fde7aae9265f8e771d2cdb06ca30c2;
13856376160 SHA256
bd3d8ef8f049bbaa03b671d25c0dafcaa8a928952db2f540c093163478d464cd;
13856182541 SHA256
0b4f3efb79582df9f85ca250536e542f467eb523b7ba541bb395cc7722835d3f.
These are failing results, not leak-free qualification or proof of OS caching.

Main trace positions: baseline, working-set admission, CPU profile, proposed
profile, working-set restoration, final retirement, then two additional
CPU/CPU-worker pairs. Match trace positions: before preparation, request ready,
exchange returned, reply validated, hinted record encoded, head published,
contexts closed, independent verification complete. Zero may mean unreached.
Additional profiles are diagnostic CPU fixtures only; never extra GPU requests.

The next hosted-only diagnostic snapshots only its own handle table, without
VA cloning, thread/context capture or object-name output. It admits at most
256 handles and emits at most 32 type counts. Owned capture/walk objects are
released with their matching APIs. Type names respect the documented byte
length, permit an optional NUL and encode other code units numerically.
CPU-fixture success is explicitly refused while this inventory is enabled,
even if snapshot initialization changes the observed delta.

A separate new-process fixture measures nine 1-ms
SleepConditionVariableSRW calls. It accepts spurious wakeups and rejects
unexpected API errors or growth after first use. It does not warm the
composition process, load a GPU or create a native volume. An initialization
explanation remains unproven until these independent results are inspected.

API sources were inspected through native GitHub source captures:
[Microsoft ProcessSnapshot.h](https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/um/ProcessSnapshot.h),
blob 2afedd9b780837c3717770481fbd2e4a6471d08b;
[PSS_HANDLE_ENTRY contract](https://github.com/MicrosoftDocs/sdk-api/blob/docs/sdk-api-src/content/processsnapshot/ns-processsnapshot-pss_handle_entry.md),
blob ea73a3e9f08d37c186fb63387ed4e57937051784;
[Microsoft STL wait primitive](https://github.com/microsoft/STL/blob/main/stl/src/primitives.hpp),
blob 3cba95a9a74b843e5a4a86b128107db9a255cfed.

The source-evidence archive at 61f16ad exceeded its 2-MiB cap by 1,391 bytes.
Git ZIP compression level 9 passed at 801188e: 2,096,461 source-archive bytes.
The existing cap and source-byte hashing remain unchanged.
