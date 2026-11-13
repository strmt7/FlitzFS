# FlitzFS

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

The experimental Windows volume application at **f4928c2d2510cacb55da664a0338c8592194d9a0**
passed isolated Windows qualification and exact-binary host checks: 377 model
checks, three balanced ownership cycles, native UTF-8 save/readback, explicit
optimization, discard and confirmed process exit. It uses the real CPU engine
and remains unmounted; physical provisioning, automatic learning and useful GPU
routing are unfinished. [Application scope](docs/WINDOWS_VOLUME_APP.md) ·
[Exact VM/host evidence](docs/evidence/volume-app-f4928c2.json).

All **13 existing core jobs** passed at that same source, including Windows
native/raw, Linux raw/codec, sanitizers and isolated mounted profiles.

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

Earlier intermittent mounted-child retirement failures remain unresolved
despite this green matrix. A [candidate external-client crash protocol](docs/MOUNT_PROCESS_ISOLATION.md)
separates the filesystem host from its application handle. Its full 8122bf5 hosted
matrix passed, including actual client/flush/self-open checks and independent
recovery. Additional CPU lifecycle-edge tests await new-source qualification.
[Source-bound evidence](docs/evidence/mount-source-isolation-8122bf5.json). See [the execution ledger](docs/DEVELOPMENT_STATE.json).

## Implemented experimental components

- A separate bounded advisory optimization-state component with two flushed
  copies and a CPU reader. Volume-owned control integration is under qualification; native provider,
  persistent write credit and learner integration remain unfinished;
  see [the experimental contract](docs/OPTIMIZATION_STATE.md).
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
