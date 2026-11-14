# FlitzFS repository assessment

Assessment date: 11 November 2026. Reviewed baseline:
[8a5633b588869eb3808d089fca76937c6781991c](https://github.com/strmt7/FlitzFS/commit/8a5633b588869eb3808d089fca76937c6781991c),
tree 01cf46b6fd34107ff53bb0f2604a2b8c1f10b8c7.
The continuing objective is the full Windows 11 AMD64 filesystem/helper/driver
product, with exceptional lossless compression, high read/write throughput,
effective AMD acceleration, independent CPU recovery and autonomous low-impact
learning/maintenance. This assessment does not redefine completion as a small
container, a successful kernel, or a green test suite.

The repository contains a substantial experimental CPU storage/filesystem core
and real disposable-VM Windows integration. It is **not yet a production
filesystem or the requested autonomous GPU-compression product**.
The detailed execution contract is [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md);
[DEVELOPMENT_PLAN.json](DEVELOPMENT_PLAN.json) is its compact harness ledger.

## 14 November integration update

The source-bound [native LZ4 composition](evidence/native-lz4-composition-a047952.json)
at a047952 passed all 13 core jobs and SDK qualification, then one bounded actual
AMD trial stored the exact GPU payload and independently recovered the current
file and prior acknowledged snapshot on CPU. Its full image and startup costs
show no whole-operation benefit yet. Normal Windows I/O remains CPU-only.

The asynchronous compression controller at f08e6b8 passed all 13 core jobs and
full SDK qualification. It addresses terminal CPU selection, late output and recovery
policy through the existing supervised worker transport. The next product slice
is ordinary Windows helper write/read integration and complete cost/benefit
measurement; this does not close historical D01 or the learning, scaling,
durability and release gates. See [current release readiness](RELEASE_READINESS.md).

The counts and architecture findings below describe the explicitly pinned
11 November baseline, not the current repository inventory.

## Review method and source identity

This is a source/evidence-based architecture, readiness and efficiency review,
not a proof that every line is correct or an exhaustive vulnerability audit.
It uses pinned live source, public headers, current CI results, retained failure
logs, measured owner RAM/GPU experiments and the current design contracts.
CocoIndex routing at bc6d18d6 used eight actual searches, provider 0.2.43,
source digest 42b278ca86c178a3a35c70d6eec21c091b3a5ad8a25c71dd6536ffb0da557733.
Relevant candidate hashes were confirmed against later live source; intervening
changes were tests/documentation. Some queries returned infrastructure self-hits,
so search output is navigation evidence rather than proof of complete coverage.

The baseline has 347 files, 17 workflow definitions, 90 documentation files and
84 test/fixture files, approximately 3.76 MB total. These are file counts, not
independent test cases. The owner's local checkout remains at d6bb574 with
pre-existing tracked/untracked work; it was preserved. Current development has
used pinned repository APIs and disposable hosted checkouts rather than
resetting or overwriting that checkout.

## What currently exists

| Area | Implemented and evidenced | Important boundary |
|---|---|---|
| CPU object storage | Bounded raw/Zstandard/zero/pattern records, stored/decoded integrity, independent CPU reopen and transactional updates | Experimental capacity limits; checksums are not authenticated encryption |
| Native file model | Persistent inode IDs/directories, sparse extents, partial writes, truncate/regrow, rename/replace, deletion/lifetime, metadata/security descriptors and allocation accounting | Small namespace/populated-data capacity; incomplete full Windows semantics |
| Durability primitives | Replicated current checkpoints, separate roots/manifests, three durable barriers, modeled write failures and explicit recovery | Qualified backend assumptions; file payload replicas and general autonomous repair are absent |
| Compaction/recompression | Preserved-source NEW-image construction, volume-head selection, managed-session handoff, incremental preparation and cancellation | Explicit primitives; broad automatic reclaim, scheduling and production scale are unfinished |
| Windows integration | Native WinFsp read-only projection plus hosted-only writable adapter/probes with real Win32 I/O, Flush, restart and RAM handoff | Disposable VM experiments, not a finished helper/app/installer or latest Windows 11 client qualification |
| GPU preparation/runtime | Current complete ROCm SDK compilation, typed runtime binding, verified module loading, exact adapter identity, bounded worker/channel and evidence-aware CPU fallback | Normal filesystem operations remain CPU-only |
| Actual GPU evidence | A tiny raw/fill decode launched, completed, matched CPU bytes and released resources; a separate metadata observation returned measured worker counters | Final data admission still rejects nonlocal counter growth; no general GPU codec/compression speed result |
| Compression adaptation | Source-bound sample/profile probes, explicit recompression and optional bounded match proposals | No persisted workload learner, collective optimizer or automatic resource scheduler |
| Repair | Integrity detection, metadata redundancy and selected explicit recovery paths | No ability to reconstruct an arbitrary destroyed sole payload copy |
| Packaging/portability | Dependency/source inventories, self-contained CPU qualification and installation-independence work | Production installation, updates, driver lifecycle and complete portable GPU runtime closure remain unqualified |

Primary code: [object_store.hpp](../include/flitzfs/object_store.hpp),
[native_filesystem.hpp](../include/flitzfs/native_filesystem.hpp),
[native_session.cpp](../src/native_session.cpp),
[store_codec.hpp](../src/store_codec.hpp),
[winfsp_native.cpp](../src/windows/winfsp_native.cpp).
The retired archive compatibility documents are historical; the current product
scope check forbids restoring archive-reader/CLI product dependencies.

## Actual capacity and codec limits

These are current implementation bounds, not desired hardware assumptions:

| Limit | Current value |
|---|---:|
| Object count | 256 |
| Decoded live object data | 16 MiB |
| Per-object logical bytes | 1 MiB |
| New payload per transaction | 4 MiB |
| Retained physical image bytes | 64 MiB |
| Commit unit | 64 KiB |
| Native nodes, including root | 256 |
| Populated native extents | 255, each 64 KiB |
| Maximum sparse logical file size | 1 TiB |
| Maximum read per call | 4 MiB |

A 1 TiB sparse address range does not provide capacity for a 1 TiB populated
file. The extent limit permits about 15.94 MiB of populated native allocation
across the model. Larger realistic file trees and populated files require
scalable indices, allocation, history reclamation and qualified format evolution.

The current object compression option defaults to automatic mode with
Zstandard level 6, supporting levels 1..9. Encoder/decoder allocation ceilings
are 16 MiB / 2 MiB. Raw, zero and repeated-pattern routes also exist.
This is not the requested hardware/load-aware foreground policy.
The bounded profile probe evaluates samples separately; changing a default level
would not implement autonomous learning.

## Current executed evidence

At the reviewed baseline, core run
[42997683191](https://github.com/strmt7/FlitzFS/actions/runs/42997683191)
passed all 13 listed jobs: Windows native, Linux/Windows raw/codec checks,
sanitizers, read-only mount and the hosted writable profiles. The Windows native
log reports 131 test groups and 1,163 CPU-only worker exchange checks;
the sanitizer log reports 113 groups. Those counts do not imply complete
Windows compatibility, GPU performance, security or power-loss coverage.

Complete-SDK run
[42991511965](https://github.com/strmt7/FlitzFS/actions/runs/42991511965)
passed 178 fake-API checks, all 17 existing admission/refusal exits and the
separate memory-diagnostic refusal. The diagnostic binary was built from
bc6d18d6; subsequent product inputs were unchanged while a test fixture was
corrected. Keep those source identities explicit.

The owner memory observation at bc6d18d6 reported:

| Worker observation | Value |
|---|---:|
| CPU-verified GPU output | 12,288 synthetic bytes |
| Explicit device buffers | 16,492 bytes |
| Local usage after work | 185,139,200 bytes |
| Nonlocal usage after work | 12,804,096 bytes |
| Worker-local OS budget | 16,168,480,768 bytes |
| Worker-nonlocal OS budget | 50,320,211,968 bytes |
| Child peak process commit | 80,035,840 bytes |
| Whole diagnostic elapsed time | 768 ms |

Launch, stream completion, statuses, CPU comparison, resource release, module
retirement and process exit were confirmed. No GPU data candidate was accepted:
the internal failure was completed_nonlocal_growth. These numbers describe one
host/runtime/workload. They are not product defaults, global free-memory
measurements, GPU kernel timings or a throughput benchmark.
[GPU_MEMORY_OBSERVATION.md](GPU_MEMORY_OBSERVATION.md) defines the evidence
boundary and private metadata protocol.

Current Microsoft API content was captured through the reviewed isolated
Crawl4AI provider. It defines aggregate process budgets/usages, not per-allocation
eviction proof. Its older rendered revision dates are explicitly retained in
[evidence/gpu-memory-research-9028be3.json](evidence/gpu-memory-research-9028be3.json);
the captures are not represented as newly published 2026 research.

Authenticated native Dependabot OPEN-alert access was refreshed during this
review and returned zero open alerts. This is a current alert snapshot, not a
complete vulnerability assessment or proof that all dependency choices are optimal.

## Problems in priority order

1. **GPU result admission rejects useful work.** The current process-wide nonlocal
   usage increase is treated as a terminal failure even after exact CPU comparison,
   clean completion and retirement. The worker, result gate and match controller
   must use one coherent, bounded policy derived from current budgets, physical/
   commit headroom and the admitted workload. Do not replace zero with constants
   inferred from this PC. Unknown telemetry, stale identity and unfinished work
   must still select the CPU result. This requires new qualification, not another
   unchanged owner-GPU retry.
2. **Hosted child retirement has an unresolved intermittent failure.** Run
   [42983895583](https://github.com/strmt7/FlitzFS/actions/runs/42983895583)
   crash-restart and run
   [42992856099](https://github.com/strmt7/FlitzFS/actions/runs/42992856099)
   crash probes exceeded the existing 15-second operation and 2-second exit
   observation bounds without confirmed exit. One reported peak was 4,771,840
   bytes. The later green matrix does not fix or explain this. Collect bounded
   phase/wait/cancellation evidence and reproduce the lifecycle defect before
   expanding mounted testing; do not merely increase the deadline.
3. **The native compression path is still CPU based.** Experimental decoder
   formats and GPU kernel success do not establish acceleration of native Zstd
   writes/reads. Match proposals currently retain independent CPU verification
   and may perform both baseline and proposed encodes. Prove native byte-exact
   end-to-end benefit including transfers, startup, validation and fallback.
4. **The complete automatic controller is absent.** A fixed default compression
   level and bounded profile probes are useful building blocks, not an adaptive
   resource governor, learned foreground selection, persistent model area or
   unattended collective optimizer. Implementation must retain deterministic
   CPU readability and treat learned decisions as advisory.
5. **Scale is experimental.** Sparse addressability does not remove the populated
   extent, object, checkpoint/history and capacity limits. Raising constants
   alone would increase memory and validation costs. Design incremental indexes,
   allocation/reclamation and bounded verification with an explicit compatibility
   and recovery story.
6. **Durability and repair are narrower than the requested product.** Existing
   metadata replication, barriers and preserved-source handoff do not recreate
   an unrecoverable payload or qualify physical power-loss behavior. Define
   fault assumptions, payload redundancy, corruption localization and
   acknowledged-write recovery before claiming self-repair.
7. **Windows product integration is unfinished.** Disposable hosted mount probes
   are valuable, but Windows 11 client behavior, trusted caller authority,
   security semantics, mapped I/O/cancellation and the helper/application/
   installation lifecycle remain release gates.
8. **Current documentation is inconsistent.** The previous development-state
   ledger still identified checkpoint 89751f2 and an unimplemented HIP endpoint.
   Historical results must remain historical; current verified behavior and open
   failures need one compact, source-bound continuation ledger.

The filesystem cannot promise survival of every future software bug, total
destruction of all replicas, or lossless reduction of every possible byte string.
The engineering target is explicit failure containment, acknowledged-write
semantics, independent recovery and measured compression gains under published
conditions. Unrecoverable bytes must be reported rather than silently invented.

## Efficiency opportunities and limits of the evidence

- The measured 768 ms diagnostic includes cold process/runtime/context/module
  work. The result controller's 1–100 ms batch deadline and the separate
  five-second startup scope need explicit lifecycle accounting. This is a
  design mismatch to investigate, not a kernel-throughput measurement.
- Eight-block/256-KiB batch limits, one request per retired process and independent
  CPU redo constrain amortization. Any resident-worker, larger-batch or reduced-
  verification design needs equivalent safety evidence before replacing them.
- Whole-image/candidate validation, checkpoint padding, history and dual CPU
  encoding can dominate useful work. Instrument their bytes, allocations and
  time separately before selecting optimizations.
- Historical synthetic compression trials and the six-file real-source probe
  are smoke/performance evidence within their exact protocols. They are not a
  representative corpus or a comparison against mature filesystems. In the
  real-source trial, 118,663 input bytes and a 327,680-byte compacted image include
  protocol metadata/history and mutations; reporting payload reduction alone
  would misrepresent complete filesystem cost.
- There are 17 workflows and large instruction/report files. Central changes
  have triggered 13 pipelines, repeating setup and broad validation. Preserve
  required gates while researching immutable focused candidate qualification,
  content-bound reuse and efficient failure-log extraction. Main-only development,
  no force-push and no owner-driver installation remain mandatory.
- The local checkout is older and dirty. Synchronizing it destructively would
  risk owner work. Native API development against the verified main tree avoids
  that risk but makes full-file edits expensive; keep changes scoped and bind
  every result to its source.
- Dependency alerts are only one input. Audit exact package/tool/runtime versions,
  compatibility, provenance and current official support before an upgrade;
  ROCm package and HIP runtime numbering must not be assumed interchangeable.

## Execution and acceptance

Follow [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md) and its JSON ledger. The first
implementation work is adaptive GPU admission plus independent hosted retirement
triage. Neither depends on claiming a complete learner or rewriting the whole
filesystem at once. The continuing goal remains active through all packages.

Owner testing remains unmounted, locked-RAM backed and bounded. No raw devices,
driver changes, services, scheduled tasks or stress tests are authorized. The
read-only disposable fixture is the initial mounting gate on isolated VMs.
No current evidence establishes superiority over all existing filesystems,
production data safety, physical power-loss survival or GPU throughput gains.

Relevant implementation contracts:
[GPU_RECOVERY.md](GPU_RECOVERY.md),
[GPU_MEMORY_OBSERVATION.md](GPU_MEMORY_OBSERVATION.md),
[RESILIENT_STORE_DESIGN.md](RESILIENT_STORE_DESIGN.md),
[COMPRESSION_STRATEGY.md](COMPRESSION_STRATEGY.md),
[AUTONOMOUS_MAINTENANCE.md](AUTONOMOUS_MAINTENANCE.md),
[WINFSP_CALLBACKS.md](WINFSP_CALLBACKS.md),
[NATIVE_SECURITY.md](NATIVE_SECURITY.md),
[NATIVE_STORE_MEASUREMENTS.md](NATIVE_STORE_MEASUREMENTS.md),
[HOST_RAM_VALIDATION.json](HOST_RAM_VALIDATION.json).
