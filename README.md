# FlitzFS

> **Milestone 1.5 / experimental CPU-only container and read-only catalog core**. This source tree is a **development prototype**, not a mounted Windows filesystem, an encryption product, or a GPU implementation. It has not been tested on the owner's Windows PC or AMD GPU. **Do not store your only copy of valuable data in this format.**

FlitzFS explores a Windows virtual filesystem for compressed and eventually authenticated/encrypted data, with adaptive CPU/GPU processing. This milestone provides the first independently addressable, corruption-checked `.flfs` chunk container plus a small, in-memory virtual directory catalog. It is intentionally isolated from any disk formatting, drive mounting, kernel, GPU, or disk-management APIs.

## Implemented now

- C++20 streaming pack/read/extract and `flitzfs` command-line utility.
- Read-only OS-handle source and container reader: pinned regular-file handles, no-follow final symlinks/reparse points, nonblocking special-file rejection on POSIX, strict offset reads.
- Small in-memory `ReadOnlyCatalog` for up to 16 `.flfs` sources: virtual nested directories, safe conservative ASCII path validation, case-insensitive lookup, collision detection, 4 MiB total index budget; no mount, persistent manifest, or write operations.
- 128 KiB default independently compressed chunks (selectable powers of two between 4 KiB and 1 MiB).
- Per-chunk Zstandard compression (when the library is available); automatic raw fallback when Zstd is not useful.
- Random byte-range reads without decompressing unrelated chunks; 4 MiB per-call cap; bounded single-chunk decoded cache.
- Bounded, verified metadata index plus a final commit footer; decoded-chunk CRC32 checks.
- New-file-only output creation (`CREATE_NEW` on Windows, `O_CREAT|O_EXCL|O_NOFOLLOW` on POSIX); no overwrites, renames, deletions, or recursion.
- Default 256 MiB input/extraction cap (up to 4 GiB with explicit `--max-mib`); single-threaded CPU processing; at least 1 GiB of projected free-space headroom before writing.
- CPU-focused tests for round trips, random access, malicious metadata, truncation, corruption, case-insensitive catalog names, path traversal, nonregular file inputs, range limits, and refusing existing outputs.

## Not implemented

- **No WinFsp mount or virtual drive. No Windows filesystem filter or disk driver.**
- **No AMD HIP/GPU operations** (not even device initialization).
- **No encryption or authentication.** CRC32 catches accidental corruption, not deliberate tampering.
- No persisted directory tree or mounted folder, concurrent writes, journaling, snapshots, crash-safe mutable filesystem, or migration from `.flzip` yet.
- The `.flfs` format is preliminary and not a stable promise. CRC32 is not an integrity guarantee against deliberate changes.

## Build and bounded CPU tests

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFLITZFS_BUILD_TESTS=ON
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure --timeout 45
```

With Zstandard headers and library available, CMake enables CPU Zstd. Without them, it builds a raw-only container implementation; a raw-only build cannot decode an existing container that contains Zstd chunks. On Windows, a Visual Studio compiler and CMake are sufficient for the raw-only variant.

### CLI (only on expendable test files)

```sh
flitzfs pack sample.dat new-sample.flfs
flitzfs info new-sample.flfs
flitzfs verify new-sample.flfs
flitzfs probe new-sample.flfs 4096 2048
flitzfs extract new-sample.flfs newly-extracted.dat
```

All `pack`/`extract` targets **must be names that do not exist**. There is deliberately no `--force` flag. `info` validates the index; `verify` decodes and checks all chunks. `pack` may leave an invalid *new* output if interrupted. This is an experimental format: always retain the source file.

Options for `pack`: `--chunk-kib 128`, `--level 3` (Zstd 1..5), `--max-mib 256` (1..4096). Option for `extract`: `--max-mib 256` (1..4096). Defaults are conservative by design.

## Safety scope

See [SAFETY.md](docs/SAFETY.md) before any experiment. The code is designed to operate only on explicitly named ordinary files; it does **not** enumerate drives, write raw sectors, format disks, modify Windows services, or schedule background workloads. The free-space check is preventative, not a guarantee against concurrent disk usage, hardware failure, or software defects. Tests should remain isolated and should never use original data.

## Research and design inputs

[Research and cross-repository review, 7 November 2026](docs/RESEARCH_2026-11-07.md) records current Windows/WinFsp, AMD HIP, Zstandard, supply-chain guidance, and an inventory of techniques considered from the maintainer's other repositories. No code was copied from the other repositories.

## Design and next milestones

- [Format v1](docs/FORMAT-v1.md): layout, bounds and verification contract.
- [Architecture](docs/ARCHITECTURE.md): how the current core is intended to support a WinFsp read-only mount and eventual GPU batching.
- [Safety and threat model](docs/SAFETY.md): resource limits, corruption/integrity limits, and explicit no-go operations.
- [Development gates](docs/ROADMAP.md): acceptance criteria before mounts, GPU tests, encryption or writable volumes.

**Reuse FlitzZip implementations only after code-level compatibility, license, and resource-safety review.** This first milestone does not silently copy archive code or claim `.flzip` interoperability.
