# Development gates and acceptance criteria

This is a staged plan, NOT a claim that all items are already implemented.

| Stage | Deliverable | Promotion gate |
|---|---|---|
| **M1: CPU chunk core** | Existing v1 `.flfs` pack, range-read, verify, new-only extraction | Linux CPU tests, raw-only tests, sanitizer checks, Windows CI, corruption/fault tests. Source must always be retained. |
| **M1.5: OS-handle and catalog foundation** | No-follow regular-file reads, exact Zstd frames, bounded in-memory virtual directory tree | CPU Zstd/raw, sanitizers, Windows CI, malicious namespace tests; no mount or writes from catalog |
| **M1.6: read concurrency and bounded catalog verification** | Per-file locking for mutable decoded cache and Windows file position; bounded whole-catalog corruption checks | Multithreaded deterministic tests; CPU Zstd/raw, ASan/UBSan, Windows CI; no mounts |
| **M2: FlitzZip compatibility** | Read-only `.flzip` archive adapter, independently verified per-block metadata | Reference corpus byte-for-byte; reject malicious paths, malformed metadata, oversized decoding; license/API review. |
| **M3: Windows read-only mount** | WinFsp user-mode mount for disposable test datasets | Windows automated integration suite; no mountpoint collisions; no unexpected privilege escalation or driver installation; clean unmount/restart; independent CPU export. |
| **M4: Adaptive CPU/HIP** | Bounded GPU decode batches; H2D/kernel/D2H telemetry and CPU fallback | Optional test on supported RX 9070 XT using tiny synthetic buffer, GPU busy/driver-loss tests, confirmed no hangs; measurable end-to-end advantage. |
| **M5: Authenticated, immutable store** | AEAD per record and authenticated metadata; password/key management | Third-party crypto; nonce/rollback analysis; fuzz/authentication failure tests; cross-platform CPU recovery. |
| **M6: Transactional writes** | Append-only landing log, COW metadata, durable flush contract, compaction | Fault injection and crash recovery across every commit point; out-of-space and concurrent-access tests. |
| **M7: Limited writable preview** | Opt-in mounted disposable Windows data folder | Windows ACL, mapped I/O, rename, locking, app-compatibility, long soak; full backups; no automatic use on user volumes. |

## Performance comparison matrix

All comparisons must use the same data, cache state and equivalent durability. Measure: allocated backing bytes including metadata; logical throughput; 4 KiB random-read p50/p95/p99 latency; sequential read/write speed; CPU/VRAM/RAM use; H2D and D2H transfer time; GPU busy-vs-idle effects; power where measured; application-visible latency; write amplification and recovery time. Compare raw NTFS, CPU-only FlitzFS, GPU-enabled FlitzFS, and Windows built-in compression when applicable.

Run correctness and safety suites before optional performance workloads. Heavy benchmarks and stress tests should **never** be enabled by default in CI or on the user's PC.
