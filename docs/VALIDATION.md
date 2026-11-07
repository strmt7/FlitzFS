# CPU storage-core validation — Milestone 1 and local M1.5

**Environment:** isolated Linux build container, 7 November 2026. GCC 14.2.0, Clang 17.0.0, CMake 3.31.6, Zstandard 1.5.7. No Windows machine, mounted filesystem, AMD RX 9070 XT, HIP runtime, or physical disk benchmark was used.

| Check | Result | What was actually verified |
|---|---|---|
| GCC + Zstandard build and CTest | PASS | Compiles and runs CPU codec tests. |
| GCC raw-only build and CTest | PASS | Compiles and runs without Zstd; raw-only fallback works. |
| Clang + AddressSanitizer + UndefinedBehaviorSanitizer | PASS | Current deterministic tests produced no sanitizer reports. |
| CLI smoke test | PASS | `pack`, `info`, `verify`, `probe`, `extract`; SHA-256 byte-identical round trip; existing-output refusal. |
| GitHub Actions Linux (M1) | PASS | Build and bounded unit tests passed on push/PR CI of commit 84efaa8. |
| GitHub Actions Windows MSVC (M1) | PASS | Raw-only build and bounded unit tests passed on push/PR CI of commit 84efaa8; M1.5 OS-handle/catalog changes still need Windows CI. |
| In-memory read-only catalog (M1.5) | Local Linux PASS, remote pending | Nested paths, case-insensitive lookup, collision/traversal rejection, random reads; no mount.
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

Passing the above is not equivalent to a fuzzing campaign, Windows build proof, power-loss test, or security audit. The format is experimental and should not hold sole copies of personal data. Windows CI and Windows path normalization must be validated before recommending any local PC use. No operations have been performed on the owner's PC.

## M1.5 added tests (local, pending GitHub CI)

- Pinned OS-level read-only regular-file handles with final-component symlink/reparse refusal; FIFO nonblocking rejection on POSIX.
- Single frame Zstandard parser checks, including forged index with trailing-frame garbage.
- In-memory directory catalog with strict ASCII relative path names, case-insensitive lookup, collision and reserved-device rejection.
- Rechecked CPU Zstd/raw, ASan/UBSan. Results below should be updated only after the exact remote commit passes.
