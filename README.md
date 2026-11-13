# FlitzFS

The [deep background compression plan](docs/COLD_COMPRESSION_RESEARCH.md) requires transparent cross-file grouping and GPU-supported learned or experimentally justified novel compression, separate from fast ingestion. It remains unimplemented and must preserve ordinary Explorer file/folder behavior, bounded access and exact CPU recovery.

The experimental [HIP LZ4 compute implementation](docs/HIP_LZ4_COMPUTE.md) now has an SDK-only allocation, submission, readback and retirement path. Selected-SDK CPU qualification passed at df8b08e ([evidence](docs/evidence/hip-lz4-compute-df8b08e.json)); it remains disconnected from ordinary file operations and no GPU performance result is claimed.


<picture>
  <source media="(prefers-color-scheme: dark)" srcset="resources/brand/flitzfs-logo-dark.png">
  <img src="resources/brand/flitzfs-logo-typed.png" alt="FlitzFS — red layered F symbol and Segoe UI Bold wordmark" width="640">
</picture>

FlitzFS is an experimental **Windows 11 AMD64 compressed filesystem**.
The goal is high sustained read/write speed, strong automatic lossless
compression, useful AMD GPU acceleration with independent CPU fallback, and
low-impact local learning that improves compression and layout over time.

**This is not a production-ready filesystem.** Its native Windows application is
experimental. Production writable integration, scaling, automatic learning/maintenance and
general repair remain unfinished. Ordinary filesystem operations remain CPU-only.
Use disposable fixtures and retain independent copies of valuable data.

The planned Windows helper/taskbar tray will create and manage both physical-disk
filesystems and memory-backed virtual volumes through the shared engine.
[Backend and application contract](docs/WINDOWS_VOLUME_BACKENDS.md).

[Current assessment](docs/REPOSITORY_ASSESSMENT.md) ·
[Detailed plan](docs/DEVELOPMENT_PLAN.md) ·
[Continuation ledger](docs/DEVELOPMENT_PLAN.json) ·
[Safety rules](docs/SAFETY.md)

## Current verified checkpoint

The funded Windows helper at **99d5365** passed all 13 integration jobs,
728 application-model assertions and a healthy owner GUI workflow with confirmed
retirement. It uses the real CPU filesystem engine; durable provider mapping,
authorization, long-lived learning and the complete Windows product remain
unfinished. [Executed helper/GUI evidence](docs/evidence/funded-helper-99d5365.json).

The independent CPU LZ4 candidate verifier at **86bc830** passed all five core
configurations: 806 assertions with LZ4 on Linux, Windows native and ASan/UBSan;
389 in codec-disabled Linux/Windows builds. It verifies private candidate bytes
and is not invoked by filesystem or GPU-worker operations.
[CPU evidence](docs/evidence/gpu-lz4-cpu-86bc830.json).

At **1d20f40**, native Windows offline compilation and exact object inspection
passed 28 admission cases, AMD64 headers, file-bound gfx1100 metadata, unchanged
output hashes and the original SDK/worker gates with 20 confirmed worker exits.
This uses the verified ROCm 10.1.0 distribution; its inspected HIP component is
7.16.26385. The candidate is neither loaded nor linked into the product.
[Offline object evidence](docs/evidence/gpu-lz4-object-1d20f40.json).

Current-AMD warp/model/stream capture admission needs a source-based marker
correction. Actual GPU output, safe runtime integration and useful complete-operation
compression/speed gains remain unproven.
[Kernel review and pending research](docs/GPU_LZ4_KERNEL_REVIEW.md).

### Earlier GPU observation, unchanged

At source **645a1d15d4f0138c45df67c570f0acd52815345a**, all 13 core hosted
jobs passed, including Windows native/raw, Linux raw/codec, sanitizers and
the isolated mounted profiles. The separate locked-RAM qualification passed
valid, malformed, falsely declared HIP and timed-out CPU-worker cases.
Hosted Windows server tests do not qualify the latest Windows 11 client product.

