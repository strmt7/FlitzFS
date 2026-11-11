# Isolated writable source and external client (experimental)

11 November 2026. D01 candidate; executed qualification is required. This
document does not declare the intermittent retirement defect resolved.

## Evidence and hypothesis

The earlier source at d1e66f6 reached its explicit-flush acknowledgement but
did not confirm child exit after the unchanged 15-second operation deadline
and two-second observation. The underlying kernel state was not captured.
The 343acaa matrix later passed; that does not explain the previous failures.

The old crash/crash-restart source also held the Win32 client file handle
inside its filesystem-host process until forced termination.
[Upstream issue 702](https://github.com/winfsp/winfsp/issues/702), created
29 October2026, describes a self-namespace-I/O termination hazard. It is a user
report without maintainer confirmation, not proof of our failure's cause.

The selected current SDK is v2.2B5, source
14d6e0280ae15e2550c44d6dbc476be51649727f.
Its [originating-process API](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/inc/winfsp/winfsp.h)
is FspFileSystemOperationProcessId, valid ONLY during Create/Open or an eligible
Rename. The exact pinned header blob is
0bfa8a25d8d33e893c52df49bdfc9bafafcbbb5b, read through the native GitHub API.
No process identity query is added to arbitrary callbacks.

## Ownership protocol

The existing controller becomes the external application client. One separately
supervised child hosts the source filesystem. There is no additional process,
service, driver, arbitrary executable, mount path or image argument.

1. The controller creates three unpredictable, protected local-session manual
   events: ready, written and acknowledgement. Objects grant only the current
   user SID; inheritance is disabled and an existing name is rejected.
2. The host opens the role-specific event rights and starts only its own fresh
   disposable mount. Setup must leave zero native file contexts.
3. Test-only Create/Open guards are armed. An actual self CreateFileW attempt
   must fail ACCESS_DENIED, without acquiring a native context or changing
   generation/extent/namespace. Original native authorization/adoption checks
   still handle admitted requests.
4. The host signals ready. The external controller opens the mounted namespace,
   creates/writes/FlushFileBuffers/reads sample.bin and keeps that handle OPEN.
   The host's pinned SDK callback must observe that controller's real PID;
   a successful original Flush callback must also be observed.
5. The controller signals written. The host independently verifies the native
   image, confirms one external context and publishes its bounded acknowledgement,
   then signals acknowledgement and performs no further data I/O.
6. The original process supervisor terminates the host at15 seconds and retains
   its exact process object until confirmed exit. The external client handle
   remains open through host and supervisor retirement, then closes.
7. Only confirmed retirement authorizes the existing resumer/CPU verifier.
   The retained source process lease prevents PID reuse during reconciliation.

Ready/written/acknowledgement phases each have five-second diagnostic bounds.
These do not replace or widen the host operation/termination deadlines.
Unknown retirement is terminal: no replacement source, no next verifier and
no successful qualification. One bounded test-only quarantine retains unknown
ownership until the failed controller exits; it is never an owner-device route.

The guard wraps the actual pinned SDK callback table only for the killed source.
It is not a replacement production authorization mechanism. Normal ordered-stop
profiles remain unchanged. General Windows helper self-I/O policy still requires
production design and client qualification.

## CPU-only prerequisite and resource containment

supervised_source_fixture.hpp schedules the existing run_bounded_process;
it does not duplicate its process creation, argument quoting, creation-time job
assignment, memory/CPU caps, kill-on-close or exit-code logic. It keeps the exact
observer and a separately confirmed supervisor-thread lease. One source slot
cannot be reused until both retire. Unknown observation never reopens a PID.

Normal Windows CTest uses only its own trusted CPU helper for real event-role,
collision, identity, cancellation, timeout, abandonment, failed-launch and
repeated handle-retirement cases. It never creates a filesystem or dispatcher.
These checks must pass before the existing isolated mount jobs can start.

The controller reuses the already qualified CPU-only diagnostic budget:
128 MiB/process,256 MiB aggregate, two active processes,20% CPU, below-normal,
ten seconds per-process user CPU time and fail-closed physical/commit headroom.
Despite its historical gpu_probe_budget.hpp name, this utility performs no GPU
operation. Each source/resumer/verifier keeps its original128-MiB one-process
20%-CPU kill-on-close child job. Source deadline remains15 seconds; ordinary/
resumer/verifier deadlines remain30 seconds; child exit observation remains2.

The hosted PowerShell wrapper adds a100-second TOTAL controller wall bound,
plus2-second termination confirmation, disposing its process handle and
checking bounded output (16 KiB stdout/4 KiB stderr). This encloses the existing
three child phases; it does not make any child deadline longer. A kernel fault
still cannot be converted into a no-freeze guarantee by user-mode limits.

Images remain <=4 MiB, source+continuation report <=2048 bytes and independent
report <=2048 bytes. No third report, recursive deletion, in-place repair,
source overwrite, GPU work, owner mount or owner-driver operation is added.
All fixtures remain on disposable GitHub-hosted Windows machines.

## Evidence compatibility

The historical schema2 evidence stays readable. The new crash source variant
adds exactly source_protocol="external-client", client_process,
host_self_io_rejected and client_flush_callback_observed. The acknowledgement
observes the last two facts; the independent verifier must report them false,
while binding the same original client PID and exact source acknowledgement.

The new live crash campaign REQUIRES that variant. Missing, mixed, malformed,
self/observer/resumer-aliased actors and invented observer claims reject.
Old reports cannot silently satisfy the new live actor-isolation gate.
Native image encodings and existing worker frames are unchanged.

The source acknowledgement still binds every old byte, metadata, security,
reservation and checkpoint property. Restart still compares its complete
original acknowledgement before acquiring the exclusive writer lease.
Continuation/final state digests include the new protocol fields. A passing
JSON record alone is not a mounted recovery result or an explanation of the
historical failure.

## Qualification required

Review CPU/raw/native/sanitizer checks, exact SDK build and both affected
mounted profiles, including real source guard rejection and independent
recovery. Preserve failures and test before/after causal evidence rather than
declaring D01 fixed from a green run. No owner experiment is authorized.

## Executed checkpoint at 8122bf5

The initial full 13-job matrix passed at
8122bf51f6ac3cf43512f8a2dc37368afa0772c5. Windows native CTest passed 131 checks,
Windows raw passed 117, and both Linux configurations and ASan/UBSan passed.
Both affected mounted profiles observed the real external client, rejected
the host's self-open and confirmed source termination with exit 1460 under the
unchanged bounds. Independent CPU recovery retained the acknowledged state.
The crash-restart profile also resumed writes under a separate process.

[Exact reports and run/actor/retirement evidence](evidence/mount-source-isolation-8122bf5.json)
retain raw integer text and distinguish the initial matrix from repetitions.
These observations do not reconstruct the older kernel state or establish
that issue 702 caused those failures. D01 remains open.

The next CPU-only fixture revision additionally covers cancellation and
destructor abandonment after acknowledgement, plus an early real child exit
before ready. It requires the exact nonzero exit 37 and confirmed retirement;
neither ready nor acknowledgement may be invented. Warm each real path, then
repeat protocol, failed launch, post-ack cancellation, post-ack abandonment and
early exit eight times with exact handle equality. No tolerance, mount, driver,
GPU operation, larger deadline or new product route is added. These new checks
require their own publishing-commit qualification.
