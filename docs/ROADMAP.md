# Development gates and acceptance criteria

This is a staged plan, not a claim that the full product is implemented.
Development stays on `main`; research, regressions and actual CI results gate
promotion. No new branches, force pushes or owner-PC operations are needed.

Optimize sustained read/write throughput and physical compression together;
minimum latency is secondary. Future codecs are not restricted to FlitzZip's
algorithms. The broad file-type corpus, compression portfolio and comparative
acceptance criteria are specified in COMPRESSION_STRATEGY.md. No superiority
claim follows from compatibility tests or one synthetic throughput comparison.

| Stage | Status / deliverable | Promotion evidence or remaining gate |
|---|---|---|
| M1: CPU chunk core | Implemented experimental v1 pack/read/verify/new-only extraction | CPU codecs, corruption/bounds tests, Linux/Windows CI; retain original data |
| M1.5: OS handles and catalog | Implemented regular-file reads, exact Zstd frames, bounded virtual tree | No-follow/special-file tests, case/path/collision tests, codec/raw/sanitizer CI |
| M1.6: concurrency and verification | Implemented per-file locks and bounded whole-catalog verification | Deterministic concurrent byte parity and corruption detection |
| M1.7: Windows virtual projection | Implemented root-relative ASCII translation and short reads | Portable tests and Windows CPU CI; no mount |
| M1.8 / release 0.2: native callback foundation | Implemented OFF-by-default WinFsp read-only backend, pre-allocation budgets and physical-path guards | Direct real-SDK Windows callback tests plus Linux/raw/sanitizers; this is NOT mounted qualification |
| M2: native compression mechanisms | Bounded CPU extent mechanisms and independent tests implemented | Native extent framing, adaptive selection, diverse-corpus ratio/throughput and CPU recovery; no archive compatibility goal |
| M3: Windows read-only mount | Lifecycle, live synthetic reads and process-crash/remount probe implemented; see per-commit CI | No collisions/recursion; explicit driver provisioning; clean unmount/restart; actual ACL/mapped-I/O/cancellation/app tests; CPU export |
| M4: adaptive CPU/HIP | Bounded six-kind batch/kernel candidate and result gate implemented; offline gfx1201 compilation only | Live supervised runtime, Windows HIP compiler/hardware qualification, real GPU parity, H2D/kernel/D2H timing and measured routing remain unimplemented/unqualified |
| M5: authenticated immutable store | Not implemented: AEAD records, authenticated manifest, key handling | Vetted crypto; nonce/clone/rollback analysis; authentication-negative/fuzz tests; independent recovery |
| M6: transactional writes | Bounded compressed-object COW engine, two barriers, integrity and CPU reopen implemented; writable mounts/reopen/compaction absent | Fault injection at every commit, out-of-space handling and acknowledged-write recovery |
| M7: limited writable preview | Not implemented: opt-in disposable mounted folder | M6 plus ACL/locking/mapped I/O/rename/app tests and independent backups; never automatic sole-copy use |

The native callback milestone was advanced ahead of archive compatibility to
test Windows ABI semantics independently of compression selection. It does not
skip or imply either of those later gates. The live namespace is a small
in-memory catalog that can now be reopened from a bounded plaintext manifest.
It is not a transactional or authenticated directory store.

## Persistent catalog addition

`CATALOG_MANIFEST.md` specifies the implemented read-only 8 KiB/16-record
collection manifest and CLI operations. This advances persistence but does not
satisfy M5 authenticated metadata or M6 mutable/durable transactions.

## Immediate integration checklist

- Keep the now-tested native callback interface separate from mount lifecycle.
- Define disposable mountpoint reservation, explicit driver presence checks,
  resource supervision, cancellation/shutdown and immutable backing containment.
- Retain the no-install/no-mount callback lane as a fast regression check.
- Develop native compressed extent framing, adaptive selection and independent
  CPU recovery. Keep GPU initialization out of ordinary CPU operations.
- Keep the implemented plaintext manifest distinct from future authenticated
  records and transactional metadata; do not stretch v1 containers into a journal.

## Performance comparison matrix

Use the same data, cache state and equivalent durability. Measure actual
allocated backing bytes including metadata; logical throughput; 4 KiB random
p50/p95/p99 latency; sequential throughput; CPU/RAM/VRAM; H2D/kernel/D2H time;
GPU busy-vs-idle effects; measured power; write amplification; recovery time.
Compare raw NTFS, CPU FlitzFS, GPU FlitzFS and built-in Windows compression when
applicable. Correctness runs are not performance evidence. Heavy benchmarks,
stress tests and GPU workloads remain disabled by default.

The CPU pipeline now reuses codec contexts and chunk workspaces without changing
v1 storage bytes. Its bounded hosted comparison alternates the pinned baseline
and candidate on identical synthetic data and verifies complete container and
extraction parity. This is an initial throughput regression check, not the
cross-filesystem/compression frontier study. See CPU_PIPELINE.md.

## GPU preparation addition (0.3.2)

The immutable batch planner, independent CPU reference, allocation-free HIP
kernel candidate and result gate are described in HIP_BATCH_PLAN.md. Offline
compilation and CPU semantic tests are evidence only for their named stages;
see HIP_BATCH_VALIDATION.md. No runtime loading, kernel launch, speedup or
filesystem GPU integration is implied. The next gate must provide an explicit
supervised worker, exact adapter/module admission and end-to-end hardware tests
before changing normal CPU-only behavior.