One explicit owner-device trial then exercised a **64 KiB synthetic file in
locked RAM**, with accepted GPU match proposals reaching native recompression,
retained handles, volume-head publication and independent CPU readback.
It used 224 KiB of explicit device buffers and peaked at 8.25 MiB of locked RAM.
Source bytes were preserved, handles stayed 116→116, locked RAM returned to zero,
and postflight found no remaining owned workers or driver/runtime changes.
There was no mount, driver installation or physical-drive filesystem image.
[Exact sources, artifacts, measurements and limitations](docs/evidence/native-gpu-ram-645a1d1.json).

This trial demonstrated **no compression or speed gain**. All 56 proposals were
verified and forwarded, but none produced a selected encoding: both profiles
stored 8,316 payload bytes. The CPU native profile took about 5.4 ms; the proposed
profile took about 714 ms, including a 707 ms cold worker exchange. This is one
CPU-first observation, not a controlled comparative benchmark or kernel timing.

Storage accounting also matters: this tiny fixture used 458,752 bytes for the
selected image plus head, or 1,310,720 bytes including retained source history.
Payload reduction alone is not filesystem compression performance. G02 remains
unfinished until a useful end-to-end benefit is demonstrated.

The historical mounted-child retirement failure cause remains unproven. The
external-client protocol and mandatory product-origin admission have separate
qualified evidence; a green matrix does not establish that historical cause.
[Isolation contract](docs/MOUNT_PROCESS_ISOLATION.md) ·
[Origin-guard evidence](docs/evidence/origin-guard-053ade8.json) ·
[Execution ledger](docs/DEVELOPMENT_STATE.json).

## Implemented experimental components

- A bounded advisory journal with two flushed copies and independent CPU
  recovery. Funded helper creation/replacement is qualified at 99d5365;
  durable provider mapping/authorization, long-lived continuation and automatic
  learning remain unfinished. See [the helper contract](docs/ADVISORY_HELPER.md).
- A bounded transactional object engine with integrity checks, immutable
  snapshots, replicated checkpoints, independent CPU reopen and explicit flush.
- A native inode/directory/extent layer with sparse 64 KiB extents, reads,
  partial writes, truncate/regrow, rename, deletion and retained open contexts.
- Source-preserving compaction and incremental recompression into a new image,
  followed by independent verification and replicated volume-head publication.
- Raw, zero, repeated-pattern and optional Zstandard storage. GPU match proposals
  are independently checked before optional CPU encoding; CPU recovery does not
  require a GPU.
- An OFF-by-default WinFsp adapter. Normal product routes remain read-only;
  writable callbacks and mount/restart trials are isolated experiments.
- Supervised bounded worker transport, fresh identity/resource admission and
  evidence-aware recovery primitives. An explicit diagnostic can use the
  installed AMD runtime without installing or replacing it.

These are storage and integration primitives. They do not supply a finished
helper application, unattended optimization service or production installer.

## Remaining development

The [full development plan](docs/DEVELOPMENT_PLAN.md) retains every product
requirement. The next priorities are causal repair of the intermittent child
retirement defect, complete pipeline cost instrumentation, useful compression
candidates and safely amortized GPU work. Correctness checks must remain intact.

Further packages cover store/namespace scaling, a small crash-safe optimization
area, resource-aware foreground policy, persistent workload learning, collective
background compression/layout, payload repair, Windows semantics and independent
comparative qualification. Background work must remain bounded and yield to
applications; GPU presence alone cannot justify a slower or redundant path.

Current prototype limits include 256 inodes, 255 populated extents, 16 MiB decoded
object live data and bounded retained history. Sparse logical capacity does not
remove those populated-data limits. Authentication/encryption and general payload
repair are not implemented. RAM fault models and process-kill tests do not prove
physical power-loss survival or protection against arbitrary future bugs.

## Development and host safety

The owner's filesystem test backing must remain **locked RAM only**. No raw-device
writes, formatting, partition changes, system-drive images, owner driver changes
or automatic driver-backed mounts are authorized. Small source/tool/report
staging on the verified HDD has a separate combined 256 MiB cap and 10 GiB reserve;
it never supplies filesystem backing. No stale services, scheduled tasks,
unconfirmed workers or unbounded resource use are permitted.

