# Explicit GPU worker request controller

GpuWorkerController connects the existing recovery policy, per-request result
gate and asynchronous Windows worker session. It is an opt-in user-mode library
primitive, not a live HIP worker or an automatic filesystem route. Normal CLI,
callbacks, storage publication and recovery remain CPU-only.

## Request lifetime and bytes

One control thread owns a persistent controller. begin consumes a prepared
immutable batch only after rejecting an overlapping waiting request. It first
attempts zero-time retirement of any previously reported worker, then evaluates
consent, workload qualification, external health evidence and ACTUAL transport
availability. External supervisor_quiescent=true cannot override a retained
child/thread lease. A denied route selects the prepared CPU bytes without
examining or launching the supplied helper.

Each admitted attempt gets a fresh gate and globally unique wire generation.
The bounded transport runs only the externally trusted helper. poll waits on
nothing: pending returns waiting; an expired gate selects CPU and signals
cancellation; a completed helper's immutable reply must pass expected backend,
generation, framing, task status, exact independent CPU comparison, fresh budget
and nonlocal-growth checks before worker bytes are selected. A backend tag
declares execution type; it does not attest that a GPU ran.

use_cpu_fallback selects CPU for a waiting request without an OS/runtime call.
Cancellation is a separate explicit signal. The supervisor still enforces its
process deadline if a caller forgets cancellation. Once either result is selected,
later replies/polls cannot change its bytes. CPU selection/deadline does not
fabricate a final worker outcome: the pending policy ticket remains active until
try_retire can classify the immutable result. A late missing component or corrupt
reply still blocks that configuration; a valid late reply remains pressure rather
than retrospectively accepting expired output. Each ticket is reported once. selected_bytes
is a borrowed immutable span valid until the next begin/destruction; consume it
before replacing the current request. Callers must bound retained results.

A new request can select its own CPU bytes while a previous helper is retiring.
Its batch replaces only the completed foreground request; the previous session
retains its independently owned frames, exact process observation and supervisor.
The policy's active ticket prevents a replacement worker even if configuration
identity changes. Keep the OLD gate/reference/generation until any late result
has been classified, even if a later CPU request has different data. try_retire
then confirms BOTH exact child and supervisor-thread exit and releases that same
policy ticket/session. It may verify one bounded immutable late reply; it never
waits for the worker. Failure counters change on actual outcome reporting, not
merely because CPU was selected. No PID reopening, sleep,
driver cleanup, discovery scan or replacement queue occurs in the controller.

Destroying a controller abandons its session to the existing supervised lifetime;
it does not kill or join on the caller. Process-wide transport admission still
prevents overlap with another controller until exact retirement. Keep one
controller across application requests so its advisory quarantine history is not
discarded. Persisted cross-restart learning/history remains future work.

## Failure evidence

| Observation | Policy outcome | Required before another candidate |
|---|---|---|
| Accepted verified reply | accepted | Fresh qualified telemetry and exact retirement |
| CPU-helper timeout/cancellation, valid late reply or setup allocation failure | temporary_pressure | Exact retirement and the policy's increasing independent healthy-observation threshold |
| HIP-worker timeout or cancellation, including cancellation after foreground gate expiry | device_fault | Exact child/thread retirement AND separately advanced recovery qualification; fresh telemetry/time alone cannot admit a retry |
| Missing helper/path/module or invalid executable image from a native launch error | runtime_unavailable | A separately qualified changed configuration; no repeat launch for unchanged identity |
| Malformed transport, wrong backend/generation/task status or corrupt bytes | integrity_failure | Changed independently qualified configuration; A-to-B-to-A retains the block |
| Abnormal child, unknown supervisor failure or uncertain retirement | device_fault | Exact retirement plus separate recovery qualification; elapsed time/VRAM alone is insufficient |

Selecting CPU alone does not decide whether a driver failed: the immutable terminal
transport result supplies that evidence. A valid late HIP reply is independently
verified and can remain pressure; a timed-out/cancelled HIP transport cannot. This
conservative classification also applies to deliberate cancellation because the
controller cannot establish whether device work was submitted. A zero-time process
exit observation proves retirement only, never hardware recovery.

Known native out-of-memory/resource errors count as pressure, not driver repair.
Unknown errors are conservative; runtime-specific HIP error classification is
still absent. The policy keeps its fixed eight-entry quarantine table and
1/2/4/8/16/32 observation hysteresis. One success does not erase flapping history;
two accepted attempts can clear the transient failure count.

External evidence must bind adapter, runtime, module, trusted helper IMAGE and
expected backend to one configuration identity. Use artifact/ABI identities, not
a developer path or a generated retry counter. Discovery, binary authorization,
package provisioning, driver-recovery qualification and measured workload benefit
are caller prerequisites still requiring implementation. This API neither finds
nor installs dependencies. CPU operation must remain possible without them.

## Resource and validation boundary

The controller owns at most one result gate, one current CPU batch, one bounded
session and constant policy state. The old gate and a different CPU batch can
coexist through retirement. Two prepared-equivalent buffers plus one accepted
output OR late-validation candidate/status have at most 1,315,616 logical bytes
under the current constants; a static assertion keeps this subtotal below
2 MiB. It excludes container/allocator overhead, caller preparation workspaces,
transport buffers and thread/runtime storage. Existing physical/commit headroom
is checked AFTER the prepared/gate buffers exist; it is not a reservation.
Selected gate bytes and decoded reply copies retain the 256 KiB output/task limits. It creates no extra
thread, queue or OS object; transport retains its one supervisor, one child,
128 MiB process/job limit, 20% CPU cap, creation-time assignment and headroom
checks. A current CPU batch may coexist with the previous session's retiring
buffers; these remain bounded, not a whole-process memory reservation.
Batch preparation deliberately includes an independent CPU decode in this
development fixture; it is not a GPU acceleration benchmark.

Hosted Windows tests use ONLY the trusted test executable, synthetic telemetry
and CPU semantic execution. They cover denied/no-work routes, overlap without
consuming input, successful terminal selection, unavailable-configuration
suppression, corrupt-result quarantine across A-to-B-to-A, actual CPU bytes while
the exact helper remains live, a different CPU request during retirement,
one-time reporting, late unavailable/corrupt-result classification (including a
different CPU request), two-success recovery and deadline cancellation/retirement.
CPU-only expected-HIP policy fixtures additionally cover explicit cancellation,
supervisor timeout and foreground expiry, unchanged-configuration quarantine
through repeated fresh telemetry, and admission only after separate recovery
evidence advances. Their HIP response tag is synthetic, never GPU evidence.
The existing strict zero-handle-growth assertion includes these tests.
Read exact-commit CI results before calling the implementation qualified.

No owner-PC execution, GPU initialization, mounted writes, driver installation,
new storage format, application installer or measured HIP benefit is enabled.
