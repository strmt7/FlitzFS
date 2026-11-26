# Evidence-aware GPU recovery and CPU selection

The portable policy governs GPU admission and independent CPU fallback. The
Windows shared-base controller uses it across installed-route attempts.

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
authenticate them. The installed-route collector supplies fresh binding inputs.
A same-identity device fault still requires separate compute qualification.

Discovery belongs off callbacks, driven by relevant device/installation changes,
with bounded processes and coalesced events. It must not repeat for every read.
This policy grants only permission to try; consent, workload qualification,
budgets, statuses, deadlines and independent CPU-byte comparison still apply.

WorkerExchangeSession now supplies asynchronous transport with an independently
owned supervisor. CPU gate selection can proceed while the exact helper is
still live, and cancellation/retirement remain off that selection path.
Replacement transport admission requires both exact-child and supervisor-thread
retirement. See WORKER_CHANNEL.md for the single-slot and failure contract.
The explicit GpuWorkerController connects this policy to asynchronous
transport, CPU selection and exact retirement. Each request has a terminal
gate. A later qualified request creates a fresh gate. CPU selection
stays independent of driver cleanup.
[Worker supervision](GPU_WORKER_CONTROLLER.md)

## Installed Windows routes

The owner captures read-only observations of the exact worker, compute module
and system runtime. Each ordinary file observation includes its volume and file
identity, length and write/change times. A known missing file becoming present
is also a change. DXGI can report that the factory used for the observation no
longer represents the current adapter topology. Observations only authorize
another authenticated discovery attempt.

The owner checks for changed inputs when a new group reaches an admitted idle
opportunity. A confirmed release of a previously busy worker transport can
also permit another attempt. If discovery measured insufficient memory
headroom, the owner retains the refused sample and can check again at a later
eligible group. Recovery requires a newer admissible observation for the same
adapter LUID, node and dedicated capacity. The current local and shared video
memory budgets, host memory and commit all contribute to this decision.
[DXGI process memory budget](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiadapter3-queryvideomemoryinfo) ·
[Windows budget recovery and installed-route evidence](evidence/windows-amd-route-pressure-recovery-2026-11-20.json)

CPU work continues while the owner discovers or qualifies an installed
route. Recovery retains the original policy and its quarantine history
across route replacement. Runtime, configuration and integrity failures
keep the affected configuration fenced. Unconfirmed worker retirement
prevents further attempts.
[Windows file identity](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_id_info) ·
[DXGI topology invalidation](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgifactory1-iscurrent)

After a confirmed deadline, one newer group at a fresh, funded idle
opportunity can trigger recovery. The owner can also respond to changed
installation, device readiness, topology or recovered memory evidence.
The workload-driven attempt stays spent until a normal GPU job passes
independent verification. This limits repeated probes during a failure
streak while leaving the CPU work queue available.

To reopen the same device configuration after a device fault, the owner
discovers the route and verifies a separate computation. A worker processes
one synthetic 64 KiB base and one revision. The parent compares the complete
response to an independently computed CPU result and confirms both worker
and supervisor retirement. The proof belongs to the exact fault and newly
pinned route.
Before resuming ordinary work, the owner also checks device readiness and
honors foreground activity, fresh resource observations and existing duty
pacing.

On the Windows AMD host, a shortened fixture deadline expired after 1,250 ms.
The CPU completed all five groups, and an independent reader verified all
15 files byte for byte. Authenticated discovery alone left the route fenced.
The separate verified computation re-admitted the same route after retirement.
The normal installed controller also selected an AMD reference group and
passed independent readback of its 23-file fixture. Driver-reset recovery and
the mounted foreground compression path remain to qualify.
[Windows AMD recovery qualification](evidence/amd-recovery-qualification-2026-11-21.json)

