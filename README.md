# FlitzFS

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="resources/brand/flitzfs-logo-dark.png">
  <img src="resources/brand/flitzfs-logo-typed.png" alt="FlitzFS — red layered F symbol and Segoe UI Bold wordmark" width="640">
</picture>

FlitzFS is an experimental **Windows 11 AMD64 compressed filesystem**.
The goal is high sustained read/write speed, strong automatic lossless
compression, useful AMD GPU acceleration with independent CPU fallback, and
low-impact local learning that improves compression and layout over time.

**This is not a production-ready filesystem.** The Windows application,
production writable integration, scaling, automatic learning/maintenance and
general repair remain unfinished. Ordinary filesystem operations remain CPU-only.
Use disposable fixtures and retain independent copies of valuable data.

[Current assessment](docs/REPOSITORY_ASSESSMENT.md) ·
[Detailed plan](docs/DEVELOPMENT_PLAN.md) ·
[Continuation ledger](docs/DEVELOPMENT_PLAN.json) ·
[Safety rules](docs/SAFETY.md)

## Current verified checkpoint

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
despite this green matrix. See [the execution ledger](docs/DEVELOPMENT_STATE.json).

## Implemented experimental components

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
