# FlitzFS

> **0.2.0 experimental CPU storage and native read-only callback preview.**
> This is not yet a mounted Windows filesystem, encryption product or GPU
> implementation. Do not store the only copy of valuable data in this format.
> No tests have been executed on the owner's Windows PC or AMD GPU.

FlitzFS develops transparent compressed Windows storage with eventual AMD HIP
acceleration and authenticated encryption. Current code provides independently
addressable `.flfs` chunks, a synchronized read-only directory catalog, a portable
Windows-path projection, and an optional native WinFsp callback adapter.

## Implemented

- C++20 streaming pack/read/extract CLI; CPU Zstandard with raw fallback.
- Independently checked chunks, 128 KiB default (4 KiB to 1 MiB, power of two),
  random access with a 4 MiB request limit and a one-chunk cache per reader.
- Read-only regular-file OS handles, final symlink/reparse-point rejection,
  strict offset reads, index validation and per-chunk CRC32 corruption checks.
- New-file-only pack/extract creation, no force/overwrite mode, 256 MiB default
  input/output budgets, explicit 4 GiB ceiling and 1 GiB free-space reserve.
- Up to 16 virtual files with nested directories, conservative ASCII names,
  case-insensitive lookup, collision rejection and per-file read locks.
- A 4 MiB aggregate serialized index limit enforced before index allocation.
  This limit is not a complete process-memory ceiling.
- Whole-catalog verification: 32 MiB default, 256 MiB maximum; Windows-style
  projection with bounded short reads and typed errors.
- **Optional native WinFsp read-only callbacks**, OFF by default: Open/Close,
  metadata, owner-only descriptors, reads, paginated directory enumeration and
  explicit write rejection. At most 256 open contexts and two active decodes;
  all contents verified under a 32 MiB total budget before callback exposure.

The native callback tests link the official user-mode SDK DLL but do **not**
create a filesystem, install/load a kernel driver, assign a mount point, start
a dispatcher or modify Windows services. See [native architecture, research,
bounds and tests](docs/WINFSP_CALLBACKS.md).

## Not implemented or qualified

There is no mount executable, virtual drive, filter driver, persistent manifest,
service, AMD GPU initialization, encryption or authenticated storage. Writable
transactions, crash recovery, snapshots and `.flzip` compatibility remain future
work. CRC32 detects accidental corruption, not malicious tampering.

The `.flfs` v1 format is experimental. Keep source files and independent backups.
Direct Reader calls are single-threaded; catalog operations serialize per file.
Never move or destroy a catalog/adapter while callbacks are active. Callback
unit tests do not prove mounted Windows behavior or full NTFS compatibility.

## Build and bounded CPU tests

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure --timeout 45 --parallel 1
```

With Zstandard headers/library available, CPU compression is enabled. Otherwise
the default build is raw-only and cannot decode existing Zstandard chunks.
Use `-DFLITZFS_REQUIRE_ZSTD=ON` to reject missing dependencies rather than silently
falling back. Use `-DFLITZFS_WITH_ZSTD=OFF` for an explicit raw-only build.
Windows raw-only builds require Visual Studio and CMake. The optional native
callback build and SDK layout are documented in [WINFSP_CALLBACKS.md](docs/WINFSP_CALLBACKS.md).
Normal configuration performs no downloads or installation.

### CLI — only on expendable test files

```sh
flitzfs pack sample.dat new-sample.flfs
flitzfs info new-sample.flfs
flitzfs verify new-sample.flfs
flitzfs probe new-sample.flfs 4096 2048
flitzfs extract new-sample.flfs newly-extracted.dat
```

Pack/extract outputs must not exist; exclusivity is enforced at OS creation.
`info` checks metadata, not all payloads. `verify` checks every decoded chunk
under a 256 MiB default limit. Explicit `--max-mib` accepts 1..4096 for pack,
extract and verify. Pack also accepts `--chunk-kib` (4..1024, power of two) and
`--level` (1..5). Interrupted pack/extract can leave a partial **new** output;
it never replaces the input or an existing output. Keep source data unmodified.

## Safety and evidence

There are no disk-formatting, partitioning, raw-sector, privileged service or
GPU operations in the code. Test fixtures are uniquely scoped and small; tests
never recursively delete directories. Free-space checks and resource limits
reduce risk but cannot guarantee safety against concurrent disk use, hardware
failure or software defects. The owner machine is not used for testing.

GitHub CI separates Linux Zstandard, Linux raw-only, Windows raw-only, Linux
ASan/UBSan, and Windows native callbacks with commit-pinned Zstandard. SDK
provisioning is checksum-pinned extraction on disposable hosted runners, not
installer execution. Check the exact commit's Actions results; do not infer a
Windows/native pass from Linux results or infer mount qualification from native
callback tests.

## Design references

- [Current native callback milestone and research](docs/WINFSP_CALLBACKS.md)
- [Wire format v1](docs/FORMAT-v1.md)
- [Safety and threat model](docs/SAFETY.md)
- [Architecture](docs/ARCHITECTURE.md) and [development gates](docs/ROADMAP.md)
- [Prior research and all 13 other repository reviews](docs/RESEARCH_2026-11-07.md)

Reuse FlitzZip only after source-level compatibility, provenance and safety
review. No private repository code, credentials, personal paths, generated
binaries or SDK payloads are committed. Development stays on `main` only.
