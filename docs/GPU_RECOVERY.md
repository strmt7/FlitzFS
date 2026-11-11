# Evidence-aware GPU recovery and CPU selection

Development date: 9 November 2026. Experimental; not a live HIP runtime.

## Implemented mechanisms

GpuResultGate::use_cpu_fallback immediately returns its immutable, independently
decoded CPU reference and terminally fences the request. It never waits for a
worker, frees GPU memory, unloads a runtime or calls a driver. Late candidate
replies cannot replace the selected CPU result. Callers must respect reference
lifetime; malformed storage rejected during CPU preparation never reaches it.

GpuRecoveryPolicy controls later admission using fixed storage and bounded work.
It has no sleeps, polling, scans, allocation, worker creation or runtime calls.
Workload qualification remains an explicit input; no speed benefit is invented.
Both policy and per-request gate use the same memory-admission calculation.

| Outcome | Later candidate admission |
|---|---|
| One temporary pressure failure | Next distinct fresh, compatible, sufficiently funded and quiescent observation can qualify |
| Repeated temporary failures | Require 2, 4, 8, 16, then 32 distinct healthy observations; replaying telemetry or incrementing its sequence over an old timestamp cannot count |
| Runtime unavailable or configuration unsupported | Keep CPU route for that configuration without repeated discovery |
| Device/context/driver fault | Require separate successful recovery qualification plus fresh budgets and supervisor-confirmed quiescence |
| Invalid/mismatched output | Quarantine the configuration; time or fresh budgets do not repair integrity |
| Unconfirmed worker retirement | CPU work may continue; every later GPU attempt stays blocked, including changed configurations |

Two successful accepted attempts clear transient failure history; one lucky
success does not. Clock/sequence rollback, changed adapter under the same
configuration and stale/invalid budgets cannot manufacture health. Reports are
single-use and ticket-bound. Late success cannot erase failure. A separate
supervisor confirmation can retire a previously reported outstanding worker.

Eight quarantined configurations are remembered so A -> B -> A cannot forget
A's fault. Exhaustion blocks GPU candidates rather than evicting safety history.
Copy/move is disabled. One control thread owns one policy per supervised session.

## Trust and integration boundary

Configuration identities must bind a separately qualified adapter/node,
runtime/module/compiler contract and discovery state. A retry loop cannot
invent identities to escape quarantine. Recovery generations come from
independent successful qualification, not a timer, notification, memory query
or worker exit alone. These are trusted API inputs; this component does not
authenticate them. Their production collector is not yet implemented.

Discovery belongs off callbacks, driven by relevant device/installation changes,
with bounded processes and coalesced events. It must not repeat for every read.
This policy grants only permission to try; consent, workload qualification,
budgets, statuses, deadlines and independent CPU-byte comparison still apply.

WorkerExchangeSession now supplies asynchronous transport with an independently
owned supervisor. CPU gate selection can proceed while the exact helper is
still live, and cancellation/retirement remain off that selection path.
Replacement transport admission requires both exact-child and supervisor-thread
retirement. See WORKER_CHANNEL.md for the single-slot and failure contract.
The explicit GpuWorkerController now connects this policy to asynchronous
transport, CPU selection and exact retirement; see GPU_WORKER_CONTROLLER.md.
Trusted runtime/discovery evidence and an actual HIP worker remain unfinished. Per-request gates stay
terminal; a later qualified request creates a NEW gate, never reuses poison.

Runtime loading, driver recovery evidence, hardware tests, production routing
and measured GPU benefit remain absent. Default CLI/callback paths stay CPU-only.
No driver cleanup can be called from CPU-fallback selection.

## Failure-classification research

AMD distinguishes missing devices, insufficient drivers, resource exhaustion
and destroyed contexts. These are different observations, not a generic timeout.
Clearing the last error does not independently qualify a repaired device.
Sources: [HIP error codes](https://rocm.docs.amd.com/projects/HIP/en/latest/reference/error_codes.html)
and [HIP error handling](https://rocmdocs.amd.com/projects/HIP/en/latest/how-to/hip_runtime_api/error_handling.html).
The runtime-error mapping still needs source review and implementation.

## Executed checks and latest host restriction

Before the later remote-only restriction, a Windows raw-only build and 19
sequential CPU CTest entries passed, including 947 new recovery/fallback checks,
1,819 existing gate/semantic checks and 705 wire checks. They exercise failure
classes, hysteresis, stale evidence, late output, delayed retirement,
configuration changes and quarantine exhaustion. No HIP runtime was loaded.

A task-scoped reviewed launcher assigned process trees to a Windows Job Object
at creation, without handle inheritance, with below-normal priority and a 20%
CPU cap. Configuration limits were 512 MiB/45 seconds; compilation 1 GiB/180
seconds, one build job; CTest 256 MiB/120 seconds and 20 seconds per test. Physical
and commit headroom plus 1 GiB reserve and at least 2 GiB free disk were checked.
Observed peak job memory was 279445504/285491200/94646272 bytes; elapsed times
were 4.90/41.50/2.14 seconds. The temporary-directory incremental-build warning
was reported; no build errors occurred. Process-supervisor and worker-exchange
tests were excluded locally and remain hosted checks.

That run used a NEW temporary disk fixture before the owner imposed the hard
host boundary. It was not RAM-only. No cleanup or additional host execution
will be performed under the current restriction. All further disk-based builds,
research tools and tests run remotely. Host RAM-only operation is not qualified:
Windows may page anonymous memory, and no RAM-disk/driver workaround is allowed.

Remote implementation commit f3ab1c5e8b55a5628010db31cd437d2b526d99a1 passed
all four workflows: [six-job CPU/native/mount matrix](https://github.com/strmt7/FlitzFS/actions/runs/42596551385),
[upstream compatibility](https://github.com/strmt7/FlitzFS/actions/runs/42596551133),
[offline HIP compilation](https://github.com/strmt7/FlitzFS/actions/runs/42596551288)
and [source evidence](https://github.com/strmt7/FlitzFS/actions/runs/42596551093).
Portable configurations each passed 18 CTest entries. These remain CPU/control
results, not live HIP/driver recovery or writable power-loss qualification.

Further review found that a partly accumulated healthy streak could survive
an explicit incompatible, missing-identity/sequence, non-quiescent or rolled-back
clock observation. The next correction resets the streak for these interruptions.
A strengthened test covers all five cases; the dedicated hosted regression lane
must observe the exact old failure at f3ab1c5, then pass the corrected test. Its
results remain pending at publication of that correction. All work after the
host boundary used remote repository tools only; the local checkout is untouched.

The same pure policy now admits typed match searches as well as decoder batches. Each overload supplies its actual immutable device allocation to the existing telemetry and quarantine logic. A match requires at least four input bytes. Workload type does not bypass an outstanding ticket, and fresh match telemetry cannot clear a device fault. See GPU_MATCH_CONTROLLER.md.