Run ordinary development builds in a disposable VM:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure --timeout 45 --parallel 1
```

The base build requires C/C++ compilers and CMake. Zstandard and LZ4 are optional
unless explicitly required. Configuration installs nothing. Hosted provisioning
uses verified isolated tools; it is not a product dependency installer.
GPU and mount probes are OFF by default and are not ordinary CTest operations.

Useful contracts:

- [Native filesystem](docs/NATIVE_FILESYSTEM.md) and [format](docs/FORMAT-v1.md)
- [Checkpoint durability](docs/REPLICATED_CHECKPOINTS.md) and [volume head](docs/VOLUME_HEAD.md)
- [Managed sessions](docs/VOLUME_SESSION.md) and [recompression](docs/RECOMPRESSION.md)
- [GPU resources](docs/GPU_RESOURCE_ENVELOPE.md), [recovery](docs/GPU_RECOVERY.md) and [transport](docs/WORKER_CHANNEL.md)
- [Native RAM/GPU probe](docs/NATIVE_GPU_RAM_PROBE.md) and [owner testing](docs/HOST_RAM_TESTING.md)
- [Windows integration](docs/NATIVE_WINDOWS_PROJECTION.md) and [writable VM trials](docs/WRITABLE_MOUNT_TRIAL.md)
- [Brand assets and actual-font provenance](resources/brand/README.md)

Development stays on main. The owner controls release; development commits are
not releases. Preserve the [license](LICENSE) and source/dependency notices.

## GPU pipeline cost preparation

New source adds input-only parent match preparation, avoiding a CPU index/sort
that the worker independently rebuilds, with unchanged request bytes and CPU
verification. A separate opt-in diagnostic records complete worker phase wall
costs; normal requests make no timing queries. Publishing-commit qualification
is pending. This is not a GPU speed/compression gain or an automatic route.
[Contract and bounds](docs/GPU_PHASE_PROFILE.md).

The qualified source99 GPU diagnostic identified initialization/allocation and
bundle checking as larger costs than its retained independent CPU oracle. The
next match implementation coalesces three device buffers/uploads into one
bounded, checked allocation, preserving the kernel ABI and verification.
See [GPU phase evidence and allocation boundary](docs/GPU_PHASE_PROFILE.md).
The new path requires separate source-bound qualification; useful GPU throughput
or compression benefit and the D01 retirement repair remain unproved.

Source b6a9b4e passed all thirteen core jobs and the twenty-exit full-SDK gate;
its changed 4-KiB match path also passed one actual GPU profile and independent CPU
comparison. No stale process or driver change was observed. The single 423.0053-ms
exchange does not establish a gain over the earlier observation.

The next D01 change separates basic, mutation and deletion mounted clients from
their filesystem hosts on disposable Windows runners. It preserves the original
workloads and adds strict actor evidence; its publishing-source checks are pending.
Restart/resumer, memory-handoff and read-only actor conversion, adapter-wide
protection and the historical termination cause remain open.

The three external-client profiles at 6dac1f1 passed all thirteen core jobs;
their source-bound records are in
[ordered actor evidence](docs/evidence/ordered-actors-6dac1f1.json).
The next change extends that separation to ordered restart sources, writable
resumers and the complete read-only normal/kill/remount campaign, retaining all
original data/metadata and recovery checks. Its qualification is pending.
RAM handoff, adapter-wide protection and the historical shutdown cause remain open.

Source9dd6a98 passed all thirteen core jobs, including external restart/resumer
and read-only normal/kill/remount clients. Eleven current Microsoft event/process
captures were admitted through the isolated crawler.

The next change moves the mounted memory-handoff file handles into its external
controller using a finite seven-event conversation. It retains all original
compression, identity, pinned-image, recovery and cleanup checks, with new CPU
order/fault/handle tests. Its publishing-source qualification is pending;
adapter-wide protection and the historical shutdown cause remain open.

All thirteen jobs passed atfa9fe90, completing external clients for the current
mounted fixtures. The next implementation adds mandatory Create/Open host-origin
admission to both adapters and requires an explicit origin provider in session
binding. Mounted probes now test product rejection; direct CPU fixtures remain
explicitly synthetic. See [origin boundary](docs/WINFSP_ORIGIN_GUARD.md).
New-source qualification is pending; full filesystem readiness is unproven.

The experimental shared engine now has a separate failed-advice rebuild path:
[contract and pending qualification](docs/ADVISORY_REBUILD.md). It preserves live
file handles, starts empty advice in a new area and retains failed ownership
within an aggregate cap. Provider mapping, persistent rewrite credit and automatic
learning remain unfinished. The parent origin guard passed all 13 core/mounted jobs;
[source-bound results](docs/evidence/origin-guard-053ade8.json).

The Windows helper now prepares a [controlled advice-recovery consumer](docs/ADVISORY_HELPER.md)
for its existing maintenance operation. It retains and accounts for new/failed
areas and keeps file access independent. At 9977674, all 13 core/mounted jobs,
461 app-model assertions and a healthy owner GUI workflow passed
([source-bound evidence](docs/evidence/advisory-helper-9977674.json)). The next
test-only follow-up covers allocator failure after binding admission; those
new cases still need VM qualification. Physical mapping, persistent rewrite
credit and autonomous learning remain open.

An explicit experimental [write-workload counter](docs/WORKLOAD_ACCOUNTING.md)
now prepares persistent provenance for future background rewrite credit. It
counts accepted input in the existing data transaction and survives CPU reopen
and image copying; default legacy behavior is unchanged. Publishing-source
qualification is pending. It does not yet authorize or schedule an optimizer.

The underlying write-workload counter at 14cfb12 passed its full 13-job matrix
and 3,945 workload assertions per examined platform log. The next
[managed/helper consumer](docs/WORKLOAD_HELPER.md) activates it during NEW helper
volume creation and preserves it across live handles, handoff and reopen.
That integration requires new-source qualification; automatic spending/learning
and useful GPU gains remain unfinished.

The managed/helper consumer at5b58568 passed all13 jobs,4,645 workload
assertions and590 VM app assertions, plus one healthy owner GUI workflow with
clean retirement. The next [input-funded advisory controls](docs/OPTIMIZATION_BUDGET.md)
add persistent settlement and charged repair/replacement with fresh-input loss
baselines. Publishing-source qualification is required; no automatic learner or
useful GPU gain is claimed.


The [funded helper consumer](docs/ADVISORY_HELPER.md) at `99d5365` passed all
13 integration jobs, 398 budget assertions with 42 injected failures in each
core configuration, 728 application-model assertions and a healthy owner GUI
workflow with confirmed retirement
([evidence](docs/evidence/funded-helper-99d5365.json)). It defers failed-advice
replacement before allocation until genuinely new input funds the attempt.

The [GPU codec source evaluation](docs/GPU_CODEC_CANDIDATES.md) passed 22
admission tests and actual source-bound searches for FlitzFS and pinned AMD
hipCOMP code. The [offline object gate](docs/HIPCOMP_OFFLINE.md) at `df51d8c`
passed all 20 source/staging cases and native Windows compilation using the
verified ROCm 10.1.0 SDK, retaining existing worker gates
([evidence](docs/evidence/gpu-lz4-offline-df51d8c.json)). The [bounded allocation planner](docs/GPU_LZ4_LAYOUT.md) at 0879cce passed all
five core configurations, including sanitizers and Windows native. Its expanded
object-reader command was corrected and the [independent CPU verifier](docs/GPU_LZ4_CPU_VERIFY.md)
at 86bc830 passed all five core configurations. The captured empty-host bundle
schema needs its next strict parser qualification. Current AMD synchronization
and scheduling captures also remain pending; see the [kernel review](docs/GPU_LZ4_KERNEL_REVIEW.md). No candidate object is loaded or used by file operations.
Automatic learning, durable provider mapping, useful GPU gains and the full
Windows product remain unfinished.


At 1d20f40, the optional Windows object gate passed all 28 admission cases,
AMD64 host headers, the exact gfx1100 bundle metadata, unchanged output digests
and the original SDK/worker gates with 20 confirmed worker exits
([evidence](docs/evidence/gpu-lz4-object-1d20f40.json)). Current-AMD page admission
still needs its observed-marker correction; no kernel or runtime is enabled.


The current selected-distribution AMD warp/model/stream captures at 5eaa07a passed
all nine scope cases and all three native pages. Source review keeps warp
lockstep distinct from memory visibility and priority hints distinct from OS
scheduling guarantees. The next [priority capability change](docs/HIP_PRIORITY_CAPABILITY.md)
corroborates reported outputs and classifies incomplete capability separately
from a stable invalid range. Its exact old/new CPU regression,235 assertions/12 scenarios, SDK/worker gates
and all13 core jobs passed atb2cf989
([evidence](docs/evidence/priority-capability-b2cf989.json)). No actual device case
or performance gain is claimed.


The explicit [historical retirement comparison](docs/RETIREMENT_REFERENCE.md)
uses independent disposable VMs and original deadlines. All13 ordinary jobs
passed at497066c, but both comparison variants stopped before driver/trial on a
Windows root-spelling mismatch. Directory identity and strict record reconciliation
are the next qualification; the historical retirement cause remains open.


At400f74c Linux passed16 source cases and Windows passed7 record cases before
rejecting fixture bytes; no manual driver/trial was run. Canonical Git capture
with separately verified exact/CRLF checkout identity is the next18-case gate.
The source fixture failure is not a historical retirement result or GPU evidence.


The historical comparison at ab37c4a passed both variants and exact CPU readback;
it did not reproduce or explain D01. The next [native HIP synchronization probe](docs/HIP_WARP_SYNC_PROBE.md)
is compile-only and requires SDK execution. No new kernel/runtime route or useful
GPU compression gain is claimed.


The separate HIP synchronization API probe passed ataf2f714, including9 CPU cases,
exact header/dependency/object identities and all original SDK/worker gates.
[Evidence](docs/evidence/hip-warp-offline-af2f714.json). This is offline API/type
qualification; actual compression output, runtime safety and useful gain remain unproven.

The native API probe's physical-input checks passed at 2fdd01c
([evidence](docs/evidence/hip-warp-stable-2fdd01c.json)).
The next [explicit native-wave candidate](docs/HIPCOMP_NATIVE_SYNC.md) combines
the pinned source with native AMD wave calls and pre-read index guards in a
private compiler fixture. Its CPU oracle and offline compilation are pending;
normal filesystem I/O remains unchanged.

The native-wave candidate qualified offline at 09c4fca
([evidence](docs/evidence/hipcomp-native-sync-09c4fca.json)).
The next [bounded byte-compression entry](docs/HIPCOMP_BOUNDED_ENTRY.md) checks
the full allocation and actual wave geometry before device-memory access.
Its new CPU/compiler gates are pending; no filesystem or worker invokes it.

The bounded entry passed CPU/offline qualification at 857d5c9
([evidence](docs/evidence/hipcomp-bounded-entry-857d5c9.json)).
The next [typed-module link fixture](docs/HIPCOMP_MODULE_LINK.md) is separately
opted in and never loaded. CPU forwarding and real SDK linking remain pending.

The private typed module qualified for linking at c86559b
([evidence](docs/evidence/hipcomp-module-link-c86559b.json)).
The [bounded candidate frames](docs/LZ4_WORKER_FRAMES.md) passed portable,
sanitizer and Windows CPU-helper checks at 959606a
([evidence](docs/evidence/lz4-worker-frames-959606a.json)).
The [checked upload/readback layer](docs/GPU_LZ4_SLAB.md) passed all five core
configurations and all 13 integration jobs at 38229d1
([evidence](docs/evidence/gpu-lz4-slab-38229d1.json)). The live host model/GUI also
passed with full cleanup ([evidence](docs/evidence/owner-gui-38229d1.json)).
The [independent CPU decoder in the SDK worker](docs/CPU_LZ4_WORKER.md) qualified
at 94ec882, including both capability states, the original 20 worker exits and
all 13 ordinary integration jobs
([evidence](docs/evidence/cpu-lz4-worker-94ec882.json)).
The [source-snapshot accounting correction](docs/SOURCE_SNAPSHOT.md) is separate
and pending. Actual GPU compression remains unimplemented.
