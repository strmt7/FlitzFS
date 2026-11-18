# Validation ledger

## AMD shared-base phase measurements — 18 November 2026

The direct sweep now reports accumulated wall times with its verified records.
The parent checked the exact request digest, adapter identity and window plan,
and compared every record with the CPU. The worker returned a trace after
finishing all windows and releasing the device resources and modules.

| Member extents | Exchange | Stream and slab setup | Host to device | Launch and stream wait | Device to host | Module release |
|---:|---:|---:|---:|---:|---:|---:|
| 7 | 1,213 ms | 533 ms | 12.4 ms | 1.0 ms | 0.9 ms | 405 ms |
| 7 | 476 ms | 261 ms | 12.1 ms | 0.9 ms | 0.8 ms | 3.0 ms |
| 97 (public source) | 866 ms | 225 ms | 17.6 ms | 5.2 ms | 417 ms | 3.4 ms |
| 193 | 997 ms | 330 ms | 438 ms | 8.2 ms | 8.2 ms | 3.9 ms |

These are host-visible phase durations. The launch and wait measurement
includes submission and synchronization. Independent CPU byte checks and
serialization are counted in reply encoding. The two seven-member repetitions
show substantial setup and retirement variance. The runtime-refusal control
used CPU and returned zero GPU timings. The ordinary sweep and all seven
selected raw and AddressSanitizer entries passed. All process trees exited.
[Source and phase measurements](evidence/amd-anchor-phases-2026-11-18.json)

An experimental pinned-copy path requested 304 KiB of host staging for one
window. The seven-member trial hit the nonlocal-memory gate, returned zero
GPU records or timings, and completed through CPU fallback. The synchronous
control verified the same workload under similar available host memory.
The measured process-tree peaks were 129 MiB for the rejected trial and
84.5 MiB for the control. Both trees exited. The experimental patch is
retained in the private validation fixture.
[Pinned transfer trial](evidence/amd-anchor-pinned-trial-2026-11-18.json)

## Resource-sized AMD shared-base sweep — 18 November 2026

One authenticated worker kept the same 64 KiB base and its index for the
entire request. It completed a fresh adapter-budget check after each window
and returned a single reply after device resources and modules had retired.
The CPU generated and verified the expected records independently.

| Member extents | Corpus | Search windows | GPU exchange | CPU search and verification |
|---:|---|---:|---:|---:|
| 7 | Seeded | 3 | 892 ms | 6.0 ms |
| 25 | Seeded | 9 | 807 ms | 19.2 ms |
| 97 | Public source | 33 | 891 ms | 67.8 ms |
| 193 | Seeded | 65 | 1,010 ms | 143.8 ms |

All replies matched the CPU byte for byte. Each GPU trial used one 816 KiB
device slab. The largest request carried 12.125 MiB of source bytes. The
fixed three-member control also reopened and verified the 360 KiB compacted
image through the CPU reader. The larger cases verified search records.

A seven-member request funded for two windows stayed on CPU without starting
a worker. A rejected runtime identity returned no GPU reply and also completed
CPU matching. The selected raw and AddressSanitizer suites passed. Every
owned trial retired. The GPU exchange still took longer than the equivalent
CPU work at each measured size. A separate intra-extent phase probe hit the
nonlocal-memory gate and returned no phase measurements.
[Source and execution evidence](evidence/amd-anchor-sweep-2026-11-18.json)

## Direct AMD shared-base search — 18 November 2026

The installed AMD runtime built one device index from a 64 KiB base and searched
three member extents. The CPU verified every suggestion against the original
bytes. All three successful direct trials returned records identical to the CPU
implementation. The indexed writer compared whole compacted images and
independently reopened and read back every member.

| Three-member fixture | Existing CPU | Tiled AMD | Direct AMD and direct CPU |
|---|---:|---:|---:|
| Seeded fragment permutation | 656 KiB | 520 KiB | 360 KiB |
| Public-source fragment permutation | 352 KiB | 352 KiB | 352 KiB |

The paired trials sent 256 KiB of direct input versus 768 KiB of tiled input.
The direct results occupied 48 KiB of payload. Tiled result payloads occupied
421–681 KiB. Direct exchanges took 0.50–0.83 seconds, and tiled exchanges took
1.52–1.96 seconds. Both exchange measurements include worker setup. The
equivalent direct CPU search took 2.7–3.0 ms including byte verification. The
public-source candidate was declined by the whole-image comparison.

The unavailable adapter and runtime identity refusal each produced a verified
360 KiB CPU image. Six raw CTest entries and seven AddressSanitizer entries
passed. All owned process trees exited. The direct AMD mode remains an explicit
calibration path.
[Source and live execution evidence](evidence/direct-amd-anchor-2026-11-18.json)

## AMD-assisted shared-base search — 18 November 2026

The existing supervised uniform HIP worker searched tiles from one base and
four related extents. Its verified LZ4 output supplied base-copy positions to
indexed preparation. Every selected copy and the resulting stored frame passed
independent CPU checks. The CPU tiled control used the same input tiles.
Each fixture permuted short fragments from a synthetic or public-source base.

| Foreground storage configuration | Fixture | CPU native | CPU tiled | AMD tiled |
|---|---|---:|---:|---:|
| LZ4 available | Seeded | 784 KiB | 776 KiB | 752 KiB |
| LZ4 available | Public source | 784 KiB | 776 KiB | 768 KiB |
| Zstandard level 6 available | Seeded | 784 KiB | 752 KiB | 600 KiB |
| Zstandard level 6 available | Public source | 416 KiB | 416 KiB | 416 KiB |

Sizes cover the complete compacted native image. The two Zstandard AMD trials
took 1.79 and 2.20 seconds including transport and storage verification. The
matched CPU tiled trials took 31 and 36 ms. The AMD result passed exact CPU
readback in all four live runs. The forced adapter-refusal case reopened
the CPU image and started zero workers. Raw and AddressSanitizer suites each
passed 1,599 modeled store interruptions and the invalid-copy fallback checks.
Ordinary routing continues to use the verified CPU path for this operation.
[Source and live execution evidence](evidence/amd-anchor-collective-2026-11-18.json)

## Resumable indexed discovery — 18 November 2026

The second owner of a 72-extent Windows fixture sampled 10 extents after
reopening and extended the same protected base. A completed unchanged scan
restored its checkpoint with zero new samples or writes. An older checkpoint
also resumed after a foreground edit, revisited the changed prefix and
verified every file. Each checkpoint reserved 128 KiB in the optional area
and consumed 4 MiB of durably counted accepted input.

The raw and AddressSanitizer lanes each passed 24 modeled advisory
interruptions across 4 KiB and 64 KiB failure units. CPU readback preserved
the files after each failure. The live Windows SDK controller published a
group and its checkpoint in 23 opportunities. The largest measured quantum
was 0.792 ms, and 147 handles remained at shutdown, matching the starting
count. [Source and execution evidence](evidence/persistent-indexed-discovery-2026-11-18.json)

## Shifted cross-file compression — 18 November 2026

The indexed optimizer now finds common data after insertions and stores changed
members as checked copy ranges and literals relative to a protected base. Twelve
variants of a public 64 KiB source sample with controlled prefix edits occupied
592 KiB before grouping and 192 KiB afterward. Both figures include the complete
compacted image with the same Zstandard level 6 settings. An independent CPU
reader verified every version before and after grouping.

The raw and AddressSanitizer lanes each recovered from 528 modeled interruptions
of the format promotion. They verified the surrounding files and subsequent
writes. The Windows SDK controller published a shifted group under live resource
observations. It used 12 opportunities, with a largest measured quantum of
0.753 ms, and returned to its starting count of 147 handles after shutdown.
[Source, fixture and execution evidence](evidence/shifted-indexed-groups-2026-11-18.json)

## AMD topology lifetime during worker setup — 18 November 2026

The uniform LZ4 worker now keeps one D3D12 device across the two HIP identity
checks when the runtime omits the node mask. It samples topology, memory and
device health at each use. It releases the device and library before stream
creation and slab allocation. The Windows metadata control also returned
the expected LUID and one node with no HIP initialization.

