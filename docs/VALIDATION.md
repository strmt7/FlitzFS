# Milestone 1 validation report

**Environment:** isolated Linux build container, 7 November 2026. GCC 14.2.0, Clang 17.0.0, CMake 3.31.6, Zstandard 1.5.7. No Windows machine, mounted filesystem, AMD RX 9070 XT, HIP runtime, or physical disk benchmark was used.

| Check | Result | What was actually verified |
|---|---|---|
| GCC + Zstandard build and CTest | PASS | Compiles and runs CPU codec tests. |
| GCC raw-only build and CTest | PASS | Compiles and runs without Zstd; raw-only fallback works. |
| Clang + AddressSanitizer + UndefinedBehaviorSanitizer | PASS | Current deterministic tests produced no sanitizer reports. |
| CLI smoke test | PASS | `pack`, `info`, `verify`, `probe`, `extract`; SHA-256 byte-identical round trip; existing-output refusal. |
| GitHub Actions Linux | Not yet run remotely | CI configuration included but not executed on GitHub. |
| GitHub Actions Windows MSVC | Not yet run remotely | Configuration included; Windows behavior remains unqualified. |
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
