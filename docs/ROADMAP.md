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
| M2: FlitzZip compatibility | Implemented bounded CPU v1–11 block decoding, CLI and projection route | Exact supported subset in FLZIP_READONLY.md; GPU execution/full CLI parity remain unimplemented; native CPU/reference/mounted qualification depends on matching CI |
| M3: Windows read-only mount | Lifecycle, live synthetic reads and process-crash/remount probe implemented; see per-commit CI | No collisions/recursion; explicit driver provisioning; clean unmount/restart; actual ACL/mapped-I/O/cancellation/app tests; CPU export |
| M4: adaptive CPU/HIP | Bounded six-kind batch/kernel candidate and result gate implemented; offline gfx1201 compilation only | Live supervised runtime, Windows HIP compiler/hardware qualification, real GPU parity, H2D/kernel/D2H timing and measured routing remain unimplemented/unqualified |
| M5: authenticated immutable store | Not implemented: AEAD records, authenticated manifest, key handling | Vetted crypto; nonce/clone/rollback analysis; authentication-negative/fuzz tests; independent recovery |
| M6: transactional writes | Not implemented: append log, COW metadata, durable flush and compaction | Fault injection at every commit, out-of-space handling and acknowledged-write recovery |
| M7: limited writable preview | Not implemented: opt-in disposable mounted folder | M6 plus ACL/locking/mapped I/O/rename/app tests and independent backups; never automatic sole-copy use |

The native callback milestone was advanced ahead of archive compatibility to
test Windows ABI semantics independently of FlitzZip integration. It does not
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
- Expand `.flzip` CPU compatibility only from source-pinned contracts and independent
  fixtures; retain explicit unsupported-codec failures and no HIP initialization.
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
evaluates recovery obligations before selecting a writable format; M6 remains
unimplemented. Ordinary-file I/O, namespace durability, integrity, independent
recovery and fault-injected writable qualification remain required. The native
research provider now runs in a bounded isolated hosted VM, never on the owner PC.