Three alternating AMD pairs used the same kernel module, profiler and 1 MiB
input. All six exchanges preserved prior data and passed independent CPU
readback. The earlier worker took 1.44–1.45 s per exchange. The new worker
took 0.92–1.43 s. Setup timings varied and two control runs spent more than
400 ms in kernel wait, so these timings cover the complete exchange.
The SDK CPU matrix passed 3,489 checks across 83 scenarios. Nine selected raw
and AddressSanitizer CTest entries passed.

The installed WinFsp callback fixture verified three CPU and AMD pairs using
1 MiB from 128 tracked source files. CPU writes took 3.97–4.21 ms and occupied
3,145,728 image bytes. AMD writes took 1.05–1.47 s and occupied 3,276,800
image bytes. The policy selected CPU for this workload. Three callback and
acquisition controls also passed after the targeted SDK build declared its
worker executable dependency.
[Source and hardware evidence](evidence/amd-topology-reuse-2026-11-18.json)

## AMD worker budget sampling — 18 November 2026

The uniform AMD worker binds the exact DXGI adapter once for budget sampling
in each attempt. It reads fresh GPU and host counters at every resource
checkpoint and refuses the attempt if the factory reports a topology change.
In five alternating pairs,
ten budget reads took 1.42–1.82 ms when rebinding and 0.16–0.19 ms when
retaining the binding, with all snapshots fresh.
[DXGI topology guidance](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgifactory1-iscurrent)

The locally built ROCm 10.1.0 worker completed three matched AMD trials. The
earlier worker completed the three controls. Each stored 135,344 encoded bytes
from 1 MiB, preserved the prior data and passed independent CPU readback.
Across eight launches, admission and completion checks took 46–56 µs with the
new worker and 2.78–3.68 ms with the earlier worker. End-to-end times overlapped.
Setup for the new worker took 1.06–1.20 s.

