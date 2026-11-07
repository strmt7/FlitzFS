# CPU storage-core validation — M1 through M1.6 CI and local M1.7

**Environment:** isolated Linux build container, 7 November 2026. GCC 14.2.0, Clang 17.0.0, CMake 3.31.6, Zstandard 1.5.7. No Windows machine, mounted filesystem, AMD RX 9070 XT, HIP runtime, or physical disk benchmark was used.

| Check | Result | What was actually verified |
|---|---|---|
| GCC + Zstandard build and CTest | PASS | Compiles and runs CPU codec tests. |
| GCC raw-only build and CTest | PASS | Compiles and runs without Zstd; raw-only fallback works. |
| Clang + AddressSanitizer + UndefinedBehaviorSanitizer | PASS | Current deterministic tests produced no sanitizer reports. |
| CLI smoke test | PASS | `pack`, `info`, `verify`, `probe`, `extract`; SHA-256 byte-identical round trip; existing-output refusal. |
| GitHub Actions Linux (M1) | PASS | Build and bounded unit tests passed on push/PR CI of commit 84efaa8. |
| GitHub Actions Windows MSVC (M1) | PASS | Raw-only build passed for M1 (84efaa8) and M1.5 (51adb76) in GitHub Windows CI run 42369383337. M1.6 concurrency additions passed Windows CI run 42370249875. |
| In-memory read-only catalog (M1.5) | PASS on Windows/Linux CI for commit 51adb76 | Nested paths, case-insensitive lookup, collision/traversal rejection, random reads; no mount. |
| Per-file synchronized catalog reads (M1.6) | PASS Windows/Linux CI for commit 63ec05f | Four deterministic concurrent reader threads, independently pinned backing files, no mount. |
| Bounded whole-catalog verification (M1.6) | PASS Windows/Linux CI for commit 63ec05f | 32 MiB default, 256 MiB maximum, rejects admission violations, decodes and detects corrupted disposable fixture. |
| In-process virtual Windows path projection (M1.7) | LOCAL Linux PASS; Windows CI pending | Root-relative ASCII paths, Win32-like EOF short reads, forged NUL/control/UNC/traversal path rejection, bounded read sizes, no mount. |
| WinFsp integration | NOT IMPLEMENTED | No mount calls or filesystem driver are present. |
| AMD HIP device / GPU workload | NOT IMPLEMENTED | No GPU touched; no claim of GPU correctness or performance. |
| Encryption, authentication, crash recovery | NOT IMPLEMENTED | CRC32 is accidental corruption detection only. |

## Deterministic test coverage

- Zero-length container round trip.
- CPU Zstandard or raw fallback on compressible and pseudo-random data.
- Whole-file and cross-chunk arbitrary byte-range reads; bounded repeated deterministic pseudo-random probes.
- Per-chunk CRC checks and full-file verify; corrupt compressed payload rejection.
- Missing footer, tampered header, invalid codec and descriptor sizes, changed reserved fields and forged index with recomputed index checksum.
- Existing target/sentinel preserved during pack and extraction attempts.
- Input/output budgets, zero-length and out-of-range access rejection.
- POSIX symlink input, container, and target refusal (Linux only).

## Known limitations

Passing the above is not equivalent to a fuzzing campaign, end-to-end Windows filesystem proof, power-loss test, thread sanitizer pass, or security audit. The format is experimental and should not hold sole copies of personal data. Windows CI for the latest milestone, Windows path normalization, and hostile concurrent mutation must be validated before recommending any local PC use. No operations have been performed on the owner's PC.

## M1.5–M1.7 additions

- Pinned OS-level read-only regular-file handles with final-component symlink/reparse refusal; FIFO nonblocking rejection on POSIX.
- Single frame Zstandard parser checks, including forged index with trailing-frame garbage.
- In-memory directory catalog with strict ASCII relative path names, case-insensitive lookup, collision and reserved-device rejection.
- Rechecked CPU Zstd/raw, ASan/UBSan. Results below should be updated only after the exact remote commit passes.

## M1.6 concurrency gate (7 November 2026, local Linux)

- Verified on a 4-thread bounded concurrency probe with 32 reads per thread, byte-for-byte comparison against disposable fixtures.
- `ReadOnlyCatalog::verify_all` rejects a too-small **aggregate** logical-byte budget before decoding and detects a corrupted payload in a newly created test fixture.
- `flitzfs verify` now refuses to decode containers above its default 256 MiB logical-byte budget unless `--max-mib N` is specified (up to 4096).
- Linux GCC+Zstd and raw-only tests pass. Clang+ASan/UBSan pass locally. ThreadSanitizer, actual Windows runtime callbacks, and AMD HIP are not qualified.
- CI must be rechecked against the exact new `main` commit; local results are not Windows proof.

## M1.7 virtual path gate (7 November 2026, local Linux)

- Three executable test suites pass in each of Zstd-enabled GCC, raw-only GCC and Clang ASan/UBSan (9 CTest executions in total).
- Projection tests exercise paths from prospective WinFsp callbacks without installing or loading WinFsp, plus short reads across EOF and zero reads beyond EOF.
- No Windows CI confirmation for M1.7 until the exact commit has a passing run.
