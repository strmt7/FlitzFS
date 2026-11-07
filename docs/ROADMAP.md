# Development gates and acceptance criteria

This is a staged plan, not a claim that the full product is implemented.
Development stays on `main`; research, regressions and actual CI results gate
promotion. No new branches, force pushes or owner-PC operations are needed.

| Stage | Status / deliverable | Promotion evidence or remaining gate |
|---|---|---|
| M1: CPU chunk core | Implemented experimental v1 pack/read/verify/new-only extraction | CPU codecs, corruption/bounds tests, Linux/Windows CI; retain original data |
| M1.5: OS handles and catalog | Implemented regular-file reads, exact Zstd frames, bounded virtual tree | No-follow/special-file tests, case/path/collision tests, codec/raw/sanitizer CI |
| M1.6: concurrency and verification | Implemented per-file locks and bounded whole-catalog verification | Deterministic concurrent byte parity and corruption detection |
| M1.7: Windows virtual projection | Implemented root-relative ASCII translation and short reads | Portable tests and Windows CPU CI; no mount |
| M1.8 / release 0.2: native callback foundation | Implemented OFF-by-default WinFsp read-only backend, pre-allocation budgets and physical-path guards | Direct real-SDK Windows callback tests plus Linux/raw/sanitizers; this is NOT mounted qualification |
| M2: FlitzZip compatibility | Not implemented: read-only .flzip adapter | Source/license/API review; independently verified block integrity; malicious corpus and byte-for-byte reference tests |
| M3: Windows read-only mount | Not implemented: lifecycle layer and disposable mount | No collisions/recursion; explicit driver provisioning; clean unmount/restart; actual ACL/mapped-I/O/cancellation/app tests; CPU export |
| M4: adaptive CPU/HIP | Not implemented: bounded GPU batches and measured routing | Tiny synthetic opt-in tests, independent CPU parity, headroom/timeout handling, full H2D/kernel/D2H timing and demonstrated end-to-end benefit |
| M5: authenticated immutable store | Not implemented: AEAD records, authenticated manifest, key handling | Vetted crypto; nonce/clone/rollback analysis; authentication-negative/fuzz tests; independent recovery |
| M6: transactional writes | Not implemented: append log, COW metadata, durable flush and compaction | Fault injection at every commit, out-of-space handling and acknowledged-write recovery |
| M7: limited writable preview | Not implemented: opt-in disposable mounted folder | M6 plus ACL/locking/mapped I/O/rename/app tests and independent backups; never automatic sole-copy use |

The native callback milestone was advanced ahead of archive compatibility to
test Windows ABI semantics independently of FlitzZip integration. It does not
skip or imply either of those later gates. The current virtual namespace is a
small in-memory catalog, not a persistent directory store.

## Immediate integration checklist

- Keep the now-tested native callback interface separate from mount lifecycle.
- Define disposable mountpoint reservation, explicit driver presence checks,
  resource supervision, cancellation/shutdown and immutable backing containment.
- Retain the no-install/no-mount callback lane as a fast regression check.
- Build `.flzip` CPU compatibility from real source and retained fixtures,
  without importing unsafe GPU initialization or claiming existing parity.
- Design persistent manifest and authenticated-record formats before writable
  storage, rather than stretching the single-stream v1 file into a journal.

## Performance comparison matrix

Use the same data, cache state and equivalent durability. Measure actual
allocated backing bytes including metadata; logical throughput; 4 KiB random
p50/p95/p99 latency; sequential throughput; CPU/RAM/VRAM; H2D/kernel/D2H time;
GPU busy-vs-idle effects; measured power; write amplification; recovery time.
Compare raw NTFS, CPU FlitzFS, GPU FlitzFS and built-in Windows compression when
applicable. Correctness runs are not performance evidence. Heavy benchmarks,
stress tests and GPU workloads remain disabled by default.