HIP reported the expected LUID with a zero node mask. The independent D3D12
probe verified one physical node in 248–293 ms including process overhead.
The current setup can request that proof twice. The metadata probes bound the
cost of a standalone verification and guide further measurement inside the
worker. All processes and locked fixture memory retired.
[D3D12 node reference](https://learn.microsoft.com/en-us/windows/win32/direct3d12/multi-engine)

The local build qualified six offline AMD targets. The SDK CPU matrix passed
3,485 checks across 83 scenarios. The raw, AddressSanitizer and SDK callback
lanes passed nine selected CTest entries. Seventeen source-staging tests passed.
[Source, artifacts and execution evidence](evidence/amd-budget-sampler-2026-11-18.json)

## Automatic indexed group discovery — 18 November 2026

An indexed fixture grouped 129 related extents around one protected base in
five publications. It progressed with 12 writes interleaved at an earlier
cursor coordinate. The largest measured scan or candidate step read 189,477
backing bytes. The sum of conservative compacted-size reductions was
16,728,064 bytes. A stale sampled generation failed before payload decoding.
Unrelated tiny files did not match based on zero padding. Rename and empty
file creation left the settled scanner idle.

Seventeen selected Windows test groups passed across the SDK, raw and
AddressSanitizer builds. The real WinFsp SDK callback test refused a busy
observation without backing I/O, then published and independently reopened
the group. A live Windows run with injected dispatcher lifecycle published
the group in 13 opportunities and retired with 147 handles before and
after. Earlier live runs hit the test deadline with the files intact.
One recorded CPU deferral after a candidate was staged.

In the installed AMD overwrite fixture, a stale completion budget rejected
one worker result after two successful pairs. CPU fallback completed the
write and passed independent verification. A later run completed all three
matched pairs, with identical occupied bytes and faster CPU response. The
AMD work compressed incoming writes to an already grouped volume.
[Source and execution evidence](evidence/automatic-indexed-discovery-2026-11-18.json)

## Unattended Windows AddressSanitizer launch — 18 November 2026

Five test executables reached the Windows loader without the required
AddressSanitizer DLL. The owner observed system dialogs. CTest 4.5's
interactive mode reset the process error mode inherited from the supervisor.
The two overnight run logs match the five affected executable names.

The build now stages the DLL from the selected MSVC toolset beside its
instrumented executables. The supervisor and generated Windows test target
set CTest's non-interactive mode. Direct supervised launches also inherit
the process error mode before Windows loads executable dependencies.
[Microsoft AddressSanitizer reference](https://learn.microsoft.com/en-us/cpp/sanitizers/asan-building?view=msvc-170) ·
[CTest non-interactive mode](https://cmake.org/cmake/help/latest/manual/ctest.1.html#cmdoption-ctest-interactive-debug-mode)

All five affected suites passed with only the Windows directories on the
test process's `PATH`. With the DLL omitted in a disposable copy, both the
direct and nested CTest launches reported `0xC0000135` and retired in under
one second. The raw-build control and five launcher argument tests passed.
[Source and execution evidence](evidence/windows-unattended-asan-2026-11-18.json)

The follow-up build verifies the staged DLL against the configured compiler's
runtime. CTest checks it once per run before starting any instrumented tests,
including filtered runs. Plain CTest refused to launch a selected test when the
DLL was missing or mismatched. It reported both failures in under a second.
[CTest fixture requirements](https://cmake.org/cmake/help/latest/prop_test/FIXTURES_REQUIRED.html)

After rebuilding, all five affected suites passed in the instrumented build
with only Windows directories on `PATH`. One affected executable also passed
when launched directly. Without its DLL in a separate fixture, the supervisor
logged `0xC0000135` and confirmed process exit. The uninstrumented Release
control ran two tests without staging a DLL or running the runtime check.
[Follow-up source and execution evidence](evidence/windows-asan-preflight-2026-11-18.json)

The Windows supervisor now keeps CTest non-interactive through nested CMake
`env`, `time` and `chdir` commands. Ordinary Windows validation includes
an inert fixture that tests the staging and verification script. The fixture
checks missing and mismatched copies, exact replacement, and verification
after the original toolset file is absent. It also checks that a changed
toolset file cannot replace a verified copy.

The final MSVC AddressSanitizer run passed all five affected suites, the
preflight and both launch and staging regressions. The uninstrumented
Release control passed three selected entries. The complete test trees
exited. A direct supervised launch lacking the DLL logged `0xC0000135`
and exited in 0.31 seconds. The ClangCL comparison exposed a separate
exception-unwinding crash after loading its runtime.
[Regression and live-host execution evidence](evidence/windows-asan-launch-regressions-2026-11-18.json)

The AMD SDK build now stages the same compiler-owned runtime for its own
executables. The native CTest preflight restored the missing DLL and ran the
inode suite in a disposable copy. It skipped the test when the compiler
copy was absent or the staged copy had a different checksum. Both refusal
cases finished in under one second. The five affected suites passed again
under MSVC AddressSanitizer and in the standard Release configuration. The
CPU-only ROCm header probe passed in both configurations with a Windows-only
`PATH`. All owned process trees exited.
[Restoration and SDK execution evidence](evidence/windows-asan-restoration-2026-11-18.json)

The filtered CTest regression verifies that a missing or mismatched runtime
blocks the dependent test and that a valid compiler copy restores execution.
The five affected suites passed again on Windows with CMake 4.5.0. The
AddressSanitizer run passed eight entries in 79.58 seconds and the Release
control passed seven. Both used only Windows system directories on `PATH`
and both process trees exited.

The supervisor now applies non-interactive mode when `ctest --build-and-test`
launches another CTest through `--test-command`. The test child observed
Windows error mode `0x0003` after the fix. In a disposable fixture with the
staging check omitted, a missing DLL produced loader status `0xC0000135` in
the log and the tree exited in 1.00 second. With ordinary generated CTest
registration, a missing or mismatched runtime blocked all five binaries.
After restoring the exact compiler DLL, all eight selected entries passed
in the instrumented build. The uninstrumented Release control passed
seven entries.
[Forwarded launch and runtime evidence](evidence/windows-ctest-forwarded-launch-2026-11-18.json)

## Cooperative indexed group preparation — 18 November 2026

The 64-member indexed fixture took 65 preparation steps. The largest step
read 102,295 backing bytes. Foreground renames and unrelated writes continued
between steps, including a write to a different extent of a selected file.
The resulting volume and its pinned predecessor passed byte-for-byte checks.
Changed selections failed before further backing I/O in 27 cases.

Fourteen selected Windows test executions passed across codec/SDK, raw and
AddressSanitizer builds. Each lane ran 528 indexed publication interruption
cases at 4 KiB and 64 KiB modeled isolation. Reopening found the complete
old or new group and preserved the acknowledged unrelated write.

Three paired 1 MiB overwrites used the installed AMD runtime and user-mode
WinFsp callbacks. Both paths produced identical image sizes and recovered
independently on CPU. CPU completed faster in this fixture. Group preparation
used CPU throughout these runs.
[Source, FlitzZip review and run evidence](evidence/cooperative-indexed-shared-base-2026-11-18.json)

## Packed compaction and indexed size accounting — 18 November 2026

In the matched 1,926-object mirrored fixtures, a three-record size proposal
read 40 KiB of metadata instead of 328 KiB. Complete compacted size fell from
1,318,912 to 1,114,112 bytes at 4 KiB isolation, and from 9,961,472 to 1,310,720
bytes at 64 KiB isolation. Grouping eight members on a 1,032-file indexed
volume reduced its compacted image from 2,506,752 to 1,589,248 bytes. Both
images passed independent readback of every file.

Twenty-one selected Windows test executions passed across codec, raw and
AddressSanitizer builds. The staged rebuild suite exercised 1,413 modeled
write and barrier interruptions and checked rejection of changed candidate
headers and a stale source during indexing. The actual Windows resource
controller advanced compaction in 34 opportunities with injected WinFsp
lifecycle, independent CPU readback and exact handle balance after join.
Three live AMD/CPU pairs reverified grouped overwrites. CPU remained faster
for the measured foreground request.
[Size, recovery and execution evidence](evidence/packed-rebuild-accounting-2026-11-18.json)

## Owner-controlled writable lifetime — 18 November 2026

An indexed writable controller with injected WinFsp lifecycle stayed active
through 31 seconds of idle time. It performed zero maintenance quanta and
zero GPU budget queries, then stopped on the owner's event. The native handle
baseline stayed balanced after join. The deferred-write control produced one
response and signaled completion for both an accepted CPU write and an
injected write failure. The failed case recovered the prior checkpoint.

Thirteen selected Windows runs passed across SDK, raw and AddressSanitizer
builds. Both optional driver-backed probes compiled. Three paired AMD/CPU
callback overwrites preserved the grouped image and verified 32 GPU-produced
mirrored records per AMD request on CPU. CPU remained faster in this fixture.
The authenticated GitHub snapshot returned zero open Dependabot alerts.
[Source, test and cleanup evidence](evidence/owned-writable-lifetime-2026-11-18.json)

## Indexed shared-base compression — 18 November 2026

Eight related 64 KiB files occupied 1,204,224 bytes before grouping and
286,720 bytes afterward in separately compacted 4 KiB model images. The
64 KiB model went from 1,441,792 to 524,288 bytes. The indexed fault campaign
covered 591 publication and last-reference retirement interruptions. Each
recovered the complete prior or new checkpoint. Sixteen selected Windows
executions passed across SDK, raw and AddressSanitizer builds.

Six paired SDK trials completed CPU and AMD overwrites of a shared member.
Each reopened the current and prior image through the CPU and verified the
three other files. Both paths occupied 5,111,808 bytes and CPU was faster.
An earlier trial took the CPU path after the submitted worker returned no
admitted candidate. Its independent readback passed and the worker retired.
[Exact sources, sizes, failure scope and trial evidence](evidence/indexed-shared-base-2026-11-18.json)

## Finite AMD exchanges — 18 November 2026

The native indexed fixture admitted 1 MiB per worker with a 128 KiB
per-launch input limit. Six matched CPU/AMD pairs covered repeated
synthetic extents and public source text. All AMD work retired with
independent CPU readback. The corpus occupied 128 KiB less on CPU,
and both inputs selected CPU under the measured foreground policy.
Eighteen selected Windows executions passed across codec, raw and
AddressSanitizer builds.
[Limits, measurements and test receipts](evidence/finite-amd-exchange-2026-11-18.json)

## Installed AMD acquisition and matched writes — 18 November 2026

The relocated executable acquired its packaged worker and compute module.
Missing and altered-package controls returned with zero device queries.
The approved route completed three alternating 512 KiB AMD/CPU pairs with
identical occupied image bytes and independent CPU readback. AMD had no
foreground benefit in those trials. The Windows SDK fixture exercised
injected lifecycle and response functions.
[Exact source, outcomes and process retirement](evidence/installed-amd-routing-2026-11-18.json)

## Controller-owned GPU budget sampling — 18 November 2026

The configured native Windows controller handled two GPU budget queries for
one indexed AMD write. The first write used CPU with stale initial telemetry.
The 512 KiB write used verified GPU advice for eight extents, and all sixteen
mirrored records passed independent CPU readback. Modeled callbacks exercise
coalescing and CPU continuation after unavailable completion telemetry.
The hardware fixture used injected filesystem lifecycle and response functions.
[Source and execution evidence](evidence/native-gpu-budget-controller-2026-11-18.json)

## Automatic indexed compaction and page allocation — 18 November 2026

The staged switch read 150,048 image bytes with a 1 MiB live file and wrote
only the replicated selector during publication. All 12 modeled selector
interruptions recovered either the verified original or the verified candidate,
with active handles intact. The locked Windows provider reduced the small
overwrite fixture from 3.75 MiB to 360 KiB. A separate snapshot pin and
retirement fixture returned resident space in bounded steps.
The live controller completed automatic selection in 34 maintenance opportunities
and reopened the selected file independently after join. The maximum observed
controller quantum was 1.4 ms in that fixture. A native-only telemetry control
reproduced the process-wide handle initialization that initially affected the
test. The final controller run retained exact post-join handle balance.
[Exact source and test results](evidence/indexed-background-compaction-2026-11-18.json)

## Owned indexed Windows volume — 18 November 2026

Local Windows checks exercise indexed startup rollback, live background
retirement and sealed CPU recovery after dispatcher join. On disposable
Windows Server 2025 with WinFsp v2.2B5, the linked provider also passed actual
external writes, flush, rename/delete, readback, unmount and CPU reopen at
source `4523ce5`. Windows 11 client qualification remains separate.
[Source and execution evidence](evidence/indexed-owned-windows-2026-11-18.json)

## Indexed deferred write and AMD output — 18 November 2026

Twelve selected Windows executions cover raw fallback, independent codec
validation, SDK callback ownership and AddressSanitizer with LZ4 enabled.
The actual AMD fixture published two indexed extents and verified the GPU
output in all four mirrored records. A separate CPU path reopened the image.
The response sink was injected, and the fixture used private memory backing.
[Exact source, binaries and measurements](evidence/indexed-lz4-deferred-2026-11-18.json)

## Indexed managed storage and on-demand memory locking — 18 November 2026

The [indexed managed checkpoint](evidence/indexed-managed-winfsp-2026-11-18.json)
covers verified image selection, interrupted compaction, independent CPU reopen
and Windows SDK callbacks. The [memory backing checkpoint](evidence/demand-locked-memory-2026-11-18.json)
covers adaptive admission, locked page growth, exact retirement, helper
behavior and injected WinFsp lifecycle. The receipts record test configurations,
process cleanup and the limits of each result.

## Shared adaptive memory-volume factory — 17 November 2026

[Contract](WINDOWS_MEMORY_VOLUME_FACTORY.md) and
[source-bound evidence](evidence/memory-volume-factory-2026-11-17.json) record
fresh adaptive admission, shared provider/accounting, retained backing, startup
refusal, explicit post-join CPU inspection and unchanged helper workflows.
Direct native RNG initialization is separated from strict FlitzFS handle balance;
the failed cold-baseline test remains available. Actual live outcomes are recorded
without turning resource deferral into fabricated idle/grouping evidence.

## Owned WinFsp background controller — 16 November 2026

[Contract](WINFSP_OWNED_CONTROLLER.md) and
[source-bound receipts](evidence/owned-winfsp-controller-2026-11-16.json) separate
actual SDK callbacks, live local telemetry and injected lifecycle ownership.
Normal/ASan checks cover activity/stop events, CPU readback, quiet unchanged
volumes, failure cleanup and joined retirement, plus callback and helper/GUI
regressions. This adds no actual mount, driver-install or GPU execution claim.

## WinFsp paged-volume callbacks — 15 November 2026

[Contract](WINFSP_PAGED_VOLUMES.md) and
[source-bound evidence](evidence/paged-winfsp-2026-11-15.json) record real
user-mode SDK callback/group behavior, flat/paged resume, modeled interruption
recovery and injected lifecycle retirement. SDK runtime execution is now
distinguished from syntax-only evidence; a driver or real mount is not invoked.

## Windows helper background integration — 15 November 2026

[Contract](WINDOWS_BACKGROUND_MAINTENANCE.md) and
[exact-source evidence](evidence/windows-background-maintenance-2026-11-15.json)
cover live local CPU/memory observations, automatic grouping in the actual
Windows provider, foreground access during preparation, GUI draft preservation
and owned retirement. Core model, live helper, SDK syntax, mounted and GPU
evidence remain separate. No new mounted/driver/GPU result is implied.

## Automatic shared-base discovery — 15 November 2026

[Contract](AUTOMATIC_GROUP_MAINTENANCE.md) and
[source-bound receipts](evidence/automatic-group-maintenance-2026-11-15.json)
cover automatic candidate discovery/extension, hash/outcome reuse, supplied
observation admission, selected-edit reconciliation and modeled publication
failures. Local CPU/AddressSanitizer and SDK syntax scope is separate from live
Windows background load, GPU, mounts, physical faults and compression records.

## Managed paged-volume integration — 15 November 2026

[Contract](MANAGED_PAGED_VOLUMES.md) and
[source-bound receipts](evidence/managed-paged-volume-2026-11-15.json) record
ordinary session operations, grouped data, advisory settlement, typed handoff,
surviving-copy repair and modeled interruption recovery. This is selected local
CPU/SDK scope, not mounted, physical, GPU or release qualification.

## Cooperative shared-base work — 15 November 2026

[Contract](SHARED_BASE_GROUPS.md#cooperative-preparation-and-foreground-reconciliation)
and [source-bound receipts](evidence/staged-shared-base-2026-11-15.json) separate
one-extent preparation, unrelated-write reconciliation and modeled publication
recovery from automatic scheduling, GPU execution and physical durability.

## Protected shared-base checkpoint — 15 November 2026

[Contract](SHARED_BASE_GROUPS.md) and
[source-bound evidence](evidence/shared-base-groups-2026-11-15.json) separate
selected Windows raw/AddressSanitizer checks and complete synthetic image-size
measurements from unexecuted GPU, mount, physical power-loss and benchmark claims.

## Local paged storage checkpoint — 15 November 2026

[Paged storage](PAGED_OBJECT_STORE.md) and its
[source-bound evidence](evidence/local-paged-store-2026-11-15.json) distinguish
selected Windows raw/AddressSanitizer suites and a million populated object
records from full native filesystem, Windows mount, GPU and power-loss coverage.
Historical results below retain their original source and scope.

## Controlled write reused-dispatch candidate

[NATIVE_UNIFORM_WRITE.md](NATIVE_UNIFORM_WRITE.md) describes optional canonical
sweep dispatch through the checked native write coordinator. Ordinary version-2
replies and stored codecs are unchanged; full-window admission and exact
retirement remain required. Source 3b7a6c9 passed all 13 core jobs and full SDK qualification, including
199 controlled native-write checks. [Evidence](evidence/native-reused-dispatch-3b7a6c9.json).
These are CPU-semantic coordinator checks; no new owner/GPU execution or ordinary
automatic GPU route is claimed.
CPU fixture results must not be reported as GPU execution.

## Qualified experimental finite-sweep observations

The opt-in [finite-sweep profile](UNIFORM_LZ4_SWEEP_PROFILE.md)
separates reusable setup, accumulated work phases and the slowest kernel/quantum.
Source 11fbf07 passed all 13 core jobs, complete SDK qualification and one
bounded instrumented synthetic hardware operation. See
[evidence](evidence/uniform-sweep-profile-11fbf07.json). These are scoped experimental results,
not automatic cost qualification or ordinary GPU filesystem routing. This does not enable ordinary GPU I/O,
automatically qualify costs, prove comparative benefit or change stored codecs.
CPU fallback may use a different encoder and stored size while retaining
independent byte-exact reads of every admitted GPU representation.

## Native fast-ingestion storage candidate

The new [fast-ingestion storage path](NATIVE_FAST_INGESTION.md) requires exact-source enabled/raw/Windows/sanitizer qualification, independent codec-4 decoding and immutable-baseline failure reproduction, resource/candidate rejection and full modeled transactional I/O fault coverage. Its added tests execute CPU code only. The storage gates passed at 0d96d903; the native inode hook passed all 13 core jobs at 3a797a3.

The [fast-ingestion storage foundation](NATIVE_FAST_INGESTION.md) passed all 13 core jobs at 0d96d903, including independent CPU recovery and transactional faults. The direct native inode candidate hook passed all 13 core jobs at 3a797a3; ordinary Windows GPU routing and useful end-to-end benefit remain unfinished.

## Executed isolated compression worker qualification — 14 November 2026

At979cd512 the [runtime fixture](HIP_LZ4_RUNTIME.md) passed complete SDK qualification: six target images, 12 staging cases, 55 admission checks/10 confirmed exits, 828 compute checks across 58 scenarios, 8 raw-control checks and all 20 original worker cases. A separately supervised actual AMD trial CPU-verified 4,096 input bytes compressed to 118 payload bytes, with 16,544 explicit device bytes, 575,130,900 ns complete cold exchange and confirmed cleanup. [Source/artifact/host evidence](evidence/hip-lz4-runtime-979cd512.json). These measurements establish neither a filesystem ratio nor a speed benefit. All13 ordinary jobs passed at 516e4e9; latest979cd512 integration results are recorded separately.


## Pending typed compression compute qualification

The SDK-only [HIP LZ4 compute candidate](HIP_LZ4_COMPUTE.md) adds new typed calls and CPU fault probes. Its source-bound compilation, positive/raw refusal matrix and full existing CI gates must execute before recording success. No owner GPU, mounted test, throughput result or storage durability result follows from these probes. Prior decoder qualification remains tied to 94ec882.


Date: **7 November 2026**. This page distinguishes implemented functionality,
executed tests and unpassed integration gates. It supersedes earlier statements
that Windows CPU/native callback validation was still pending. Historical
results remain in Git history and the earlier CI runs below.

## Initial callback-only code qualification (historical)

Code commit: **`dcb3a8b313af6e7b77453acf9474e506ef5bc83f`**.
GitHub Actions: [run 42377524934](https://github.com/strmt7/FlitzFS/actions/runs/42377524934)
completed **success**. The preceding physical-path safety commit
`62907088f767b2d67481bfb36ffff9bc3ef9bbfe` also passed all five jobs in
[run 42376916260](https://github.com/strmt7/FlitzFS/actions/runs/42376916260).

| Lane | Actual coverage | Result |
|---|---|---|
| Ubuntu 24.04 + CPU Zstandard | Five portable CTest executables | PASS |
| Ubuntu 24.04 raw-only | Five portable executables without codec dependency | PASS |
| Windows Server 2022 / MSVC raw-only | Five portable executables on Windows | PASS |
| Linux Clang ASan + UBSan | Five portable executables, fail on sanitizer report | PASS |
| Windows Server 2022 / MSVC + Zstandard + WinFsp SDK | Portable tests plus native callback executable (six total) | PASS |

The final portable sources were also rebuilt and tested locally with GCC 14.2
plus Zstandard, GCC raw-only, and Clang 17 ASan/UBSan: five executables in each
configuration, **15 passed executions**. This is a count of executable suites,
not a claim of thousands of independent tests or a performance benchmark.

The native lane builds commit-pinned Zstandard 1.5.7 and asserts that its
compressible fixture actually generated Zstandard chunks. It uses the official
WinFsp 2.1 user-mode headers/library/DLL, not substitute SDK declarations.
The tests invoke callbacks directly against a user-mode FSP_FILE_SYSTEM struct.
**No filesystem is created, dispatcher started, mount assigned or kernel driver
installed/loaded by the test code.** No operations ran on the owner's PC/GPU.

## What this development round added and checked

- **Entire filename validation:** a regression reproduced the previous bug
  where validation stopped at the first dot. ADS syntax, wildcards, control/NUL,
  separators and unsupported high bytes in extensions now fail validation.
- **Pre-allocation admission:** index size is checked against the remaining
  catalog allowance before reading or allocating it. Tiny forged headers prove
  rejection before reaching absent index/footer payloads. Empty indices can
  fit a zero budget. Serialized-index size is not a full process-memory cap.
- **Native callbacks:** typed NTSTATUS failures, Open/Close, stable metadata,
  short/EOF reads, cross-chunk byte parity, readonly Flush and explicit denials
  for create/write/delete/truncate/rename/ACL/label mutations.
- **Native lifecycle and bounds:** Cleanup preserves later reads until Close;
  two concurrent readers are byte-exact; 256-context exhaustion fails closed;
  stale-close tokens do not close a newly reused slot. Invalid read sizes and
  null buffers are rejected. Construction refuses a corrupted fixture.
- **Security descriptors:** valid protected single-owner read/execute ACE,
  correct size negotiation, no write grant and no undersized-buffer overwrite.
  This is not qualification of actual mounted authorization or source ACL parity.
- **Directory continuation:** SDK-packed records, short output buffers, markers,
  case folding and dot entries. A portable regression reproduced skipped
  punctuation/space-prefixed names with plain lexical markers. The shared
  explicit dot-rank comparator and expanded native pagination test now pass.
- **Physical host paths:** pure no-I/O tests cover device/extended namespaces,
  reserved aliases including superscript digits, streams, NUL, ambiguous suffixes
  and length limits. Synthetic integration tests ensure NUL paths cannot create
  a truncated-name output. No test opens a physical device.

## Dependency and CI failures retained, not hidden

The first native run stopped on duplicate DLL names in the official MSI. A
narrow correction then exposed a duplicated import-library basename: the SDK
and optional fsext payloads have the same filename. Both failures were in SDK
staging, not suppressed tests. The final extractor selects exact component IDs
from pinned v2.1 Product.wxs, verifies the release checksum, and still rejects
ambiguous payloads. It never executes installer/custom actions.

The corrected initial callback milestone passed in
[run 42376512623](https://github.com/strmt7/FlitzFS/actions/runs/42376512623).
The Windows build logs include an upstream WinFsp alignment-padding warning
(C4324), and the pinned checkout v4 action triggers a Node20-deprecation warning
while the hosted runner forces Node24. Neither is hidden or presented as a
warning-free build. No broad warning suppression was added.

## Research rounds and decisions

The detailed ABI/lifetime/security/dependency research is in
[WINFSP_CALLBACKS.md](WINFSP_CALLBACKS.md). Subsequent rounds checked:

1. Exact SDK component identities, rather than selecting the first matching
   filename: [WinFsp v2.1 Product.wxs](https://github.com/winfsp/winfsp/blob/v2.1/build/VStudio/installer/Product.wxs).
2. Microsoft namespace/device alias semantics before physical I/O:
   [Naming Files, Paths, and Namespaces](https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file).
3. Directory markers must use the same order as emitted directory records,
   including special dot entries: [WinFsp native API](https://winfsp.dev/doc/WinFsp-API-winfsp.h/).

The prior review of all 13 other repositories remains in
[RESEARCH_2026-11-07.md](RESEARCH_2026-11-07.md). No other repository was modified
and no private code was copied into this public project.

## Earlier retained milestones

M1: CPU core `84efaa8`; M1.5: handle/catalog commit `51adb76`, Windows CI
42369383337; M1.6: concurrency commit `63ec05f`, CI 42370249875; M1.7: projection
commit `f39e927`, CI 42370565055. Older local-only qualification notes describe
their point in time, not the current native callback state.

## Explicitly NOT qualified or implemented

There is still no general-purpose mount application or full `.flzip` codec compatibility,
AMD HIP execution, encryption/authentication, transactional writable volume or
power-loss recovery. The synthetic hosted mount and process-crash qualifications
are recorded below; persistent plaintext catalogs are now implemented. These
do not qualify in-flight cancellation, cross-user ACL enforcement, Windows 11,
broad application compatibility, ThreadSanitizer, long soaks, broad fuzzing,
independent security audit or end-to-end performance comparisons. Parent-path
link/race containment is incomplete; lexical gates are not a general sandbox.

Passing these tests does not establish production safety, immunity to hangs or
resource exhaustion, or any GPU speed/compression advantage. Preserve original
data and do not install or mount this preview on the owner's PC automatically.

## Continuation: lifecycle, process limits and real-mount probe

At base `41cff2eea5d77f5b9582cca16c4b3c192f1e6a7b`, Actions run 42380771960
completed all five existing jobs successfully, including Windows Job Object and
injected lifecycle tests. The optional synthetic live probe is a new, separate
qualification gate. Read `LIVE_MOUNT_PROBE.md`; no previous callback-only result
counts as a real-mounted result. Run 42384272839 at
`913b9e2f980a85b7564a23bd2b825ee7dae51f3c` subsequently passed all six jobs,
including actual driver-backed synthetic mounted I/O.

## Actual process-crash/remount evidence

Run 42385143601 at `e4d5a1d9eb1b0f9b121841a7c053e8ac06aa3ab1` passed all six
jobs. In Windows job 128441114383 the supervised normal mount returned 0,
the deliberately abandoned mounted child returned timeout 1460 with termination
confirmed, and a separate recovery/remount child returned 0. The readiness
receipt was required, original/backing bytes matched, the mount disappeared,
and the same directory was mounted and read again before clean shutdown.

Recorded peak job committed bytes were 9,846,784 (normal), 9,842,688 (terminated)
and 9,547,776 (recovery), below the enforced 128 MiB cap. These are committed
process/job bytes, not total physical RAM, kernel memory or performance results.
This is not a power-loss, in-flight cancellation or Windows 11 qualification.

## Persistent catalog continuation

The new bounded manifest parser/loader, real CLI smoke tests and persisted
live-probe bindings require their matching commit's Windows CI. Portable
validation passed all 12 CTest entries (eight test executables plus four real
CLI invocations) in each of GCC/Zstandard, GCC/raw-only and Clang ASan/UBSan,
for 36 executions. The manifest tests reproduced and repaired a reserved-name
normalization gap before publication. These are correctness tests, not speed
benchmarks or proof of production safety.
No remote-control credentials, owner-PC logs or private repository content are
part of these artifacts. No commands were executed on the owner's PC.

## Bounded FLZIP CPU compatibility continuation

Baseline `a7eea03d33a4119e37023205d25cfa6c4909abd7` was retrieved as a
commit-bound artifact, verified by SHA-256, and its complete source tree matched
`c807d158a4e2a3b8dbd148b7355000e9c9cecedc`. All 12 baseline CTest entries
passed locally before implementation.

The added reader/parser, snapshot/cache, catalog/projection, and CLI tests cover
raw/fill/pattern and optional Deflate/Zstd. Negative cases include version/kind
mismatches, count/length limits, overlapping payloads, path conflicts, CRC errors
in unrequested files, exact-stream violations, unknown Zstd output size,
truncations and deterministic mutations. The CLI test runs 12 real subprocess
checks and confirms all five synthetic fixture hashes remain unchanged.

All 14 portable CTest entries passed in each of local GCC with Zstandard 1.5.7
and zlib 1.3.1, raw-only GCC, and Clang ASan/UBSan: 42 entry executions. This count
includes the CLI contract as one entry, not 12 separately counted CTest entries.
Windows/native constructor ABI, the upstream serializer oracle, zlib 1.4 static
staging and actual mounted FLZIP behavior still require the new commit's CI.
Pending checks must not be reported as passed. No owner-PC/GPU access or remote-control credential is
part of this change. See FLZIP_READONLY.md for scope and primary research.

The match-proposal tests require exact CPU fallback for malformed geometry and unavailable sequence support, independent byte decoding of accepted candidates and managed-session publication. Actual CPU/raw/sanitizer/Windows execution must be inspected on the matching commit. This is not GPU execution or a speed comparison.

The GPU-match CPU suite checks its exact prefix index against independent history scans, compares small searches with an exhaustive oracle and rejects malformed geometry, cyclic/forward links and corrupted results. A managed-session fixture feeds the validated proposals into real staged encoding and independent CPU reopen. These are CPU semantic tests. The matching HIP kernel and separate table require complete-SDK compile/link evidence on the same commit before claiming offline compilation.

The match wire suite rejects truncation, header/reserved changes, mismatched request/source/backend, malformed lanes and cross-protocol input. The real Windows memory-channel fixture drives a valid reply into managed-session publication, falls back on a malformed reply and verifies CPU publication before cancellation/retirement of a deliberately held child. Full SDK admission also checks malformed match input and a valid match request with the private bundle missing. Configuration is not an executed GPU result.

Match-controller tests cover disabled/unqualified work, overlapping requests, accepted proposals entering a native volume, missing/corrupt/late workers, a newer CPU source during retirement, repeated unavailable configurations, fresh-evidence pressure recovery, expiry/cancellation/device quarantine and invalid post-run telemetry. The fixture executes only CPU code. Existing decoder controller and policy tests remain enabled to check the shared classification and request-ID changes.

SDK runtime-binding qualification requires malformed static-PE refusal fixtures, the actual header-derived direct-function inventory and CPU fake-runtime identity/error tests. The admission child must clear its own inherited LLVM_PATH while leaving the probe parent unchanged. Inspect these receipts on the matching commit; they are not live driver, GPU or speed evidence. Existing six worker-admission exits and all decoder/match tests remain required.

The metadata qualification requires real signed OS-file byte identity, fake trust-policy failures/cleanup, malformed identity refusal, missing/wrong bytes and stable repeated handle accounting under the existing child budget. All six prior worker admission cases plus the new metadata/refusal children require nine confirmed exits. Results must match the executed commit and cannot be presented as AMD runtime execution or speed.

Installed-driver qualification requires all nine old metadata/private cases plus eight kernel-only/driver cases, with 17 confirmed child exits. Separate self-bounded metadata/decode/match diagnostics must refuse an unavailable/mismatched runtime without publication. Inspect actual module imports and numeric receipts on the matching commit. These checks contain no AMD GPU execution and cannot support a speed claim.

Adapter-identity qualification requires an actual bounded DXGI enumeration, real field/uniqueness checks, no HIP initialization and a correctly represented empty AMD list when applicable. The diagnostic's additional DXGI import is audited locally to its SDK target; the general portable CPU import policy remains unchanged. Metadata cannot establish GPU execution or speed.

## First installed-driver host refusal and diagnostic qualification

On 11 November 2026, commit 37aaed8f075383b8ae47cd9e10fe1dd668315a52
made one explicit 12,288-byte synthetic decode attempt on an RX 9070 XT using the
installed, unchanged 32.0.32015.12012 driver. The worker exited with
0x46530003 (unsupported_configuration), with exit confirmed, no timeout and no
candidate publication or accepted GPU result. Wall time was 142 ms; reported
worker time was 119 ms. Child/aggregate peak commitment was
9,662,464/11,071,488 bytes. The planned 16,492 device bytes do not prove allocation.
The exact failure stage and whether HIP initialized are unknown. No match attempt
followed. These are refusal/resource observations, not speed measurements.

That commit's ordinary CPU, raw-only, Windows-native and sanitizer jobs passed.
Its hosted crash-restart mount job 130339172334 failed: the extended writer timed
out and its exit could not be confirmed within the existing termination window.
The gate correctly refused recovery. This resembles an earlier intermittent
hosted failure; its cause remains unresolved. Other mounted lanes passed. Do not
convert this failure to a pass or transfer those mounted tests to the owner PC.

The new diagnostics require portable category/stage invariants, fake-API stage
checks, actual Windows child-exit/classifier checks and no-HIP DXGI budget
inspection. They preserve all existing seventeen admission children and refusal
probes. Qualification is pending until exact matching hosted receipts pass.

### Diagnostic qualification completed; node refusal observed

Commit 9c1637a3144b743271de9c521070d2c2765ae78d passed all 13 triggered
workflows. Windows-native CTest passed 126 groups; the real Windows transport
fixture passed 867 checks, including staged failure-category handling. The SDK
fake API probe passed 108 checks and retained all 17 confirmed admission exits.
Linux CPU/raw-only, Windows raw-only and sanitizer checks passed. Its hosted
crash-restart job 130350565766 confirmed writer termination, resumed the store
and independently verified the preserved 7,001 bytes and continued state. This
successful run does not explain the earlier intermittent unconfirmed exit.

On the owner PC, fresh no-HIP DXGI budget inspection passed in 33 ms (peak process
job commitment 1,871,872 bytes). Installed-runtime metadata passed in 167 ms.
One separately bounded diagnostic decode then returned raw 0x46530703, category
0x46530003 and stage adapter_node: 161 ms wall time, 110 ms internal elapsed,
12,288 CPU bytes verified, no candidate publication, no timeout and confirmed
child exit. Child/aggregate peak commitment was 9,662,464/11,067,392 bytes.
The actual HIP node mask remains unknown. No stream or kernel call was reached
by the worker after the failed identity check. Driver version/status remained
32.0.32015.12012/OK; no owned diagnostic process remained. These observations
are not a GPU speed result, mounted-owner test or general safety qualification.

The following batch adds hosted private-runtime import/license inventory and
fixed-choice current AMD research. Its own matching execution receipts are
required before interpreting the inventory; it ships no AMD binaries.

### Private runtime inspection and research-scope correction

Commit c8e69f9288533ce85b49243ee6db25e34d177a48 completed the hosted SDK
inventory without loading a runtime or exporting AMD binaries. Its three staged
DLLs total 16,777,120 bytes. Normal imports of rocm_kpack.dll include
msvcp140.dll, vcruntime140.dll and vcruntime140_1.dll, which are outside that
staged closure. Their deployment and provenance must be qualified before
calling a private package host-independent. The inventory observed 17,864
entries and 27 license-named files totaling 75,742 bytes; this does not qualify
redistribution of the runtime or its dependencies.

All six requested AMD pages returned native HTTP 200 with substantive text.
Review nevertheless rejected the old Radeon/Ryzen requirements page: its banner
explicitly limits coverage to releases through 7.2.1 and redirects readers to the
unified documentation. Its native successful capture remains preserved, but it
must not establish ROCm 10.1 compatibility. The current release notes identify
10.1.0 dated 5 October 2026. The observed-driver release notes map driver-store
32.0.32015.12012 to Adrenalin 26.10.2; the ROCm-specific page identifies a different
26.10.41.05 package. Those facts alone do not prove compatibility of the private
SDK with the owner's unchanged driver.

The next research batch uses the unified matrix and current license page, rejects
explicit retired-documentation banners while retaining native receipts, and
adds a fixed Microsoft adapter-topology source group. Its regression and native
capture results remain required; no new host experiment follows from this change.

## Native compression profile probe

The new advisory probe measures at most one existing profile per advance on a
sample of at most 65,536 bytes. It preserves the existing CPU verifier, optional
hint fallback and allocator caps. Its regression tests include a committed RAM
object flowing through measurement and the existing recompressor, with unchanged
source bytes/write/flush counters and independent target readback. All matching
CPU, raw-only, Windows-native and sanitizer results are pending until the new
commit executes. These are correctness checks, not speed/ratio superiority or a
completed automatic learning/scheduling system.

## Installed-runtime properties-only diagnostic

The owner explicitly requested another GPU check. Fresh DXGI budget and runtime
metadata passed; one bounded 12,288-byte decode trial again refused at adapter_node
with no candidate publication, timeout or unconfirmed exit. Wall time was 171 ms;
child/aggregate peak commitment was 9,682,944/11,083,776 bytes. The CPU reference
passed, driver 32.0.32015.12012 remained OK and no owned process remained.

The new diagnostic captures the raw fields without device selection or a
FlitzFS kernel load/launch. Portable wire, real Windows CPU transport and fake
HIP identity-only tests must pass on its exact commit. The SDK refusal matrix
retains all 17 original admission children plus the existing diagnostic cases.
Matching execution results are pending; no compute gate is relaxed.

## Single physical-node proof preparation, 11 November 2026

77931001b8a1238fd29f9e41081d2025ab657f23 passed complete-SDK run 42979952055
and core run 42979952021, including all isolated hosted mount scenarios. The
owner properties-only result confirmed raw HIP node mask zero with exact LUID;
no compute was requested. GPU_NODE_PROOF.md records the measurements, bounded
new native collector and current primary-source receipts. New proof-policy,
fake-API and hosted software-device checks must pass on the exact subsequent
commit before owner topology or compute execution. At this source checkpoint
actual FlitzFS GPU execution and speed are still unproven.

## Executed native node proof and retained decode refusal

At 96cb919, SDK run 42983895333 and CPU/native/sanitizer prerequisites passed.
The owner OS-only collector verified the exact AMD LUID, healthy single physical
node and confirmed retirement. One tiny decode then failed with device_fault /
none, retaining verified CPU data and no published GPU reply. Exact measurements
are in evidence/gpu-node-proof-96cb919.json. Subsequent changes only preserve
which existing memory predicate failed; they need their own exact CI. Current
Microsoft memory documentation will be captured with isolated Crawl4AI before
changing residency/admission rules. GPU execution and speed remain unqualified.

## Worker memory observation preparation

The b7de640 fixture correction passed core run 42988002142, including its
hosted mount matrix; the earlier 96cb919 crash-restart retirement timeout remains
unresolved and retained. The pinned 9028be3 worker then reached the final
completed_nonlocal_growth check after successful GPU launch/completion/statuses
and exact 12,288-byte CPU comparison. Evidence is in
evidence/gpu-internal-decode-9028be3.json. No candidate was accepted.

GPU_MEMORY_OBSERVATION.md defines a new explicit metadata-only diagnostic with
three process-local budget samples and execution/retirement flags. Source now
adds portable golden/malformed/cross-protocol checks, Windows CPU-channel cases
and a separate SDK bad-runtime refusal. These new configured checks require
exact executed CI before another owner diagnostic. All existing admission and
fallback behavior remains unchanged; this is not a performance pass.

## Native Windows application and controlled owner check

At f4928c2d2510cacb55da664a0338c8592194d9a0, application run43341114027/
job131609363436 passed the separate PE audit,377 model checks/three strictly
balanced ownership cycles and actual native GUI-control workflow. The same
hashed artifact then passed owner Windows build26300 checks: three179-to179
model cycles, working-set restoration, healthy provider cleanup and a clean GUI
create/save/read/optimize/discard/exit workflow. Its GUI child peaked at14209024
committed bytes. Neither host run needed forced termination. Postflight found
no remaining FlitzFS process; WinFsp remained Stopped/Manual.

These numbers are bounded qualification observations, not throughput/compression
benchmarks or complete Windows/client/driver/accessibility/power-loss coverage.
The GUI's dynamic readiness polling yields different assertion counts per run;
the complete required command and byte-comparison sequence passed in each.
The app is unmounted and initializes no FlitzFS HIP compute. Preserve prior
zero-benefit GPU evidence and all full-product gates.
[Exact artifact/source/resource/failure record](evidence/volume-app-f4928c2.json).

## Executed priority capability checkpoint

At b2cf9899417d52c2266d86fa1246b874001376a3, SDK run43432703582 passed235 CPU
assertions across12 priority scenarios. The exact historical probe reported
category1179844613 with one query; the corrected probe reported1179844611 with
two. Neither loaded HIP or executed GPU work. Every original SDK/worker gate,
including20 confirmed exits, passed; core run43432703721 passed all 13 jobs.
[Source-bound evidence](evidence/priority-capability-b2cf989.json).


## Historical retirement reconstruction

The manual retirement-reference workflow is isolated from product targets and
normal tests. Read [RETIREMENT_REFERENCE.md](RETIREMENT_REFERENCE.md) for exact
old-source pins, one trial per disposable VM, phase receipts and original limits.
No owner driver/mount/GPU operation or stored-format change follows. Unknown
retirement remains terminal; a green reconstruction cannot explain the old kernel
state by itself. Publishing-source execution is still required.

At497066c all 13 ordinary jobs and8 Linux source tests passed. Both historical
comparison variants failed preparation before driver installation or trial.
The correction binds actual directory identities and independently reconciles raw
phase/acknowledgement/result/CPU records. Windows/Linux source tests and actual
controlled trials require new-source qualification; D01 remains unresolved.

[Exact gate, source-preparation failure and recency review](evidence/retirement-preparation-497066c.json).


At400f74c Linux passed16 source cases and Windows passed7 record cases before
rejecting fixture bytes; no manual driver/trial was run. Canonical Git capture
with separately verified exact/CRLF checkout identity is the next18-case gate.
The source fixture failure is not a historical retirement result or GPU evidence.


At ab37c4a, all18 source cases passed on both OSes, all 13 core jobs passed, and
both historical held/closed-handle trials retired under original limits with
exact7001-byte CPU recovery. Their one-shot outcome remains inconclusive for D01.
[Raw source-bound evidence](evidence/retirement-reference-ab37c4a.json).
The separate [native synchronization API probe](HIP_WARP_SYNC_PROBE.md) now needs
its nine-case CPU and exact SDK compiler qualification. Vendor kernels, runtime
routes and stored encodings remain unchanged; useful GPU gain is unproven.


The native synchronization API probe passed its9 CPU cases, actual selected header,
compiler/dependency/object checks and original SDK/20-worker gates ataf2f714
([evidence](evidence/hip-warp-offline-af2f714.json)). Exact physical-input stability
and complete automatic trigger coverage are the next narrow qualification;
no vendor kernel, runtime or GPU-output behavior changes.


At 2fdd01c, all 13 ordinary jobs and the SDK/worker gates passed, including
physical-input stability of the native synchronization probe
([evidence](evidence/hip-warp-stable-2fdd01c.json)).
The separate [native-wave candidate](HIPCOMP_NATIVE_SYNC.md) is experimental,
OFF by default and awaiting its own hosted qualification: seven composition
cases, a compiled CPU index oracle and strict offline object inspection.
No GPU object is loaded, stored codec added or runtime path enabled. D01 remains open.


The native-wave composition passed at 09c4fca: seven source cases, the actual
compiled index oracle, both offline objects and all SDK/20-worker checks.
All 13 ordinary jobs passed; all 121 sanitizer results were reconciled.
[Evidence](evidence/hipcomp-native-sync-09c4fca.json).
The next [bounded byte entry](HIPCOMP_BOUNDED_ENTRY.md) retains the complete
256-KiB allocation cap and checks actual wave geometry before slab access.
Its new CPU/source/image/compiler gates are pending. No runtime or stored codec
is enabled; actual GPU output, useful gain and D01 remain unqualified.


The bounded entry qualified at 857d5c9: all five validator configurations,
all 13 ordinary jobs, 12 source/image cases, exact native AMDGPU symbols and the
complete SDK/20-worker checks passed
([evidence](evidence/hipcomp-bounded-entry-857d5c9.json)).
The next [private typed-module fixture](HIPCOMP_MODULE_LINK.md) adds CPU argument
forwarding and separately opted-in linking. It remains unqualified until its own
execution; no module loading, binary artifact, worker integration or GPU gain follows.


The private typed-module link qualified at c86559b, including all 13 ordinary
jobs, 122 sanitizer tests, five 144524-check validator configurations and 20
confirmed worker exits ([evidence](evidence/hipcomp-module-link-c86559b.json)).
The [LZ4 candidate frames](LZ4_WORKER_FRAMES.md) qualified at 959606a in all five
core configurations and all 13 ordinary integration jobs
([evidence](evidence/lz4-worker-frames-959606a.json)). Enabled builds reported
621 framing checks; disabled builds reported 593. Windows CPU exchanges retained
79 initial/final handles. The [checked slab layer](GPU_LZ4_SLAB.md) then passed
all five core configurations and all 13 jobs at 38229d1: 968 checks with the
decoder and 939 without it ([evidence](evidence/gpu-lz4-slab-38229d1.json)).
The live owner-host model and GUI passed, with exact retirement and complete
staging cleanup ([evidence](evidence/owner-gui-38229d1.json)).
The next [static SDK-worker CPU decoder](CPU_LZ4_WORKER.md) is explicitly opt-in.
New staging, decoder/protocol and both worker-capability configurations require
publishing-source execution. No actual GPU compressor or normal-I/O route follows.
D01 remains open.

The optional static CPU LZ4 worker qualified at 94ec882: all five core
configurations, all 13 ordinary integration jobs, ten staging cases per
configuration, the same decoder's 806/621/968 verifier/frame/slab checks and both
success/unavailable worker retirements. All original 20 admission cases remained.
[Evidence](evidence/cpu-lz4-worker-94ec882.json).
The separate source-snapshot job failed the obsolete 512-file preflight and is
retained as a failure; SOURCE_SNAPSHOT.md defines the corrected combined
file/directory accounting within the unchanged archive-byte/768-entry limits.
New snapshot execution remains pending. No actual GPU compressor is enabled.
## Native compression composition diagnostic

The separate experimental [native LZ4 composition fixture](NATIVE_LZ4_COMPOSITION.md) adds hosted CPU candidate/fallback verification and an explicit GPU-to-native-file diagnostic. It requires matching qualification before owner execution. Normal filesystem I/O does not launch this probe. Its exact-source SDK and 13-job core qualification passed at a047952. One actual GPU payload was stored and independently CPU-recovered; useful end-to-end benefit and physical durability remain unproved.

The new [asynchronous compression controller](ASYNC_LZ4_CONTROLLER.md) reuses bounded worker supervision and evidence-aware recovery while making CPU source selection terminal and independent of worker retirement. Its 13-job core matrix and full SDK qualification passed at f08e6b8; ordinary Windows I/O remains CPU-only.

The experimental [Windows LZ4 advice bridge](NATIVE_WINDOWS_LZ4_ADVICE.md) forwards optional verified candidates through owned session writes and actual SDK dispatch. Existing access, budget, metadata, count and terminal-failure rules remain; its 13-job core matrix passed at 25ea952, including 432 new advised transaction-fault cases. This does not start GPU work from normal writes.

The new [resource-adaptive execution path](ADAPTIVE_LZ4_EXECUTION.md) replaces the tiny prototype ceiling as product/development policy. Its 64-bit uniform layout and resource/time planner size work from demand, actual headroom, qualified costs and device capabilities. New source-bound qualification is pending; streaming transport/runtime integration is unfinished. No adaptive GPU execution or performance gain is claimed from CPU planning or offline compilation.


The experimental [uniform version-2 compression path](UNIFORM_LZ4_RUNTIME.md) adds owned variable-count messages, guarded slab/CPU verification, real sized-channel CPU integration and a typed SDK compute wrapper. Its publishing-source qualification is pending. Resource-sized transport at `38e1f080` passed all 13 core jobs and full SDK qualification; see evidence/adaptive-transport-38e1f080.json. Authenticated version-2 worker packaging, actual larger-window GPU dispatch, normal Windows routing and useful end-to-end benefit remain unfinished.


Uniform frames/slab and typed compute at `f8c0d05` passed all 149 native Windows groups, both Linux builds, Windows raw-only, sanitizers and full SDK qualification. Dependent mount checks remain separate. The explicit authenticated uniform worker package and its metadata-only admission tests are now added and require publishing-source qualification. This enables no ordinary GPU route or owner-device execution; configuration-qualified cost/grant/recovery and Windows write integration remain required.


The explicit resource-admitted uniform controller adds complete parent/child accounting, persistent recovery/quarantine, fresh observation after CPU validation and constant-work terminal CPU selection; see UNIFORM_LZ4_CONTROLLER.md. Its publishing-source checks are pending. The packaged worker at `c96115e` passed all 13 core jobs and SDK qualification. Real cost acquisition, controlled Windows writes and actual uniform-device benefits remain open; no normal GPU route is enabled.


The [uniform GPU/native-write calibration](UNIFORM_GPU_CALIBRATION.md) passed all 13 core jobs, 151 native Windows groups and full SDK qualification at `7e87bb4`. One explicitly admitted owner GPU trial stored 16,918 encoded bytes from 131,072 source bytes and recovered current/prior data through the independent CPU reader. The complete image remained 983,040 bytes, equal to the CPU fixture. Its 602.85 ms exchange included 419.35 ms device initialization and 4.76 ms kernel launch-and-wait. This establishes actual uniform GPU/native-store integration, not comparative speed or whole-image size gains. [Exact source, checks, phases and cleanup evidence](evidence/uniform-gpu-native-7e87bb4.json) is retained. Ordinary callbacks remain CPU-only; reusable configuration-keyed measurements, safe startup amortization, controlled application integration and useful total-cost results remain required.

The [finite uniform sweep](UNIFORM_LZ4_SWEEP.md) passed all 13 core jobs, 152 native Windows groups and full SDK qualification at `847423f`, but its first owner GPU trial failed with native exit `0xc0000409` near the 128 MiB child memory limit. No GPU result was accepted. Independent CPU fallback recovered the complete 1 MiB file and prior data, all locks retired and no FlitzFS process remained. The cause is unproven; [the failure evidence](evidence/uniform-sweep-847423f-failure.json) is retained. Do not repeat that source or hide the result by raising its cap. Reusable bounded-operation device resources and new fault/ownership qualification are next; no speed gain or normal GPU callback route is claimed.

The [reusable uniform compute scope](UNIFORM_LZ4_REUSE.md) passed all 13 core jobs, 152 native Windows groups and full SDK qualification at `d33efd6`. One bounded owner GPU operation stored 135,344 encoded bytes from 1 MiB of source exactly and passed independent current/prior CPU recovery. Worker peak commit was 88.44 MiB under the unchanged 128 MiB cap; all locks and owned processes retired. The complete image stayed equal to the CPU fixture. [Exact evidence](evidence/uniform-reuse-d33efd6.json) records the 626 ms exchange and its limits. This is no comparative speed/whole-image gain or diagnosis of the retained old abnormal exit. Ordinary callbacks remain CPU-only; useful measured costs, autonomous policy, controlled Windows integration and deep collective compression remain open.

## Owned deferred Windows writes — a4ad516

All 13 [core jobs](https://github.com/strmt7/FlitzFS/actions/runs/43633235875)
and the complete [ROCm SDK gate](https://github.com/strmt7/FlitzFS/actions/runs/43633243098)
passed for a4ad5166d5063d820cfdd704a600ea20a23f1d93. Windows native CTest passed 154 groups, including 54
deferred-write checks and the retained 199 controlled-native-write checks.

Four direct CPU cases cover copied input, exact completion facts, verified
CPU-semantic worker advice, held-worker read-triggered fallback and queued
shutdown with two concurrent stop callers. The real hosted RAM-handoff mount
passed with CPU-only deferred responses, observed client flush, independent
current/prior readback, zero remaining contexts/locked bytes and complete unmount.

The SDK retained 3,485 compute checks across 83 scenarios (10 success/73 refusal),
15 raw checks across two refusal scenarios, 25 confirmed admission exits and
zero command timeouts. These are CPU/SDK tests, not new hardware execution.

[Digest-verified evidence](evidence/owned-deferred-a4ad516.json) retains source,
artifacts and scope. The [initial compilation failure](evidence/owned-deferred-7ef0dd3-build-failure.json)
remains recorded. No owner binary, mount, driver, GUI or GPU operation ran this
round. Actual GPU completion through this service, automatic costs/routing and
comparative benefit remain unqualified.