The live Windows SDK fixture published two CPU groups during one missing-package
attempt. Restoring the approved inputs inside that fixture permitted one more
attempt and one HIP identity query. The same volume then verified two groups
in one AMD exchange. It published one additional group and declined the other.
All twelve files passed CPU readback. The worker and controller retired.
[Source-bound recovery evidence](evidence/amd-installed-recovery-2026-11-18.json)

### Installed native read bindings

The Windows controller retains one recovery policy and one evidence sequence
for its installed read route. A configuration, integrity or device fault
fences the exact configuration. The owner releases its package pins after
confirmed retirement
and retains the input witness. Independent CPU read service continues while
the owner waits for another qualified read opportunity.

A fresh opportunity can trigger discovery after relevant package, runtime,
device readiness or topology evidence changes. An elapsed interval only
limits the frequency of those observations. A newly authenticated binding
must pass the existing quarantine before it can own work. Alternating
configurations preserves previous failures, and an unconfirmed worker
blocks replacement. A same-configuration device fault still needs separate
verified recovery computation.

Before starting or submitting a prepared read, the installed route also checks
the bound Windows device instances. A device that becomes unavailable before
launch can return through discovery after a known ready observation, with
fresh resource checks on the next request. This preflight updates no compute
failure history. All requests retain the independent CPU read path and exact
reply verification. The recovery checks run on the controller, off the
Windows read callback.
[Device status](https://learn.microsoft.com/en-us/windows/win32/api/cfgmgr32/nf-cfgmgr32-cm_get_devnode_status) ·
[DXGI adapter enumeration](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgifactory1-iscurrent)

### Fresh observations before worker startup

A live prepared compression worker previously reported a stale memory
sample before HIP initialization. Prepared compression and native LZ4 read
workers now use the same bounded renewal rule as completed compression. If age alone prevented admission, the worker takes one newer
measurement from its selected adapter sampler. The timestamp includes the
whole query and continues to use the existing 50 ms age limit. The second
measurement must begin after expiration was observed and retain the adapter
LUID, node and dedicated capacity. Both measurements pass the same resource
checks. The uniform compression engine retains the accepted sample as its
pre-initialization baseline.

An invalid budget, changed adapter, constrained headroom or reversed clock
keeps the original failure classification. A replacement that is itself
stale still yields temporary pressure. The single renewal takes place
inside the existing supervised worker lifetime. The owner can select CPU
if the worker declines admission, with the existing retirement rules.
[Windows query contract](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiadapter3-queryvideomemoryinfo) ·
[Windows elapsed time](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-gettickcount64) ·
[Source and validation evidence](evidence/windows-hip-startup-budget-renewal-2026-11-25.json)

The rebuilt reader delivered two checked extents in the live Radeon WinFsp
fixture. The compression worker passed the offline SDK build and CPU
qualification. One prepared compression burst failed to verify both
replies. The probe now records the worker exit, timeout, readiness and
completion details for a follow-up. Existing host engine load deferred
that follow-up. [Device and SDK results](evidence/windows-hip-startup-budget-renewal-2026-11-25.json)

## Windows device readiness during driver changes

Configuration Manager supplies a read-only view of the present AMD PCI display
devices before route discovery opens DXGI. The selected PCI vendor, device,
subsystem and revision must match the Windows instance identity. An instance
must be started and clear of removal and problem flags. If identical PCI
signatures prevent distinguishing devices at this stage, all matching
instances must meet that condition. The existing DXGI LUID, topology, worker
identity and memory checks still qualify the actual route.
[Device instance identity](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/device-instance-ids) ·
[PCI identifiers](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/identifiers-for-pci-devices) ·
[Configuration Manager status](https://learn.microsoft.com/en-us/windows/win32/api/cfgmgr32/nf-cfgmgr32-cm_get_devnode_status)

The owner rechecks the selected device instances before shared-base dispatch
and before the separate qualified write route collects DXGI telemetry or
starts its worker. The filesystem lock stays free during these reads. A
blocked anchor route drops its worker binding. The owner remembers the
interruption, and a ready observation at a new admitted group opportunity can
trigger fresh discovery. While the device stays unavailable, changes to the
files alone cannot trigger another attempt. A route with an unconfirmed
worker retirement still refuses reacquisition.

The RX 9070 XT reported Code 21 after an external Radeon update. The installed
package fixture completed two CPU groups and verified all ten files. The
controller made one discovery attempt, zero HIP identity queries and zero
GPU submissions, then retired. The current runtime digest differs from the
approved package profile. Healthy-device execution and recovery with the new
runtime still need direct qualification.
[Device removal status](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/cm-prob-will-be-removed) ·
[Source-bound live and sanitizer evidence](evidence/windows-amd-device-readiness-2026-11-20.json)

After restart, Windows reported the Radeon ready with driver 32.0.32029.2010.
The signed driver runtime returned the same priority range in two independent
queries. An explicit AMD batch verified all 20 public revision inputs and
reduced the protected compacted code volume from 286,720 to 278,528 bytes.
The staged package then acquired the freshly bound runtime through the owned
controller, verified one batch, and recovered its files through independent
CPU readback. Its worker retired inside the admitted process budget.
[AMD driver release notes](https://www.amd.com/en/resources/support-articles/release-notes/RN-RAD-WIN-26-11-2.html) ·
[Native Windows runtime evidence](evidence/windows-native-cpu-sampling-amd-resume-2026-11-20.json)

## Failure-classification research

AMD distinguishes missing devices, insufficient drivers, resource exhaustion
and destroyed contexts. These are different observations, not a generic timeout.
Clearing the last error does not independently qualify a repaired device.
Sources: [HIP error codes](https://rocm.docs.amd.com/projects/HIP/en/latest/reference/error_codes.html)
and [HIP error handling](https://rocmdocs.amd.com/projects/HIP/en/latest/how-to/hip_runtime_api/error_handling.html).
The runtime-error mapping still needs source review and implementation.

## Initial checks and historical host restriction

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
host boundary. It was not RAM-only. The host restriction at that checkpoint moved remaining builds, research and
tests to remote runners. Current Windows development and test-volume admission
follow the owner's later direction in AGENTS.md.

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

## Packed XOR byte verification

The Windows owner keeps a packed XOR recovery ticket after the worker retires
and the reply passes its transport checks. The serialized writer reports
a verdict for each nonempty slot as it compares the canonical reconstructed
bytes with the exact staged extent. If a result differs, the current member
uses CPU encoding. The owner quarantines that configuration and drops all
remaining advice before another GPU launch. A correct body that loses the
size comparison counts as verified. A batch with only empty slots, or one
abandoned before all checks, can continue on CPU without recording GPU success.
[Validation evidence](evidence/amd-packed-xor-verdicts-2026-11-19.json)

## Measured AMD batch sizing

The Windows anchor scheduler starts a newly bound route with the shortest
prefix of complete search groups that meets the package minimum. This keeps
whole-group reference selection available during calibration. A verified
job contributes its measured worker elapsed time and exact kernel-launch
count. The estimator reserves one third of the hard deadline and admits
complete groups within the resulting launch allowance. A slower verified
sample reduces the allowance immediately. Faster samples lower the estimated
cost gradually. The existing memory, device, retirement, and duty checks
still govern each request.

The supervisor measures the child from resume through the process wait.
The model uses successful completion followed by independent byte
verification. Foreground observation of a retired reply can retain the
batch while verification waits for an admitted idle opportunity. The
unsubmitted groups can continue on CPU. A timed-out worker supplies no
reply or timing sample and continues through the existing CPU fallback
and device recovery policy.

The measurement is local to an active route and starts fresh on rebind.
Windows applies limits from the parent jobs in a process's job hierarchy.
Treat guarded CPU-contention trials as correctness evidence for the tested
envelope. [Microsoft Job Objects](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects) ·
[Windows AMD batch evidence](evidence/amd-measured-batch-sizing-2026-11-21.json)