## Executable durability gate

PERSISTENCE_MODEL.md defines a test-only commit-persistence experiment. It
evaluates recovery obligations separately from the bounded object engine;
M6 remains incomplete. Ordinary-file I/O, namespace durability, integrity, independent
recovery and fault-injected writable qualification remain required. The native
research provider now runs in a bounded isolated hosted VM, never on the owner PC.

## Concrete transaction engine gate

OBJECT_STORE_FORMAT.md records the new bounded native object format, admission,
recovery and fault-test contract. It provides a base for compressed immutable
extents and durable namespace transactions. Capacity changes require explicit
decoded/physical/staging budgets, bounded metadata validation, no-write negative
admission and recovery tests. Production scaling and exposed application writes
still require reclaim, writable file reopen, process/OS interruptions and
physical durability qualification. Do not call the object engine a qualified mounted
filesystem or general power-loss guarantee. GPU work remains optional and must
never become a dependency of acknowledged-data recovery.

## Filesystem scope correction

Archive reader/CLI/catalog/mount routes and archive serialization tests are
removed. FlitzZip supplies mechanism inspiration and optional test-only CPU
comparisons. Its archive format/product parity is not a FlitzFS requirement.
EXTENT_CODECS.md replaces the former archive compatibility development track.

## Compression connected to transactions

The object engine admits native zero/motif/Zstandard candidates, independent
CPU round-trip checking and decoded integrity before publication. It preserves
raw-mode bytes and keeps codec failure outside the I/O-poisoned state. This
advances M6 storage primitives; native file/extent namespace, writable reopen,
reclaim and mounted mutation semantics remain required. The hosted comparison
is RAM-backed native-engine evidence, not cross-filesystem qualification.

## Continued persistent storage

Exclusive custom-backend resume, uncertain-tail preservation and missing-root
repair advance the transactional persistence foundation. The new hosted process
interruption gate must verify acknowledged history and successful continuation
on Windows and Linux before public existing-file admission or writable namespace
integration. The file adapter's existing-image mode remains internal, hosted-only
and confined to uniquely owned fixtures. See STORE_RESUME_AND_INTERRUPTION.md.

## Native file semantics connected to transactions

NATIVE_FILESYSTEM.md specifies the implemented directory/inode/extent layer:
atomic metadata/data updates, sparse holes, partial writes, truncate/regrow,
identity-preserving rename/replace, removal and persistent snapshots. It operates
within the current bounded object store; this does not satisfy production
capacity, mounted application semantics or full Windows metadata compatibility.
The hosted interruption gate includes a combined native namespace/data update
and continuation after recovery, alongside the original storage/repair cases.

## Bounded capacity and incremental staging

The native object envelope now admits 256 live records under an independent
16 MiB decoded live-data ceiling. The file layer admits 256 inodes and 255
nonzero 64 KiB extents, with a 63-dirty-extent transaction allowance and a proven
64 KiB metadata ceiling. Final-budget admission precedes codec work; overlap
validation sorts bounded ranges. Writer staging checks namespace and touched
records instead of rereading the full volume on each write. Public snapshot,
open and resume still fully verify reachable data. NATIVE_FILESYSTEM.md defines
the immutable-backend assumption and delayed detection of untouched corruption.
These are developmental scaling steps; multi-gigabyte allocation, reclamation,
writable mounted semantics and hardware qualification remain outstanding.

## Compacted copy and autonomous maintenance direction

COMPACTED_COPY.md supplies a source-preserving native relocation primitive:
latest-snapshot/lease checks, a new volume identity, CPU-verified copied records,
two complete separately flushed roots, interruption recovery and continuation.
It excludes obsolete history from the destination; source storage is retained.
Automatic durable handoff, reader retirement and actual reclamation remain.

AUTONOMOUS_MAINTENANCE.md records the required workload-driven learning,
automatic compression/layout/fragmentation improvement, low-priority bounded
CPU/GPU execution and verified redundant self-repair. Learning state is advisory
and cannot authorize unsafe publication or replace independent CPU recovery.
The executor/model/replica-repair mechanisms are not implemented by copying.

## Current acknowledged-metadata replication

The native file layer now uses two current roots and isolated replicated
manifests, with pre-write health fencing and independent repair. This closes
the previously identified latest-root-loss metadata gap for upgraded checkpoints;
REPLICATED_CHECKPOINTS.md specifies the exact fault envelope and costs. It does
not close file-payload redundancy, physical persistence, automatic handoff/
reclamation, resource-priority/learning or AMD-runtime/application gates.

- Asynchronous Windows transport now separates CPU-result selection from helper
  cancellation and exact process/thread retirement. Its single-slot lifetime
  and abandonment checks run as CPU-only hosted fixtures. Production recovery-
  policy/runtime identity integration, automatic private AMD component setup,
  actual HIP dispatch and hardware fault/benefit qualification remain required.

## Redundant payload writes

The native file layer elides exact unchanged bytes and ordered updates that
restore the same inode/block contents. It preserves metadata publication,
replicated barriers, source-read verification, independent recovery and all
existing limits. NATIVE_FILESYSTEM.md specifies the bounded comparison cost and
fault/regression checks. This reduces one source of encoding/write amplification;
it does not establish a throughput advantage over other filesystems or implement
background learning, writable mounts, production capacity or GPU dispatch.
